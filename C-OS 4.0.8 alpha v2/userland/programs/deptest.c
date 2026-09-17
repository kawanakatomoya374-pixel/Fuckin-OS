/* deptest.c - proves DT_NEEDED automatic dependency loading.
 *
 * Opens ONLY libdepend2.c-osll. That library declares DT_NEEDED on
 * libbase.c-osll and calls into it, but this program never opens
 * libbase itself - the loader must find and load it automatically, or
 * the relocation for cos_base_value cannot resolve and the dlopen fails.
 * Before automatic dependency loading existed, this exact case was
 * rejected at load time and a program had to open every transitive
 * dependency itself, in the right order.
 */
#include "cos.h"

int main(void) {
    int64_t h = cos_dlopen("/libdepend2.c-osll");
    if (h <= 0) {
        cos_printf("DEP_FAIL dlopen(/libdepend2.c-osll) failed - "
                   "DT_NEEDED dependency was not auto-loaded\n");
        return 1;
    }

    typedef long (*wrapper_fn)(void);
    wrapper_fn f = (wrapper_fn)cos_dlsym(h, "cos_depend_wrapper");
    if (!f) {
        cos_printf("DEP_FAIL dlsym(cos_depend_wrapper) returned NULL\n");
        return 1;
    }

    /* cos_depend_wrapper() calls cos_base_value() (=555) and adds 1.
     * Checking the VALUE, not just that the call returned, is what proves
     * the cross-library relocation actually resolved to the real function
     * rather than to something that merely did not crash. */
    long v = f();
    if (v != 556) {
        cos_printf("DEP_FAIL cross-library call returned %ld, expected 556\n", v);
        return 1;
    }

    /* Opening the same library again must return the SAME handle, not a
     * second copy at a different base address - two copies would have
     * separate, silently divergent global state. */
    int64_t h2 = cos_dlopen("/libdepend2.c-osll");
    if (h2 != h) {
        cos_printf("DEP_FAIL reopening gave handle %ld, expected %ld (dedup failed)\n",
                   (long)h2, (long)h);
        return 1;
    }

    cos_printf("DEP_PASS DT_NEEDED auto-loaded the dependency, "
               "cross-library call returned 556, reopen deduped\n");
    return 0;
}
