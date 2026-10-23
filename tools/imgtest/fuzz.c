/* fuzz - robustness test for the C-OS image decoders (run under ASan/UBSan).
 *
 *   fuzz [-n iters_per_seed] [-t max_seconds] [-s rng_seed] [-m max_dim] seed-files...
 *
 * For every seed file: (1) decode it truncated at EVERY length (files above
 * 64 KiB use ~4000 evenly spread lengths), (2) apply -n random mutations
 * (byte flips, field smashing, chunk insert/delete/duplicate; PNG chunk CRCs
 * are repaired afterwards so mutations reach the DEFLATE/filter code),
 * (3) run a set of hand-built "decompression bomb" headers.  Every decode must
 * return without crashing, without hanging (5 s watchdog), without leaking
 * (live kmalloc count back to 0) and without absurd allocations.
 * The offending input is written to fuzz-fail-<n>.bin; exit status 1 on failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include "png_decoder.h"
#include "jpeg_codec.h"
#include "bmp_codec.h"
#include "memory.h"

static const uint8_t* g_cur; static size_t g_len; static const char* g_what = "";
static int g_failures = 0;

static void dump_input(void) {
    if (!g_cur) return;
    char name[64];
    snprintf(name, sizeof name, "fuzz-fail-%d.bin", g_failures);
    FILE* f = fopen(name, "wb");
    if (f) { fwrite(g_cur, 1, g_len, f); fclose(f); }
    fprintf(stderr, "[fuzz] failing input (%s) saved to %s\n", g_what, name);
}
extern void __sanitizer_set_death_callback(void (*cb)(void)) __attribute__((weak));
static void on_alarm(int sig) {
    (void)sig;
    fprintf(stderr, "[fuzz] HANG (>5 s) in %s\n", g_what);
    dump_input();
    _exit(3);
}

static uint64_t rng_s = 88172645463325252ull;
static uint64_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return rng_s; }

static uint32_t crc_tab[256];
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; }
}
static uint32_t crc32b(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu; while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8); return ~c;
}
static void png_fix_crcs(uint8_t* d, size_t n) {
    size_t pos = 8;
    while (pos + 12 <= n) {
        uint32_t len = ((uint32_t)d[pos] << 24) | ((uint32_t)d[pos+1] << 16) | ((uint32_t)d[pos+2] << 8) | d[pos+3];
        if (len > n || pos + 12 + len > n) break;
        uint32_t c = crc32b(d + pos + 4, 4 + len);
        d[pos+8+len] = c >> 24; d[pos+9+len] = c >> 16; d[pos+10+len] = c >> 8; d[pos+11+len] = c;
        pos += 12 + len;
    }
}

static uint8_t* g_out; static size_t g_out_cap;
static long g_calls = 0;
static size_t g_max_dim = 300;

static int decode_one(const uint8_t* d, size_t n, const char* what) {
    g_cur = d; g_len = n; g_what = what;
    size_t need = g_max_dim * g_max_dim * 4;
    if (g_out_cap < need) { free(g_out); g_out = (uint8_t*)malloc(need); g_out_cap = need; }
    struct itimerval it = {{0, 0}, {5, 0}};
    setitimer(ITIMER_REAL, &it, NULL);
    imgtest_peak_bytes = imgtest_live_bytes;
    uint64_t w, h, sw, sh;
    jpeg_info_t ji;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) {
        cos_jpeg_codec_decode(d, n, g_out, g_max_dim, g_max_dim, 0, &ji);
        cos_jpeg_codec_decode(d, n, g_out, g_max_dim, g_max_dim, JPEG_DEC_FIT, &ji);
        cos_jpeg_codec_decode(d, n, g_out, 17, 11, JPEG_DEC_FIT | JPEG_DEC_NO_ORIENT, &ji);
        cos_jpeg_codec_probe(d, n, &ji);
    } else if (n >= 8 && d[0] == 0x89) {
        png_decode(d, n, g_out, g_max_dim, g_max_dim, &w, &h);
        cos_png_decode_fit(d, n, g_out, 37, 23, &w, &h, &sw, &sh);
        cos_png_probe(d, n, (uint32_t*)&sw, (uint32_t*)&sh);
    } else if (n >= 2 && d[0] == 'B') {
        cos_bmp_decode(d, n, g_out, g_max_dim, g_max_dim, &w, &h);
        cos_bmp_decode_fit(d, n, g_out, 29, 31, &w, &h, &sw, &sh);
        cos_bmp_probe(d, n, (uint32_t*)&sw, (uint32_t*)&sh);
    } else if (n >= 2 && d[0] == 0x1f) {
        uint64_t len;
        gzip_inflate(d, n, g_out, g_out_cap < 70000 ? g_out_cap : 70000, &len);
        gzip_inflate(d, n, g_out, 1000, &len);
    }
    struct itimerval off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &off, NULL);
    g_calls++;
    int bad = 0;
    if (imgtest_live_allocs != 0) { fprintf(stderr, "[fuzz] LEAK (%ld blocks) in %s\n", imgtest_live_allocs, what); bad = 1; }
    if (imgtest_peak_bytes > (size_t)400 << 20) { fprintf(stderr, "[fuzz] huge allocation %zu in %s\n", imgtest_peak_bytes, what); bad = 1; }
    if (bad) { dump_input(); g_failures++; imgtest_live_allocs = 0; imgtest_live_bytes = 0; }
    return bad;
}

static uint8_t* load(const char* p, size_t* n) {
    FILE* f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc((size_t)l + 1);
    if (fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}

static void mutate(uint8_t* d, size_t* np, size_t cap) {
    size_t n = *np;
    int k = 1 + (int)(rnd() % 4);
    while (k--) {
        if (n == 0) break;
        switch (rnd() % 9) {
        case 0: d[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: d[rnd() % n] = (uint8_t)rnd(); break;
        case 2: d[rnd() % n] = 0xFF; break;
        case 3: d[rnd() % n] = 0; break;
        case 4: {
            static const uint32_t v[] = {0, 1, 0xFF, 0xFFFF, 0x7FFF, 0x8000, 0x10000, 0x7FFFFFFF, 0xFFFFFFFF, 0x40000000};
            size_t p = rnd() % n; uint32_t x = v[rnd() % 10];
            for (int i = 0; i < 4 && p + i < n; ++i) d[p + i] = (uint8_t)(x >> (8 * (rnd() & 1 ? i : 3 - i)));
            break; }
        case 5: {
            size_t p = rnd() % n, l = 1 + rnd() % 64; if (p + l > n) l = n - p;
            memmove(d + p, d + p + l, n - p - l); n -= l; break; }
        case 6: {
            size_t p = rnd() % n, l = 1 + rnd() % 64; if (p + l > n) l = n - p;
            if (n + l <= cap) { memmove(d + p + l, d + p, n - p); n += l; }
            break; }
        case 7: {
            size_t p = rnd() % n, q = rnd() % n, l = 1 + rnd() % 32;
            if (p + l > n) l = n - p;
            if (q + l > n) l = n - q;
            memmove(d + q, d + p, l); break; }
        default: if (n > 4) { n -= 1 + rnd() % (n / 2 > 1 ? n / 2 : 1); } break;
        }
    }
    *np = n;
}

static void put32be(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static int bombs(void) {
    int bad = 0;
    struct { const char* name; uint8_t* d; size_t n; } t[16]; int nt = 0;
    for (int v = 0; v < 3; ++v) {            /* PNG: huge dims + a handful of IDAT bytes */
        uint8_t* p = (uint8_t*)calloc(1, 80);
        static const uint8_t sig[8] = {0x89,'P','N','G',13,10,0x1a,10};
        memcpy(p, sig, 8);
        put32be(p + 8, 13); memcpy(p + 12, "IHDR", 4);
        put32be(p + 16, v == 2 ? 0x7FFFFFFFu : 65535); put32be(p + 20, v == 2 ? 0x7FFFFFFFu : 65535);
        p[24] = v == 1 ? 8 : 16; p[25] = 6; p[28] = v == 1 ? 1 : 0;
        put32be(p + 29, crc32b(p + 12, 17));
        put32be(p + 33, 6); memcpy(p + 37, "IDAT", 4);
        p[41] = 0x78; p[42] = 0x01; p[43] = 0x03; p[44] = 0; p[45] = 0; p[46] = 0;
        put32be(p + 47, crc32b(p + 37, 10));
        put32be(p + 51, 0); memcpy(p + 55, "IEND", 4); put32be(p + 59, crc32b(p + 55, 4));
        t[nt].name = "png-bomb"; t[nt].d = p; t[nt].n = 63; nt++;
    }
    {                                         /* JPEG: SOF0 65535x65535 / 20000x20000, 3 comps 4:4:4 */
        static const uint8_t j[] = {
            0xFF,0xD8, 0xFF,0xDB,0,67,0, 16,11,10,16,24,40,51,61,12,12,14,19,26,58,60,55,14,13,16,24,40,57,69,56,14,17,22,29,51,87,80,62,18,22,37,56,68,109,103,77,24,35,55,64,81,104,113,92,49,64,78,87,103,121,120,101,72,92,95,98,112,100,103,99,
            0xFF,0xC0,0,17,8,0xFF,0xFF,0xFF,0xFF,3, 1,0x11,0, 2,0x11,0, 3,0x11,0,
            0xFF,0xDA,0,12,3, 1,0x00, 2,0x00, 3,0x00, 0,63,0, 0x12,0x34,0x56, 0xFF,0xD9 };
        uint8_t* p = (uint8_t*)malloc(sizeof j); memcpy(p, j, sizeof j);
        t[nt].name = "jpeg-bomb-65535"; t[nt].d = p; t[nt].n = sizeof j; nt++;
        uint8_t* q = (uint8_t*)malloc(sizeof j); memcpy(q, j, sizeof j);
        q[2+2+2+65+5+3] = 0x4E; q[2+2+2+65+5+4] = 0x20; q[2+2+2+65+5+5] = 0x4E; q[2+2+2+65+5+6] = 0x20;
        t[nt].name = "jpeg-bomb-20000"; t[nt].d = q; t[nt].n = sizeof j; nt++;
    }
    for (int v = 0; v < 2; ++v) {             /* BMP: 65535x65535 24-bit / RLE8 */
        uint8_t* p = (uint8_t*)calloc(1, 64);
        p[0] = 'B'; p[1] = 'M'; p[10] = 54; p[14] = 40;
        p[18] = 0xFF; p[19] = 0xFF; p[22] = 0xFF; p[23] = 0xFF; p[26] = 1; p[28] = v ? 8 : 24; p[30] = v ? 1 : 0;
        t[nt].name = "bmp-bomb"; t[nt].d = p; t[nt].n = 64; nt++;
    }
    for (int i = 0; i < nt; ++i) {
        clock_t c0 = clock();
        bad |= decode_one(t[i].d, t[i].n, t[i].name);
        double s = (double)(clock() - c0) / CLOCKS_PER_SEC;
        if (s > 2.0) { fprintf(stderr, "[fuzz] bomb %d too slow (%.1fs)\n", i, s); bad = 1; g_failures++; }
        free(t[i].d);
    }
    return bad;
}

