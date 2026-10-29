/* Parser: expressions (precedence climbing, calls, lambdas, comprehensions, match expressions). */
#include "parser_int.h"

static Node *postfix_tail(Parser *P, Node *e);
static Node *unary(Parser *P);

/* ---------------------------------------------------------------- */
/* expressions                                                       */
/* ---------------------------------------------------------------- */

static void parse_call_args(Parser *P, Node *c) {
    advance(P); /* ( */
    enter(P);
    NodeVec v = {0};
    while (!at(P, TK_RPAREN)) {
        if (at(P, TK_ELLIPSIS)) {
            advance(P);
            nv_push(P, &v, expression(P));
            c->flag = 1;
            accept(P, TK_COMMA);
            if (!at(P, TK_RPAREN)) perr_at(P, cur(P), "spread argument must be last");
            break;
        }
        nv_push(P, &v, expression(P));
        if (!accept(P, TK_COMMA)) break;
    }
    expect(P, TK_RPAREN, "')'");
    leave(P);
    c->list = v.v;
    c->n = v.n;
}

/* f"text {expr} text": desugared to a `..` chain (one CONCAT, SPEC 7.4). */
static Node *fstr_text(Parser *P, const Token *t, const char *s, size_t n) {
    Node *x = mk(P, N_STRING, t);
    x->str = arena_strndup(P->A, s, n);
    x->slen = n;
    return x;
}

static void toklist_abort(void *p) { toklist_free(p); }

static Node *parse_fstring(Parser *P, const Token *t) {
    const char *s = t->sval;
    size_t n = t->slen, i = 0;
    Node *acc = NULL;
    int nparts = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != 1) j++;
        Node *part = NULL;
        if (j > i) part = fstr_text(P, t, s + i, j - i);
        if (part) {
            acc = acc ? bin(P, t, TK_DOTDOT, acc, part) : part;
            nparts++;
        }
        if (j >= n) break;
        size_t k = j + 1;
        while (k < n && s[k] != 2) k++;
        size_t elen = k - (j + 1);
        char *buf = malloc(elen + 3);
        if (!buf) { fputs("out of memory\n", stderr); exit(14); }
        buf[0] = '(';
        memcpy(buf + 1, s + j + 1, elen);
        buf[elen + 1] = ')';
        buf[elen + 2] = 0;
        front_push(P->F, front_free_ptr, buf);
        TokenList sub;
        memset(&sub, 0, sizeof sub);
        front_push(P->F, toklist_abort, &sub);
        lex_source_at(P->F, buf, elen + 2, &sub, t->line, t->col + 1);
        Parser sp;
        memset(&sp, 0, sizeof sp);
        sp.F = P->F; sp.A = P->A; sp.t = sub.v; sp.n = sub.n; sp.depth = P->depth; sp.ndestr = P->ndestr;
        Node *e = expression(&sp);
        if (!at(&sp, TK_NEWLINE) && !at(&sp, TK_EOF)) perr_at(&sp, cur(&sp), "unexpected %s in f-string expression", tok_name(cur(&sp)->kind));
        front_pop(P->F);
        front_pop(P->F);
        toklist_free(&sub);
        free(buf);
        if (!acc) acc = bin(P, t, TK_DOTDOT, fstr_text(P, t, "", 0), e);
        else acc = bin(P, t, TK_DOTDOT, acc, e);
        nparts++;
        i = k + 1;
    }
    if (!acc) return fstr_text(P, t, "", 0);
    return acc;
}


typedef struct CompClause {
    int is_for;
    Node *node; /* S_FOR (without body) or the condition */
    Node *pre;  /* destructuring declarations that open the body of a `for [a, b] in` clause */
} CompClause;

/* [e for x in it if c ...] and {k: v for ...} (SPEC 11.5). Desugared into an N_SEQ:
 *   $cN = [] / {} ; for x in it: if c: $cN.append(e)  /  $cN[k] = v ; result $cN */
