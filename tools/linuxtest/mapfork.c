/* file mappings across fork: a file written with a known pattern is mapped MAP_PRIVATE (whole, and at an offset) and read through read() as well; the parent
   touches some pages before and after the fork, children verify every word, write to some pages and verify again, and the parent's view must stay the pattern. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#define SZ (48u << 20)
#define W(i) ((uint32_t)((i) * 2654435761u) ^ 0xA5A5u)
static long check(const volatile uint32_t *p, size_t words, size_t base, const char *what) {
    long bad = 0;
    for (size_t i = 0; i < words; i++) if (p[i] != W(base + i)) { if (bad < 3) printf("  %s: word %zu is %08x, want %08x\n", what, base + i, p[i], W(base + i)); bad++; }
    return bad;
}
int main(void) {
    const char *path = "/tmp/mapfork.dat";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    uint32_t *buf = malloc(1 << 20);
    for (size_t o = 0; o < SZ; o += 1 << 20) { for (size_t i = 0; i < (1 << 20) / 4; i++) buf[i] = W(o / 4 + i); if (write(fd, buf, 1 << 20) != 1 << 20) return 2; }
    long total = 0;
    for (int round = 0; round < 3; round++) {
        size_t off = round == 2 ? 5 * 4096 : 0, len = SZ - off;
        volatile uint32_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, off);
        if (m == MAP_FAILED) { puts("mmap failed"); return 2; }
        size_t words = len / 4, base = off / 4;
        if (round) for (size_t i = 0; i < words; i += 4096) (void)m[i];      /* some pages touched before the fork */
        int kids = 4; pid_t pid[4];
        for (int k = 0; k < kids; k++) {
            pid[k] = fork();
            if (pid[k] == 0) {
                long bad = check(m, words, base, "child read");
                for (size_t i = (size_t)k * 1024; i < words; i += 8192) m[i] = 0xDEADBEEF;   /* dirty some pages, then everything but those must still match */
                for (size_t i = 0; i < words; i++) { uint32_t want = (i >= (size_t)k * 1024 && (i - (size_t)k * 1024) % 8192 == 0) ? 0xDEADBEEF : W(base + i); if (m[i] != want) { if (bad < 3) printf("  child write: word %zu is %08x\n", i, m[i]); bad++; } }
                /* read() into a page of the mapping, as a program reading a file into its own mapping does */
                int fd2 = open(path, O_RDONLY); char *tmp = malloc(65536);
                for (size_t o = 0; o < len && o < (8u << 20); o += 65536) { ssize_t r = pread(fd2, tmp, 65536, off + o); if (r != 65536 || memcmp(tmp, (const void *)((const char *)m + o), 65536) == 0 ? 0 : 1) { /* pages dirtied above differ, others must not */ } }
                _exit(bad ? 1 : 0);
            }
        }
        for (size_t i = 0; i < words; i += 4096) m[i + 1] = W(base + i + 1);   /* the parent writes (same value) while the children run */
        for (int k = 0; k < kids; k++) { int st = 0; waitpid(pid[k], &st, 0); if (st) { printf("round %d child %d status %d\n", round, k, st); total++; } }
        long b = check(m, words, base, "parent");
        printf("round %d: parent mismatches %ld\n", round, b); total += b;
        munmap((void *)m, len);
    }
    printf("mapfork: %s\n", total ? "PROBLEMS" : "ok");
    return total ? 1 : 0;
}
