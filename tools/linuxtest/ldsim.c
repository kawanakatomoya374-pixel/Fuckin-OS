/* what ld.so does when it loads a library: map the whole file read-only (the address-space reservation), mprotect it PROT_NONE, then map the segments over it with
   MAP_FIXED (text r-x, rodata r--, data rw-, then an anonymous bss). Every segment must read back as the file's bytes. Repeated, in the parent and in forked children. */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
static long one(const char *path, int iter) {
    int fd = open(path, O_RDONLY); if (fd < 0) { printf("  open failed iter %d errno %d\n", iter, errno); return 1000; } struct stat st; fstat(fd, &st); size_t len = st.st_size & ~4095ul;
    char *base = mmap(NULL, len + (1 << 20), PROT_READ, MAP_PRIVATE, fd, 0); if (base == MAP_FAILED) { perror("mmap whole"); return 1000; }
    size_t t = len / 3 & ~4095ul, r = len * 2 / 3 & ~4095ul, d = len - (len / 8 & ~4095ul);
    mprotect(base + t, len + (1 << 20) - t, PROT_NONE);
    char *text = mmap(base + t, r - t, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, t);
    char *ro = mmap(base + r, d - r, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, r);
    char *data = mmap(base + d, len - d, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, d);
    mmap(base + len, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    if (text != base + t || ro != base + r || data != base + d) { printf("  fixed maps: %p %p %p (want %p %p %p)\n", text, ro, data, base + t, base + r, base + d); perror("mmap"); return 1000; }
    char *ref = malloc(len); ssize_t pr = pread(fd, ref, len, 0); if (pr != (ssize_t)len) { printf("  pread returned %zd, want %zu (errno %d)\n", pr, len, errno); return 1000; }
    long bad = 0;
    for (size_t o = 0; o < len; o += 4096) {
        size_t n = len - o < 4096 ? len - o : 4096;
        if (o >= t || o < t) { if (memcmp(base + o, ref + o, n)) { if (bad < 3) printf("  iter %d: page at +0x%zx differs (first word %08x want %08x)\n", iter, o, *(unsigned *)(base + o), *(unsigned *)(ref + o)); bad++; } }
    }
    for (size_t i = 0; i < 4096 && i < 1 << 20; i += 8) if (*(long *)(base + len + i)) { bad++; break; }
    free(ref); munmap(base, len + (1 << 20)); close(fd); return bad;
}
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/linux/usr/lib/firefox/libgkcodecs.so"; long total = 0;
    int iters = argc > 2 ? atoi(argv[2]) : 20;
    for (int i = 0; i < iters; i++) total += one(path, i);
    printf("parent: %ld bad\n", total);
    for (int k = 0; k < 3; k++) {
        pid_t p = fork();
        if (p == 0) { long b = 0; for (int i = 0; i < iters; i++) b += one(path, i); printf("child %d: %ld bad\n", k, b); _exit(b != 0); }
        total += 0; int st; waitpid(p, &st, 0); if (st) total++;
    }
    printf("ldsim: %s\n", total ? "PROBLEMS" : "ok"); return total != 0;
}
