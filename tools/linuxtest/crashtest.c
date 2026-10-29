#include "cos.h"
#include <stdlib.h>
int main(int argc, char **argv) {
    int64_t h = 0; cos_win_info_t wi;
    cos_puts("crashtest: start\n");
    if (argc > 1 && argv[1][0] == 'w') { h = cos_win2_create("CrashTest", 300, 200, &wi); for (int i = 0; i < 300 * 200; i++) wi.pixels[i] = 0x336699; cos_win2_present(h); cos_sleep_ms(1500); }
    if (argc > 1 && argv[1][0] == 'h') { h = cos_win2_create("HangTest", 460, 300, &wi); for (int i = 0; i < 460 * 300; i++) wi.pixels[i] = 0x336699; cos_win2_present(h); uint64_t t0 = cos_time_ms(); uint64_t until = argc > 2 ? 1000ull * (uint64_t)atoi(argv[2]) : 1000000ull; for (volatile unsigned long long k = 0; cos_time_ms() - t0 < until; k++) {} return 0; }
    volatile int *p = (volatile int *)0;
    if (argc > 1 && argv[1][0] == 'z') { volatile int z = 0; return 10 / z; }
    *p = 1;
    return 0;
}
