/* Layer 1+2: UTF-8 validation, INDENT/DEDENT lexer (SPEC 3) */
#include "sp.h"
#include <ctype.h>
#include <errno.h>

/* ---------------------------------------------------------------- */
/* front-end diagnostics                                             */
/* ---------------------------------------------------------------- */
void front_error(Front *F, const char *kind, int code, int line, int col, size_t off,
                 const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(F->diag.msg, sizeof F->diag.msg, fmt, ap);
    va_end(ap);
    F->diag.kind = kind;
    F->diag.code = code;
    F->diag.line = line;
    F->diag.col = col;
    F->diag.offset = off;
    snprintf(F->diag.file, sizeof F->diag.file, "%s", F->file ? F->file : "<input>");
    F->has_diag = 1;
    while (F->ncl > 0) {
        F->ncl--;
        F->cl[F->ncl].fn(F->cl[F->ncl].p);
    }
    front_done(F);
    F->file = NULL;
    longjmp(F->jb, 1);
}

void front_push(Front *F, void (*fn)(void *), void *p) {
    if (F->ncl == F->capcl) {
        int nc = F->capcl ? F->capcl * 2 : 16;
        FrontCl *np = realloc(F->cl, sizeof *np * (size_t)nc);
        if (!np) { fputs("out of memory\n", stderr); exit(14); }
        F->cl = np;
        F->capcl = nc;
    }
    F->cl[F->ncl].fn = fn;
    F->cl[F->ncl].p = p;
    F->ncl++;
}
void front_pop(Front *F) { F->ncl--; }
void front_free_ptr(void *p) { free(p); }
void front_done(Front *F) {
    free(F->cl);
    F->cl = NULL;
    F->ncl = F->capcl = 0;
}

/* ---------------------------------------------------------------- */
/* arena                                                             */
/* ---------------------------------------------------------------- */
struct ArenaBlk {
    struct ArenaBlk *next;
    size_t used, cap;
    char data[];
};
struct Arena {
    struct ArenaBlk *head;
};

Arena *arena_new(void) {
    Arena *a = calloc(1, sizeof *a);
    return a;
}

void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 65536 ? n : 65536;
        struct ArenaBlk *b = malloc(sizeof *b + cap);
        if (!b) {
            fputs("out of memory\n", stderr);
            exit(14);
        }
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    memset(p, 0, n);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void arena_free(Arena *a) {
    struct ArenaBlk *b = a->head;
    while (b) {
        struct ArenaBlk *n = b->next;
        free(b);
        b = n;
    }
    free(a);
}

