/* See cmem.h. Every block has a 16-byte header holding its class size; classes: 16-byte steps up to 1 KiB, then four per power of two.
 * Blocks come from a bump pointer over 4 MiB slabs taken from libcos; anything over 1 MiB goes straight to libcos. */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "cos.h"

#undef malloc
#undef free
#undef calloc
#undef realloc

#define HDR 16
#define SLAB (4u << 20)
#define BIG (1u << 20)
#define NCLASS 192
typedef struct { size_t size; size_t big; } Hdr;
static void *s_free[NCLASS];
static char *s_bump, *s_end;
static unsigned long s_slabs, s_bigs, s_bigbytes, s_calls;

static unsigned cls(size_t n, size_t *cs) {
    if (n <= 1024) { n = (n + 15) & ~(size_t)15; if (n < 16) n = 16; *cs = n; return (unsigned)(n / 16 - 1); }
    int msb = 63 - __builtin_clzl((unsigned long)(n - 1));
    size_t step = (size_t)1 << (msb - 2);
    size_t c = ((n + step - 1) / step) * step;
    *cs = c;
    return 64u + (unsigned)(msb - 10) * 4u + (unsigned)(c / step - 5);
}

void *cm_malloc(size_t n) {
    if (n == 0) n = 1;
    if (n > BIG) {
        Hdr *h = (Hdr *)cos_malloc(n + HDR);
        if (!h) return NULL;
        h->size = n; h->big = 1; s_bigs++; s_bigbytes += n;
        return (char *)h + HDR;
    }
    size_t cs; unsigned c = cls(n, &cs);
    void *p = s_free[c];
    if (p) { s_free[c] = *(void **)p; return p; }
    if ((size_t)(s_end - s_bump) < cs + HDR) {
        char *slab = (char *)cos_malloc(SLAB);
        if (!slab) return NULL;
        s_slabs++; s_bump = slab; s_end = slab + SLAB;                    /* the tail of the previous slab is simply left unused */
    }
    Hdr *h = (Hdr *)s_bump;
    s_bump += cs + HDR;
    h->size = cs; h->big = 0;
    return (char *)h + HDR;
}

void cm_free(void *p) {
    if (!p) return;
    Hdr *h = (Hdr *)((char *)p - HDR);
    if (h->big) { s_bigs--; s_bigbytes -= h->size; cos_free(h); return; }
    size_t cs; unsigned c = cls(h->size, &cs);
    *(void **)p = s_free[c]; s_free[c] = p;
}

void *cm_calloc(size_t n, size_t m) {
    if (m && n > (size_t)-1 / m) return NULL;
    void *p = cm_malloc(n * m);
    if (p) memset(p, 0, n * m);
    return p;
}

void *cm_realloc(void *p, size_t n) {
    if (!p) return cm_malloc(n);
    if (n == 0) { cm_free(p); return NULL; }
    Hdr *h = (Hdr *)((char *)p - HDR);
    size_t old = h->size;
    if (!h->big && n <= old) return p;
    void *q = cm_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, old < n ? old : n);
    cm_free(p);
    return q;
}

/* Debug: heap summary on the serial log. */
void cm_report(void) {
    char b[160];
    snprintf(b, sizeof b, "[CRAFT] heap: slabs=%lu (%lu MB) big=%lu (%lu KB)\n", s_slabs, s_slabs * 4, s_bigs, s_bigbytes >> 10);
    cos_puts(b);
}
