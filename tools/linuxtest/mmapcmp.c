/* mmap a real file (MAP_PRIVATE and MAP_SHARED) from several threads at once and compare with pread(): file mapping contents must equal file contents */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
static const char *path; static long bad[8];
static void *run(void *arg) {
    long t = (long)arg; int fd = open(path, O_RDONLY); struct stat st; fstat(fd, &st); size_t len = st.st_size;
    char *buf = malloc(1 << 16);
    for (int it = 0; it < 6; it++) {
        int shared = (it + t) & 1; size_t off = ((it + t) % 3) * 4096; if (off >= len) off = 0;
        char *m = mmap(NULL, len - off, PROT_READ, shared ? MAP_SHARED : MAP_PRIVATE, fd, off);
        if (m == MAP_FAILED) { printf("t%ld mmap failed\n", t); bad[t]++; continue; }
        for (size_t o = 0; o < len - off; o += 1 << 16) {
            size_t n = len - off - o < (1 << 16) ? len - off - o : (1 << 16);
            if (pread(fd, buf, n, off + o) != (ssize_t)n) { bad[t]++; continue; }
            if (memcmp(buf, m + o, n)) { if (bad[t] < 3) printf("t%ld it%d (%s off %zu): mismatch at %zu\n", t, it, shared ? "shared" : "private", off, o); bad[t]++; }
        }
        munmap(m, len - off);
    }
    close(fd); free(buf); return NULL;
}
int main(int argc, char **argv) {
    path = argc > 1 ? argv[1] : "/linux/usr/lib/firefox/omni.ja"; pthread_t th[4];
    for (long i = 0; i < 4; i++) pthread_create(&th[i], NULL, run, (void *)i);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    long total = 0; for (int i = 0; i < 4; i++) total += bad[i];
    printf("mmapcmp %s: %s\n", path, total ? "PROBLEMS" : "ok"); return total != 0;
}