/* ---------------------------------------------------------------- */
/* utf-8                                                             */
/* ---------------------------------------------------------------- */
int utf8_validate(const char *s, size_t n, size_t *bad) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        unsigned c = p[i];
        size_t need;
        uint32_t cp;
        if (c < 0x80) { i++; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
        else { if (bad) *bad = i; return 0; }
        for (size_t k = 1; k <= need; k++) {
            if (i + k >= n || (p[i + k] & 0xC0) != 0x80) { if (bad) *bad = i; return 0; }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            if (bad) *bad = i;
            return 0;
        }
        i += need + 1;
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* tokens                                                            */
/* ---------------------------------------------------------------- */
static const struct { const char *s; int k; size_t n; } kw_table[] = {
#define X(n, s) {s, TK_KW_##n, sizeof(s) - 1},
    KEYWORD_LIST(X)
#undef X
};

int keyword_kind(const char *s, size_t len) {
    if (len < 2 || len > 8) return -1; /* the shortest keyword is "as", the longest "override" / "continue" */
    for (size_t i = 0; i < sizeof kw_table / sizeof kw_table[0]; i++)
        if (kw_table[i].s[0] > s[0]) break; /* the table is sorted */
        else if (kw_table[i].n == len && kw_table[i].s[0] == s[0] && memcmp(kw_table[i].s, s, len) == 0)
            return kw_table[i].k;
    return -1;
}

const char *tok_name(int kind) {
    switch (kind) {
    case TK_EOF: return "end of file";
    case TK_NEWLINE: return "newline";
    case TK_INDENT: return "indent";
    case TK_DEDENT: return "dedent";
    case TK_IDENT: return "identifier";
    case TK_INT: return "integer";
    case TK_FLOAT: return "float";
    case TK_STRING: return "string";
    case TK_PLUS: return "'+'"; case TK_MINUS: return "'-'"; case TK_STAR: return "'*'";
    case TK_SLASH: return "'/'"; case TK_SLASHSLASH: return "'//'";
    case TK_PERCENT: return "'%'"; case TK_STARSTAR: return "'**'";
    case TK_DOTDOT: return "'..'"; case TK_AMP: return "'&'"; case TK_PIPE: return "'|'";
    case TK_CARET: return "'^'"; case TK_TILDE: return "'~'"; case TK_SHL: return "'<<'";
    case TK_SHR: return "'>>'"; case TK_EQEQ: return "'=='"; case TK_NE: return "'!='";
    case TK_LT: return "'<'"; case TK_LE: return "'<='"; case TK_GT: return "'>'";
    case TK_GE: return "'>='"; case TK_ASSIGN: return "'='"; case TK_PLUSEQ: return "'+='";
    case TK_MINUSEQ: return "'-='"; case TK_STAREQ: return "'*='"; case TK_SLASHEQ: return "'/='";
    case TK_SLASHSLASHEQ: return "'//='"; case TK_PERCENTEQ: return "'%='";
    case TK_DOTDOTEQ: return "'..='"; case TK_LPAREN: return "'('"; case TK_RPAREN: return "')'";
    case TK_LBRACK: return "'['"; case TK_RBRACK: return "']'"; case TK_LBRACE: return "'{'";
    case TK_RBRACE: return "'}'"; case TK_COLON: return "':'"; case TK_COMMA: return "','";
    case TK_DOT: return "'.'"; case TK_ELLIPSIS: return "'...'";
    case TK_QQ: return "'?" "?'"; case TK_STARSTAREQ: return "'**='"; case TK_AMPEQ: return "'&='";
    case TK_PIPEEQ: return "'|='"; case TK_CARETEQ: return "'^='"; case TK_SHLEQ: return "'<<='";
    case TK_SHREQ: return "'>>='"; case TK_FSTRING: return "f-string";
    case TK_FATARROW: return "'=>'"; case TK_PIPEGT: return "'|>'";
    case TK_QDOT: return "'?.'"; case TK_QBRACK: return "'?['"; case TK_QQASSIGN: return "'?" "?='";
    default:
        if (kind > TK_KW_AND - 1 && kind < TK_KW_END_) {
            static const char *names[] = {
#define X(n, s) "'" s "'",
                KEYWORD_LIST(X)
#undef X
            };
            return names[kind - TK_KW_AND];
        }
        return "token";
    }
}

void toklist_free(TokenList *t) {
    for (int i = 0; i < t->n; i++) free(t->v[i].sval);
    free(t->v);
    t->v = NULL;
    t->n = t->cap = 0;
}

/* ---------------------------------------------------------------- */
/* lexer                                                             */
/* ---------------------------------------------------------------- */
typedef struct Lexer {
    Front *F;
    const char *src;
    size_t len, pos;
    int line, col;
    TokenList *out;
    int *indent;
    int nind, capind;
    int paren;
    int content_on_line; /* an emitted token on the current logical line */
} Lexer;

static SP_NORETURN void lerr(Lexer *L, int line, int col, size_t off, const char *msg) {
    front_error(L->F, "LexError", E_LEX, line, col, off, "%s", msg);
}

static int peekc(Lexer *L, size_t k) {
    return L->pos + k < L->len ? (unsigned char)L->src[L->pos + k] : -1;
}

static void adv(Lexer *L) {
    unsigned char c = (unsigned char)L->src[L->pos++];
    if (c == '\n') { L->line++; L->col = 1; }
    else if ((c & 0xC0) != 0x80) L->col++;
}

static Token *push_tok(Lexer *L, int kind, int line, int col, size_t off, size_t len) {
    TokenList *t = L->out;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 256;
        t->v = realloc(t->v, (size_t)t->cap * sizeof *t->v);
        if (!t->v) { fputs("out of memory\n", stderr); exit(14); }
    }
    Token *k = &t->v[t->n++];
    memset(k, 0, sizeof *k);
    k->kind = kind;
    k->line = line;
    k->col = col;
    k->offset = off;
    k->s = L->src + off;
    k->len = len;
    return k;
}

