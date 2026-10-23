#include "mock_term.h"
#include "../../userland/plugins/terminal/textutils.c"
void run_textutils_test(void) {
    t_setup(); cos_plugin_host_t h = { 1, sizeof h, "terminal", &t_api }; CHECK(init(&h) == 0, "textutils init");
    mock_put("f.txt", "alpha\nBeta line\ngamma\nbeta again\n");
    t_call("grep -n beta f.txt"); CHECK(strstr(g_out, "f.txt:4: beta again") && !strstr(g_out, "Beta"), "grep: %s", g_out); g_out[0] = 0;
    t_call("grep -i beta f.txt"); CHECK(strstr(g_out, "Beta line") && strstr(g_out, "beta again"), "grep -i: %s", g_out); g_out[0] = 0;
    t_call("head -n 2 f.txt"); CHECK(!strcmp(g_out, "alpha\nBeta line\n"), "head: %s", g_out); g_out[0] = 0;
    t_call("tail -n 1 f.txt"); CHECK(!strcmp(g_out, "beta again\n"), "tail: %s", g_out); g_out[0] = 0;
    t_call("wc f.txt"); CHECK(strstr(g_out, "4") && strstr(g_out, "6") && strstr(g_out, "33"), "wc: %s", g_out); g_out[0] = 0;
    CHECK(t_call("grep x nofile") == 0 && strstr(g_out, "cannot read"), "missing file: %s", g_out);
}
