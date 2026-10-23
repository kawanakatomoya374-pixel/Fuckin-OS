#include "mock_term.h"
#include "../../userland/plugins/terminal/calc.c"
void run_calc_test(void) {
    t_setup(); cos_plugin_host_t h = { 1, sizeof h, "terminal", &t_api }; CHECK(init(&h) == 0, "calc init");
    t_call("calc (2+3)*4"); CHECK(strstr(g_out, "= 20"), "calc 20: %s", g_out); g_out[0] = 0;
    t_call("calc 2^10"); CHECK(strstr(g_out, "= 1024"), "pow: %s", g_out); g_out[0] = 0;
    t_call("calc 0x10+1"); CHECK(strstr(g_out, "= 17"), "hex: %s", g_out); g_out[0] = 0;
    t_call("calc 7/2"); CHECK(strstr(g_out, "= 3.500000"), "frac: %s", g_out); g_out[0] = 0;
    t_call("calc -3*-3"); CHECK(strstr(g_out, "= 9"), "neg: %s", g_out); g_out[0] = 0;
    CHECK(t_call("calc 1/0") != 0 && strstr(g_out, "division by zero"), "div0: %s", g_out); g_out[0] = 0;
    CHECK(t_call("calc 2+") != 0, "syntax error");
}
