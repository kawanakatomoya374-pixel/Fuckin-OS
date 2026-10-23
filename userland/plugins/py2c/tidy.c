/* tidy.c - plugin of Py2C Studio: a source filter that runs before every conversion: tabs become four spaces, trailing spaces go, the file ends with one newline.
 * The "Tidy" button does the same to the text in the editor. */
#include "cos_plugin.h"
#include "cos_plugin_py2c.h"
#include "cos_plugin_util.h"
static const py2c_api_t *A;
static char tmp[65536 + 4096];
static int tidy(char *py, size_t cap) {
    size_t n = pu_len(py), o = 0, ls = 0;
    if (n + 1 >= sizeof tmp / 2) return 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || py[i] == '\n') {
            size_t e = i; while (e > ls && (py[e - 1] == ' ' || py[e - 1] == '\t' || py[e - 1] == '\r')) e--;
            for (size_t k = ls; k < e; k++) { if (py[k] == '\t') { for (int s = 0; s < 4; s++) tmp[o++] = ' '; } else tmp[o++] = py[k]; }
            if (i < n) tmp[o++] = '\n';
            ls = i + 1;
        }
    }
    while (o > 0 && tmp[o - 1] == '\n') o--;
    tmp[o++] = '\n';
    if (o >= cap) return 0;
    if (o == n && !pu_ncmp(py, tmp, n)) return 0;
    pu_cpy(py, tmp, o); py[o] = 0; return 1;
}
static void button(void) {
    char *copy = tmp + 0; (void)copy;
    static char work[65536]; size_t n = A->source_len(); if (n >= sizeof work) return;
    pu_cpy(work, A->source(), n + 1);
    if (tidy(work, sizeof work)) { A->set_source(work, pu_len(work)); A->base.status("Source tidied"); } else A->base.status("Nothing to tidy");
}
static int init(const cos_plugin_host_t *h) {
    A = (const py2c_api_t *)h->api;
    if (!A || A->abi != PY2CP_ABI) return -1;
    if (A->add_source_filter(tidy)) return -1;
    return A->add_button("Tidy", button);
}
static void fini(void) { A = 0; }
COS_PLUGIN("py2c", "tidy", "1.0", init, fini);
