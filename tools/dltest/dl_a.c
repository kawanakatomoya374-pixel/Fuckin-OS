/* dl_a.c - a library with a constructor and a destructor: libdla (/lib/dla.c-osll). */
#include <stdint.h>
static int ctor_runs, dtor_runs;
static volatile int *g_flag;                    /* set by the program: the destructor writes 1 here, so the program can see it ran */
int dla_value_store = 7;
__attribute__((constructor)) static void a_init(void) { ctor_runs++; dla_value_store = 42; }
__attribute__((destructor)) static void a_fini(void) { dtor_runs++; if (g_flag) *g_flag = 1; }
void dla_set_flag(volatile int *p) { g_flag = p; }
int dla_ctor_runs(void) { return ctor_runs; }
int dla_value(void) { return dla_value_store; }
int dla_add(int a, int b) { return a + b; }
