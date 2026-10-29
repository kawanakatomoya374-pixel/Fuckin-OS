/* Parser: names, function literals, statements, patterns and the program entry point. */
#include "parser_int.h"

static Node *statement(Parser *P);
static Node *block(Parser *P);

static Node *dname(Parser *P, const Token *t, const char *s, size_t n);
static Node *parse_match_x(Parser *P, const char *res, size_t reslen);

/* ---------------------------------------------------------------- */
/* names                                                             */
/* ---------------------------------------------------------------- */
void perr_at(Parser *P, const Token *t, const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    front_error(P->F, "ParseError", E_PARSE, t->line, t->col, t->offset, "%s", msg);
}

int is_name_tok(int k) { return k == TK_IDENT || (k >= TK_KW_AND && k < TK_KW_END_); }

const Token *expect_name(Parser *P, const char *what) {
    if (!is_name_tok(cur(P)->kind)) perr_at(P, cur(P), "expected %s, found %s", what, tok_name(cur(P)->kind));
    return advance(P);
}


/* ---------------------------------------------------------------- */
/* functions                                                         */
/* ---------------------------------------------------------------- */
Node *parse_func_rest(Parser *P, const Token *ftok, const Token *name, int lambda) {
    Node *f = mk(P, N_FUNC, ftok);
    if (name) {
        f->str = arena_strndup(P->A, name->s, name->len);
        f->slen = name->len;
    }
    expect(P, TK_LPAREN, "'('");
    NodeVec ps = {0};
    int seen_default = 0;
    while (!at(P, TK_RPAREN)) {
        if (at(P, TK_ELLIPSIS)) {
            advance(P);
            const Token *rt = expect(P, TK_IDENT, "rest parameter name");
            Node *r = mk(P, N_NAME, rt);
            r->str = arena_strndup(P->A, rt->s, rt->len);
            r->slen = rt->len;
            r->decl = mkdecl(P, rt->s, rt->len, 0);
            f->c = r;
            accept(P, TK_COMMA);
            if (!at(P, TK_RPAREN)) perr_at(P, cur(P), "rest parameter must be last");
            break;
        }
        const Token *pt = expect(P, TK_IDENT, "parameter name");
        Node *pn = mk(P, N_NAME, pt);
        pn->str = arena_strndup(P->A, pt->s, pt->len);
        pn->slen = pt->len;
        pn->decl = mkdecl(P, pt->s, pt->len, 0);
        for (int i = 0; i < ps.n; i++)
            if (ps.v[i]->slen == pn->slen && memcmp(ps.v[i]->str, pn->str, pn->slen) == 0)
                perr_at(P, pt, "duplicate parameter '%s'", pn->str);
        if (accept(P, TK_ASSIGN)) {
            pn->a = expression(P);
            seen_default = 1;
        } else if (seen_default) {
            perr_at(P, pt, "parameter without default follows parameter with default");
        }
        nv_push(P, &ps, pn);
        if (!accept(P, TK_COMMA)) break;
    }
    expect(P, TK_RPAREN, "')'");
    if (f->c) {
        for (int i = 0; i < ps.n; i++)
            if (ps.v[i]->slen == f->c->slen && memcmp(ps.v[i]->str, f->c->str, f->c->slen) == 0)
                perr_at(P, cur(P), "duplicate parameter '%s'", f->c->str);
    }
    f->list = ps.v;
    f->n = ps.n;
    if (lambda == 2) {
        expect(P, TK_FATARROW, "'=>' before lambda body");
        f->flag = 1;
        f->a = expression(P);
    } else if (lambda) {
        expect(P, TK_COLON, "':' before lambda body");
        f->flag = 1;
        f->a = expression(P);
    } else {
        f->a = block(P);
    }
    return f;
}

/* ---------------------------------------------------------------- */
/* statements                                                        */
/* ---------------------------------------------------------------- */
static Node *block(Parser *P) {
    const Token *t = expect(P, TK_COLON, "':'");
    expect(P, TK_NEWLINE, "newline after ':'");
    expect(P, TK_INDENT, "an indented block");
    enter(P);
    Node *b = mk(P, N_NIL, t);
    b->kind = S_BLOCK;
    NodeVec v = {0};
    while (!at(P, TK_DEDENT) && !at(P, TK_EOF)) nv_push(P, &v, statement(P));
    if (!accept(P, TK_DEDENT)) perr_at(P, cur(P), "unexpected end of file in block");
    leave(P);
    b->list = v.v;
    b->n = v.n;
    return b;
}

Node *make_name(Parser *P, const Token *t) {
    Node *n = mk(P, N_NAME, t);
    n->str = arena_strndup(P->A, t->s, t->len);
    n->slen = t->len;
    return n;
}

static void end_simple(Parser *P) {
    if (P->pending_nl) { P->pending_nl = 0; return; }
    if (at(P, TK_NEWLINE)) { advance(P); return; }
    if (at(P, TK_EOF)) return;
    perr_at(P, cur(P), "expected end of line, found %s", tok_name(cur(P)->kind));
}