static int is_id_start(int c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_id_cont(int c) { return is_id_start(c) || (c >= '0' && c <= '9'); }
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void lex_number(Lexer *L) {
    int line = L->line, col = L->col;
    size_t start = L->pos;
    int base = 10;
    int c0 = peekc(L, 0), c1 = peekc(L, 1);
    if (c0 == '0' && (c1 == 'x' || c1 == 'X')) base = 16;
    else if (c0 == '0' && (c1 == 'b' || c1 == 'B')) base = 2;
    else if (c0 == '0' && (c1 == 'o' || c1 == 'O')) base = 8;

    if (base != 10) {
        adv(L); adv(L);
        uint64_t v = 0;
        int ndig = 0, last_us = 0;
        for (;;) {
            int c = peekc(L, 0);
            if (c == '_') {
                if (ndig == 0 || last_us) lerr(L, L->line, L->col, L->pos, "misplaced '_' in number literal");
                last_us = 1; adv(L); continue;
            }
            int h = c < 0 ? -1 : hexval(c);
            if (h < 0 || h >= base) break;
            if (v > (UINT64_MAX >> (base == 16 ? 4 : base == 8 ? 3 : 1)))
                lerr(L, line, col, start, "integer literal out of range");
            v = v * (uint64_t)base + (uint64_t)h;
            ndig++; last_us = 0; adv(L);
        }
        if (ndig == 0) lerr(L, line, col, start, "missing digits in number literal");
        if (last_us) lerr(L, L->line, L->col, L->pos, "misplaced '_' in number literal");
        if (is_id_cont(peekc(L, 0))) lerr(L, L->line, L->col, L->pos, "invalid character in number literal");
        if (v > (uint64_t)INT64_MAX + 1) lerr(L, line, col, start, "integer literal out of range");
        Token *t = push_tok(L, TK_INT, line, col, start, L->pos - start);
        if (v == (uint64_t)INT64_MAX + 1) { t->big = 1; t->ival = INT64_MIN; }
        else t->ival = (int64_t)v;
        return;
    }

    /* decimal */
    char buf[512];
    size_t bl = 0;
    int isfloat = 0;
    int c = peekc(L, 0);
    if (c == '0' && peekc(L, 1) >= '0' && peekc(L, 1) <= '9')
        lerr(L, line, col, start, "leading zeros are not allowed in decimal literals");
    int last_us = 0, ndig = 0;
    #define DIGITS() \
        for (;;) { \
            int d = peekc(L, 0); \
            if (d == '_') { \
                if (ndig == 0 || last_us) lerr(L, L->line, L->col, L->pos, "misplaced '_' in number literal"); \
                last_us = 1; adv(L); continue; \
            } \
            if (d < '0' || d > '9') break; \
            if (bl + 2 >= sizeof buf) lerr(L, line, col, start, "number literal too long"); \
            buf[bl++] = (char)d; ndig++; last_us = 0; adv(L); \
        } \
        if (last_us) lerr(L, L->line, L->col, L->pos, "misplaced '_' in number literal");
    DIGITS();
    if (peekc(L, 0) == '.' && peekc(L, 1) >= '0' && peekc(L, 1) <= '9') {
        isfloat = 1;
        buf[bl++] = '.';
        adv(L);
        ndig = 0; last_us = 0;
        DIGITS();
    }
    c = peekc(L, 0);
    if (c == 'e' || c == 'E') {
        int c1b = peekc(L, 1), c2 = peekc(L, 2);
        int ok = (c1b >= '0' && c1b <= '9') || ((c1b == '+' || c1b == '-') && c2 >= '0' && c2 <= '9');
        if (ok) {
            isfloat = 1;
            buf[bl++] = 'e';
            adv(L);
            if (peekc(L, 0) == '+' || peekc(L, 0) == '-') { buf[bl++] = (char)peekc(L, 0); adv(L); }
            ndig = 0; last_us = 0;
            DIGITS();
        }
    }
    #undef DIGITS
    buf[bl] = 0;
    if (is_id_cont(peekc(L, 0))) lerr(L, L->line, L->col, L->pos, "invalid character in number literal");
    if (isfloat) {
        char *end;
        double d = strtod(buf, &end);
        if (isinf(d)) lerr(L, line, col, start, "float literal out of range");
        Token *t = push_tok(L, TK_FLOAT, line, col, start, L->pos - start);
        t->fval = d;
    } else {
        uint64_t v = 0;
        for (size_t i = 0; i < bl; i++) {
            unsigned d = (unsigned)(buf[i] - '0');
            if (v > (UINT64_MAX - d) / 10) lerr(L, line, col, start, "integer literal out of range");
            v = v * 10 + d;
        }
        if (v > (uint64_t)INT64_MAX + 1) lerr(L, line, col, start, "integer literal out of range");
        Token *t = push_tok(L, TK_INT, line, col, start, L->pos - start);
        if (v == (uint64_t)INT64_MAX + 1) { t->big = 1; t->ival = INT64_MIN; }
        else t->ival = (int64_t)v;
    }
}

static size_t utf8_put(char *dst, uint32_t cp) {
    if (cp < 0x80) { dst[0] = (char)cp; return 1; }
    if (cp < 0x800) { dst[0] = (char)(0xC0 | (cp >> 6)); dst[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12)); dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    dst[0] = (char)(0xF0 | (cp >> 18)); dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); dst[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static void lex_string(Lexer *L, int fstr) {
    int line = L->line, col = L->col;
    size_t start = L->pos;
    if (fstr) adv(L); /* the 'f' prefix */
    int q = peekc(L, 0);
    int triple = (peekc(L, 1) == q && peekc(L, 2) == q);
    adv(L);
    if (triple) { adv(L); adv(L); }
    size_t cap = 64, n = 0;
    char *buf = malloc(cap);
    if (!buf) { fputs("out of memory\n", stderr); exit(14); }
#define PUT(ptr, cnt) do { size_t c_ = (cnt); \
        while (n + c_ + 1 > cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) exit(14); } \
        memcpy(buf + n, (ptr), c_); n += c_; } while (0)
    for (;;) {
        int c = peekc(L, 0);
        if (c < 0) { free(buf); lerr(L, line, col, start, "unterminated string literal"); }
        if (c == q) {
            if (!triple) { adv(L); break; }
            if (peekc(L, 1) == q && peekc(L, 2) == q) { adv(L); adv(L); adv(L); break; }
            char ch = (char)c; PUT(&ch, 1); adv(L); continue;
        }
        if (c == '\n' && !triple) { free(buf); lerr(L, line, col, start, "newline in string literal"); }
        if (fstr && (c == 1 || c == 2)) { free(buf); lerr(L, L->line, L->col, L->pos, "control character in f-string"); }
        if (fstr && c == '}') {
            if (peekc(L, 1) != '}') { free(buf); lerr(L, L->line, L->col, L->pos, "single '}' in f-string (write '}}' for a literal brace)"); }
            char ch = '}'; PUT(&ch, 1); adv(L); adv(L); continue;
        }
        if (fstr && c == '{') {
            if (peekc(L, 1) == '{') { char ch = '{'; PUT(&ch, 1); adv(L); adv(L); continue; }
            int eline = L->line, ecol = L->col; size_t eoff = L->pos;
            char mk1 = 1; PUT(&mk1, 1); adv(L);
            int depth = 1;
            for (;;) {
                int d = peekc(L, 0);
                if (d < 0 || (d == '\n' && !triple)) { free(buf); lerr(L, eline, ecol, eoff, "unterminated '{' in f-string"); }
                if (d == '{') depth++;
                else if (d == '}') { if (--depth == 0) break; }
                else if (d == '"' || d == '\'') {
                    if (d == q && !triple) { free(buf); lerr(L, L->line, L->col, L->pos, "use the other quote character inside an f-string expression"); }
                    char qc = (char)d; PUT(&qc, 1); adv(L);
                    for (;;) {
                        int s2 = peekc(L, 0);
                        if (s2 < 0 || s2 == '\n') { free(buf); lerr(L, eline, ecol, eoff, "unterminated string in f-string expression"); }
                        char sc = (char)s2; PUT(&sc, 1); adv(L);
                        if (s2 == '\\') { int s3 = peekc(L, 0); if (s3 >= 0) { char s3c = (char)s3; PUT(&s3c, 1); adv(L); } continue; }
                        if (s2 == d) break;
                    }
                    continue;
                }
                char dc = (char)d; PUT(&dc, 1); adv(L);
            }
            char mk2 = 2; PUT(&mk2, 1); adv(L);
            continue;
        }
        if (c == '\\') {
            int eline = L->line, ecol = L->col; size_t eoff = L->pos;
            adv(L);
            int e = peekc(L, 0);
            char ch;
            switch (e) {
            case '\\': ch = '\\'; break;
            case '"': ch = '"'; break;
            case '\'': ch = '\''; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case '0': ch = 0; break;
            case 'x': {
                adv(L);
                int h1 = hexval(peekc(L, 0)), h2 = hexval(peekc(L, 1));
                if (h1 < 0 || h2 < 0) { free(buf); lerr(L, eline, ecol, eoff, "invalid \\x escape"); }
                if (h1 * 16 + h2 > 0x7F) { free(buf); lerr(L, eline, ecol, eoff, "\\x escape must be 00-7F"); }
                ch = (char)(h1 * 16 + h2);
                adv(L); adv(L);
                PUT(&ch, 1);
                continue;
            }
            case 'u': {
                adv(L);
                if (peekc(L, 0) != '{') { free(buf); lerr(L, eline, ecol, eoff, "invalid \\u escape"); }
                adv(L);
                uint32_t cp = 0; int nd = 0;
                while (hexval(peekc(L, 0)) >= 0) {
                    cp = cp * 16 + (uint32_t)hexval(peekc(L, 0));
                    if (++nd > 6) { free(buf); lerr(L, eline, ecol, eoff, "invalid \\u escape"); }
                    adv(L);
                }
                if (nd == 0 || peekc(L, 0) != '}' || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    free(buf); lerr(L, eline, ecol, eoff, "invalid \\u escape");
                }
                adv(L);
                char tmp[4]; size_t k = utf8_put(tmp, cp);
                PUT(tmp, k);
                continue;
            }
            default:
                free(buf);
                lerr(L, eline, ecol, eoff, "invalid escape sequence");
            }
            adv(L);
            PUT(&ch, 1);
            continue;
        }
        char ch = (char)c;
        PUT(&ch, 1);
        adv(L);
    }
#undef PUT
    buf[n] = 0;
    Token *t = push_tok(L, fstr ? TK_FSTRING : TK_STRING, line, col, start, L->pos - start);
    t->sval = buf;
    t->slen = n;
}

