/* highlight.c - a small C tokeniser for syntax colouring.
 *
 * It works one line at a time. The only state that crosses a line boundary
 * is "we are inside a block comment", which the caller caches per line
 * (Doc.hs) so a keystroke re-tokenises only the visible lines. */
#include "studio.h"

static const char *const KEYWORDS[] = {
    "if", "else", "while", "for", "do", "switch", "case", "default", "break", "continue",
    "return", "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "extern",
    "const", "volatile", "register", "inline", "auto", "restrict",
    "void", "int", "char", "short", "long", "float", "double", "signed", "unsigned", "_Bool",
    "true", "false", "NULL", "_Noreturn", "_Alignas", "_Alignof", "_Static_assert", NULL
};
static const char *const TYPES[] = { "FILE", "va_list", "jmp_buf", "DIR", "bool", NULL };

static bool is_id0(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_id(int c)  { return is_id0(c) || (c >= '0' && c <= '9'); }

static bool in_list(const char *const *list, const char *s, int n) {
    for (int i = 0; list[i]; ++i)
        if ((int)strlen(list[i]) == n && !memcmp(list[i], s, (size_t)n)) return true;
    return false;
}

static void emit(Tok *out, int *cnt, int max, int start, int len, int kind) {
    if (len <= 0 || *cnt >= max) return;
    out[(*cnt)++] = (Tok){ start, len, kind };
}

int hl_line(const char *s, int n, bool *in_comment, Tok *out, int max) {
    int cnt = 0, i = 0;

    if (*in_comment) {                          /* continue a block comment */
        int j = 0;
        while (j < n && !(s[j] == '*' && j + 1 < n && s[j + 1] == '/')) ++j;
        if (j < n) { j += 2; *in_comment = false; }
        emit(out, &cnt, max, 0, j, TK_CMT);
        i = j;
        if (*in_comment) return cnt;
    }

    /* preprocessor line: '#' as the first non-blank character */
    int k = i;
    while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
    bool pp = (k < n && s[k] == '#' && i == 0) || (k < n && s[k] == '#' && cnt == 0);
    if (pp) {
        int j = k;
        while (j < n) {
            if (s[j] == '/' && j + 1 < n && s[j + 1] == '/') break;
            if (s[j] == '/' && j + 1 < n && s[j + 1] == '*') break;
            ++j;
        }
        emit(out, &cnt, max, k, j - k, TK_PP);
        i = j;
    }

    int first_ident = -1;                       /* token index of the first identifier on the line */
    while (i < n) {
        int c = (unsigned char)s[i];
        if (c == ' ' || c == '\t') { ++i; continue; }

        if (c == '/' && i + 1 < n && s[i + 1] == '/') {          /* line comment */
            emit(out, &cnt, max, i, n - i, TK_CMT);
            break;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {          /* block comment */
            int j = i + 2;
            while (j < n && !(s[j] == '*' && j + 1 < n && s[j + 1] == '/')) ++j;
            if (j < n) j += 2; else *in_comment = true;
            emit(out, &cnt, max, i, j - i, TK_CMT);
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {                              /* string / char literal */
            int j = i + 1;
            while (j < n && s[j] != c) { if (s[j] == '\\' && j + 1 < n) ++j; ++j; }
            if (j < n) ++j;
            emit(out, &cnt, max, i, j - i, TK_STR);
            i = j;
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9')) {
            int j = i + 1;
            while (j < n && (is_id(s[j]) || s[j] == '.' ||
                   ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E') && !(s[i] == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')))))
                ++j;
            emit(out, &cnt, max, i, j - i, TK_NUM);
            i = j;
            continue;
        }
        if (is_id0(c)) {
            int j = i + 1;
            while (j < n && is_id(s[j])) ++j;
            int len = j - i, kind = TK_TEXT;
            int q = j;
            while (q < n && (s[q] == ' ' || s[q] == '\t')) ++q;
            if (in_list(KEYWORDS, s + i, len)) kind = TK_KW;
            else if (in_list(TYPES, s + i, len)) kind = TK_TYPE;
            else if (len > 2 && s[j - 2] == '_' && s[j - 1] == 't') kind = TK_TYPE;
            else if (q < n && s[q] == '(') kind = TK_FN;
            else {
                /* declaration heuristic: "name ident;" / "name *ident =" / "name ident," ... */
                int r = q;
                while (r < n && s[r] == '*') ++r;
                if (r > j && r < n && is_id0(s[r]) && (cnt == 0 || out[cnt - 1].kind == TK_KW || first_ident < 0)) {
                    int e = r + 1;
                    while (e < n && is_id(s[e])) ++e;
                    while (e < n && (s[e] == ' ' || s[e] == '\t')) ++e;
                    if (e >= n || s[e] == ';' || s[e] == ',' || s[e] == '=' || s[e] == '[' || s[e] == ')') kind = TK_TYPE;
                } else if (q > j && q < n && is_id0(s[q]) && first_ident < 0) {
                    int e = q + 1;
                    while (e < n && is_id(s[e])) ++e;
                    while (e < n && (s[e] == ' ' || s[e] == '\t')) ++e;
                    if (e >= n || s[e] == ';' || s[e] == ',' || s[e] == '=' || s[e] == '[' || s[e] == ')') kind = TK_TYPE;
                }
            }
            /* the identifier after struct/union/enum names a type */
            if (kind == TK_TEXT && cnt > 0 && out[cnt - 1].kind == TK_KW) {
                const Tok *pt = &out[cnt - 1];
                if ((pt->len == 6 && !memcmp(s + pt->start, "struct", 6)) ||
                    (pt->len == 5 && !memcmp(s + pt->start, "union", 5)) ||
                    (pt->len == 4 && !memcmp(s + pt->start, "enum", 4))) kind = TK_TYPE;
            }
            if (first_ident < 0) first_ident = cnt;
            emit(out, &cnt, max, i, len, kind);
            i = j;
            continue;
        }
        ++i;                                                     /* punctuation: default colour */
    }
    return cnt;
}

/* ---- per-document block-comment state ------------------------------- */
void doc_hs_sync(Doc *d, size_t upto_line) {
    size_t nl = buf_lines(&d->b);
    if (upto_line >= nl) upto_line = nl - 1;
    if (d->hs_ver != d->b.version) {            /* changed without doc_touch(): recompute from scratch */
        d->hs_valid = 0;
        d->hs_ver = d->b.version;
    }
    if (d->hs_n < nl + 2) {
        size_t nn = nl + 64;
        uint8_t *p = (uint8_t *)realloc(d->hs, nn);
        if (!p) return;
        d->hs = p; d->hs_n = nn;
    }
    if (d->hs_valid == 0) { d->hs[0] = 0; d->hs_valid = 1; }
    const char *t = buf_text(&d->b);
    Tok scratch[256];
    while (d->hs_valid <= upto_line) {
        size_t l = d->hs_valid - 1;
        size_t a = buf_line_start(&d->b, l), e = buf_line_end(&d->b, l);
        bool ic = d->hs[l] != 0;
        hl_line(t + a, (int)(e - a), &ic, scratch, 256);
        d->hs[l + 1] = ic ? 1 : 0;
        d->hs_valid++;
    }
}
bool doc_line_in_comment(Doc *d, size_t line) {
    doc_hs_sync(d, line);
    return d->hs && d->hs[line] != 0;
}