static int is_assign_op(int k) {
    return k == TK_ASSIGN || k == TK_PLUSEQ || k == TK_MINUSEQ || k == TK_STAREQ ||
           k == TK_SLASHEQ || k == TK_SLASHSLASHEQ || k == TK_PERCENTEQ || k == TK_DOTDOTEQ ||
           k == TK_STARSTAREQ || k == TK_AMPEQ || k == TK_PIPEEQ || k == TK_CARETEQ || k == TK_SHLEQ || k == TK_SHREQ;
}


/* ---------------------------------------------------------------- */
/* match                                                             */
/* ---------------------------------------------------------------- */
static Node *parse_pattern(Parser *P) {
    const Token *t = cur(P);
    enter(P);
    Node *n;
    switch (t->kind) {
    case TK_MINUS: {
        advance(P);
        const Token *num = cur(P);
        Node *lit;
        if (num->kind == TK_INT) {
            advance(P);
            lit = mk(P, N_INT, num);
            lit->ival = num->big ? INT64_MIN : -num->ival;
        } else if (num->kind == TK_FLOAT) {
            advance(P);
            lit = mk(P, N_FLOAT, num);
            lit->fval = -num->fval;
        } else {
            perr_at(P, num, "expected a number after '-' in pattern");
        }
        n = mk(P, P_LIT, t);
        n->a = lit;
        break;
    }
    case TK_INT: case TK_FLOAT: case TK_STRING: case TK_KW_TRUE: case TK_KW_FALSE: case TK_KW_NIL:
        n = mk(P, P_LIT, t);
        n->a = primary(P);
        break;
    case TK_IDENT:
        advance(P);
        if (t->len == 1 && t->s[0] == '_') {
            n = mk(P, P_WILD, t);
        } else {
            n = mk(P, P_BIND, t);
            n->decl = mkdecl(P, t->s, t->len, 0);
        }
        break;
    case TK_LBRACK: {
        advance(P);
        n = mk(P, P_ARRAY, t);
        NodeVec v = {0};
        while (!at(P, TK_RBRACK)) {
            nv_push(P, &v, parse_pattern(P));
            if (!accept(P, TK_COMMA)) break;
        }
        expect(P, TK_RBRACK, "']'");
        n->list = v.v;
        n->n = v.n;
        break;
    }
    case TK_LBRACE: {
        advance(P);
        n = mk(P, P_TABLE, t);
        NodeVec v = {0};
        while (!at(P, TK_RBRACE)) {
            const Token *k = cur(P);
            Node *e = mk(P, P_ENTRY, k);
            if (k->kind == TK_STRING) {
                advance(P);
                e->str = arena_strndup(P->A, k->sval, k->slen);
                e->slen = k->slen;
            } else {
                const Token *kt = expect_name(P, "pattern key");
                e->str = arena_strndup(P->A, kt->s, kt->len);
                e->slen = kt->len;
            }
            expect(P, TK_COLON, "':'");
            e->a = parse_pattern(P);
            nv_push(P, &v, e);
            if (!accept(P, TK_COMMA)) break;
        }
        expect(P, TK_RBRACE, "'}'");
        n->list = v.v;
        n->n = v.n;
        break;
    }
    default:
        perr_at(P, t, "expected a pattern, found %s", tok_name(t->kind));
    }
    leave(P);
    return n;
}


static Node *parse_match(Parser *P) { return parse_match_x(P, NULL, 0); }

/* match statement; with `res` it is a match expression whose arms are `case p => e` and
 * assign e to the hidden variable `res` (SPEC 11.4). */
static Node *parse_match_x(Parser *P, const char *res, size_t reslen) {
    const Token *t = advance(P); /* match */
    Node *n = mk(P, S_MATCH, t);
    n->a = expression(P);
    expect(P, TK_COLON, "':'");
    expect(P, TK_NEWLINE, "newline after ':'");
    expect(P, TK_INDENT, "an indented block of 'case' clauses");
    NodeVec v = {0};
    while (at(P, TK_KW_CASE)) {
        const Token *ct = advance(P);
        Node *cs = mk(P, N_CASE, ct);
        cs->a = parse_pattern(P);
        if (accept(P, TK_KW_IF)) cs->b = expression(P);
        if (at(P, TK_FATARROW)) {
            const Token *at_ = advance(P);
            Node *e = expression(P);
            end_simple(P);
            Node *st;
            if (res) {
                Token rt = *ct;
                rt.s = res;
                rt.len = reslen;
                st = mk(P, S_ASSIGN, at_);
                st->op = TK_ASSIGN;
                st->a = make_name(P, &rt);
                st->b = e;
            } else {
                st = mk(P, S_EXPR, at_);
                st->a = e;
            }
            Node *blk = mk(P, S_BLOCK, at_);
            blk->list = arena_alloc(P->A, sizeof *blk->list);
            blk->list[0] = st;
            blk->n = 1;
            cs->c = blk;
        } else {
            if (res) perr_at(P, cur(P), "a match expression needs 'case pattern => expression'");
            cs->c = block(P);
        }
        nv_push(P, &v, cs);
    }
    if (v.n == 0) perr_at(P, cur(P), "expected 'case'");
    if (!accept(P, TK_DEDENT)) perr_at(P, cur(P), "expected 'case' or end of match block");
    n->list = v.v;
    n->n = v.n;
    return n;
}