/* r"text" / r'text' / r"""text""": no escape processing, no interpolation. */
static void lex_raw(Lexer *L) {
    int line = L->line, col = L->col;
    size_t start = L->pos;
    adv(L); /* the 'r' prefix */
    int q = peekc(L, 0);
    int triple = (peekc(L, 1) == q && peekc(L, 2) == q);
    adv(L);
    if (triple) { adv(L); adv(L); }
    size_t cap = 64, n = 0;
    char *buf = malloc(cap);
    if (!buf) { fputs("out of memory\n", stderr); exit(14); }
    for (;;) {
        int c = peekc(L, 0);
        if (c < 0) { free(buf); lerr(L, line, col, start, "unterminated string literal"); }
        if (c == q && (!triple || (peekc(L, 1) == q && peekc(L, 2) == q))) {
            adv(L);
            if (triple) { adv(L); adv(L); }
            break;
        }
        if (c == '\n' && !triple) { free(buf); lerr(L, line, col, start, "newline in string literal"); }
        if (n + 2 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); exit(14); }
            buf = nb;
        }
        buf[n++] = (char)c;
        adv(L);
    }
    buf[n] = 0;
    Token *t = push_tok(L, TK_STRING, line, col, start, L->pos - start);
    t->sval = buf;
    t->slen = n;
}

