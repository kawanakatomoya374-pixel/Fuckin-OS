#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "memory.h"
#include "serial.h"

long   imgtest_live_allocs = 0;
size_t imgtest_live_bytes = 0;
size_t imgtest_peak_bytes = 0;
size_t imgtest_alloc_limit = 0;
int    imgtest_verbose = 0;

typedef struct { size_t n; size_t pad; } hdr_t; /* 16 bytes, keeps 16-byte alignment */

void* kmalloc(size_t n) {
    if (n == 0) return NULL;
    if (imgtest_alloc_limit && n > imgtest_alloc_limit) return NULL;
    hdr_t* h = (hdr_t*)malloc(sizeof(hdr_t) + n);
    if (!h) return NULL;
    h->n = n;
    imgtest_live_allocs++;
    imgtest_live_bytes += n;
    if (imgtest_live_bytes > imgtest_peak_bytes) imgtest_peak_bytes = imgtest_live_bytes;
    return h + 1;
}

void kfree(void* p) {
    if (!p) return;
    hdr_t* h = (hdr_t*)p - 1;
    imgtest_live_allocs--;
    imgtest_live_bytes -= h->n;
    free(h);
}

void serial_puts(const char* s) { if (imgtest_verbose) fputs(s, stderr); }
void serial_putdec(uint64_t v) { if (imgtest_verbose) fprintf(stderr, "%llu", (unsigned long long)v); }
void serial_puthex(uint64_t v) { if (imgtest_verbose) fprintf(stderr, "%llx", (unsigned long long)v); }