/* primary: `match x:` with `case p => e` arms, used as a value. */
Node *match_expr(Parser *P) {
    const Token *t = cur(P);
    char nm[24];
    int nl = snprintf(nm, sizeof nm, "$m%d", (*P->ndestr)++);
    Node *m = parse_match_x(P, nm, (size_t)nl);
    P->pending_nl = 1;
    Token ft = *t;
    ft.s = nm;
    ft.len = (size_t)nl;
    Node *let = dlet(P, t, 0, nm, (size_t)nl, mk(P, N_NIL, t));
    Node *seq = mk(P, N_SEQ, t);
    Node *blk = mk(P, S_BLOCK, t);
    blk->list = arena_alloc(P->A, sizeof *blk->list * 2);
    blk->list[0] = let;
    blk->list[1] = m;
    blk->n = 2;
    seq->a = blk;
    seq->b = make_name(P, &ft);
    return seq;
}

static Node *parse_class(Parser *P) {
    const Token *t = advance(P); /* class */
    const Token *nt = expect(P, TK_IDENT, "class name");
    Node *n = mk(P, S_CLASS, nt);
    n->decl = mkdecl(P, nt->s, nt->len, 1);
    n->str = arena_strndup(P->A, nt->s, nt->len);
    n->slen = nt->len;
    if (accept(P, TK_KW_EXTENDS)) {
        const Token *pt = expect(P, TK_IDENT, "parent class name");
        Node *pn = make_name(P, pt);
        if (at(P, TK_DOT)) {
            const Token *dt = advance(P);
            const Token *mt = expect(P, TK_IDENT, "class name");
            Node *m = mk(P, N_MEMBER, dt);
            m->a = pn;
            m->str = arena_strndup(P->A, mt->s, mt->len);
            m->slen = mt->len;
            pn = m;
        }
        n->a = pn;
    }
    (void)t;
    expect(P, TK_COLON, "':'");
    expect(P, TK_NEWLINE, "newline after ':'");
    expect(P, TK_INDENT, "a class body");
    NodeVec v = {0};
    while (!at(P, TK_DEDENT) && !at(P, TK_EOF)) {
        const Token *m = cur(P);
        if (m->kind == TK_KW_PASS) {
            advance(P);
            end_simple(P);
        } else if (m->kind == TK_KW_LET) {
            advance(P);
            const Token *ft = expect(P, TK_IDENT, "field name");
            Node *f = mk(P, S_LET, ft);
            f->decl = mkdecl(P, ft->s, ft->len, 0);
            if (accept(P, TK_ASSIGN)) f->a = expression(P);
            end_simple(P);
            nv_push(P, &v, f);
        } else if (m->kind == TK_KW_FUNC || m->kind == TK_KW_OVERRIDE) {
            int ov = 0;
            if (m->kind == TK_KW_OVERRIDE) {
                ov = 1;
                advance(P);
                if (!at(P, TK_KW_FUNC)) perr_at(P, cur(P), "expected 'func' after 'override'");
            }
            const Token *ft = advance(P); /* func */
            const Token *mn = expect(P, TK_IDENT, "method name");
            Node *fn = parse_func_rest(P, ft, mn, 0);
            Node *s = mk(P, S_FUNC, mn);
            s->a = fn;
            s->str = fn->str;
            s->slen = fn->slen;
            s->op = ov;
            nv_push(P, &v, s);
        } else {
            perr_at(P, m, "only 'let', 'func', 'override func' and 'pass' are allowed in a class body");
        }
    }
    if (!accept(P, TK_DEDENT)) perr_at(P, cur(P), "unexpected end of file in class body");
    n->list = v.v;
    n->n = v.n;
    return n;
}

/* let name = e  after `if` / `while`: returns the hidden temporary's declaration and the binding name */
static Node *let_head(Parser *P, const Token *t, const Token **bind, char *tmp, size_t tmpsz, int *tl) {
    advance(P); /* let */
    *bind = expect(P, TK_IDENT, "variable name");
    expect(P, TK_ASSIGN, "'=' after 'let' name");
    Node *rhs = expression(P);
    *tl = snprintf(tmp, tmpsz, "$l%d", (*P->ndestr)++);
    return dlet(P, t, 1, tmp, (size_t)*tl, rhs);
}

