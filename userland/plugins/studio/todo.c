/* todo.c - plugin of C-OS Studio: "List TODO / FIXME" writes every such comment of the open file to the Output panel as path:line: text. */
#include "cos_plugin.h"
#include "cos_plugin_studio.h"
#include "cos_plugin_util.h"
static const studio_api_t *A;
static char buf[131072];
static void list(void) {
    if (!A->has_doc()) { A->base.status("Open a file first"); return; }
    size_t n = A->doc_read(0, buf, sizeof buf - 1); buf[n] = 0;
    const char *path = A->doc_path(); int hits = 0, ln = 1;
    for (char *l = buf; *l; ln++) {
        char *e = l; while (*e && *e != '\n') e++;
        char s = *e; *e = 0;
        const char *k = pu_find(l, "TODO", 0); if (!k) k = pu_find(l, "FIXME", 0); if (!k) k = pu_find(l, "XXX", 0);
        if (k) { while (*l == ' ' || *l == '\t') l++; char m[300]; A->base.snprintf(m, sizeof m, "%s:%d: note: %s", path[0] ? path : "untitled", ln, l); A->output(m); hits++; }
        *e = s; l = *e ? e + 1 : e;
    }
    char m[80]; A->base.snprintf(m, sizeof m, hits ? "%d TODO/FIXME found" : "No TODO/FIXME found", hits); A->output(m); A->base.status(m);
}
static int init(const cos_plugin_host_t *h) {
    A = (const studio_api_t *)h->api;
    if (!A || A->abi != STUDIOP_ABI) return -1;
    return A->add_command("List TODO / FIXME", list);
}
static void fini(void) { A = 0; }
COS_PLUGIN("studio", "todo", "1.0", init, fini);
