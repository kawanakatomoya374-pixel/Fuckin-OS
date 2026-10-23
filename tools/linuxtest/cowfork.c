/* copy-on-write fork under load: threads of the parent keep updating a big array while the main thread forks; the children trash their copy
   (by stores and by read() into it) and exit. The parent's array must hold exactly what its threads wrote. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#define T 3
#define SLOTS 16384u
#define STRIDE 2048u
#define N (SLOTS * T * STRIDE + 4096u)   /* ~400 MiB of uint32, touched all over */
static volatile unsigned *A; static volatile int stop; static int pp[T][2];
static unsigned cnt[T][SLOTS]; static volatile int bad_io;
static void *worker(void *arg) {
    long t = (long)arg; unsigned s = 12345u + (unsigned)t * 777u;
    while (!stop) {
        s = s * 1664525u + 1013904223u;
        unsigned slot = (s >> 8) % SLOTS, idx = (slot * T + (unsigned)t) * STRIDE;   /* each thread owns a set of words; slots are spread over many pages */
        if (idx >= N) continue;
        A[idx]++; cnt[t][slot]++;
        unsigned v = s; if (write(pp[t][1], &v, 4) == 4) { if (read(pp[t][0], (void *)&A[idx + 1], 4) == 4 && A[idx + 1] != v) bad_io++; }   /* the kernel stores into the shared array */
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc > 1) return 0;                                    /* the exec'd child */
    for (int t = 0; t < T; ++t) pipe(pp[t]);
    A = calloc(N, 4); pthread_t th[T];
    for (long t = 0; t < T; ++t) pthread_create(&th[t], 0, worker, (void *)t);
    /* a JIT-like page: patched (RW), run (RX) and forked while it is shared */
    unsigned char *jit = mmap(0, 4096, PROT_READ | PROT_EXEC | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); int jbad = 0;
    mprotect(jit, 4096, PROT_READ | PROT_EXEC);
    int zero = open("/dev/zero", O_RDONLY), bad = 0;
    for (int f = 0; f < 12; ++f) {
        pid_t p = fork();
        if (p == 0) {
            mprotect(jit, 4096, PROT_READ | PROT_WRITE); jit[1] ^= 0xFF; mprotect(jit, 4096, PROT_READ | PROT_EXEC);
            for (unsigned i = 0; i < N; i += 4096) A[i] = 0xDEADBEEF;
            read(zero, (void *)A, 1 << 20); read(zero, (char *)A + (8 << 20), 1 << 20);
            memset((void *)A, 0xAB, 4 << 20);
            if (f & 1) { char *av[] = { "/linux/root/cowfork", "x", 0 }; execv(av[0], av); }
            _exit(0);
        }
        int st; waitpid(p, &st, 0);
        mprotect(jit, 4096, PROT_READ | PROT_WRITE); jit[0] = 0xB8; *(unsigned *)(jit + 1) = 1000u + (unsigned)f; jit[5] = 0xC3; mprotect(jit, 4096, PROT_READ | PROT_EXEC);
        if (((int (*)(void))jit)() != 1000 + f) jbad++;
    }
    stop = 1; for (int t = 0; t < T; ++t) pthread_join(th[t], 0);
    for (long t = 0; t < T; ++t) for (unsigned slot = 0; slot < SLOTS; ++slot) { unsigned idx = (slot * T + t) * STRIDE; if (idx < N && A[idx] != cnt[t][slot]) bad++; }
    printf("cowfork: %d wrong words, %d wrong jit results, %d wrong pipe reads\n", bad, jbad, bad_io); bad += jbad + bad_io;
    return bad != 0;
}