/* `if let x = e:` runs the block with x bound only when e is not nil; elif / else work as usual (SPEC 11.1) */
static Node *parse_if(Parser *P) {
    const Token *t = advance(P); /* if */
    Node *n = mk(P, S_IF, t);
    NodeVec v = {0};
    Node *tmpdecl = NULL;
    char tmp[32];
    if (at(P, TK_KW_LET)) {
        const Token *bind;
        int tl;
        tmpdecl = let_head(P, t, &bind, tmp, sizeof tmp, &tl);
        nv_push(P, &v, bin(P, t, TK_NE, dname(P, t, tmp, (size_t)tl), mk(P, N_NIL, t)));
        Node *body = block(P);
        Node *bl = dlet(P, bind, 0, bind->s, bind->len, dname(P, t, tmp, (size_t)tl));
        nv_push(P, &v, block_with_prefix(P, bl, body));
    } else {
        nv_push(P, &v, expression(P));
        nv_push(P, &v, block(P));
    }
    while (at(P, TK_KW_ELIF)) {
        advance(P);
        nv_push(P, &v, expression(P));
        nv_push(P, &v, block(P));
    }
    if (accept(P, TK_KW_ELSE)) n->c = block(P);
    n->list = v.v;
    n->n = v.n;
    if (tmpdecl) {
        Node *blk = mk(P, S_BLOCK, t);
        blk->list = arena_alloc(P->A, sizeof *blk->list * 2);
        blk->list[0] = tmpdecl;
        blk->list[1] = n;
        blk->n = 2;
        return blk;
    }
    return n;
}

/* After "for x in": is there a filter "if cond :" (no "else" => not a conditional expression)? */
static int for_filter_ahead(Parser *P) {
    int depth = 0, seen_if = 0;
    for (int k = 0;; k++) {
        int kind = peek_at(P, k)->kind;
        if (kind == TK_EOF || kind == TK_NEWLINE) return 0;
        if (kind == TK_LPAREN || kind == TK_LBRACK || kind == TK_LBRACE) depth++;
        else if (kind == TK_RPAREN || kind == TK_RBRACK || kind == TK_RBRACE) depth--;
        else if (depth == 0) {
            if (kind == TK_KW_IF) seen_if = 1;
            else if (kind == TK_KW_ELSE && seen_if) return 0;
            else if (kind == TK_COLON) return seen_if;
        }
    }
}

static Node *parse_loop(Parser *P, const Token *label) {
    const Token *t = cur(P);
    Node *n;
    if (t->kind == TK_KW_WHILE) {
        advance(P);
        n = mk(P, S_WHILE, t);
        if (at(P, TK_KW_LET)) {
            /* while let x = e: body  ==  while true: let $l = e; if $l == nil: break; let x = $l; body */
            const Token *bind;
            char tmp[32];
            int tl;
            Node *tmpdecl = let_head(P, t, &bind, tmp, sizeof tmp, &tl);
            Node *body = block(P);
            Node *brk = mk(P, S_BREAK, t);
            Node *bblk = mk(P, S_BLOCK, t);
            bblk->list = arena_alloc(P->A, sizeof *bblk->list);
            bblk->list[0] = brk;
            bblk->n = 1;
            Node *chk = mk(P, S_IF, t);
            chk->list = arena_alloc(P->A, sizeof *chk->list * 2);
            chk->list[0] = bin(P, t, TK_EQEQ, dname(P, t, tmp, (size_t)tl), mk(P, N_NIL, t));
            chk->list[1] = bblk;
            chk->n = 2;
            Node *pre = mk(P, S_MULTI, t);
            pre->list = arena_alloc(P->A, sizeof *pre->list * 3);
            pre->list[0] = tmpdecl;
            pre->list[1] = chk;
            pre->list[2] = dlet(P, bind, 0, bind->s, bind->len, dname(P, t, tmp, (size_t)tl));
            pre->n = 3;
            n->a = mk(P, N_TRUE, t);
            n->b = block_with_prefix(P, pre, body);
        } else {
            n->a = expression(P);
            n->b = block(P);
        }
    } else {
        advance(P); /* for */
        n = mk(P, S_FOR, t);
        Node *pat = NULL;
        const Token *v1 = cur(P);
        if (at(P, TK_LBRACK) || at(P, TK_LBRACE)) {
            pat = loop_pattern(P, t, &n->decl);
            n->ival = 1;
        } else {
            v1 = expect(P, TK_IDENT, "loop variable");
            n->decl = mkdecl(P, v1->s, v1->len, 0);
            n->ival = 1;
        }
        if (!pat && accept(P, TK_COMMA)) {
            const Token *v2 = expect(P, TK_IDENT, "loop variable");
            if (v2->len == v1->len && memcmp(v2->s, v1->s, v1->len) == 0)
                perr_at(P, v2, "duplicate loop variable '%.*s'", (int)v2->len, v2->s);
            n->decl2 = mkdecl(P, v2->s, v2->len, 0);
            n->ival = 2;
        }
        expect(P, TK_KW_IN, "'in'");
        int filt = for_filter_ahead(P);
        n->a = filt ? pipe_expr(P) : expression(P);
        if (filt) {
            /* for x in xs if c: body  ==  for x in xs: if c: body */
            const Token *it = advance(P);
            Node *cond = or_expr(P);
            Node *body = block(P);
            Node *iff = mk(P, S_IF, it);
            iff->list = arena_alloc(P->A, sizeof *iff->list * 2);
            iff->list[0] = cond;
            iff->list[1] = body;
            iff->n = 2;
            Node *blk = mk(P, S_BLOCK, it);
            blk->list = arena_alloc(P->A, sizeof *blk->list);
            blk->list[0] = iff;
            blk->n = 1;
            n->b = blk;
        } else {
            n->b = block(P);
        }
        if (pat) n->b = block_with_prefix(P, pat, n->b);
    }
    if (label) {
        n->str = arena_strndup(P->A, label->s, label->len);
        n->slen = label->len;
    }
    return n;
}

