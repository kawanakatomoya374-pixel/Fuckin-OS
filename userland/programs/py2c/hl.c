/* hl.c - syntax highlighting for Python and C, one line at a time with a small carry state for multi-line strings and comments. */
#include "p2.h"

static const char *const PY_KW[] = { "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif", "else", "except", "finally", "for", "from",
    "global", "if", "import", "in", "is", "lambda", "match", "case", "nonlocal", "not", "or", "pass", "raise", "return", "try", "while", "with", "yield", "None", "True", "False", NULL };
static const char *const PY_BUILTIN[] = { "print", "len", "range", "input", "str", "int", "float", "bool", "abs", "round", "min", "max", "sum", "sorted", "enumerate", "zip", "isinstance",
    "type", "any", "all", "map", "filter", "list", "tuple", "dict", "set", "divmod", "pow", "format", "callable", "self", "super", "repr", "reversed", "open", "iter", "next", "ord", "chr", NULL };
static const char *const C_KW[] = { "auto", "break", "case", "const", "continue", "default", "do", "else", "enum", "extern", "for", "goto", "if", "inline", "register", "return",
    "sizeof", "static", "struct", "switch", "typedef", "union", "volatile", "while", "NULL", "true", "false", NULL };
static const char *const C_TYPE[] = { "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "bool", "size_t", "ssize_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "FILE", "P2C_Object", NULL };

static bool in_list(const char *const *l, const char *s, int n) {
    for (int i = 0; l[i]; ++i) if ((int)strlen(l[i]) == n && !strncmp(l[i], s, (size_t)n)) return true;
    return false;
}
static bool idstart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (unsigned char)c >= 0x80; }
static bool idchar(char c) { return idstart(c) || (c >= '0' && c <= '9'); }

/* index just after the closing triple quote q-q-q at or after `from`, or -1 */
static int find_triple_end(const char *s, int n, int from, char q) {
    for (int j = from; j < n; ) {
        if (s[j] == '\\') { j += 2; continue; }
        if (s[j] == q && j + 2 < n && s[j + 1] == q && s[j + 2] == q) return j + 3;
        j++;
    }
    return -1;
}
#define EMIT(st, ln, k) do { if (nt < max && (ln) > 0) { out[nt].start = (st); out[nt].len = (ln); out[nt].kind = (k); nt++; } } while (0)

static int hl_py(const char *s, int n, int *state, Tok *out, int max) {
    int nt = 0, i = 0;
    int st = *state;
    if (st == 1 || st == 2) {                                   /* inside a triple-quoted string */
        char q = st == 1 ? '\'' : '"';
        int e = find_triple_end(s, n, 0, q);
        if (e < 0) { EMIT(0, n, K_STR); return nt; }
        EMIT(0, e, K_STR); *state = 0; i = e;
    }
    int prev_def = 0;                                           /* 1 after "def", 2 after "class" */
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '#') { EMIT(i, n - i, K_CMT); break; }
        if (c == '@' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t' || i == 0)) { int j = i + 1; while (j < n && (idchar(s[j]) || s[j] == '.')) j++; EMIT(i, j - i, K_DECO); i = j; continue; }
        /* string, with an optional prefix (r, b, f, u and combinations) */
        int ps = i, pj = i;
        while (pj < n && pj - i < 3 && (s[pj] == 'r' || s[pj] == 'R' || s[pj] == 'b' || s[pj] == 'B' || s[pj] == 'f' || s[pj] == 'F' || s[pj] == 'u' || s[pj] == 'U')) pj++;
        if (pj < n && (s[pj] == '"' || s[pj] == '\'') && (pj == i || !idchar(i > 0 ? s[i - 1] : ' '))) {
            char q = s[pj];
            if (pj + 2 < n && s[pj + 1] == q && s[pj + 2] == q) {             /* triple quote */
                int e = find_triple_end(s, n, pj + 3, q);
                if (e < 0) { EMIT(ps, n - ps, K_STR); *state = q == '\'' ? 1 : 2; return nt; }
                EMIT(ps, e - ps, K_STR); i = e; continue;
            }
            int j = pj + 1;
            while (j < n && s[j] != q) { if (s[j] == '\\') j++; j++; }
            if (j < n) j++;
            if (j > n) j = n;
            EMIT(ps, j - ps, K_STR); i = j; continue;
        }
        if (c >= '0' && c <= '9') {
            int j = i + 1;
            if (c == '0' && j < n && (s[j] == 'x' || s[j] == 'X' || s[j] == 'b' || s[j] == 'B' || s[j] == 'o' || s[j] == 'O')) { j++; while (j < n && (idchar(s[j]))) j++; }
            else { while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == '_' || s[j] == '.' || s[j] == 'e' || s[j] == 'E' || ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E')) || s[j] == 'j')) j++; }
            EMIT(i, j - i, K_NUM); i = j; continue;
        }
        if (idstart(c)) {
            int j = i + 1; while (j < n && idchar(s[j])) j++;
            int len = j - i; int kind = K_TEXT;
            if (prev_def == 1) kind = K_FN; else if (prev_def == 2) kind = K_TYPE;
            else if (in_list(PY_KW, s + i, len)) kind = K_KW;
            else if (in_list(PY_BUILTIN, s + i, len)) kind = K_BUILTIN;
            else { int k = j; while (k < n && s[k] == ' ') k++; if (k < n && s[k] == '(') kind = K_FN; }
            prev_def = (len == 3 && !strncmp(s + i, "def", 3)) ? 1 : (len == 5 && !strncmp(s + i, "class", 5)) ? 2 : 0;
            EMIT(i, len, kind); i = j; continue;
        }
        EMIT(i, 1, K_PUNCT); i++;
    }
    return nt;
}

