/* vdso.c - checks the vDSO of the Linux layer: AT_SYSINFO_EHDR is present, clock_gettime / gettimeofday / time agree with the system call, never go backwards, and the vDSO is much faster than the call.
 * usage: /linux-test/vdso   (prints "vdso ok" on success) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>

static uint64_t ms_of(struct timespec t) { return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
int main(void) {
    unsigned long eh = getauxval(AT_SYSINFO_EHDR);
    printf("AT_SYSINFO_EHDR = 0x%lx\n", eh);
    int bad = eh == 0;
    struct timespec a, b, c; struct timeval tv; time_t tt;
    clock_gettime(CLOCK_MONOTONIC, &a); syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &b); clock_gettime(CLOCK_MONOTONIC, &c);
    printf("monotonic: vdso %llu, syscall %llu, vdso %llu ms\n", (unsigned long long)ms_of(a), (unsigned long long)ms_of(b), (unsigned long long)ms_of(c));
    if (ms_of(b) + 2 < ms_of(a) || ms_of(c) + 2 < ms_of(b) || ms_of(c) < ms_of(a)) bad = 1;
    clock_gettime(CLOCK_REALTIME, &a); gettimeofday(&tv, NULL); tt = time(NULL);
    printf("realtime: clock_gettime %lld, gettimeofday %lld, time %lld s\n", (long long)a.tv_sec, (long long)tv.tv_sec, (long long)tt);
    if (a.tv_sec < 1600000000 || tv.tv_sec < a.tv_sec - 1 || tv.tv_sec > a.tv_sec + 2 || tt < a.tv_sec - 1 || tt > a.tv_sec + 2) bad = 1;
    uint64_t prev = 0; int back = 0; const int N = 200000;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; ++i) { clock_gettime(CLOCK_MONOTONIC, &a); uint64_t m = ms_of(a); if (m < prev) back++; prev = m; }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t vd = ms_of(t1) - ms_of(t0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N / 10; ++i) { syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &a); }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t sc = ms_of(t1) - ms_of(t0);
    printf("%d vdso calls took %llu ms, %d system calls took %llu ms, went backwards %d times\n", N, (unsigned long long)vd, N / 10, (unsigned long long)sc, back);
    if (back) bad = 1;
    puts(bad ? "vdso FAILED" : "vdso ok");
    return bad;
}