static Node *parse_try(Parser *P) {
    const Token *t = advance(P);
    Node *n = mk(P, S_TRY, t);
    n->a = block(P);
    if (!at(P, TK_KW_CATCH)) perr_at(P, cur(P), "expected 'catch' after try block");
    advance(P);
    if (at(P, TK_IDENT)) {
        const Token *e = advance(P);
        n->decl = mkdecl(P, e->s, e->len, 0);
    }
    n->b = block(P);
    return n;
}

/* dotted module path -> arena string; returns the last identifier token */
static const Token *parse_module_path(Parser *P, Node *n) {
    char buf[256];
    size_t bl = 0;
    const Token *last = NULL;
    do {
        last = expect(P, TK_IDENT, "module name");
        size_t need = last->len + (bl ? 1u : 0u);
        if (bl + need >= sizeof buf) perr_at(P, last, "module path too long");
        if (bl) buf[bl++] = '.';
        memcpy(buf + bl, last->s, last->len);
        bl += last->len;
    } while (accept(P, TK_DOT));
    n->str = arena_strndup(P->A, buf, bl);
    n->slen = bl;
    return last;
}

static Node *parse_import(Parser *P) {
    const Token *t = advance(P);
    Node *n = mk(P, S_IMPORT, t);
    const Token *last = parse_module_path(P, n);
    const Token *bind = last;
    if (accept(P, TK_KW_AS)) bind = expect(P, TK_IDENT, "alias name");
    n->decl = mkdecl(P, bind->s, bind->len, 1);
    return n;
}

static Node *parse_from(Parser *P) {
    const Token *t = advance(P);
    Node *n = mk(P, S_FROM, t);
    parse_module_path(P, n);
    expect(P, TK_KW_IMPORT, "'import'");
    NodeVec v = {0};
    do {
        const Token *it = expect(P, TK_IDENT, "imported name");
        Node *in = make_name(P, it);
        const Token *bind = it;
        if (accept(P, TK_KW_AS)) bind = expect(P, TK_IDENT, "alias name");
        in->decl = mkdecl(P, bind->s, bind->len, 1);
        nv_push(P, &v, in);
    } while (accept(P, TK_COMMA));
    n->list = v.v;
    n->n = v.n;
    return n;
}

/* let/const [a, b, ...rest] = e   and   let/const {x, y: z} = e
 * Desugared to a hidden temporary plus ordinary declarations (S_MULTI, same scope). */
static Node *dname(Parser *P, const Token *t, const char *s, size_t n) {
    Node *x = mk(P, N_NAME, t);
    x->str = arena_strndup(P->A, s, n);
    x->slen = n;
    return x;
}

Node *dlet(Parser *P, const Token *t, int is_const, const char *name, size_t len, Node *init) {
    Node *d = mk(P, is_const ? S_CONST : S_LET, t);
    d->str = arena_strndup(P->A, name, len);
    d->slen = len;
    d->decl = mkdecl(P, name, len, is_const);
    d->a = init;
    return d;
}

typedef struct DItem { const Token *name; const Token *key; int rest; } DItem;

/* parses [a, b, ...rest] or {x, y: z} (the opening bracket is the current token) */
static int parse_decl_pattern(Parser *P, DItem *items, int *parr) {
    int arr = at(P, TK_LBRACK);
    const Token *open = advance(P);
    int close = arr ? TK_RBRACK : TK_RBRACE;
    int ni = 0;
    enter(P);
    while (!at(P, close)) {
        if (ni == 256) perr_at(P, cur(P), "too many names in destructuring (limit 256)");
        DItem *it = &items[ni];
        memset(it, 0, sizeof *it);
        if (arr && at(P, TK_ELLIPSIS)) {
            advance(P);
            it->rest = 1;
            it->name = expect(P, TK_IDENT, "name after '...'");
            ni++;
            accept(P, TK_COMMA);
            if (!at(P, close)) perr_at(P, cur(P), "'...' must be the last element");
            break;
        }
        if (arr) {
            it->name = expect(P, TK_IDENT, "variable name");
        } else {
            const Token *k = cur(P);
            if (!is_name_tok(k->kind)) perr_at(P, k, "expected table key, found %s", tok_name(k->kind));
            advance(P);
            it->key = k;
            it->name = k;
            if (accept(P, TK_COLON)) it->name = expect(P, TK_IDENT, "variable name");
            else if (k->kind != TK_IDENT) perr_at(P, k, "keyword '%.*s' needs a binding name ('key: name')", (int)k->len, k->s);
        }
        ni++;
        if (!accept(P, TK_COMMA)) break;
    }
    expect(P, close, arr ? "']'" : "'}'");
    leave(P);
    if (ni == 0) perr_at(P, open, "empty destructuring pattern");
    *parr = arr;
    return ni;
}

