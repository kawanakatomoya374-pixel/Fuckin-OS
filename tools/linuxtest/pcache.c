/* pcache.c - the shared cache of read-only file mappings (paging.c): several processes map the same file; the mapped bytes are right, the frames are shared (MemFree does not drop by
 * a copy per process), a write after mprotect stays private, and a file that is written is not served from the cache any more.
 * usage: /linux-test/pcache   (prints "pcache ok" on success) */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#define SZ (16u << 20)
#define PATH "/tmp/pcache.dat"
static uint32_t W(size_t i, uint32_t gen) { return (uint32_t)(i * 2654435761u) ^ 0xA5A5u ^ gen; }
static long memfree_kb(void) { FILE *f = fopen("/proc/meminfo", "r"); char l[128]; long v = -1; if (!f) return -1; while (fgets(l, sizeof l, f)) if (sscanf(l, "MemFree: %ld kB", &v) == 1) break; fclose(f); return v; }
static uint64_t sum_map(const volatile uint32_t *m, size_t words) { uint64_t s = 0; for (size_t i = 0; i < words; i++) s += m[i]; return s; }
static uint64_t sum_expect(size_t words, uint32_t gen) { uint64_t s = 0; for (size_t i = 0; i < words; i++) s += W(i, gen); return s; }
static int child(uint32_t gen, int hold) {
    int fd = open(PATH, O_RDONLY); if (fd < 0) return 2;
    const volatile uint32_t *m = mmap(NULL, SZ, PROT_READ, MAP_PRIVATE, fd, 0); if (m == MAP_FAILED) return 3;
    uint64_t got = sum_map(m, SZ / 4), want = sum_expect(SZ / 4, gen);
    if (got != want) { printf("  child: checksum %llx, want %llx\n", (unsigned long long)got, (unsigned long long)want); return 4; }
    if (hold) sleep((unsigned)hold);
    return 0;
}
static int run_child(const char *self, uint32_t gen, int hold, pid_t *out) {
    char g[16], h[16]; snprintf(g, sizeof g, "%u", gen); snprintf(h, sizeof h, "%d", hold);
    char *av[] = { (char *)self, "child", g, h, NULL };
    return posix_spawn(out, self, NULL, NULL, av, environ);
}
int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "child")) return child((uint32_t)atoi(argv[2]), atoi(argv[3]));
    int bad = 0;
    int fd = open(PATH, O_CREAT | O_RDWR | O_TRUNC, 0644); if (fd < 0) { puts("cannot create the file"); return 2; }
    uint32_t *buf = malloc(1 << 20);
    for (size_t o = 0; o < SZ; o += 1 << 20) { for (size_t i = 0; i < (1 << 20) / 4; i++) buf[i] = W(o / 4 + i, 0); if (write(fd, buf, 1 << 20) != 1 << 20) return 2; }
    close(fd); fd = open(PATH, O_RDONLY);
    long free0 = memfree_kb();
    volatile uint32_t *m = mmap(NULL, SZ, PROT_READ, MAP_PRIVATE, fd, 0); if (m == MAP_FAILED) { puts("mmap failed"); return 2; }
    if (sum_map(m, SZ / 4) != sum_expect(SZ / 4, 0)) { puts("FAIL parent checksum"); bad = 1; }
    long free1 = memfree_kb();
    pid_t pid[3];
    for (int k = 0; k < 3; k++) if (run_child(argv[0], 0, 6, &pid[k])) { puts("FAIL spawn"); return 2; }
    sleep(4);
    long free2 = memfree_kb();
    printf("MemFree: before %ld kB, after the parent mapped %ld kB (-%ld), with 3 more processes mapping %ld kB (-%ld; a copy each would be -%u)\\n", free0, free1, free0 - free1, free2, free1 - free2, 3 * (SZ / 1024));
    if (free1 - free2 > (long)(SZ / 1024)) { puts("FAIL: the frames are not shared"); bad = 1; }
    for (int k = 0; k < 3; k++) { int st = 0; waitpid(pid[k], &st, 0); if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL child %d status %x\\n", k, st); bad = 1; } }
    /* fork: the children inherit the shared read-only mapping and read it; the parent's view and the cache stay right when they are gone */
    for (int k = 0; k < 2; k++) {
        pid_t f = fork();
        if (f == 0) { _exit(sum_map(m, SZ / 4) == sum_expect(SZ / 4, 0) ? 0 : 5); }
        int fst = 0; waitpid(f, &fst, 0); if (!WIFEXITED(fst) || WEXITSTATUS(fst)) { puts("FAIL: a forked child saw other bytes"); bad = 1; }
    }
    if (sum_map(m, SZ / 4) != sum_expect(SZ / 4, 0)) { puts("FAIL: the parent's mapping changed after the forks"); bad = 1; }
    /* a write after mprotect is private: the other mapping and the cache keep the file's bytes */
    volatile uint32_t *w = mmap(NULL, SZ, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mprotect((void *)w, SZ, PROT_READ | PROT_WRITE)) { puts("FAIL mprotect"); bad = 1; } else { w[100] = 0xDEADBEEF; w[SZ / 8] = 0xDEADBEEF; }
    if (m[100] != W(100, 0) || m[SZ / 8] != W(SZ / 8, 0)) { puts("FAIL: a private write changed the other mapping"); bad = 1; }
    pid_t cp; run_child(argv[0], 0, 0, &cp); int st = 0; waitpid(cp, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) { puts("FAIL: a later process saw the private write"); bad = 1; }
    /* a file that is written is forgotten: the next mapping sees the new bytes */
    int wfd = open(PATH, O_WRONLY); uint32_t v = W(5000, 7); if (pwrite(wfd, &v, 4, 5000 * 4) != 4) { puts("FAIL pwrite"); bad = 1; } close(wfd);
    volatile uint32_t *m2 = mmap(NULL, SZ, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m2[5000] != v) { printf("FAIL: after the file was written the mapping shows %08x, want %08x\\n", m2[5000], v); bad = 1; }
    puts(bad ? "pcache FAILED" : "pcache ok");
    return bad;
}
