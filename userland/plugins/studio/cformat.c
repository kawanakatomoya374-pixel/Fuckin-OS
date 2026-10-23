/* cformat.c - plugin of C-OS Studio: "Format C (4 spaces)" re-indents the file by its braces and parentheses (strings, character constants and comments are respected). */
#include "cos_plugin.h"
#include "cos_plugin_studio.h"
#include "cos_plugin_util.h"
static const studio_api_t *A;
static char in[131072], out[131072 + 65536];
static void format(void) {
    if (!A->has_doc()) { A->base.status("Open a file first"); return; }
    size_t n = A->doc_read(0, in, sizeof in - 1); in[n] = 0;
    if (n >= sizeof in - 1) { A->base.status("File too large to format (128 KiB)"); return; }
    size_t o = 0; int depth = 0, paren = 0, block = 0;
    for (size_t ls = 0; ls <= n && o + 200 < sizeof out; ) {
        size_t e = ls; while (e < n && in[e] != '\n') e++;
        size_t s = ls; while (s < e && (in[s] == ' ' || in[s] == '\t')) s++;
        size_t te = e; while (te > s && (in[te - 1] == ' ' || in[te - 1] == '\t' || in[te - 1] == '\r')) te--;
        int lead = depth, pp = in[s] == '#';
        if (!block && s < te && in[s] == '}') lead--;
        if (lead < 0) lead = 0;
        if (s < te) {
            int ind = pp ? 0 : lead * 4 + (paren > 0 ? 4 : 0);
            if (block) { for (size_t i = ls; i < te; i++) out[o++] = in[i]; }           /* inside a block comment: untouched */
            else { for (int i = 0; i < ind; i++) out[o++] = ' '; for (size_t i = s; i < te; i++) out[o++] = in[i]; }
        }
        /* update the state from the line */
        int str = 0, chr = 0;
        for (size_t i = s; i < te; i++) {
            char c = in[i];
            if (block) { if (c == '*' && in[i + 1] == '/') { block = 0; i++; } continue; }
            if (str) { if (c == '\\') i++; else if (c == '"') str = 0; continue; }
            if (chr) { if (c == '\\') i++; else if (c == '\'') chr = 0; continue; }
            if (c == '/' && in[i + 1] == '/') break;
            if (c == '/' && in[i + 1] == '*') { block = 1; i++; continue; }
            if (c == '"') str = 1; else if (c == '\'') chr = 1;
            else if (c == '{') depth++; else if (c == '}') { if (depth > 0) depth--; }
            else if (c == '(') paren++; else if (c == ')') { if (paren > 0) paren--; }
        }
        if (e < n) out[o++] = '\n';
        ls = e + 1; if (e >= n) break;
    }
    if (o == n && !pu_ncmp(in, out, n)) { A->base.status("Already formatted"); return; }
    A->doc_replace(0, n, out, o); A->base.status("Formatted (4 spaces)");
}
static int init(const cos_plugin_host_t *h) {
    A = (const studio_api_t *)h->api;
    if (!A || A->abi != STUDIOP_ABI) return -1;
    return A->add_command("Format C (4 spaces)", format);
}
static void fini(void) { A = 0; }
COS_PLUGIN("studio", "cformat", "1.0", init, fini);
