#include "mock.h"
void run_studio_tests(void); void run_cformat_test(void); void run_calc_test(void); void run_textutils_test(void); void run_alias_test(void); void run_tidy_test(void);
int main(void) {
    run_studio_tests(); run_cformat_test(); run_calc_test(); run_textutils_test(); run_alias_test(); run_tidy_test();
    printf("plugin unit tests: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