int main(int argc, char** argv) {
    long iters = 2000; double max_sec = 1e9; int first = 1;
    for (; first < argc && argv[first][0] == '-'; first += 2) {
        if (!strcmp(argv[first], "-n")) iters = atol(argv[first + 1]);
        else if (!strcmp(argv[first], "-t")) max_sec = atof(argv[first + 1]);
        else if (!strcmp(argv[first], "-s")) rng_s ^= strtoull(argv[first + 1], NULL, 10) * 0x9E3779B97F4A7C15ull;
        else if (!strcmp(argv[first], "-m")) g_max_dim = (size_t)atol(argv[first + 1]);
    }
    crc_init();
    signal(SIGALRM, on_alarm);
    if (__sanitizer_set_death_callback) __sanitizer_set_death_callback(dump_input);
    time_t t0 = time(NULL);
    long trunc_cases = 0, mut_cases = 0;
    for (int a = first; a < argc; ++a) {
        size_t n; uint8_t* seed = load(argv[a], &n);
        if (!seed) { fprintf(stderr, "cannot read %s\n", argv[a]); continue; }
        int is_png = n > 8 && seed[0] == 0x89;
        size_t step = n <= 65536 ? 1 : n / 4000;
        uint8_t* buf = (uint8_t*)malloc(n + 4096);
        for (size_t l = 0; l <= n; l += step) {
            memcpy(buf, seed, l);
            decode_one(buf, l, "truncated seed");
            trunc_cases++;
            if (difftime(time(NULL), t0) > max_sec) break;
        }
        for (long it = 0; it < iters && difftime(time(NULL), t0) <= max_sec; ++it) {
            size_t m = n; memcpy(buf, seed, n);
            mutate(buf, &m, n + 4096);
            if (is_png && (rnd() & 3)) png_fix_crcs(buf, m);
            decode_one(buf, m, "mutated seed");
            mut_cases++;
        }
        free(buf); free(seed);
    }
    bombs();
    printf("fuzz: %ld truncation cases, %ld mutation cases, %ld decode calls, %d failures\n",
           trunc_cases, mut_cases, g_calls, g_failures);
    return g_failures ? 1 : 0;
}
