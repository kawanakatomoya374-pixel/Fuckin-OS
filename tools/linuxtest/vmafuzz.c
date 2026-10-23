/* random mmap / munmap / mremap / mprotect / madvise against a model: what is mapped must hold its pattern, what is not mapped must give EFAULT (write() to a pipe checks it without faulting) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#define NP 2048                                    /* pages of the arena we watch */
static unsigned char *base; static int mapped[NP]; static unsigned char val[NP]; static int prot_w[NP];
static int pfd[2]; static unsigned long rs = 88172645463325252ul;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 11); }
static int bad;
static int readable(int pg) { char c; ssize_t r = write(pfd[1], base + (size_t)pg * 4096 + 100, 1); if (r == 1) { read(pfd[0], &c, 1); return 1; } return 0; }
static void verify(const char *what) {
    for (int p = 0; p < NP; ++p) {
        int r = readable(p);
        if (mapped[p] && !r) { printf("after %s: page %d should be mapped\n", what, p); bad++; return; }
        if (!mapped[p] && r) { printf("after %s: page %d should be unmapped\n", what, p); bad++; return; }
        if (mapped[p] && base[(size_t)p * 4096 + 100] != val[p]) { printf("after %s: page %d holds %d, expected %d\n", what, p, base[(size_t)p * 4096 + 100], val[p]); bad++; return; }
        if (mapped[p] && base[(size_t)p * 4096 + 4095] != val[p]) { printf("after %s: page %d tail differs\n", what, p); bad++; return; }
    }
}
int main(void) {
    pipe(pfd);
    base = mmap(0, (size_t)NP * 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);            /* reserve, then carve it up like malloc arenas do */
    munmap(base, (size_t)NP * 4096);
    for (int step = 0; step < 4000 && !bad; ++step) {
        int op = rnd() % 7; int p = rnd() % NP, n = 1 + rnd() % 24; if (p + n > NP) n = NP - p; char what[64];
        if (op == 0 || op == 1) {                                  /* map anonymous memory exactly there (fixed) and fill it */
            void *m = mmap(base + (size_t)p * 4096, (size_t)n * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            snprintf(what, sizeof what, "mmap fixed %d+%d", p, n);
            if (m == MAP_FAILED) { bad++; printf("%s failed\n", what); break; }
            for (int i = p; i < p + n; ++i) { unsigned char v = (unsigned char)(1 + rnd() % 250); memset(base + (size_t)i * 4096, v, 4096); mapped[i] = 1; val[i] = v; prot_w[i] = 1; }
        } else if (op == 2) {                                      /* unmap a range */
            snprintf(what, sizeof what, "munmap %d+%d", p, n); munmap(base + (size_t)p * 4096, (size_t)n * 4096); for (int i = p; i < p + n; ++i) mapped[i] = 0;
        } else if (op == 3) {                                      /* protect a mapped range read-only and back */
            snprintf(what, sizeof what, "mprotect %d+%d", p, n);
            int all = 1; for (int i = p; i < p + n; ++i) if (!mapped[i]) all = 0;
            if (all) { mprotect(base + (size_t)p * 4096, (size_t)n * 4096, PROT_READ); mprotect(base + (size_t)p * 4096, (size_t)n * 4096, PROT_READ | PROT_WRITE); }
        } else if (op == 4) {                                      /* give pages back: they must read as zero, then refill */
            snprintf(what, sizeof what, "madvise dontneed %d+%d", p, n);
            int all = 1; for (int i = p; i < p + n; ++i) if (!mapped[i]) all = 0;
            if (all) { madvise(base + (size_t)p * 4096, (size_t)n * 4096, MADV_DONTNEED); for (int i = p; i < p + n; ++i) { if (base[(size_t)i * 4096 + 100] != 0) { bad++; printf("%s: page %d not zero\n", what, i); } val[i] = 0; } }
        } else if (op == 5) {                                      /* shrink a mapping from its tail with mremap */
            int all = 1; for (int i = p; i < p + n; ++i) if (!mapped[i]) all = 0;
            snprintf(what, sizeof what, "mremap shrink %d+%d", p, n);
            if (all && n > 1) { int k = 1 + rnd() % (n - 1); void *r = mremap(base + (size_t)p * 4096, (size_t)n * 4096, (size_t)k * 4096, 0); if (r == MAP_FAILED) { /* a merged mapping may refuse */ } else for (int i = p + k; i < p + n; ++i) mapped[i] = 0; }
        } else {                                                   /* grow a fully mapped range in place (never moving it) */
            int all = 1; for (int i = p; i < p + n; ++i) if (!mapped[i]) all = 0;
            snprintf(what, sizeof what, "mremap grow %d+%d", p, n);
            if (all && p + n + 4 <= NP) { void *r = mremap(base + (size_t)p * 4096, (size_t)n * 4096, (size_t)(n + 2) * 4096, 0); if (r == (void *)(base + (size_t)p * 4096)) for (int i = p + n; i < p + n + 2; ++i) { mapped[i] = 1; val[i] = 0; } }
        }
        if (step % 5 == 0 || bad) verify(what);
    }
    printf("vmafuzz: %d problems\n", bad); return bad != 0;
}