/* declarations binding each name of the pattern from the hidden temporary `tmp` */
static void emit_pattern(Parser *P, int is_const, const DItem *items, int ni, int arr, const char *tmp,
                         size_t tl, NodeVec *v) {
    for (int i = 0; i < ni; i++) {
        const Token *nt = items[i].name;
        Node *init;
        if (arr && items[i].rest) {
            Node *m = mk(P, N_MEMBER, nt);
            m->a = dname(P, nt, tmp, tl);
            m->str = arena_strndup(P->A, "slice", 5);
            m->slen = 5;
            init = mk(P, N_CALL, nt);
            init->a = m;
            Node *idx = mk(P, N_INT, nt);
            idx->ival = i;
            init->list = arena_alloc(P->A, sizeof(Node *));
            init->list[0] = idx;
            init->n = 1;
        } else if (arr) {
            init = mk(P, N_INDEX, nt);
            init->a = dname(P, nt, tmp, tl);
            init->b = mk(P, N_INT, nt);
            init->b->ival = i;
        } else {
            init = mk(P, N_MEMBER, items[i].key);
            init->a = dname(P, nt, tmp, tl);
            init->str = arena_strndup(P->A, items[i].key->s, items[i].key->len);
            init->slen = items[i].key->len;
        }
        nv_push(P, v, dlet(P, nt, is_const, nt->s, nt->len, init));
    }
}

static Node *parse_destructure(Parser *P) {
    const Token *kw = advance(P);
    int is_const = kw->kind == TK_KW_CONST;
    DItem items[256];
    int arr = 0;
    int ni = parse_decl_pattern(P, items, &arr);
    expect(P, TK_ASSIGN, "'=' after destructuring pattern");
    Node *rhs = expression(P);
    char tmp[32];
    int tl = snprintf(tmp, sizeof tmp, "$d%d", (*P->ndestr)++);
    Node *multi = mk(P, S_MULTI, kw);
    NodeVec v = {0};
    nv_push(P, &v, dlet(P, kw, 1, tmp, (size_t)tl, rhs));
    emit_pattern(P, is_const, items, ni, arr, tmp, (size_t)tl, &v);
    multi->list = v.v;
    multi->n = v.n;
    return multi;
}

/* `for [a, b] in xs:` / `for {x, y} in xs:`: the loop variable is a hidden temporary and the body
 * starts with the destructuring declarations. Returns the declarations as an S_MULTI. */
Node *loop_pattern(Parser *P, const Token *kw, Decl **decl) {
    DItem items[256];
    int arr = 0;
    int ni = parse_decl_pattern(P, items, &arr);
    char tmp[32];
    int tl = snprintf(tmp, sizeof tmp, "$d%d", (*P->ndestr)++);
    *decl = mkdecl(P, tmp, (size_t)tl, 0);
    Node *multi = mk(P, S_MULTI, kw);
    NodeVec v = {0};
    emit_pattern(P, 0, items, ni, arr, tmp, (size_t)tl, &v);
    multi->list = v.v;
    multi->n = v.n;
    return multi;
}

/* a block whose statements are `pre` followed by those of `body` */
Node *block_with_prefix(Parser *P, Node *pre, Node *body) {
    Node *blk = mk(P, S_BLOCK, &P->t[0]);
    blk->line = body->line;
    blk->col = body->col;
    blk->offset = body->offset;
    blk->list = arena_alloc(P->A, sizeof *blk->list * (size_t)(body->n + 1));
    blk->list[0] = pre;
    for (int i = 0; i < body->n; i++) blk->list[i + 1] = body->list[i];
    blk->n = body->n + 1;
    return blk;
}

static int is_target(const Node *e) { return e->kind == N_NAME || e->kind == N_MEMBER || e->kind == N_INDEX; }

/* a, b = b, a   and   a, b = pair  (SPEC 5.2.2): every right-hand side is evaluated first into hidden
 * temporaries, then the targets are assigned left to right. */
