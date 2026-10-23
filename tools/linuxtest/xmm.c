/* XMM/x87 registers must survive preemption, system calls and page faults: each thread keeps its own pattern in xmm0-xmm7 and checks it again and again. */
#define _GNU_SOURCE
#include <emmintrin.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#define NT 4
static volatile int bad[NT];
static void *worker(void *arg) {
    long t = (long)arg;
    for (int round = 0; round < 3000; ++round) {
        __m128i p0 = _mm_set1_epi32(0x11110000 + (int)t), p1 = _mm_set1_epi32(0x22220000 + (int)t), p2 = _mm_set1_epi64x(0x3333444455556666ll + t);
        __m128d d = _mm_set1_pd(1.5 + (double)t);
        __asm__ volatile("" : "+x"(p0), "+x"(p1), "+x"(p2), "+x"(d));
        for (int i = 0; i < 20; ++i) {
            syscall(SYS_getpid);                                      /* a system call */
            char *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); m[17] = 1; munmap(m, 4096);     /* a page fault */
            usleep(0);
            __m128i a = _mm_cmpeq_epi32(p0, _mm_set1_epi32(0x11110000 + (int)t)), b = _mm_cmpeq_epi32(p1, _mm_set1_epi32(0x22220000 + (int)t));
            __m128i c = _mm_cmpeq_epi32(p2, _mm_set1_epi64x(0x3333444455556666ll + t));
            if (_mm_movemask_epi8(a) != 0xFFFF || _mm_movemask_epi8(b) != 0xFFFF || _mm_movemask_epi8(c) != 0xFFFF || _mm_cvtsd_f64(d) != 1.5 + (double)t) bad[t]++;
        }
    }
    return 0;
}
int main(void) {
    pthread_t th[NT]; for (long t = 0; t < NT; ++t) pthread_create(&th[t], 0, worker, (void *)t);
    for (int t = 0; t < NT; ++t) pthread_join(th[t], 0);
    int tot = 0; for (int t = 0; t < NT; ++t) tot += bad[t];
    printf("xmm: %d corrupted checks\n", tot); return tot != 0;
}
