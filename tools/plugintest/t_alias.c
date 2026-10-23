#include "mock_term.h"
#include "../../userland/plugins/terminal/alias.c"
void run_alias_test(void) {
    t_setup(); cos_plugin_host_t h = { 1, sizeof h, "terminal", &t_api }; CHECK(init(&h) == 0, "alias init");
    t_call("alias hi=calc 6*7"); CHECK(strstr(g_out, "alias hi = calc 6*7"), "alias set: %s", g_out);
    char out[200]; CHECK(t_rw[0]("hi now", out, sizeof out) == 1 && !strcmp(out, "calc 6*7 now"), "rewrite: %s", out);
    CHECK(t_rw[0]("ls /", out, sizeof out) == 0, "other lines untouched");
    t_call("unalias hi"); CHECK(t_rw[0]("hi", out, sizeof out) == 0, "unalias");
}
