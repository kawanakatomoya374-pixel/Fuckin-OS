/* updtest.c - what the app-update test installs (tools/run_update_test.sh): built twice with -DVER=2 and -DVER=3 */
#include <stdio.h>
#ifndef VER
#define VER 1
#endif
int main(void) { printf("updtest version %d\n", VER); return 0; }