static Node *parse_multi_assign(Parser *P, Node *first) {
    Node *tg[16];
    int nt = 0;
    tg[nt++] = first;
    const Token *t0 = cur(P);
    while (accept(P, TK_COMMA)) {
        if (nt == 16) perr_at(P, cur(P), "too many assignment targets (limit 16)");
        tg[nt++] = expression(P);
    }
    for (int i = 0; i < nt; i++)
        if (!is_target(tg[i])) perr_at(P, t0, "invalid assignment target");
    const Token *op = expect(P, TK_ASSIGN, "'=' in multiple assignment");
    Node *rhs[16];
    int nr = 0;
    do {
        if (nr == 16) perr_at(P, cur(P), "too many values in assignment (limit 16)");
        rhs[nr++] = expression(P);
    } while (accept(P, TK_COMMA));
    if (nr != nt && nr != 1) perr_at(P, op, "assignment has %d targets but %d values", nt, nr);
    Node *multi = mk(P, S_MULTI, op);
    NodeVec v = {0};
    char tmp[16][32];
    int tl[16];
    if (nr == nt) {
        for (int i = 0; i < nt; i++) {
            tl[i] = snprintf(tmp[i], sizeof tmp[i], "$d%d", (*P->ndestr)++);
            nv_push(P, &v, dlet(P, op, 1, tmp[i], (size_t)tl[i], rhs[i]));
        }
        for (int i = 0; i < nt; i++) {
            Node *a = mk(P, S_ASSIGN, op);
            a->op = TK_ASSIGN;
            a->a = tg[i];
            a->b = dname(P, op, tmp[i], (size_t)tl[i]);
            nv_push(P, &v, a);
        }
    } else {
        tl[0] = snprintf(tmp[0], sizeof tmp[0], "$d%d", (*P->ndestr)++);
        nv_push(P, &v, dlet(P, op, 1, tmp[0], (size_t)tl[0], rhs[0]));
        for (int i = 0; i < nt; i++) {
            Node *ix = mk(P, N_INDEX, op);
            ix->a = dname(P, op, tmp[0], (size_t)tl[0]);
            ix->b = mk(P, N_INT, op);
            ix->b->ival = i;
            Node *a = mk(P, S_ASSIGN, op);
            a->op = TK_ASSIGN;
            a->a = tg[i];
            a->b = ix;
            nv_push(P, &v, a);
        }
    }
    multi->list = v.v;
    multi->n = v.n;
    return multi;
}

static int pure_target(const Node *e) {
    switch (e->kind) {
    case N_NAME: case N_INT: case N_STRING: return 1;
    case N_MEMBER: return pure_target(e->a);
    case N_INDEX: return pure_target(e->a) && pure_target(e->b);
    default: return 0;
    }
}

static Node *clone_target(Parser *P, const Node *e) {
    Node *c = arena_alloc(P->A, sizeof *c);
    *c = *e;
    if (e->kind == N_MEMBER || e->kind == N_INDEX) c->a = clone_target(P, e->a);
    if (e->kind == N_INDEX) c->b = clone_target(P, e->b);
    return c;
}

/* a ??= v: sugar for `if a == nil: a = v` (SPEC 7.9); the target is evaluated twice, so it may
 * only be a name or a chain of member / constant-index accesses on names. */
static Node *parse_coalesce_assign(Parser *P, Node *target) {
    const Token *op = advance(P);
    if (!is_target(target) || !pure_target(target))
        perr_at(P, op, "'?" "?=' needs a simple target (a name, or a.b / a[k] built from names and constants)");
    Node *val = expression(P);
    Node *cond = bin(P, op, TK_EQEQ, clone_target(P, target), mk(P, N_NIL, op));
    Node *asg = mk(P, S_ASSIGN, op);
    asg->op = TK_ASSIGN;
    asg->a = target;
    asg->b = val;
    Node *blk = mk(P, S_BLOCK, op);
    blk->list = arena_alloc(P->A, sizeof *blk->list);
    blk->list[0] = asg;
    blk->n = 1;
    Node *iff = mk(P, S_IF, op);
    iff->list = arena_alloc(P->A, sizeof *iff->list * 2);
    iff->list[0] = cond;
    iff->list[1] = blk;
    iff->n = 2;
    return iff;
}