static Node *parse_comprehension(Parser *P, const Token *open, Node *elem, Node *key) {
    char nm[24];
    int nl = snprintf(nm, sizeof nm, "$c%d", (*P->ndestr)++);
    CompClause cl[16];
    int nc = 0;
    while (at(P, TK_KW_FOR) || at(P, TK_KW_IF)) {
        if (nc == 16) perr_at(P, cur(P), "too many clauses in comprehension");
        const Token *ct = advance(P);
        if (ct->kind == TK_KW_FOR) {
            Node *f = mk(P, S_FOR, ct);
            cl[nc].pre = NULL;
            const Token *v1 = cur(P);
            if (at(P, TK_LBRACK) || at(P, TK_LBRACE)) {
                cl[nc].pre = loop_pattern(P, ct, &f->decl);
                f->ival = 1;
            } else {
                v1 = expect(P, TK_IDENT, "loop variable");
                f->decl = mkdecl(P, v1->s, v1->len, 0);
                f->ival = 1;
            }
            if (!cl[nc].pre && accept(P, TK_COMMA)) {
                const Token *v2 = expect(P, TK_IDENT, "loop variable");
                if (v2->len == v1->len && memcmp(v2->s, v1->s, v1->len) == 0)
                    perr_at(P, v2, "duplicate loop variable '%.*s'", (int)v2->len, v2->s);
                f->decl2 = mkdecl(P, v2->s, v2->len, 0);
                f->ival = 2;
            }
            expect(P, TK_KW_IN, "'in'");
            f->a = pipe_expr(P);
            cl[nc].is_for = 1;
            cl[nc++].node = f;
        } else {
            cl[nc].is_for = 0;
            cl[nc].pre = NULL;
            cl[nc++].node = or_expr(P);
        }
    }
    if (!cl[0].is_for) perr_at(P, open, "a comprehension must start with 'for'");
    Token ft = *open;
    ft.s = nm;
    ft.len = (size_t)nl;
    Node *ref = make_name(P, &ft);
    Node *inner;
    if (key) {
        Node *ix = mk(P, N_INDEX, open);
        ix->a = ref;
        ix->b = key;
        inner = mk(P, S_ASSIGN, open);
        inner->op = TK_ASSIGN;
        inner->a = ix;
        inner->b = elem;
    } else {
        Node *m = mk(P, N_MEMBER, open);
        m->a = ref;
        m->str = arena_strndup(P->A, "append", 6);
        m->slen = 6;
        Node *call = mk(P, N_CALL, open);
        call->a = m;
        call->list = arena_alloc(P->A, sizeof *call->list);
        call->list[0] = elem;
        call->n = 1;
        inner = mk(P, S_EXPR, open);
        inner->a = call;
    }
    for (int i = nc - 1; i >= 0; i--) {
        Node *blk = mk(P, S_BLOCK, open);
        blk->list = arena_alloc(P->A, sizeof *blk->list);
        blk->list[0] = inner;
        blk->n = 1;
        if (cl[i].is_for) {
            cl[i].node->b = cl[i].pre ? block_with_prefix(P, cl[i].pre, blk) : blk;
            inner = cl[i].node;
        } else {
            Node *iff = mk(P, S_IF, open);
            iff->list = arena_alloc(P->A, sizeof *iff->list * 2);
            iff->list[0] = cl[i].node;
            iff->list[1] = blk;
            iff->n = 2;
            inner = iff;
        }
    }
    Node *init = mk(P, key ? N_TABLE : N_ARRAY, open);
    Node *let = dlet(P, open, 0, nm, (size_t)nl, init);
    Node *seq = mk(P, N_SEQ, open);
    Node *blk = mk(P, S_BLOCK, open);
    blk->list = arena_alloc(P->A, sizeof *blk->list * 2);
    blk->list[0] = let;
    blk->list[1] = inner;
    blk->n = 2;
    seq->a = blk;
    seq->b = make_name(P, &ft);
    return seq;
}


