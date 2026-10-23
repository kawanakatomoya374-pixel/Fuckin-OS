/* ssp_test.c - overflows a local buffer on purpose: the stack protector (canary) must stop the program before the corrupted return address is used.
 * Built by tools/run_terminal_test.sh with tools/cos-cc (programs get -fstack-protector-strong) and started from the terminal. */
#include "cos.h"
__attribute__((noinline)) static int smash(const char *src, int n) {
    volatile char buf[16];
    for (int i = 0; i < n; i++) buf[i] = src[i];
    return buf[3];
}
int main(void) {
    static char data[256];
    for (int i = 0; i < 256; i++) data[i] = 'A';
    cos_puts("before the overflow\n");
    int r = smash(data, 200);
    cos_puts("not reached\n");
    return r;
}