static Node *simple_statement(Parser *P) {
    const Token *t = cur(P);
    Node *n;
    switch (t->kind) {
    case TK_KW_PASS:
        advance(P);
        n = mk(P, S_PASS, t);
        break;
    case TK_KW_LET:
    case TK_KW_CONST:
        if (peek_at(P, 1)->kind == TK_LBRACK || peek_at(P, 1)->kind == TK_LBRACE) {
            n = parse_destructure(P);
            break;
        }
        if (t->kind == TK_KW_CONST) goto parse_const;
        {
        advance(P);
        const Token *nt = expect(P, TK_IDENT, "variable name");
        n = mk(P, S_LET, nt);
        n->str = arena_strndup(P->A, nt->s, nt->len);
        n->slen = nt->len;
        n->decl = mkdecl(P, nt->s, nt->len, 0);
        if (accept(P, TK_ASSIGN)) n->a = expression(P);
        break;
        }
    parse_const: {
        advance(P);
        const Token *nt = expect(P, TK_IDENT, "constant name");
        n = mk(P, S_CONST, nt);
        n->str = arena_strndup(P->A, nt->s, nt->len);
        n->slen = nt->len;
        n->decl = mkdecl(P, nt->s, nt->len, 1);
        expect(P, TK_ASSIGN, "'=' (const requires an initializer)");
        n->a = expression(P);
        break;
    }
    case TK_KW_IMPORT: n = parse_import(P); break;
    case TK_KW_FROM: n = parse_from(P); break;
    case TK_KW_RETURN:
        advance(P);
        n = mk(P, S_RETURN, t);
        if (!at(P, TK_NEWLINE) && !at(P, TK_EOF)) n->a = expression(P);
        break;
    case TK_KW_BREAK:
    case TK_KW_CONTINUE:
        advance(P);
        n = mk(P, t->kind == TK_KW_BREAK ? S_BREAK : S_CONTINUE, t);
        if (at(P, TK_IDENT)) {
            const Token *l = advance(P);
            n->str = arena_strndup(P->A, l->s, l->len);
            n->slen = l->len;
        }
        break;
    case TK_KW_THROW:
        advance(P);
        n = mk(P, S_THROW, t);
        n->a = expression(P);
        break;
    case TK_KW_PANIC:
        advance(P);
        n = mk(P, S_PANIC, t);
        n->a = expression(P);
        break;
    case TK_KW_ASSERT:
        advance(P);
        n = mk(P, S_ASSERT, t);
        n->a = expression(P);
        if (accept(P, TK_COMMA)) n->b = expression(P);
        break;
    case TK_KW_DEFER: {
        advance(P);
        n = mk(P, S_DEFER, t);
        n->a = postfix(P);
        if (n->a->kind != N_CALL) perr_at(P, t, "'defer' requires a call expression");
        if (n->a->flag) perr_at(P, t, "'defer' does not accept a spread argument");
        break;
    }
    case TK_KW_YIELD:
        advance(P);
        n = mk(P, S_YIELD, t);
        if (!at(P, TK_NEWLINE) && !at(P, TK_EOF)) n->a = expression(P);
        break;
    default: {
        Node *e = expression(P);
        if (at(P, TK_COMMA) || at(P, TK_QQASSIGN)) {
            n = at(P, TK_COMMA) ? parse_multi_assign(P, e) : parse_coalesce_assign(P, e);
        } else if (is_assign_op(cur(P)->kind)) {
            const Token *op = advance(P);
            if (e->kind != N_NAME && e->kind != N_MEMBER && e->kind != N_INDEX)
                perr_at(P, op, "invalid assignment target");
            n = mk(P, S_ASSIGN, op);
            n->op = op->kind;
            n->a = e;
            n->b = expression(P);
        } else {
            n = mk(P, S_EXPR, t);
            n->a = e;
        }
        break;
    }
    }
    end_simple(P);
    return n;
}

static Node *statement(Parser *P) {
    const Token *t = cur(P);
    enter(P);
    Node *n;
    switch (t->kind) {
    case TK_KW_IF: n = parse_if(P); break;
    case TK_KW_WHILE:
    case TK_KW_FOR: n = parse_loop(P, NULL); break;
    case TK_KW_TRY: n = parse_try(P); break;
    case TK_KW_CLASS: n = parse_class(P); break;
    case TK_KW_MATCH: n = parse_match(P); break;
    case TK_KW_OVERRIDE: perr_at(P, t, "'override' is only valid inside a class");
    case TK_KW_EXPORT: {
        advance(P);
        leave(P);
        Node *e = statement(P);
        if (e->kind != S_LET && e->kind != S_CONST && e->kind != S_FUNC && e->kind != S_CLASS)
            perr_at(P, t, "'export' must be followed by let, const, func or class");
        e->exported = 1;
        return e;
    }
    case TK_KW_FUNC:
        if (peek_at(P, 1)->kind == TK_IDENT) {
            advance(P);
            const Token *nt = advance(P);
            Node *f = parse_func_rest(P, t, nt, 0);
            n = mk(P, S_FUNC, nt);
            n->a = f;
            n->str = f->str;
            n->slen = f->slen;
            n->decl = mkdecl(P, nt->s, nt->len, 1);
            break;
        }
        n = simple_statement(P);
        break;
    case TK_IDENT:
        if (peek_at(P, 1)->kind == TK_COLON &&
            (peek_at(P, 2)->kind == TK_KW_WHILE || peek_at(P, 2)->kind == TK_KW_FOR)) {
            advance(P);
            advance(P);
            n = parse_loop(P, t);
            break;
        }
        n = simple_statement(P);
        break;
    case TK_INDENT: perr_at(P, t, "unexpected indent");
    case TK_NEWLINE: advance(P); leave(P); return statement(P);
    default: n = simple_statement(P); break;
    }
    leave(P);
    return n;
}

Node *parse_program(Front *F, Arena *A, const TokenList *toks) {
    Parser P;
    memset(&P, 0, sizeof P);
    P.F = F;
    P.A = A;
    P.t = toks->v;
    P.n = toks->n;
    int ndestr = 0;
    P.ndestr = &ndestr;
    Node *root = mk(&P, S_BLOCK, &toks->v[0]);
    NodeVec v = {0};
    while (!at(&P, TK_EOF)) {
        if (at(&P, TK_DEDENT)) perr_at(&P, cur(&P), "unexpected dedent");
        nv_push(&P, &v, statement(&P));
    }
    root->list = v.v;
    root->n = v.n;
    return root;
}