Node *primary(Parser *P) {
    const Token *t = cur(P);
    switch (t->kind) {
    case TK_KW_MATCH: return match_expr(P);
    case TK_KW_NIL: advance(P); return mk(P, N_NIL, t);
    case TK_KW_TRUE: advance(P); return mk(P, N_TRUE, t);
    case TK_KW_FALSE: advance(P); return mk(P, N_FALSE, t);
    case TK_INT: {
        advance(P);
        if (t->big) perr_at(P, t, "integer literal out of range");
        Node *n = mk(P, N_INT, t);
        n->ival = t->ival;
        return n;
    }
    case TK_FLOAT: {
        advance(P);
        Node *n = mk(P, N_FLOAT, t);
        n->fval = t->fval;
        return n;
    }
    case TK_STRING: {
        advance(P);
        Node *n = mk(P, N_STRING, t);
        n->str = arena_strndup(P->A, t->sval, t->slen);
        n->slen = t->slen;
        return n;
    }
    case TK_FSTRING: {
        advance(P);
        return parse_fstring(P, t);
    }
    case TK_IDENT: {
        advance(P);
        Node *n = mk(P, N_NAME, t);
        n->str = arena_strndup(P->A, t->s, t->len);
        n->slen = t->len;
        return n;
    }
    case TK_LPAREN: {
        advance(P);
        enter(P);
        Node *e = expression(P);
        leave(P);
        expect(P, TK_RPAREN, "')'");
        return e;
    }
    case TK_LBRACK: {
        advance(P);
        enter(P);
        Node *n = mk(P, N_ARRAY, t);
        NodeVec v = {0};
        while (!at(P, TK_RBRACK)) {
            nv_push(P, &v, expression(P));
            if (v.n == 1 && (at(P, TK_KW_FOR))) {
                Node *cmp = parse_comprehension(P, t, v.v[0], NULL);
                expect(P, TK_RBRACK, "']'");
                leave(P);
                return cmp;
            }
            if (!accept(P, TK_COMMA)) break;
        }
        expect(P, TK_RBRACK, "']'");
        leave(P);
        n->list = v.v;
        n->n = v.n;
        return n;
    }
    case TK_LBRACE: {
        advance(P);
        enter(P);
        Node *n = mk(P, N_TABLE, t);
        NodeVec v = {0};
        while (!at(P, TK_RBRACE)) {
            const Token *kt = cur(P);
            Node *key;
            if (is_name_tok(kt->kind)) {
                advance(P);
                key = mk(P, N_STRING, kt);
                key->str = arena_strndup(P->A, kt->s, kt->len);
                key->slen = kt->len;
            } else if (kt->kind == TK_STRING) {
                advance(P);
                key = mk(P, N_STRING, kt);
                key->str = arena_strndup(P->A, kt->sval, kt->slen);
                key->slen = kt->slen;
            } else if (kt->kind == TK_INT) {
                advance(P);
                if (kt->big) perr_at(P, kt, "integer literal out of range");
                key = mk(P, N_INT, kt);
                key->ival = kt->ival;
            } else if (kt->kind == TK_LPAREN) {
                advance(P);
                key = expression(P);
                expect(P, TK_RPAREN, "')'");
            } else {
                perr_at(P, kt, "expected table key, found %s", tok_name(kt->kind));
            }
            Node *val;
            if (kt->kind == TK_IDENT && (at(P, TK_COMMA) || at(P, TK_RBRACE))) {
                val = make_name(P, kt); /* {x, y} is {x: x, y: y} (SPEC 8.4) */
            } else {
                expect(P, TK_COLON, "':' after table key");
                val = expression(P);
            }
            if (v.n == 0 && at(P, TK_KW_FOR)) {
                /* comprehension: the key is an expression (a bare name is a variable) */
                if (is_name_tok(kt->kind) && key->kind == N_STRING && !(kt->kind == TK_STRING)) key = make_name(P, kt);
                Node *cmp = parse_comprehension(P, t, val, key);
                expect(P, TK_RBRACE, "'}'");
                leave(P);
                return cmp;
            }
            nv_push(P, &v, key);
            nv_push(P, &v, val);
            if (!accept(P, TK_COMMA)) break;
        }
        expect(P, TK_RBRACE, "'}'");
        leave(P);
        n->list = v.v;
        n->n = v.n;
        return n;
    }
    case TK_KW_SUPER: {
        advance(P);
        Node *n;
        if (at(P, TK_LPAREN)) {
            n = mk(P, N_SUPER_CALL, t);
        } else if (accept(P, TK_DOT)) {
            const Token *nt = expect_name(P, "method name");
            n = mk(P, N_SUPER_METHOD, t);
            n->str = arena_strndup(P->A, nt->s, nt->len);
            n->slen = nt->len;
            if (!at(P, TK_LPAREN)) perr_at(P, cur(P), "'super.%s' must be called", n->str);
        } else {
            perr_at(P, cur(P), "'super' must be followed by '(' or '.name('");
        }
        parse_call_args(P, n);
        if (n->flag) perr_at(P, t, "'super' does not accept a spread argument");
        return n;
    }
    default:
        perr_at(P, t, "expected expression, found %s", tok_name(t->kind));
    }
}

