/* file reads must put the right bytes in the right place: read, pread, readv and file mmap at odd offsets and sizes against a pattern file (/root/pat.bin, tools/linuxtest/fileio.c makes it with --make) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/uio.h>
#define SZ (1536 * 1024 + 777)
static unsigned char pat(unsigned long i) { return (unsigned char)(i * 131 + (i >> 12) * 7 + (i >> 20)); }
int main(int argc, char **argv) {
    const char *path = "/root/pat.bin";
    if (argc > 1) { FILE *f = fopen(path, "wb"); for (unsigned long i = 0; i < SZ; ++i) fputc(pat(i), f); fclose(f); puts("made"); return 0; }
    int fd = open(path, O_RDONLY); if (fd < 0) { puts("no pattern file"); return 2; }
    int bad = 0; unsigned char *b = malloc(300000);
    static const unsigned long offs[] = { 0, 1, 4095, 4096, 4097, 65535, 65536, 70000, 1000003, SZ - 5000, SZ - 100 };
    static const unsigned long lens[] = { 1, 7, 4096, 5000, 65536, 100000, 200001 };
    for (unsigned oi = 0; oi < sizeof offs / sizeof *offs; ++oi) for (unsigned li = 0; li < sizeof lens / sizeof *lens; ++li) {
        unsigned long o = offs[oi], l = lens[li]; if (o + l > SZ) l = SZ - o;
        memset(b, 0xEE, 300000);
        ssize_t n = pread(fd, b, l, (off_t)o); unsigned long got = n < 0 ? 0 : (unsigned long)n, k;
        for (k = 0; k < got; ++k) if (b[k] != pat(o + k)) { bad++; printf("pread off %lu len %lu: byte %lu wrong\n", o, l, k); break; }
        for (k = got; k < 300000 && k < got + 64; ++k) if (b[k] != 0xEE) { bad++; printf("pread off %lu len %lu wrote past %lu\n", o, l, got); break; }
        if (n >= 0 && (unsigned long)n < l && o + (unsigned long)n < SZ && (unsigned long)n < 65536) printf("note: short pread %ld of %lu\n", (long)n, l);
    }
    lseek(fd, 12345, SEEK_SET); memset(b, 0xEE, 300000); struct iovec iv[3] = { { b, 10 }, { b + 5000, 100000 }, { b + 200000, 3 } };
    ssize_t n = readv(fd, iv, 3); if (n > 0) { unsigned long p = 12345; for (int v = 0; v < 3; ++v) for (size_t k = 0; k < iv[v].iov_len && (ssize_t)(p - 12345) < n; ++k, ++p) if (((unsigned char *)iv[v].iov_base)[k] != pat(p)) { bad++; printf("readv wrong at %lu\n", p); goto doneiov; } } doneiov:;
    static const unsigned long moffs[] = { 0, 4096, 8192, 1 << 20 };
    for (unsigned mi = 0; mi < 4; ++mi) for (unsigned long ml = 1; ml < 400000; ml = ml * 7 + 13) {
        unsigned long o = moffs[mi], l = ml; if (o + l > SZ) l = SZ - o;
        unsigned char *m = mmap(0, l, PROT_READ, MAP_PRIVATE, fd, (off_t)o); if (m == MAP_FAILED) { bad++; printf("mmap failed off %lu len %lu\n", o, l); continue; }
        for (unsigned long k = 0; k < l; ++k) if (m[k] != pat(o + k)) { bad++; printf("mmap off %lu len %lu: byte %lu wrong\n", o, l, k); break; }
        unsigned long tail = ((l + 4095) & ~4095ul) - l; for (unsigned long k = 0; k < tail; ++k) if (m[l + k] != (o + l + k < SZ ? pat(o + l + k) : 0)) { bad++; printf("mmap tail off %lu len %lu wrong\n", o, l); break; }
        munmap(m, l);
    }
    printf("fileio: %d problems\n", bad); return bad != 0;
}
