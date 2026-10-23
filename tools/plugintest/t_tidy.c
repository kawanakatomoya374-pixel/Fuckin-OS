#include "mock.h"
#include "cos_plugin_py2c.h"
static py2c_api_t api;
#include "../../userland/plugins/py2c/tidy.c"
void run_tidy_test(void) {
    char t[200] = "def f():\n\treturn 1   \n\n\n"; 
    CHECK(tidy(t, sizeof t) == 1, "changed"); CHECK(!strcmp(t, "def f():\n    return 1\n"), "tidy: [%s]", t);
    CHECK(tidy(t, sizeof t) == 0, "already tidy");
}