static Node *mk_seq1(Parser *P, const Token *t, Node *let, Node *res) {
    Node *seq = mk(P, N_SEQ, t);
    Node *blk = mk(P, S_BLOCK, t);
    blk->list = arena_alloc(P->A, sizeof *blk->list);
    blk->list[0] = let;
    blk->n = 1;
    seq->a = blk;
    seq->b = res;
    return seq;
}

static Node *hidden_ref(Parser *P, const Token *t, const char *nm, int nl) {
    Token ft = *t;
    ft.s = nm;
    ft.len = (size_t)nl;
    return make_name(P, &ft);
}


/* a?.b  a?.f(x)  a?[i]: if a is nil the whole rest of the chain is nil (SPEC 7.11).
 * Desugared to  { let $oN = a; $oN != nil ? $oN.b... : nil }. */
static Node *optional_chain(Parser *P, Node *e) {
    const Token *t = advance(P); /* ?. or ?[ */
    char nm[24];
    int nl = snprintf(nm, sizeof nm, "$o%d", (*P->ndestr)++);
    Node *let = dlet(P, t, 0, nm, (size_t)nl, e);
    Node *inner;
    if (t->kind == TK_QDOT) {
        const Token *nt = expect_name(P, "member name");
        inner = mk(P, N_MEMBER, t);
        inner->a = hidden_ref(P, t, nm, nl);
        inner->str = arena_strndup(P->A, nt->s, nt->len);
        inner->slen = nt->len;
    } else {
        enter(P);
        inner = mk(P, N_INDEX, t);
        inner->a = hidden_ref(P, t, nm, nl);
        inner->b = expression(P);
        expect(P, TK_RBRACK, "']'");
        leave(P);
    }
    inner = postfix_tail(P, inner);
    Node *cond = bin(P, t, TK_NE, hidden_ref(P, t, nm, nl), mk(P, N_NIL, t));
    Node *c = mk(P, N_COND, t);
    c->a = cond;
    c->b = inner;
    c->c = mk(P, N_NIL, t);
    return mk_seq1(P, t, let, c);
}

Node *postfix(Parser *P) { return postfix_tail(P, primary(P)); }

static Node *postfix_tail(Parser *P, Node *e) {
    for (;;) {
        const Token *t = cur(P);
        if (t->kind == TK_LPAREN) {
            Node *c = mk(P, N_CALL, t);
            c->a = e;
            parse_call_args(P, c);
            e = c;
        } else if (t->kind == TK_LBRACK) {
            advance(P);
            enter(P);
            Node *lo = NULL, *hi = NULL;
            int is_slice = 0;
            if (!at(P, TK_COLON)) lo = expression(P);
            if (accept(P, TK_COLON)) {
                is_slice = 1;
                if (!at(P, TK_RBRACK)) hi = expression(P);
            }
            expect(P, TK_RBRACK, "']'");
            leave(P);
            if (is_slice) {
                /* a[i:j] is sugar for a.slice(i, j) (SPEC 8.2/8.3) */
                Node *m = mk(P, N_MEMBER, t);
                m->a = e;
                m->str = arena_strndup(P->A, "slice", 5);
                m->slen = 5;
                Node *c = mk(P, N_CALL, t);
                c->a = m;
                NodeVec av = {0};
                if (!lo) { lo = mk(P, N_INT, t); lo->ival = 0; }
                nv_push(P, &av, lo);
                if (hi) nv_push(P, &av, hi);
                c->list = av.v;
                c->n = av.n;
                e = c;
            } else {
                if (!lo) perr_at(P, t, "expected index expression");
                Node *x = mk(P, N_INDEX, t);
                x->a = e;
                x->b = lo;
                e = x;
            }
        } else if (t->kind == TK_QDOT || t->kind == TK_QBRACK) {
            return optional_chain(P, e);
        } else if (t->kind == TK_DOT) {
            advance(P);
            const Token *nt = expect_name(P, "member name");
            Node *m = mk(P, N_MEMBER, t);
            m->a = e;
            m->str = arena_strndup(P->A, nt->s, nt->len);
            m->slen = nt->len;
            e = m;
        } else {
            return e;
        }
    }
}


