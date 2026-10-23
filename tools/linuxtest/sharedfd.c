/* one descriptor shared by threads (and by forked children): pread() at random offsets must return the bytes of that offset, never those of another reader's seek */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#define SZ (4u << 20)
#define W(i) ((uint32_t)((i) * 2654435761u) ^ 0x5A5Au)
static int fd; static long bad[8];
static void *run(void *arg) {
    long t = (long)arg; unsigned s = 77u + (unsigned)t * 1009u; uint32_t *b = malloc(8192);
    for (int it = 0; it < 4000; it++) {
        s = s * 1103515245u + 12345u; size_t off = ((s >> 8) % (SZ / 4 - 2048)) * 4; size_t n = 4096 + 4 * (s % 1024);
        ssize_t r = pread(fd, b, n, off); if (r != (ssize_t)n) { if (bad[t] < 2) printf("  pid %d t%ld: pread(%zu bytes at %zu) returned %zd errno %d\n", (int)getpid(), t, n, off, r, errno); bad[t]++; continue; }
        for (size_t i = 0; i < n / 4; i++) if (b[i] != W(off / 4 + i)) { if (bad[t] < 2) printf("  t%ld: off %zu word %zu wrong\n", t, off, i); bad[t]++; break; }
    }
    free(b); return NULL;
}
static long threads(void) {
    pthread_t th[4]; memset(bad, 0, sizeof bad);
    for (long i = 0; i < 4; i++) if (pthread_create(&th[i], NULL, run, (void *)i)) { printf("  pid %d: pthread_create failed\n", (int)getpid()); bad[i] = 1000; th[i] = 0; }
    for (int i = 0; i < 4; i++) if (th[i]) pthread_join(th[i], NULL);
    long total = 0; for (int i = 0; i < 4; i++) total += bad[i]; return total;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fd = open("/tmp/sharedfd.dat", O_CREAT | O_RDWR | O_TRUNC, 0644); uint32_t *buf = malloc(1 << 20);
    for (size_t o = 0; o < SZ; o += 1 << 20) { for (size_t i = 0; i < (1 << 20) / 4; i++) buf[i] = W(o / 4 + i); write(fd, buf, 1 << 20); }
    long total = threads(); printf("threads: %ld bad\n", total);
    pid_t p[2]; for (int k = 0; k < 2; k++) { p[k] = fork(); if (p[k] == 0) { long b = threads(); _exit(b != 0); } }
    long b = threads(); total += b; for (int k = 0; k < 2; k++) { int st; waitpid(p[k], &st, 0); if (st) total++; }
    printf("sharedfd: %s\n", total ? "PROBLEMS" : "ok"); return total != 0;
}
