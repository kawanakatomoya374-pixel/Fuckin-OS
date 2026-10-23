/* dl_use.c - /bin/dluse.c-os: linked against libdla; returns dla_add(2, 3) as its exit code. 5 normally, 1005 (mod 256 = 237) with the preload library. */
#include "cos.h"
extern int dla_add(int, int);
int main(void) { return dla_add(2, 3) & 0xFF; }
