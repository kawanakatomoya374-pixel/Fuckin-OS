/* dl_b.c - depends on libdla (DT_NEEDED): /lib/dlb.c-osll. Its constructor reads what libdla's constructor wrote, so the order of construction is visible. */
#include <stdint.h>
extern int dla_add(int, int);
extern int dla_value(void);
static volatile int *g_flag;
static int saw_dla_value;
__attribute__((constructor)) static void b_init(void) { saw_dla_value = dla_value(); }
__attribute__((destructor)) static void b_fini(void) { if (g_flag) *g_flag = 2; }
void dlb_set_flag(volatile int *p) { g_flag = p; }
int dlb_saw(void) { return saw_dla_value; }
int dlb_twice(int x) { return dla_add(x, x); }