static void end_line_tokens(Lexer *L) {
    /* called when a newline is consumed at paren depth 0 and the line had content */
    if (L->content_on_line) {
        push_tok(L, TK_NEWLINE, L->line, L->col, L->pos, 0);
        L->content_on_line = 0;
    }
}

void lex_source(Front *F, const char *src, size_t len, TokenList *out) { lex_source_at(F, src, len, out, 1, 1); }

void lex_source_at(Front *F, const char *src, size_t len, TokenList *out, int line0, int col0) {
    Lexer L;
    memset(&L, 0, sizeof L);
    L.F = F; L.src = src; L.len = len; L.line = line0; L.col = col0; L.out = out;
    L.capind = 32;
    L.indent = malloc(sizeof(int) * (size_t)L.capind);
    L.indent[L.nind++] = 0;

    if (len >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
        free(L.indent);
        lerr(&L, 1, 1, 0, "byte order mark is not allowed");
    }
    size_t bad;
    if (!utf8_validate(src, len, &bad)) {
        free(L.indent);
        /* compute position of bad byte */
        int ln = 1, cl = 1;
        for (size_t i = 0; i < bad; i++) {
            if (src[i] == '\n') { ln++; cl = 1; } else if (((unsigned char)src[i] & 0xC0) != 0x80) cl++;
        }
        lerr(&L, ln, cl, bad, "invalid UTF-8");
    }

    int at_line_start = 1;
    for (;;) {
        if (at_line_start && L.paren == 0) {
            int n = 0;
            while (peekc(&L, 0) == ' ') { adv(&L); n++; }
            int c = peekc(&L, 0);
            if (c == '\t') { free(L.indent); lerr(&L, L.line, L.col, L.pos, "tab characters are not allowed"); }
            if (c == '\r') { free(L.indent); lerr(&L, L.line, L.col, L.pos, "stray carriage return"); }
            if (c == '\n') { adv(&L); continue; }
            if (c < 0) break;
            if (c == '#') {
                if (peekc(&L, 1) == '*') {
                    int sl = L.line, sc = L.col; size_t so = L.pos;
                    adv(&L); adv(&L);
                    int closed = 0;
                    while (peekc(&L, 0) >= 0) {
                        if (peekc(&L, 0) == '*' && peekc(&L, 1) == '#') { adv(&L); adv(&L); closed = 1; break; }
                        adv(&L);
                    }
                    if (!closed) { free(L.indent); lerr(&L, sl, sc, so, "unterminated block comment"); }
                    while (peekc(&L, 0) == ' ') adv(&L);
                    if (peekc(&L, 0) == '#' ) { while (peekc(&L, 0) >= 0 && peekc(&L, 0) != '\n') adv(&L); }
                    if (peekc(&L, 0) == '\n') adv(&L);
                    else if (peekc(&L, 0) >= 0) { free(L.indent); lerr(&L, L.line, L.col, L.pos, "'*#' must be the last thing on its line"); }
                    continue;
                }
                while (peekc(&L, 0) >= 0 && peekc(&L, 0) != '\n') adv(&L);
                if (peekc(&L, 0) == '\n') adv(&L);
                continue;
            }
            if (c == '|' && peekc(&L, 1) == '>' && L.out->n > 0 && L.out->v[L.out->n - 1].kind == TK_NEWLINE) {
                /* a line that starts with `|>` continues the previous expression (SPEC 7.10) */
                L.out->n--;
                L.content_on_line = 1;
                at_line_start = 0;
                continue;
            }
            int top = L.indent[L.nind - 1];
            if (n > top) {
                if (n != top + 4) { free(L.indent); lerr(&L, L.line, 1, L.pos - (size_t)n, "indentation must be exactly 4 spaces deeper"); }
                if (L.nind == L.capind) { L.capind *= 2; L.indent = realloc(L.indent, sizeof(int) * (size_t)L.capind); }
                L.indent[L.nind++] = n;
                push_tok(&L, TK_INDENT, L.line, 1, L.pos - (size_t)n, (size_t)n);
            } else if (n < top) {
                while (L.nind > 1 && n < L.indent[L.nind - 1]) {
                    L.nind--;
                    push_tok(&L, TK_DEDENT, L.line, 1, L.pos - (size_t)n, 0);
                }
                if (n != L.indent[L.nind - 1]) { free(L.indent); lerr(&L, L.line, 1, L.pos - (size_t)n, "inconsistent dedent"); }
            }
            at_line_start = 0;
        }

        int c = peekc(&L, 0);
        if (c < 0) break;
        if (c == ' ') { adv(&L); continue; }
        if (c == '\t') { free(L.indent); lerr(&L, L.line, L.col, L.pos, "tab characters are not allowed"); }
        if (c == '\r') { free(L.indent); lerr(&L, L.line, L.col, L.pos, "stray carriage return"); }
        if (c == '\n') {
            adv(&L);
            if (L.paren == 0) { end_line_tokens(&L); at_line_start = 1; }
            continue;
        }
        if (c == '#') {
            if (peekc(&L, 1) == '*') { free(L.indent); lerr(&L, L.line, L.col, L.pos, "block comment must start its own line"); }
            while (peekc(&L, 0) >= 0 && peekc(&L, 0) != '\n') adv(&L);
            continue;
        }
        int line = L.line, col = L.col;
        size_t off = L.pos;
        if (c >= 0x80) { free(L.indent); lerr(&L, line, col, off, "non-ASCII character outside string or comment"); }
        L.content_on_line = 1;
        if (c == 'r' && (peekc(&L, 1) == '"' || peekc(&L, 1) == '\'')) { lex_raw(&L); continue; }
        if (c == 'f' && (peekc(&L, 1) == '"' || peekc(&L, 1) == '\'')) { lex_string(&L, 1); continue; }
        if (is_id_start(c)) {
            while (is_id_cont(peekc(&L, 0))) adv(&L);
            size_t n = L.pos - off;
            int k = keyword_kind(src + off, n);
            push_tok(&L, k >= 0 ? k : TK_IDENT, line, col, off, n);
            continue;
        }
        if (c >= '0' && c <= '9') { lex_number(&L); continue; }
        if (c == '"' || c == '\'') { lex_string(&L, 0); continue; }

        int c1 = peekc(&L, 1), c2 = peekc(&L, 2);
        int kind = -1, n = 1;
        switch (c) {
        case '+': if (c1 == '=') { kind = TK_PLUSEQ; n = 2; } else kind = TK_PLUS; break;
        case '-': if (c1 == '=') { kind = TK_MINUSEQ; n = 2; } else kind = TK_MINUS; break;
        case '*':
            if (c1 == '*') { if (c2 == '=') { kind = TK_STARSTAREQ; n = 3; } else { kind = TK_STARSTAR; n = 2; } }
            else if (c1 == '=') { kind = TK_STAREQ; n = 2; }
            else kind = TK_STAR;
            break;
        case '/':
            if (c1 == '/') { if (c2 == '=') { kind = TK_SLASHSLASHEQ; n = 3; } else { kind = TK_SLASHSLASH; n = 2; } }
            else if (c1 == '=') { kind = TK_SLASHEQ; n = 2; }
            else kind = TK_SLASH;
            break;
        case '%': if (c1 == '=') { kind = TK_PERCENTEQ; n = 2; } else kind = TK_PERCENT; break;
        case '.':
            if (c1 == '.') {
                if (c2 == '.') { kind = TK_ELLIPSIS; n = 3; }
                else if (c2 == '=') { kind = TK_DOTDOTEQ; n = 3; }
                else { kind = TK_DOTDOT; n = 2; }
            } else kind = TK_DOT;
            break;
        case '&': if (c1 == '=') { kind = TK_AMPEQ; n = 2; } else kind = TK_AMP; break;
        case '|': if (c1 == '=') { kind = TK_PIPEEQ; n = 2; } else if (c1 == '>') { kind = TK_PIPEGT; n = 2; } else kind = TK_PIPE; break;
        case '^': if (c1 == '=') { kind = TK_CARETEQ; n = 2; } else kind = TK_CARET; break;
        case '?':
            if (c1 == '?') { if (c2 == '=') { kind = TK_QQASSIGN; n = 3; } else { kind = TK_QQ; n = 2; } }
            else if (c1 == '.' && !(c2 >= '0' && c2 <= '9')) { kind = TK_QDOT; n = 2; }
            else if (c1 == '[') { kind = TK_QBRACK; n = 2; L.paren++; }
            break;
        case '~': kind = TK_TILDE; break;
        case '<':
            if (c1 == '<') { if (c2 == '=') { kind = TK_SHLEQ; n = 3; } else { kind = TK_SHL; n = 2; } }
            else if (c1 == '=') { kind = TK_LE; n = 2; }
            else kind = TK_LT;
            break;
        case '>':
            if (c1 == '>') { if (c2 == '=') { kind = TK_SHREQ; n = 3; } else { kind = TK_SHR; n = 2; } }
            else if (c1 == '=') { kind = TK_GE; n = 2; }
            else kind = TK_GT;
            break;
        case '=': if (c1 == '=') { kind = TK_EQEQ; n = 2; } else if (c1 == '>') { kind = TK_FATARROW; n = 2; } else kind = TK_ASSIGN; break;
        case '!': if (c1 == '=') { kind = TK_NE; n = 2; } break;
        case '(': kind = TK_LPAREN; L.paren++; break;
        case ')': kind = TK_RPAREN; L.paren--; break;
        case '[': kind = TK_LBRACK; L.paren++; break;
        case ']': kind = TK_RBRACK; L.paren--; break;
        case '{': kind = TK_LBRACE; L.paren++; break;
        case '}': kind = TK_RBRACE; L.paren--; break;
        case ':': kind = TK_COLON; break;
        case ',': kind = TK_COMMA; break;
        default: break;
        }
        if (kind < 0) {
            char m[64];
            snprintf(m, sizeof m, "unexpected character '%c'", c);
            free(L.indent);
            lerr(&L, line, col, off, m);
        }
        if (L.paren < 0) { free(L.indent); front_error(F, "ParseError", E_PARSE, line, col, off, "unmatched closing bracket"); }
        for (int i = 0; i < n; i++) adv(&L);
        push_tok(&L, kind, line, col, off, (size_t)n);
    }

    if (L.paren > 0) {
        free(L.indent);
        front_error(F, "ParseError", E_PARSE, L.line, L.col, L.pos, "unclosed bracket at end of file");
    }
    if (L.content_on_line) push_tok(&L, TK_NEWLINE, L.line, L.col, L.pos, 0);
    while (L.nind > 1) { L.nind--; push_tok(&L, TK_DEDENT, L.line, L.col, L.pos, 0); }
    push_tok(&L, TK_EOF, L.line, L.col, L.pos, 0);
    free(L.indent);
}
