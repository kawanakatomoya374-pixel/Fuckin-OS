/* texttools.c - plugin of C-OS Studio: Tools menu commands that change the selection (or the whole file when nothing is selected):
 * trim trailing spaces, tabs to spaces, sort lines, remove duplicate lines, reverse lines, UPPERCASE, lowercase. One undo step each. */
#include "cos_plugin.h"
#include "cos_plugin_studio.h"
#include "cos_plugin_util.h"
static const studio_api_t *A;
#define CAP 131072
static char in[CAP], out[CAP + 4096];

/* the range to work on: the selection, widened to whole lines for line commands; else the whole file */
static int range(size_t *from, size_t *to, int whole_lines) {
    if (!A->has_doc()) { A->base.status("Open a file first"); return 0; }
    size_t a = A->sel_start(), b = A->sel_end(), len = A->doc_length();
    if (a == b) { a = 0; b = len; }
    else if (whole_lines) {
        size_t la = A->line_of(a), lb = A->line_of(b > a && b > 0 ? b - 1 : b);
        a = A->line_start(la);
        b = lb + 1 < A->line_count() ? A->line_start(lb + 1) : len;
    }
    if (b - a >= CAP) { A->base.status("Too large for this command (128 KiB)"); return 0; }
    *from = a; *to = b; return 1;
}
static void apply(size_t from, size_t to, size_t n, const char *msg) {
    if (n == to - from && !pu_ncmp(in, out, n)) { A->base.status("Nothing to change"); return; }
    A->doc_replace(from, to - from, out, n); A->set_selection(from, from + n); A->base.status(msg);
}
static size_t load(size_t from, size_t to) { size_t n = A->doc_read(from, in, to - from); in[n] = 0; return n; }

static void trim(void) {
    size_t f, t; if (!range(&f, &t, 0)) return; size_t n = load(f, t), o = 0, ls = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || in[i] == '\n') {
            size_t e = i; while (e > ls && (in[e - 1] == ' ' || in[e - 1] == '\t' || in[e - 1] == '\r')) e--;
            pu_cpy(out + o, in + ls, e - ls); o += e - ls; if (i < n) out[o++] = '\n'; ls = i + 1;
        }
    }
    apply(f, t, o, "Trailing spaces removed");
}
static void untab(void) {
    size_t f, t; if (!range(&f, &t, 0)) return; size_t n = load(f, t), o = 0; int col = 0;
    for (size_t i = 0; i < n && o + 8 < sizeof out; i++) {
        if (in[i] == '\t') { int k = 4 - col % 4; while (k--) { out[o++] = ' '; col++; } }
        else { out[o++] = in[i]; col = in[i] == '\n' ? 0 : col + 1; }
    }
    apply(f, t, o, "Tabs replaced by spaces");
}
/* line commands work on an index of lines inside `in` */
static char *ln[4096]; static size_t ll[4096]; static int nl; static int trailing_nl;
static int split(size_t n) {
    nl = 0; trailing_nl = n && in[n - 1] == '\n';
    for (size_t i = 0; i < n; ) {
        size_t e = i; while (e < n && in[e] != '\n') e++;
        if (nl >= 4096) { A->base.status("Too many lines for this command"); return 0; }
        ln[nl] = in + i; ll[nl] = e - i; nl++; i = e + 1;
    }
    return 1;
}
static size_t join(int *order, int cnt) {
    size_t o = 0;
    for (int k = 0; k < cnt; k++) { int i = order[k]; pu_cpy(out + o, ln[i], ll[i]); o += ll[i]; if (k + 1 < cnt || trailing_nl) out[o++] = '\n'; }
    return o;
}
static int cmp_lines(int a, int b) { size_t m = ll[a] < ll[b] ? ll[a] : ll[b]; int c = pu_ncmp(ln[a], ln[b], m); return c ? c : (ll[a] > ll[b]) - (ll[a] < ll[b]); }
static void sort_lines(void) {
    size_t f, t; if (!range(&f, &t, 1)) return; size_t n = load(f, t); if (!split(n)) return;
    static int ord[4096]; for (int i = 0; i < nl; i++) ord[i] = i;
    for (int i = 1; i < nl; i++) { int v = ord[i], j = i - 1; while (j >= 0 && cmp_lines(ord[j], v) > 0) { ord[j + 1] = ord[j]; j--; } ord[j + 1] = v; }
    apply(f, t, join(ord, nl), "Lines sorted");
}
static void dedupe(void) {
    size_t f, t; if (!range(&f, &t, 1)) return; size_t n = load(f, t); if (!split(n)) return;
    static int ord[4096]; int c = 0;
    for (int i = 0; i < nl; i++) { int seen = 0; for (int k = 0; k < c && !seen; k++) if (ll[ord[k]] == ll[i] && !pu_ncmp(ln[ord[k]], ln[i], ll[i])) seen = 1; if (!seen) ord[c++] = i; }
    apply(f, t, join(ord, c), "Duplicate lines removed");
}
static void reverse(void) {
    size_t f, t; if (!range(&f, &t, 1)) return; size_t n = load(f, t); if (!split(n)) return;
    static int ord[4096]; for (int i = 0; i < nl; i++) ord[i] = nl - 1 - i;
    apply(f, t, join(ord, nl), "Lines reversed");
}
static void upper(void) { size_t f, t; if (!range(&f, &t, 0)) return; size_t n = load(f, t); for (size_t i = 0; i < n; i++) out[i] = pu_upper(in[i]); apply(f, t, n, "UPPERCASE"); }
static void lower(void) { size_t f, t; if (!range(&f, &t, 0)) return; size_t n = load(f, t); for (size_t i = 0; i < n; i++) out[i] = pu_lower(in[i]); apply(f, t, n, "lowercase"); }

static int init(const cos_plugin_host_t *h) {
    A = (const studio_api_t *)h->api;
    if (!A || A->abi != STUDIOP_ABI) return -1;
    if (A->add_command("Text: Trim trailing spaces", trim)) return -1;
    A->add_command("Text: Tabs to spaces", untab);
    A->add_command("Text: Sort lines", sort_lines);
    A->add_command("Text: Remove duplicate lines", dedupe);
    A->add_command("Text: Reverse lines", reverse);
    A->add_command("Text: UPPERCASE", upper);
    A->add_command("Text: lowercase", lower);
    return 0;
}
static void fini(void) { A = 0; }
COS_PLUGIN("studio", "texttools", "1.0", init, fini);
