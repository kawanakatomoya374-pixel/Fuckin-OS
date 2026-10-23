/* docstats.c - plugin of C-OS Studio: "Document statistics" (lines, words, bytes, longest line, functions, comment share) in the Output panel. */
#include "cos_plugin.h"
#include "cos_plugin_studio.h"
#include "cos_plugin_util.h"
static const studio_api_t *A;
static char buf[131072];
static void stats(void) {
    if (!A->has_doc()) { A->base.status("Open a file first"); return; }
    size_t n = A->doc_read(0, buf, sizeof buf - 1); buf[n] = 0;
    long lines = 0, words = 0, longest = 0, cur = 0, blank = 0, comment = 0, funcs = 0, depth = 0; int in = 0, in_block = 0, only_ws = 1;
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') { lines++; if (cur > longest) longest = cur; if (only_ws) blank++; cur = 0; only_ws = 1; in = 0; continue; }
        cur++; if (c != ' ' && c != '\t' && c != '\r') only_ws = 0;
        if (c == ' ' || c == '\t') in = 0; else if (!in) { in = 1; words++; }
        if (!in_block && c == '/' && buf[i + 1] == '/') { comment++; while (i < n && buf[i] != '\n') i++; i--; continue; }
        if (!in_block && c == '/' && buf[i + 1] == '*') { in_block = 1; comment++; }
        if (in_block && c == '*' && buf[i + 1] == '/') in_block = 0;
        if (!in_block) { if (c == '{') { if (depth == 0) funcs++; depth++; } else if (c == '}' && depth > 0) depth--; }
    }
    if (n && buf[n - 1] != '\n') lines++;
    char m[200];
    A->base.snprintf(m, sizeof m, "%s: %ld lines (%ld blank), %ld words, %lu bytes", A->doc_path()[0] ? A->doc_path() : "untitled", lines, blank, words, (unsigned long)n); A->output(m);
    A->base.snprintf(m, sizeof m, "  longest line %ld chars, %ld top-level blocks, %ld comment lines/blocks", longest, funcs, comment); A->output(m);
    A->base.snprintf(m, sizeof m, "%ld lines, %ld words, %lu bytes", lines, words, (unsigned long)n); A->base.status(m);
}
static int init(const cos_plugin_host_t *h) {
    A = (const studio_api_t *)h->api;
    if (!A || A->abi != STUDIOP_ABI) return -1;
    return A->add_command("Document statistics", stats);
}
static void fini(void) { A = 0; }
COS_PLUGIN("studio", "docstats", "1.0", init, fini);
