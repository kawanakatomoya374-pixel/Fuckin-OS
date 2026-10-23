/* A module that tries to call a kernel function the export table does not offer (an undefined symbol): the loader must refuse it. */
#include "cosys.h"
#pragma GCC visibility push(default)
extern void serial_puts(const char *);
#pragma GCC visibility pop
static int bad_init(void) { serial_puts("direct kernel call"); return 0; }
static void bad_exit(void) { }
COSYS_MODULE("needs_kernel", "1.0", COSYS_LEVEL_SERVICE, bad_init, bad_exit);