static Node *power(Parser *P) {
    Node *b = postfix(P);
    if (at(P, TK_STARSTAR)) {
        const Token *t = advance(P);
        Node *n = mk(P, N_BINARY, t);
        n->op = TK_STARSTAR;
        n->a = b;
        n->b = unary(P);
        return n;
    }
    return b;
}

static Node *unary(Parser *P) {
    const Token *t = cur(P);
    if (t->kind == TK_MINUS || t->kind == TK_TILDE) {
        advance(P);
        if (t->kind == TK_MINUS && at(P, TK_INT) && cur(P)->big) {
            const Token *it = advance(P);
            /* -9223372036854775808 : only valid as the whole operand (no ** after) */
            if (at(P, TK_STARSTAR)) perr_at(P, it, "integer literal out of range");
            Node *n = mk(P, N_INT, it);
            n->ival = INT64_MIN;
            return n;
        }
        enter(P);
        Node *n = mk(P, N_UNARY, t);
        n->op = t->kind;
        n->a = unary(P);
        leave(P);
        return n;
    }
    return power(P);
}

Node *bin(Parser *P, const Token *t, int op, Node *a, Node *b) {
    Node *n = mk(P, N_BINARY, t);
    n->op = op;
    n->a = a;
    n->b = b;
    return n;
}

static Node *term(Parser *P) {
    Node *l = unary(P);
    while (at(P, TK_STAR) || at(P, TK_SLASH) || at(P, TK_SLASHSLASH) || at(P, TK_PERCENT)) {
        const Token *t = advance(P);
        l = bin(P, t, t->kind, l, unary(P));
    }
    return l;
}
static Node *additive(Parser *P) {
    Node *l = term(P);
    while (at(P, TK_PLUS) || at(P, TK_MINUS)) {
        const Token *t = advance(P);
        l = bin(P, t, t->kind, l, term(P));
    }
    return l;
}
static Node *concat(Parser *P) {
    Node *l = additive(P);
    while (at(P, TK_DOTDOT)) {
        const Token *t = advance(P);
        l = bin(P, t, TK_DOTDOT, l, additive(P));
    }
    return l;
}
static Node *shift(Parser *P) {
    Node *l = concat(P);
    while (at(P, TK_SHL) || at(P, TK_SHR)) {
        const Token *t = advance(P);
        l = bin(P, t, t->kind, l, concat(P));
    }
    return l;
}
static Node *bit_and(Parser *P) {
    Node *l = shift(P);
    while (at(P, TK_AMP)) {
        const Token *t = advance(P);
        l = bin(P, t, TK_AMP, l, shift(P));
    }
    return l;
}
static Node *bit_xor(Parser *P) {
    Node *l = bit_and(P);
    while (at(P, TK_CARET)) {
        const Token *t = advance(P);
        l = bin(P, t, TK_CARET, l, bit_and(P));
    }
    return l;
}
static Node *bit_or(Parser *P) {
    Node *l = bit_xor(P);
    while (at(P, TK_PIPE)) {
        const Token *t = advance(P);
        l = bin(P, t, TK_PIPE, l, bit_xor(P));
    }
    return l;
}

