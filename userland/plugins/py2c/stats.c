/* stats.c - plugin of Py2C Studio: "Stats" button: size of the Python source and the generated C, number of functions. */
#include "cos_plugin.h"
#include "cos_plugin_py2c.h"
#include "cos_plugin_util.h"
static const py2c_api_t *A;
static int count_lines(const char *s) { int n = 0; for (; *s; s++) if (*s == '\n') n++; return n; }
static int count_defs(const char *s) { int n = 0; for (const char *p = s; (p = pu_find(p, "def ", 0)); p += 4) if (p == s || p[-1] == '\n' || p[-1] == ' ') n++; return n; }
static void button(void) {
    char m[160];
    A->base.snprintf(m, sizeof m, "Python: %d lines, %d def; C: %d lines, %lu bytes", count_lines(A->source()), count_defs(A->source()), count_lines(A->output()), (unsigned long)pu_len(A->output()));
    A->base.status(m);
}
static int init(const cos_plugin_host_t *h) {
    A = (const py2c_api_t *)h->api;
    if (!A || A->abi != PY2CP_ABI) return -1;
    return A->add_button("Stats", button);
}
static void fini(void) { A = 0; }
COS_PLUGIN("py2c", "stats", "1.0", init, fini);
