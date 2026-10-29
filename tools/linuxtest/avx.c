/* avx.c - checks that AVX works for Linux programs: CPUID/XGETBV report it, a 256-bit loop gives the right result while the timer interrupts and the other threads preempt it
 * (the upper halves of the YMM registers must survive every task switch: XSAVE state, gdt.c), and a signal handler that uses AVX does not damage the interrupted code.
 * usage: /linux-test/avx   (prints "avx ok" on success, "avx unavailable" when the machine has no AVX2) */
#define _GNU_SOURCE
#include <immintrin.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <cpuid.h>

static int have_avx2(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 27)) || !(c & (1u << 28))) return 0;       /* OSXSAVE, AVX */
    unsigned lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6) return 0;                                                                       /* the OS keeps XMM and YMM state */
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d) || !(b & (1u << 5))) return 0;                       /* AVX2 */
    return 1;
}

__attribute__((target("avx2"))) static int ymm_loop(uint32_t seed, long iters) {
    __m256i acc = _mm256_set1_epi32((int)seed), step = _mm256_set_epi32(1, 2, 3, 4, 5, 6, 7, 8);
    uint32_t ref[8]; for (int i = 0; i < 8; ++i) ref[i] = seed;
    static const uint32_t st[8] = { 8, 7, 6, 5, 4, 3, 2, 1 };
    for (long n = 0; n < iters; ++n) {
        acc = _mm256_add_epi32(acc, step);
        for (int i = 0; i < 8; ++i) ref[i] += st[i];
        if ((n & 1023) == 0) usleep(0);                                    /* also give the CPU away on purpose */
    }
    uint32_t out[8]; _mm256_storeu_si256((__m256i *)out, acc);
    return memcmp(out, ref, sizeof out) == 0;
}

static volatile int bad;
static void *worker(void *arg) { if (!ymm_loop((uint32_t)(uintptr_t)arg, 300000)) bad++; return NULL; }

__attribute__((target("avx2"))) static void on_alarm(int s) {         /* clobbers the whole YMM file: the kernel must put the interrupted code's registers back (rt_sigreturn) */
    (void)s;
    __asm__ volatile("vpcmpeqd %%ymm0,%%ymm0,%%ymm0\n vmovdqa %%ymm0,%%ymm1\n vmovdqa %%ymm0,%%ymm2\n vmovdqa %%ymm0,%%ymm3\n vmovdqa %%ymm0,%%ymm4\n vmovdqa %%ymm0,%%ymm5\n vmovdqa %%ymm0,%%ymm6\n vmovdqa %%ymm0,%%ymm7\n"
                     "vmovdqa %%ymm0,%%ymm8\n vmovdqa %%ymm0,%%ymm9\n vmovdqa %%ymm0,%%ymm10\n vmovdqa %%ymm0,%%ymm11\n vmovdqa %%ymm0,%%ymm12\n vmovdqa %%ymm0,%%ymm13\n vmovdqa %%ymm0,%%ymm14\n vmovdqa %%ymm0,%%ymm15" ::: "ymm0","ymm1","ymm2","ymm3","ymm4","ymm5","ymm6","ymm7","ymm8","ymm9","ymm10","ymm11","ymm12","ymm13","ymm14","ymm15");
}
#include <sys/time.h>
static int sig_test(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_alarm; sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = { { 0, 2000 }, { 0, 2000 } }; setitimer(ITIMER_REAL, &it, NULL);
    int ok = ymm_loop(9, 400000);
    struct itimerval off; memset(&off, 0, sizeof off); setitimer(ITIMER_REAL, &off, NULL);
    return ok;
}

int main(void) {
    if (!have_avx2()) { puts("avx unavailable"); return 0; }
    pthread_t t[3];
    for (int i = 0; i < 3; ++i) pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)(1000 + i));
    if (!ymm_loop(7, 300000)) bad++;
    for (int i = 0; i < 3; ++i) pthread_join(t[i], NULL);
    if (!sig_test()) bad++;
    puts(bad ? "avx FAILED" : "avx ok");
    return bad ? 1 : 0;
}