static int hl_c(const char *s, int n, int *state, Tok *out, int max) {
    int nt = 0, i = 0;
    if (*state == 3) {
        int j = 0; while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
        if (j + 1 < n) { EMIT(0, j + 2, K_CMT); i = j + 2; *state = 0; } else { EMIT(0, n, K_CMT); return nt; }
    }
    int k0 = 0; while (k0 < n && (s[k0] == ' ' || s[k0] == '\t')) k0++;
    if (i == 0 && k0 < n && s[k0] == '#') {
        int j = k0 + 1; while (j < n && (s[j] == ' ' || idchar(s[j]))) j++;
        EMIT(k0, j - k0, K_DECO); i = j;
        if (i < n && s[i] == '<') { int e = i; while (e < n && s[e] != '>') e++; if (e < n) e++; EMIT(i, e - i, K_STR); i = e; }
    }
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { EMIT(i, n - i, K_CMT); break; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            int j = i + 2; while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
            if (j + 1 < n) { EMIT(i, j + 2 - i, K_CMT); i = j + 2; continue; }
            EMIT(i, n - i, K_CMT); *state = 3; return nt;
        }
        if (c == '"' || c == '\'') { int j = i + 1; while (j < n && s[j] != c) { if (s[j] == '\\') j++; j++; } if (j < n) j++; if (j > n) j = n; EMIT(i, j - i, K_STR); i = j; continue; }
        if (c >= '0' && c <= '9') { int j = i + 1; while (j < n && (idchar(s[j]) || s[j] == '.')) j++; EMIT(i, j - i, K_NUM); i = j; continue; }
        if (idstart(c)) {
            int j = i + 1; while (j < n && idchar(s[j])) j++;
            int len = j - i, kind = K_TEXT;
            if (in_list(C_KW, s + i, len)) kind = K_KW; else if (in_list(C_TYPE, s + i, len)) kind = K_TYPE;
            else { int k = j; while (k < n && s[k] == ' ') k++; if (k < n && s[k] == '(') kind = K_FN; }
            EMIT(i, len, kind); i = j; continue;
        }
        EMIT(i, 1, K_PUNCT); i++;
    }
    return nt;
}

int hl_line(int lang, const char *s, int n, int *state, Tok *out, int max) {
    if (lang == L_PY) return hl_py(s, n, state, out, max);
    if (lang == L_C) return hl_c(s, n, state, out, max);
    return 0;
}

/* per-line start states, rebuilt in one pass when the document changed */
void hl_prepare(Doc *d) {
    if (d->lang == L_TEXT) return;
    if (d->lst_ver == d->ver && d->lst) return;
    size_t nl = doc_lines(d);
    if (nl > d->lst_cap) { size_t nc = nl + 256; uint8_t *nb = (uint8_t *)realloc(d->lst, nc); if (!nb) return; d->lst = nb; d->lst_cap = nc; }
    int st = 0; Tok toks[256];
    for (size_t l = 0; l < nl; ++l) {
        d->lst[l] = (uint8_t)st;
        size_t a = doc_line_start(d, l), e = doc_line_end(d, l);
        hl_line(d->lang, d->t + a, (int)(e - a), &st, toks, 256);
    }
    d->lst_ver = d->ver;
}
