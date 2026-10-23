/* snippets.c - plugin of Py2C Studio: toolbar buttons that insert Python snippets at the cursor ("def", "class", "loop"). */
#include "cos_plugin.h"
#include "cos_plugin_py2c.h"
static const py2c_api_t *A;
static void snip_def(void) { A->insert("def name(a, b):\n    return a + b\n"); }
static void snip_class(void) { A->insert("class Point:\n    def __init__(self, x, y):\n        self.x = x\n        self.y = y\n"); }
static void snip_loop(void) { A->insert("for i in range(10):\n    print(i)\n"); }
static int init(const cos_plugin_host_t *h) {
    A = (const py2c_api_t *)h->api;
    if (!A || A->abi != PY2CP_ABI) return -1;
    if (A->add_button("+def", snip_def)) return -1;
    A->add_button("+class", snip_class); A->add_button("+loop", snip_loop);
    return 0;
}
static void fini(void) { A = 0; }
COS_PLUGIN("py2c", "snippets", "1.0", init, fini);