static int is_cmp(int k) { return k == TK_EQEQ || k == TK_NE || k == TK_LT || k == TK_LE || k == TK_GT || k == TK_GE; }

static int at_cmp(Parser *P) {
    int k = cur(P)->kind;
    return is_cmp(k) || k == TK_KW_IN || (k == TK_KW_NOT && peek_at(P, 1)->kind == TK_KW_IN);
}

/* one comparison; `x in c` is `$in(x, c)` (a prelude function), `x not in c` its negation (SPEC 7.12) */
static Node *cmp_node(Parser *P, const Token *t, int kind, Node *l, Node *r) {
    if (kind != TK_KW_IN && kind != TK_KW_NOT) return bin(P, t, kind, l, r);
    Node *c = mk(P, N_CALL, t);
    Token ft = *t;
    ft.s = "$in";
    ft.len = 3;
    c->a = make_name(P, &ft);
    c->list = arena_alloc(P->A, sizeof *c->list * 2);
    c->list[0] = l;
    c->list[1] = r;
    c->n = 2;
    if (kind == TK_KW_IN) return c;
    Node *u = mk(P, N_UNARY, t);
    u->op = TK_KW_NOT;
    u->a = c;
    return u;
}

#define MAX_CMP_CHAIN 8
/* a < b <= c: each middle operand is evaluated once, left to right, and later operands only if
 * the earlier comparisons held (SPEC 7.6). Desugared with hidden variables and `and`. */
static Node *cmp_chain(Parser *P, int i, int nops, const Token **ot, const int *ok, Node **opd, Node *left) {
    char nm[24];
    if (i == nops - 1) return cmp_node(P, ot[i], ok[i], left, opd[i + 1]);
    int nl = snprintf(nm, sizeof nm, "$k%d", (*P->ndestr)++);
    Node *let = dlet(P, ot[i], 0, nm, (size_t)nl, opd[i + 1]);
    Node *first = cmp_node(P, ot[i], ok[i], left, hidden_ref(P, ot[i], nm, nl));
    Node *rest = cmp_chain(P, i + 1, nops, ot, ok, opd, hidden_ref(P, ot[i], nm, nl));
    Node *a = mk(P, N_AND, ot[i]);
    a->a = first;
    a->b = rest;
    return mk_seq1(P, ot[i], let, a);
}

static Node *comparison(Parser *P) {
    Node *l = bit_or(P);
    if (!at_cmp(P)) return l;
    const Token *ot[MAX_CMP_CHAIN];
    int ok[MAX_CMP_CHAIN];
    Node *opd[MAX_CMP_CHAIN + 1];
    int nops = 0;
    opd[0] = l;
    while (at_cmp(P)) {
        if (nops == MAX_CMP_CHAIN) perr_at(P, cur(P), "too many chained comparisons (limit %d)", MAX_CMP_CHAIN);
        const Token *t = advance(P);
        ok[nops] = t->kind;
        if (t->kind == TK_KW_NOT) advance(P); /* not in */
        ot[nops] = t;
        opd[nops + 1] = bit_or(P);
        nops++;
    }
    if (nops == 1) return cmp_node(P, ot[0], ok[0], opd[0], opd[1]);
    char nm[24];
    int nl = snprintf(nm, sizeof nm, "$k%d", (*P->ndestr)++);
    Node *let = dlet(P, ot[0], 0, nm, (size_t)nl, opd[0]);
    return mk_seq1(P, ot[0], let, cmp_chain(P, 0, nops, ot, ok, opd, hidden_ref(P, ot[0], nm, nl)));
}

static Node *not_expr(Parser *P) {
    if (at(P, TK_KW_NOT)) {
        const Token *t = advance(P);
        enter(P);
        Node *n = mk(P, N_UNARY, t);
        n->op = TK_KW_NOT;
        n->a = not_expr(P);
        leave(P);
        return n;
    }
    return comparison(P);
}

