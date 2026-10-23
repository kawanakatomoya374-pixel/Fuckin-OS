/* kbase.c - a library module: it only exports a function for other modules to import (DT_NEEDED). */
#include "cosys.h"
COSYS_EXPORT int kbase_add(int a, int b) { return a + b; }
static int kbase_init(void) { cosys_log("kbase ready"); return 0; }
static void kbase_exit(void) { }
COSYS_MODULE("kbase", "1.0", COSYS_LEVEL_BASIC, kbase_init, kbase_exit);
