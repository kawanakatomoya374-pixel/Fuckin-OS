/* sprint fmt: rewrites whitespace only (indentation, operator and comma spacing, trailing blanks, blank-line runs, end-of-file newline). The result is re-lexed and must be token-for-token identical to the input, otherwise nothing is written (exit 70). */
#include "tools_int.h"

/* ------------------------------------------------------------------ */
/* small helpers                                                        */
/* ------------------------------------------------------------------ */
typedef struct SB {
    char *p;
    size_t n, cap;
} SB;

static void sb_grow(SB *b, size_t extra) {
    if (b->n + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->n + extra + 1) nc *= 2;
    char *np = realloc(b->p, nc);
    if (!np) { (void)fputs("sprint: out of memory\n", stderr); exit(14); }
    b->p = np;
    b->cap = nc;
}
static void sb_put(SB *b, const char *s, size_t n) {
    sb_grow(b, n);
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_putc(SB *b, char c) { sb_put(b, &c, 1); }
static void sb_spaces(SB *b, size_t n) {
    sb_grow(b, n);
    memset(b->p + b->n, ' ', n);
    b->n += n;
    b->p[b->n] = 0;
}

void print_front_diag(const Diag *d, int json) {
    if (json) {
        (void)fprintf(stderr, "{\"kind\":\"%s\",\"code\":%d,\"file\":\"%s\",\"line\":%d,\"col\":%d,\"message\":\"", d->kind,
                      d->code, d->file, d->line, d->col);
        for (const char *s = d->msg; *s; s++) {
            if (*s == '"' || *s == '\\') (void)fputc('\\', stderr);
            if ((unsigned char)*s >= 0x20) (void)fputc(*s, stderr);
        }
        (void)fputs("\"}\n", stderr);
    } else {
        (void)fprintf(stderr, "%s:%d:%d: %s: %s\n", d->file, d->line, d->col, d->kind, d->msg);
    }
}

/* ------------------------------------------------------------------ */
/* lexing with error capture                                            */
/* ------------------------------------------------------------------ */
int lex_guarded(Front *F, const char *src, size_t len, TokenList *tl) {
    if (setjmp(F->jb) == 0) {
        lex_source(F, src, len, tl);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* fmt                                                                  */
/* ------------------------------------------------------------------ */
static int is_struct(int k) { return k == TK_NEWLINE || k == TK_INDENT || k == TK_DEDENT || k == TK_EOF; }

static int is_binop(int k) {
    switch (k) {
    case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH: case TK_SLASHSLASH: case TK_PERCENT:
    case TK_STARSTAR: case TK_DOTDOT: case TK_AMP: case TK_PIPE: case TK_CARET: case TK_SHL: case TK_SHR:
    case TK_EQEQ: case TK_NE: case TK_LT: case TK_LE: case TK_GT: case TK_GE: case TK_ASSIGN:
    case TK_PLUSEQ: case TK_MINUSEQ: case TK_STAREQ: case TK_SLASHEQ: case TK_SLASHSLASHEQ:
    case TK_PERCENTEQ: case TK_DOTDOTEQ: case TK_QQ: case TK_STARSTAREQ: case TK_AMPEQ: case TK_PIPEEQ:
    case TK_CARETEQ: case TK_SHLEQ: case TK_SHREQ: case TK_FATARROW: case TK_PIPEGT: case TK_QQASSIGN:
        return 1;
    default:
        return 0;
    }
}

static int ends_operand(int k) {
    switch (k) {
    case TK_IDENT: case TK_INT: case TK_FLOAT: case TK_STRING: case TK_FSTRING: case TK_RPAREN: case TK_RBRACK:
    case TK_RBRACE: case TK_KW_TRUE: case TK_KW_FALSE: case TK_KW_NIL:
        return 1;
    default:
        return 0;
    }
}

static int is_kw(int k) { return k >= TK_KW_AND && k < TK_KW_END_; }

static int is_wordlike(int k) {
    return k == TK_IDENT || k == TK_INT || k == TK_FLOAT || k == TK_STRING || k == TK_FSTRING ||
           (k >= TK_KW_AND && k < TK_KW_END_);
}

/* Number of spaces between two tokens on one source line: 0, 1, or -1 = keep (collapsed to 0/1). */
static int gap_between(int a, int a_unary, int b, int b_unary, int in_brace) {
    if (b == TK_RPAREN || b == TK_RBRACK || b == TK_RBRACE || b == TK_COMMA || b == TK_COLON) return 0;
    if (a == TK_LPAREN || a == TK_LBRACK || a == TK_LBRACE) return 0;
    if (a == TK_COMMA) return 1;
    if (a == TK_COLON) return in_brace ? 1 : -1;
    if (a == TK_DOT || b == TK_DOT || a == TK_ELLIPSIS || a == TK_QDOT || b == TK_QDOT || b == TK_QBRACK || a == TK_QBRACK) return 0;
    if (a_unary) return 0;
    if (b == TK_LPAREN) {
        if (a == TK_IDENT || a == TK_RPAREN || a == TK_RBRACK || a == TK_RBRACE || a == TK_STRING || a == TK_FSTRING ||
            a == TK_KW_SUPER || a == TK_KW_FUNC)
            return 0;
        return 1;
    }
    if (b == TK_LBRACK) {
        if (a == TK_IDENT || a == TK_RPAREN || a == TK_RBRACK || a == TK_RBRACE || a == TK_STRING || a == TK_FSTRING)
            return 0;
        return 1;
    }
    if (b == TK_TILDE) return 1;
    if (is_binop(a) || (is_binop(b) && !b_unary)) return 1;
    if (b_unary) return 1;
    if (is_wordlike(a) && is_wordlike(b)) return 1;
    if (is_kw(a) || is_kw(b)) return 1;
    if (b == TK_ELLIPSIS || b == TK_LBRACE) return 1;
    return -1;
}

typedef struct Fmt {
    SB out;
    const char *src;
    size_t len;
    int blank;           /* pending blank lines */
    int any;             /* something was emitted */
    size_t orig_prev;    /* original / new indentation of the previous logical line start */
    size_t new_prev;
    int have_prev;
    size_t line_orig, line_new; /* current logical line's first line */
} Fmt;

static void fmt_newline(Fmt *f) {
    /* strip blanks at the end of the current line was done by construction */
    sb_putc(&f->out, '\n');
}

static void fmt_flush_blank(Fmt *f) {
    int n = f->blank > 2 ? 2 : f->blank;
    if (f->any)
        for (int i = 0; i < n; i++) sb_putc(&f->out, '\n');
    f->blank = 0;
}

static size_t rtrim_len(const char *s, size_t n) {
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    return n;
}

/* new indentation for a comment line with original indentation c, given the previous logical line
 * (orig_prev/new_prev) and the next code line (orig_next/new_next; have_next = 0 at end of file). */
static size_t comment_indent(const Fmt *f, size_t c, int have_next, size_t orig_next, size_t new_next) {
    /* snap to the code line (previous or next) whose original indentation is closest */
    size_t dn = have_next ? (orig_next > c ? orig_next - c : c - orig_next) : (size_t)-1;
    size_t dp = f->have_prev ? (f->orig_prev > c ? f->orig_prev - c : c - f->orig_prev) : (size_t)-1;
    if (have_next && dn <= dp) return new_next;
    if (f->have_prev) return f->new_prev;
    return 0;
}

/* Process the text between two tokens (or file start / end) that contains at least one newline. */
static void fmt_gap_lines(Fmt *f, const char *g, size_t gl, int has_prev, int have_next, size_t orig_next,
                          size_t new_next) {
    size_t i = 0, start = 0;
    int piece_no = 0;
    int in_block = 0;
    while (start <= gl) {
        size_t end = start;
        while (end < gl && g[end] != '\n') end++;
        int last = end == gl; /* final piece: indentation of the next token (or EOF) */
        const char *p = g + start;
        size_t pl = end - start;
        if (!last) {
            if (in_block) {
                sb_put(&f->out, p, rtrim_len(p, pl));
                fmt_newline(f);
                size_t k;
                for (k = 0; k + 1 < pl; k++)
                    if (p[k] == '*' && p[k + 1] == '#') break;
                if (k + 1 < pl) in_block = 0;
            } else {
                size_t lead = 0;
                while (lead < pl && (p[lead] == ' ' || p[lead] == '\t')) lead++;
                size_t cl = rtrim_len(p + lead, pl - lead);
                if (piece_no == 0 && has_prev) {
                    if (cl) { /* trailing comment of the previous code line */
                        sb_spaces(&f->out, 2);
                        sb_put(&f->out, p + lead, cl);
                    }
                    fmt_newline(f);
                } else if (cl == 0) {
                    f->blank++;
                } else {
                    fmt_flush_blank(f);
                    size_t ni = comment_indent(f, lead, have_next, orig_next, new_next);
                    sb_spaces(&f->out, ni);
                    sb_put(&f->out, p + lead, cl);
                    fmt_newline(f);
                    f->any = 1;
                    if (cl >= 2 && p[lead] == '#' && p[lead + 1] == '*') {
                        size_t k;
                        for (k = lead + 2; k + 1 < lead + cl; k++)
                            if (p[k] == '*' && p[k + 1] == '#') break;
                        if (!(k + 1 < lead + cl)) in_block = 1;
                    }
                }
            }
        }
        piece_no++;
        if (last) break;
        start = end + 1;
        i++;
    }
    (void)i;
}

static void fmt_tokens(Fmt *f, const TokenList *tl) {
    const char *src = f->src;
    size_t prev_end = 0;
    const Token *prev = NULL;
    int depth = 0, logical_start = 1, prev_operand = 0, prev_unary = 0;
    int prev_kind = -1, prev_kind_before = -1;
    char brk[64];
    int nbrk = 0;
    for (int k = 0; k < tl->n; k++) {
        const Token *t = &tl->v[k];
        if (t->kind == TK_NEWLINE) { logical_start = 1; continue; }
        if (t->kind == TK_INDENT) { depth++; continue; }
        if (t->kind == TK_DEDENT) { depth--; continue; }
        if (t->kind == TK_EOF) break;
        const char *g = src + prev_end;
        size_t gl = t->offset - prev_end;
        int has_nl = memchr(g, '\n', gl) != NULL;
        int unary = 0;
        if ((t->kind == TK_MINUS || t->kind == TK_PLUS) && !prev_operand) unary = 1;
        if (t->kind == TK_TILDE) unary = 1;
        if (!has_nl && prev) {
            int sp = gap_between(prev_kind, prev_unary, t->kind, unary, nbrk > 0 && (nbrk <= 64 && brk[nbrk - 1] == '{'));
            if (sp < 0) sp = gl > 0 ? 1 : 0;
            sb_spaces(&f->out, (size_t)sp);
        } else {
            /* start of a new output line */
            size_t orig = 0;
            {
                size_t e = gl;
                size_t s = e;
                while (s > 0 && g[s - 1] != '\n') s--;
                orig = e - s;
            }
            size_t ni;
            if (logical_start) {
                ni = (size_t)depth * 4;
            } else {
                ni = f->line_new + (orig > f->line_orig ? orig - f->line_orig : 0);
            }
            fmt_gap_lines(f, g, gl, prev != NULL, 1, orig, ni);
            if (logical_start) {
                if (!prev || has_nl) {
                    /* previous logical line is now "behind us" */
                }
            }
            fmt_flush_blank(f);
            sb_spaces(&f->out, ni);
            f->any = 1;
            if (logical_start) {
                f->have_prev = 1;
                f->orig_prev = orig;
                f->new_prev = ni;
                f->line_orig = orig;
                f->line_new = ni;
            }
        }
        sb_put(&f->out, t->s, t->len);
        logical_start = 0;
        prev = t;
        prev_end = t->offset + t->len;
        prev_kind_before = prev_kind;
        prev_kind = t->kind;
        prev_unary = unary;
        prev_operand = ends_operand(t->kind) || (is_kw(t->kind) && (prev_kind_before == TK_DOT || prev_kind_before == TK_QDOT));
        if (t->kind == TK_LPAREN || t->kind == TK_LBRACK || t->kind == TK_QBRACK || t->kind == TK_LBRACE) {
            if (nbrk < 64) brk[nbrk] = t->kind == TK_LBRACE ? '{' : '(';
            nbrk++;
        } else if ((t->kind == TK_RPAREN || t->kind == TK_RBRACK || t->kind == TK_RBRACE) && nbrk > 0) {
            nbrk--;
        }
    }
    /* tail: comments and blank lines after the last token */
    {
        const char *g = src + prev_end;
        size_t gl = f->len - prev_end;
        if (gl) {
            /* make sure the text ends with a newline so the last piece is empty */
            if (!(g[gl - 1] == '\n')) {
                SB tmp = {0};
                sb_put(&tmp, g, gl);
                sb_putc(&tmp, '\n');
                fmt_gap_lines(f, tmp.p, tmp.n, prev != NULL, 0, 0, 0);
                free(tmp.p);
            } else {
                fmt_gap_lines(f, g, gl, prev != NULL, 0, 0, 0);
            }
        } else if (prev) {
            fmt_newline(f);
        }
    }
    /* exactly one trailing newline */
    while (f->out.n >= 2 && f->out.p[f->out.n - 1] == '\n' && f->out.p[f->out.n - 2] == '\n') f->out.n--;
    f->out.p[f->out.n] = 0;
}

/* Re-lex the formatted text and check it is token-for-token equal to the original. */
static int tokens_equal(const TokenList *a, const TokenList *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) {
        const Token *x = &a->v[i], *y = &b->v[i];
        if (x->kind != y->kind) return 0;
        if (is_struct(x->kind)) continue;
        if (x->len != y->len || memcmp(x->s, y->s, x->len) != 0) return 0;
    }
    return 1;
}

/* Format `src` (LF-normalised, valid UTF-8). Returns 0 and fills *out, 1 on a syntax error
 * (diag in F), 2 if the safety check failed. The caller frees out->p. */
static int fmt_text(Front *F, const char *src, size_t len, SB *out) {
    TokenList tl;
    memset(&tl, 0, sizeof tl);
    if (lex_guarded(F, src, len, &tl)) return 1;
    front_done(F);
    Fmt f;
    memset(&f, 0, sizeof f);
    f.src = src;
    f.len = len;
    fmt_tokens(&f, &tl);
    int rc = 0;
    if (f.out.n == 0) sb_put(&f.out, "", 0);
    TokenList tl2;
    memset(&tl2, 0, sizeof tl2);
    Front F2;
    memset(&F2, 0, sizeof F2);
    F2.file = F->file;
    if (lex_guarded(&F2, f.out.p, f.out.n, &tl2) || !tokens_equal(&tl, &tl2)) rc = 2;
    front_done(&F2);
    toklist_free(&tl2);
    toklist_free(&tl);
    if (rc == 0) *out = f.out;
    else free(f.out.p);
    return rc;
}

static char *read_all_stdin(size_t *len) {
    SB b = {0};
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) sb_put(&b, buf, n);
    if (!b.p) sb_put(&b, "", 0);
    *len = b.n;
    return b.p;
}

static int write_atomic(const char *path, const char *data, size_t n) {
    char tmp[1100];
    (void)snprintf(tmp, sizeof tmp, "%s.fmt.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) { (void)remove(tmp); return -1; }
    return 0;
}

int load_source(const char *path, char **src, size_t *len) {
    if (strcmp(path, "-") == 0) *src = read_all_stdin(len);
    else *src = sp_read_file(path, len);
    if (!*src) return -1;
    *len = sp_normalize(*src, *len);
    return 0;
}

int check_text(const char *path, const char *src, size_t len, int json) {
    size_t bad;
    if (!utf8_validate(src, len, &bad) || memchr(src, 0, len)) {
        (void)fprintf(stderr, "%s: LexError: invalid UTF-8 or NUL byte in source\n", path);
        (void)json;
        return 1;
    }
    return 0;
}

int tool_fmt(int argc, char **argv) {
    int check = 0, write = 0, json = 0, i = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--check") == 0) check = 1;
        else if (strcmp(argv[i], "--write") == 0 || strcmp(argv[i], "-w") == 0) write = 1;
        else if (strcmp(argv[i], "--json") == 0) json = 1;
        else if (strcmp(argv[i], "--") == 0) { i++; break; }
        else return 2;
    }
    if (i >= argc || (!check && !write && argc - i != 1)) return 2;
    int rc = 0;
    for (; i < argc; i++) {
        const char *path = argv[i];
        char *src;
        size_t len;
        if (load_source(path, &src, &len) != 0) {
            (void)fprintf(stderr, "sprint: cannot read '%s'\n", path);
            rc = rc ? rc : 3;
            continue;
        }
        if (check_text(path, src, len, json)) { free(src); rc = 12; continue; }
        Front F;
        memset(&F, 0, sizeof F);
        F.file = path;
        (void)snprintf(F.diag.file, sizeof F.diag.file, "%s", path);
        SB out = {0};
        int st = fmt_text(&F, src, len, &out);
        if (st == 1) {
            print_front_diag(&F.diag, json);
            rc = 12;
        } else if (st == 2) {
            (void)fprintf(stderr, "%s: internal error: formatted text is not token-equivalent; file left unchanged\n", path);
            rc = 70;
        } else {
            int same = out.n == len && memcmp(out.p, src, len) == 0;
            if (check) {
                if (!same) {
                    (void)printf("would reformat %s\n", path);
                    if (rc == 0) rc = 1;
                }
            } else if (write) {
                if (!same && strcmp(path, "-") != 0 && write_atomic(path, out.p, out.n) != 0) {
                    (void)fprintf(stderr, "sprint: cannot write '%s'\n", path);
                    rc = 3;
                } else if (strcmp(path, "-") == 0) {
                    (void)fwrite(out.p, 1, out.n, stdout);
                }
            } else {
                (void)fwrite(out.p, 1, out.n, stdout);
            }
            free(out.p);
        }
        free(src);
    }
    return rc;
}