static Node *and_expr(Parser *P) {
    Node *l = not_expr(P);
    while (at(P, TK_KW_AND)) {
        const Token *t = advance(P);
        Node *n = mk(P, N_AND, t);
        n->a = l;
        n->b = not_expr(P);
        l = n;
    }
    return l;
}

Node *or_expr(Parser *P) {
    Node *l = and_expr(P);
    while (at(P, TK_KW_OR)) {
        const Token *t = advance(P);
        Node *n = mk(P, N_OR, t);
        n->a = l;
        n->b = and_expr(P);
        l = n;
    }
    return l;
}

static Node *coalesce(Parser *P) {
    Node *l = or_expr(P);
    while (at(P, TK_QQ)) {
        const Token *t = advance(P);
        Node *n = mk(P, N_COALESCE, t);
        n->a = l;
        n->b = or_expr(P);
        l = n;
    }
    return l;
}

/* x |> f  /  x |> f(a, b): sugar for f(x) / f(x, a, b) (SPEC 7.10). */
Node *pipe_expr(Parser *P) {
    Node *l = coalesce(P);
    while (at(P, TK_PIPEGT)) {
        const Token *t = advance(P);
        Node *r = postfix(P);
        if (r->kind == N_CALL && r->flag == 0) {
            Node **nl = arena_alloc(P->A, sizeof *nl * (size_t)(r->n + 1));
            nl[0] = l;
            for (int i = 0; i < r->n; i++) nl[i + 1] = r->list[i];
            r->list = nl;
            r->n++;
            l = r;
        } else {
            Node *c = mk(P, N_CALL, t);
            c->a = r;
            c->list = arena_alloc(P->A, sizeof *c->list);
            c->list[0] = l;
            c->n = 1;
            c->flag = 0;
            if (r->kind == N_CALL) { /* trailing spread: f(...xs) is not a pipe target */
                perr_at(P, t, "cannot pipe into a call with a spread argument");
            }
            l = c;
        }
    }
    return l;
}

Node *conditional(Parser *P) {
    Node *x = pipe_expr(P);
    if (at(P, TK_KW_IF)) {
        const Token *t = advance(P);
        Node *c = or_expr(P);
        expect(P, TK_KW_ELSE, "'else' in conditional expression");
        Node *n = mk(P, N_COND, t);
        n->a = c;
        n->b = x;
        n->c = expression(P);
        return n;
    }
    return x;
}

/* Is the '(' at the cursor the start of an arrow-function parameter list "( ... ) =>" ? */
static int arrow_ahead(Parser *P) {
    int depth = 0;
    for (int k = 0;; k++) {
        int kind = peek_at(P, k)->kind;
        if (kind == TK_EOF) return 0;
        if (kind == TK_LPAREN || kind == TK_LBRACK || kind == TK_LBRACE) depth++;
        else if (kind == TK_RPAREN || kind == TK_RBRACK || kind == TK_RBRACE) {
            if (--depth == 0) return peek_at(P, k + 1)->kind == TK_FATARROW;
        }
    }
}

Node *expression(Parser *P) {
    enter(P);
    Node *e;
    if (at(P, TK_KW_FUNC)) {
        const Token *f = advance(P);
        e = parse_func_rest(P, f, NULL, 1);
    } else if (at(P, TK_IDENT) && peek_at(P, 1)->kind == TK_FATARROW) {
        /* x => expr */
        const Token *pt = advance(P);
        advance(P);
        e = mk(P, N_FUNC, pt);
        Node *pn = mk(P, N_NAME, pt);
        pn->str = arena_strndup(P->A, pt->s, pt->len);
        pn->slen = pt->len;
        pn->decl = mkdecl(P, pt->s, pt->len, 0);
        e->list = arena_alloc(P->A, sizeof *e->list);
        e->list[0] = pn;
        e->n = 1;
        e->flag = 1;
        e->a = expression(P);
    } else if (at(P, TK_LPAREN) && arrow_ahead(P)) {
        /* (a, b = 1) => expr */
        const Token *f = cur(P);
        e = parse_func_rest(P, f, NULL, 2);
    } else {
        e = conditional(P);
    }
    leave(P);
    return e;
}
