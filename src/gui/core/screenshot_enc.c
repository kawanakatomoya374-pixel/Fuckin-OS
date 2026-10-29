/* screenshot_enc.c - image encoders for screenshots: PNG (deflate from userland/lib/cosarc.c), JPEG (baseline, 4:4:4, standard Huffman tables) and WebP (VP8L lossless, no
 * prediction: every channel byte as an 8-bit prefix code - a valid file every decoder reads, about the size of the raw pixels). Input: w*h pixels as 0x00RRGGBB.
 * Each function returns a malloc'ed buffer (free() it) and its size in *n, or NULL. Tested against Pillow (tools/ssenc_test.c). */
#pragma GCC optimize ("O2")
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "cosarc.h"
#include "screenshot_enc.h"

/* ---- a growing byte buffer ---- */
typedef struct { uint8_t *p; size_t n, cap; int bad; } bb_t;
static void bb_put(bb_t *b, const void *d, size_t n) {
    if (b->bad) return;
    if (b->n + n > b->cap) { size_t nc = b->cap ? b->cap : 4096; while (nc < b->n + n) nc *= 2; uint8_t *q = (uint8_t *)realloc(b->p, nc); if (!q) { b->bad = 1; return; } b->p = q; b->cap = nc; }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void bb_u8(bb_t *b, unsigned v) { uint8_t c = (uint8_t)v; bb_put(b, &c, 1); }
static void bb_be16(bb_t *b, unsigned v) { bb_u8(b, v >> 8); bb_u8(b, v); }
static void bb_be32(bb_t *b, uint32_t v) { bb_be16(b, v >> 16); bb_be16(b, v & 0xFFFF); }
static void bb_le32(bb_t *b, uint32_t v) { bb_u8(b, v); bb_u8(b, v >> 8); bb_u8(b, v >> 16); bb_u8(b, v >> 24); }

/* ================================================================== PNG */
typedef struct { const uint8_t *d; size_t n, pos; } mrd_t;
static size_t mrd(void *ctx, void *buf, size_t n) { mrd_t *m = (mrd_t *)ctx; size_t k = m->n - m->pos < n ? m->n - m->pos : n; memcpy(buf, m->d + m->pos, k); m->pos += k; return k; }
static int bbwr(void *ctx, const void *buf, size_t n) { bb_put((bb_t *)ctx, buf, n); return ((bb_t *)ctx)->bad ? 1 : 0; }
static void png_chunk(bb_t *o, const char *type, const uint8_t *d, size_t n) {
    bb_be32(o, (uint32_t)n); bb_put(o, type, 4); if (n) bb_put(o, d, n);
    uint32_t c = arc_crc32(0, type, 4); if (n) c = arc_crc32(c, d, n);
    bb_be32(o, c);
}
uint8_t *ss_encode_png(const uint32_t *px, int w, int h, size_t *n) {
    size_t row = 1 + (size_t)w * 3, rawn = row * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(rawn); if (!raw) return NULL;
    for (int y = 0; y < h; ++y) { uint8_t *r = raw + (size_t)y * row; r[0] = 0; for (int x = 0; x < w; ++x) { uint32_t c = px[(size_t)y * w + x]; r[1 + 3 * x] = (uint8_t)(c >> 16); r[2 + 3 * x] = (uint8_t)(c >> 8); r[3 + 3 * x] = (uint8_t)c; } }
    uint32_t a = 1, b = 0; for (size_t i = 0; i < rawn; ++i) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    bb_t z = { 0 }; bb_u8(&z, 0x78); bb_u8(&z, 0x01);
    mrd_t m = { raw, rawn, 0 }; uint64_t cs = 0;
    int rc = arc_deflate(mrd, &m, bbwr, &z, NULL, NULL, &cs, 3);
    free(raw);
    if (rc != 0 || z.bad) { free(z.p); return NULL; }
    bb_be32(&z, (b << 16) | a);
    bb_t o = { 0 }; static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A }; bb_put(&o, sig, 8);
    uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
    png_chunk(&o, "IHDR", ihdr, 13); png_chunk(&o, "IDAT", z.p, z.n); png_chunk(&o, "IEND", NULL, 0);
    free(z.p);
    if (o.bad) { free(o.p); return NULL; }
    *n = o.n; return o.p;
}

/* ================================================================== JPEG */
static const uint8_t k_zz[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
static const uint8_t k_ql[64] = { 16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99 };
static const uint8_t k_qc[64] = { 17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99 };
static const uint8_t k_dcl_b[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 }, k_dcc_b[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t k_dc_v[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t k_acl_b[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t k_acl_v[162] = { 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa };
static const uint8_t k_acc_b[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t k_acc_v[162] = { 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa };

typedef struct { uint16_t code[256]; uint8_t len[256]; } huff_t;
static void huff_build(huff_t *h, const uint8_t *bits, const uint8_t *vals) {
    memset(h, 0, sizeof *h); unsigned code = 0, k = 0;
    for (int l = 1; l <= 16; ++l) { for (int i = 0; i < bits[l - 1]; ++i) { h->code[vals[k]] = (uint16_t)code; h->len[vals[k]] = (uint8_t)l; code++; k++; } code <<= 1; }
}
typedef struct { bb_t *o; uint32_t acc; int nb; } bw_t;
static void bw_bits(bw_t *w, unsigned v, int n) {            /* MSB first, 0xFF is followed by a zero byte */
    w->acc = (w->acc << n) | (v & ((1u << n) - 1)); w->nb += n;
    while (w->nb >= 8) { unsigned byte = (w->acc >> (w->nb - 8)) & 0xFF; bb_u8(w->o, byte); if (byte == 0xFF) bb_u8(w->o, 0); w->nb -= 8; }
}
static void bw_flush(bw_t *w) { if (w->nb) bw_bits(w, 0x7F, 8 - w->nb); }
static int bitlen(int v) { int n = 0; if (v < 0) v = -v; while (v) { n++; v >>= 1; } return n; }

static void block_encode(bw_t *w, const float *blk, const uint16_t *q, int *pred, const huff_t *dc, const huff_t *ac, const float cs[8][8]) {
    float tmp[64]; int co[64];
    for (int y = 0; y < 8; ++y) for (int u = 0; u < 8; ++u) { float s = 0; for (int x = 0; x < 8; ++x) s += blk[y * 8 + x] * cs[u][x]; tmp[y * 8 + u] = s; }
    for (int v = 0; v < 8; ++v) for (int u = 0; u < 8; ++u) { float s = 0; for (int y = 0; y < 8; ++y) s += tmp[y * 8 + u] * cs[v][y]; float f = s / (float)q[v * 8 + u]; co[v * 8 + u] = (int)(f < 0 ? f - 0.5f : f + 0.5f); }
    int diff = co[0] - *pred; *pred = co[0];
    int nb = bitlen(diff); bw_bits(w, dc->code[nb], dc->len[nb]); if (nb) bw_bits(w, diff < 0 ? (unsigned)(diff - 1) : (unsigned)diff, nb);
    int run = 0;
    for (int k = 1; k < 64; ++k) {
        int c = co[k_zz[k]];
        if (!c) { run++; continue; }
        while (run > 15) { bw_bits(w, ac->code[0xF0], ac->len[0xF0]); run -= 16; }
        int s = bitlen(c); int sym = (run << 4) | s; bw_bits(w, ac->code[sym], ac->len[sym]); bw_bits(w, c < 0 ? (unsigned)(c - 1) : (unsigned)c, s); run = 0;
    }
    if (run) bw_bits(w, ac->code[0x00], ac->len[0x00]);
}
static void dqt(bb_t *o, int id, const uint16_t *q) { bb_be16(o, 0xFFDB); bb_be16(o, 67); bb_u8(o, id); for (int i = 0; i < 64; ++i) bb_u8(o, q[k_zz[i]]); }
static void dht(bb_t *o, int tc_th, const uint8_t *bits, const uint8_t *vals, int nv) { bb_be16(o, 0xFFC4); bb_be16(o, 2 + 1 + 16 + nv); bb_u8(o, tc_th); bb_put(o, bits, 16); bb_put(o, vals, (size_t)nv); }

uint8_t *ss_encode_jpeg(const uint32_t *px, int w, int h, int quality, size_t *n) {
    if (w < 1 || h < 1 || w > 65535 || h > 65535) return NULL;
    if (quality < 1) quality = 1; if (quality > 100) quality = 100;
    int sc = quality < 50 ? 5000 / quality : 200 - quality * 2;
    uint16_t ql[64], qc[64];
    for (int i = 0; i < 64; ++i) { int a = (k_ql[i] * sc + 50) / 100, b = (k_qc[i] * sc + 50) / 100; ql[i] = (uint16_t)(a < 1 ? 1 : a > 255 ? 255 : a); qc[i] = (uint16_t)(b < 1 ? 1 : b > 255 ? 255 : b); }
    static float cs[8][8]; static int cs_ok;
    if (!cs_ok) {   /* cs[u][x] = c(u)/2 * cos((2x+1) u pi / 16), by the recurrence of a cosine table (no libm in the kernel) */
        static const float t[8][8] = {
            { 0.353553391f, 0.353553391f, 0.353553391f, 0.353553391f, 0.353553391f, 0.353553391f, 0.353553391f, 0.353553391f },
            { 0.490392640f, 0.415734806f, 0.277785117f, 0.097545161f, -0.097545161f, -0.277785117f, -0.415734806f, -0.490392640f },
            { 0.461939766f, 0.191341716f, -0.191341716f, -0.461939766f, -0.461939766f, -0.191341716f, 0.191341716f, 0.461939766f },
            { 0.415734806f, -0.097545161f, -0.490392640f, -0.277785117f, 0.277785117f, 0.490392640f, 0.097545161f, -0.415734806f },
            { 0.353553391f, -0.353553391f, -0.353553391f, 0.353553391f, 0.353553391f, -0.353553391f, -0.353553391f, 0.353553391f },
            { 0.277785117f, -0.490392640f, 0.097545161f, 0.415734806f, -0.415734806f, -0.097545161f, 0.490392640f, -0.277785117f },
            { 0.191341716f, -0.461939766f, 0.461939766f, -0.191341716f, -0.191341716f, 0.461939766f, -0.461939766f, 0.191341716f },
            { 0.097545161f, -0.277785117f, 0.415734806f, -0.490392640f, 0.490392640f, -0.415734806f, 0.277785117f, -0.097545161f } };
        memcpy(cs, t, sizeof cs); cs_ok = 1;
    }
    huff_t hdl, hdc, hal, hac; huff_build(&hdl, k_dcl_b, k_dc_v); huff_build(&hdc, k_dcc_b, k_dc_v); huff_build(&hal, k_acl_b, k_acl_v); huff_build(&hac, k_acc_b, k_acc_v);
    bb_t o = { 0 };
    bb_be16(&o, 0xFFD8);
    bb_be16(&o, 0xFFE0); bb_be16(&o, 16); bb_put(&o, "JFIF\0", 5); bb_u8(&o, 1); bb_u8(&o, 1); bb_u8(&o, 0); bb_be16(&o, 1); bb_be16(&o, 1); bb_u8(&o, 0); bb_u8(&o, 0);
    dqt(&o, 0, ql); dqt(&o, 1, qc);
    bb_be16(&o, 0xFFC0); bb_be16(&o, 17); bb_u8(&o, 8); bb_be16(&o, (unsigned)h); bb_be16(&o, (unsigned)w); bb_u8(&o, 3);
    bb_u8(&o, 1); bb_u8(&o, 0x11); bb_u8(&o, 0); bb_u8(&o, 2); bb_u8(&o, 0x11); bb_u8(&o, 1); bb_u8(&o, 3); bb_u8(&o, 0x11); bb_u8(&o, 1);
    dht(&o, 0x00, k_dcl_b, k_dc_v, 12); dht(&o, 0x10, k_acl_b, k_acl_v, 162); dht(&o, 0x01, k_dcc_b, k_dc_v, 12); dht(&o, 0x11, k_acc_b, k_acc_v, 162);
    bb_be16(&o, 0xFFDA); bb_be16(&o, 12); bb_u8(&o, 3); bb_u8(&o, 1); bb_u8(&o, 0x00); bb_u8(&o, 2); bb_u8(&o, 0x11); bb_u8(&o, 3); bb_u8(&o, 0x11); bb_u8(&o, 0); bb_u8(&o, 63); bb_u8(&o, 0);
    bw_t bw = { &o, 0, 0 }; int pred[3] = { 0, 0, 0 };
    float blk[3][64];
    for (int by = 0; by < h; by += 8) for (int bx = 0; bx < w; bx += 8) {
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
            int sx = bx + x < w ? bx + x : w - 1, sy = by + y < h ? by + y : h - 1;
            uint32_t c = px[(size_t)sy * w + sx]; float r = (float)((c >> 16) & 255), g = (float)((c >> 8) & 255), b = (float)(c & 255);
            blk[0][y * 8 + x] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
            blk[1][y * 8 + x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
            blk[2][y * 8 + x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
        }
        block_encode(&bw, blk[0], ql, &pred[0], &hdl, &hal, cs); block_encode(&bw, blk[1], qc, &pred[1], &hdc, &hac, cs); block_encode(&bw, blk[2], qc, &pred[2], &hdc, &hac, cs);
    }
    bw_flush(&bw); bb_be16(&o, 0xFFD9);
    if (o.bad) { free(o.p); return NULL; }
    *n = o.n; return o.p;
}

/* ================================================================== WebP (VP8L, lossless) */
/* Subtract-green transform, LZ77 (hash chains over pixels, distance as "plane code" D + 120), and prefix codes built from the real histograms (Huffman, lengths <= 15). */
typedef struct { bb_t *o; uint64_t acc; int nb; } lw_t;     /* LSB first */
static void lw_bits(lw_t *w, uint32_t v, int n) { if (n <= 0) return; w->acc |= (uint64_t)(v & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1))) << w->nb; w->nb += n; while (w->nb >= 8) { bb_u8(w->o, (unsigned)(w->acc & 0xFF)); w->acc >>= 8; w->nb -= 8; } }
static void lw_msb(lw_t *w, unsigned code, int len) { for (int i = len - 1; i >= 0; --i) lw_bits(w, (code >> i) & 1, 1); }      /* a prefix code: its first bit is the MSB */

#define PC_MAX 280
typedef struct { uint8_t len[PC_MAX]; uint16_t code[PC_MAX]; int n; } pc_t;

/* Huffman code lengths for the histogram, none longer than maxbits (the histogram is flattened and the tree rebuilt until it fits) */
static void huff_lengths(const uint32_t *freq, int n, int maxbits, uint8_t *len) {
    uint32_t f[PC_MAX]; for (int i = 0; i < n; ++i) f[i] = freq[i];
    for (;;) {
        int w[2 * PC_MAX], par[2 * PC_MAX], alive[2 * PC_MAX], cnt = 0, nn = 0;
        for (int i = 0; i < n; ++i) { len[i] = 0; if (f[i]) { w[nn] = (int)f[i]; par[nn] = -1; alive[nn] = 1; nn++; cnt++; } }
        int sym[PC_MAX], k = 0; for (int i = 0; i < n; ++i) if (f[i]) sym[k++] = i;
        if (cnt == 0) return;
        if (cnt == 1) { len[sym[0]] = 1; return; }
        int total = nn;
        for (int merges = 0; merges < cnt - 1; ++merges) {
            int a = -1, b = -1;
            for (int i = 0; i < total; ++i) if (alive[i]) { if (a < 0 || w[i] < w[a]) { b = a; a = i; } else if (b < 0 || w[i] < w[b]) b = i; }
            w[total] = w[a] + w[b]; par[total] = -1; alive[total] = 1; par[a] = total; par[b] = total; alive[a] = alive[b] = 0; total++;
        }
        int maxd = 0;
        for (int i = 0; i < cnt; ++i) { int d = 0; for (int x = i; par[x] >= 0; x = par[x]) d++; len[sym[i]] = (uint8_t)d; if (d > maxd) maxd = d; }
        if (maxd <= maxbits) return;
        for (int i = 0; i < n; ++i) if (f[i]) f[i] = (f[i] + 1) / 2;
    }
}
static void pc_assign(pc_t *p) {
    int bl[17] = { 0 }; unsigned next[17];
    for (int i = 0; i < p->n; ++i) bl[p->len[i]]++;
    bl[0] = 0; unsigned c = 0; for (int l = 1; l <= 16; ++l) { c = (c + (unsigned)bl[l - 1]) << 1; next[l] = c; }
    for (int i = 0; i < p->n; ++i) if (p->len[i]) p->code[i] = (uint16_t)next[p->len[i]]++;
}
static const uint8_t k_cl_order[19] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
/* writes the code (histogram -> lengths -> bits of the description); p is ready for pc_put afterwards */
static void pc_write(lw_t *w, pc_t *p, const uint32_t *freq, int n) {
    p->n = n; huff_lengths(freq, n, 15, p->len);
    int used = 0, only = 0; for (int i = 0; i < n; ++i) if (p->len[i]) { used++; only = i; }
    if (used <= 1) {                                    /* a simple code with one symbol: no bits per symbol */
        if (used == 0) only = 0;
        lw_bits(w, 1, 1); lw_bits(w, 0, 1);
        if (only < 2) { lw_bits(w, 0, 1); lw_bits(w, (unsigned)only, 1); } else { lw_bits(w, 1, 1); lw_bits(w, (unsigned)only, 8); }
        memset(p->len, 0, sizeof p->len); return;
    }
    pc_assign(p);
    /* the lengths as tokens: 0-15 literal, 16 repeat the last non-zero 3-6 times, 17 zeros 3-10, 18 zeros 11-138 */
    uint8_t tk[PC_MAX], te[PC_MAX]; int nt = 0; uint32_t cf[19] = { 0 }; int prev = 8;
    for (int i = 0; i < n; ) {
        int v = p->len[i], run = 1; while (i + run < n && p->len[i + run] == v) run++;
        if (v == 0) {
            int r = run; while (r >= 3) { if (r >= 11) { int c = r > 138 ? 138 : r; tk[nt] = 18; te[nt++] = (uint8_t)(c - 11); r -= c; } else { tk[nt] = 17; te[nt++] = (uint8_t)(r - 3); r = 0; } }
            for (; r > 0; --r) { tk[nt] = 0; te[nt++] = 0; }
        } else {
            int r = run; if (v != prev) { tk[nt] = (uint8_t)v; te[nt++] = 0; r--; prev = v; }
            while (r >= 3) { int c = r > 6 ? 6 : r; tk[nt] = 16; te[nt++] = (uint8_t)(c - 3); r -= c; }
            for (; r > 0; --r) { tk[nt] = (uint8_t)v; te[nt++] = 0; }
        }
        i += run;
    }
    for (int i = 0; i < nt; ++i) cf[tk[i]]++;
    { int u = 0; for (int i = 0; i < 19; ++i) if (cf[i]) u++; if (u < 2) { if (!cf[0]) cf[0] = 1; else cf[1] = 1; } }
    pc_t cl; cl.n = 19; huff_lengths(cf, 19, 7, cl.len); pc_assign(&cl);
    int ncl = 19; while (ncl > 4 && cl.len[k_cl_order[ncl - 1]] == 0) ncl--;
    lw_bits(w, 0, 1); lw_bits(w, (unsigned)(ncl - 4), 4);
    for (int i = 0; i < ncl; ++i) lw_bits(w, cl.len[k_cl_order[i]], 3);
    lw_bits(w, 0, 1);                                   /* max_symbol = the whole alphabet */
    for (int i = 0; i < nt; ++i) { lw_msb(w, cl.code[tk[i]], cl.len[tk[i]]); if (tk[i] == 16) lw_bits(w, te[i], 2); else if (tk[i] == 17) lw_bits(w, te[i], 3); else if (tk[i] == 18) lw_bits(w, te[i], 7); }
}
static void pc_put(lw_t *w, const pc_t *p, int sym) { if (p->len[sym]) lw_msb(w, p->code[sym], p->len[sym]); }

/* value >= 1 -> prefix symbol, extra bits count and value (the LZ77 length / distance coding of VP8L) */
static void prefix_of(unsigned v, int *code, int *nex, unsigned *ev) {
    if (v <= 4) { *code = (int)v - 1; *nex = 0; *ev = 0; return; }
    unsigned d = v - 1; int hb = 31 - __builtin_clz(d); int second = (int)((d >> (hb - 1)) & 1);
    *nex = hb - 1; *ev = d & ((1u << *nex) - 1); *code = 2 * hb + second;
}
typedef struct { uint32_t a, b; } tok_t;            /* literal: a = pixel, b = 0xFFFFFFFF; copy: a = length, b = distance */

uint8_t *ss_encode_webp(const uint32_t *px0, int w, int h, size_t *n) {
    if (w < 1 || h < 1 || w > 16384 || h > 16384) return NULL;
    size_t np = (size_t)w * (size_t)h;
    uint32_t *px = (uint32_t *)malloc(np * 4); tok_t *tk = (tok_t *)malloc(np * sizeof(tok_t)); int32_t *prev = (int32_t *)malloc(np * 4); int32_t *head = (int32_t *)malloc(65536 * 4);
    if (!px || !tk || !prev || !head) { free(px); free(tk); free(prev); free(head); return NULL; }
    for (size_t i = 0; i < np; ++i) { uint32_t c = px0[i]; uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255; px[i] = 0xFF000000u | (((r - g) & 255) << 16) | (g << 8) | ((b - g) & 255); }   /* subtract green */
    memset(head, 0xFF, 65536 * 4);
    size_t ntk = 0;
    uint32_t fg[280] = { 0 }, fr[256] = { 0 }, fb[256] = { 0 }, fa[256] = { 0 }, fd[40] = { 0 };
    for (size_t i = 0; i < np; ) {
        int bestl = 0; size_t bestd = 0;
        if (i + 2 < np) {
            uint32_t hk = ((px[i] * 0x9E3779B1u) ^ (px[i + 1] * 0x85EBCA6Bu) ^ (px[i + 2] * 0xC2B2AE35u)) >> 16;
            int32_t j = head[hk]; int chain = 0;
            while (j >= 0 && chain < 24 && i - (size_t)j < (1u << 19)) {
                int l = 0; while (l < 4096 && i + (size_t)l < np && px[(size_t)j + l] == px[i + l]) l++;
                if (l > bestl) { bestl = l; bestd = i - (size_t)j; if (l >= 4096) break; }
                j = prev[j]; chain++;
            }
        }
        size_t adv = 1;
        if (bestl >= 3) { tk[ntk].a = (uint32_t)bestl; tk[ntk].b = (uint32_t)bestd; adv = (size_t)bestl; }
        else { tk[ntk].a = px[i]; tk[ntk].b = 0xFFFFFFFFu; }
        for (size_t q = 0; q < adv && i + q + 2 < np; ++q) { size_t pos = i + q; uint32_t hk = ((px[pos] * 0x9E3779B1u) ^ (px[pos + 1] * 0x85EBCA6Bu) ^ (px[pos + 2] * 0xC2B2AE35u)) >> 16; prev[pos] = head[hk]; head[hk] = (int32_t)pos; }
        if (tk[ntk].b == 0xFFFFFFFFu) { uint32_t c = tk[ntk].a; fg[(c >> 8) & 255]++; fr[(c >> 16) & 255]++; fb[c & 255]++; fa[c >> 24]++; }
        else { int code, nex; unsigned ev; prefix_of(tk[ntk].a, &code, &nex, &ev); fg[256 + code]++; prefix_of(tk[ntk].b + 120, &code, &nex, &ev); fd[code]++; }
        ntk++; i += adv;
    }
    free(prev); free(head);
    bb_t v = { 0 }; lw_t lw = { &v, 0, 0 };
    lw_bits(&lw, 0x2F, 8); lw_bits(&lw, (uint32_t)(w - 1), 14); lw_bits(&lw, (uint32_t)(h - 1), 14); lw_bits(&lw, 0, 1); lw_bits(&lw, 0, 3);
    lw_bits(&lw, 1, 1); lw_bits(&lw, 2, 2); lw_bits(&lw, 0, 1);       /* one transform: subtract green; no more */
    lw_bits(&lw, 0, 1);                                               /* no color cache */
    lw_bits(&lw, 0, 1);                                               /* one set of prefix codes */
    pc_t pg, pr, pb, pa, pd;
    pc_write(&lw, &pg, fg, 280); pc_write(&lw, &pr, fr, 256); pc_write(&lw, &pb, fb, 256); pc_write(&lw, &pa, fa, 256); pc_write(&lw, &pd, fd, 40);
    for (size_t t = 0; t < ntk; ++t) {
        if (tk[t].b == 0xFFFFFFFFu) { uint32_t c = tk[t].a; pc_put(&lw, &pg, (c >> 8) & 255); pc_put(&lw, &pr, (c >> 16) & 255); pc_put(&lw, &pb, c & 255); pc_put(&lw, &pa, (int)(c >> 24)); }
        else {
            int code, nex; unsigned ev;
            prefix_of(tk[t].a, &code, &nex, &ev); pc_put(&lw, &pg, 256 + code); lw_bits(&lw, ev, nex);
            prefix_of(tk[t].b + 120, &code, &nex, &ev); pc_put(&lw, &pd, code); lw_bits(&lw, ev, nex);
        }
    }
    if (lw.nb) lw_bits(&lw, 0, 8 - lw.nb);
    free(px); free(tk);
    if (v.bad) { free(v.p); return NULL; }
    bb_t o = { 0 }; size_t padded = v.n + (v.n & 1);
    bb_put(&o, "RIFF", 4); bb_le32(&o, (uint32_t)(4 + 8 + padded)); bb_put(&o, "WEBP", 4); bb_put(&o, "VP8L", 4); bb_le32(&o, (uint32_t)v.n); bb_put(&o, v.p, v.n); if (v.n & 1) bb_u8(&o, 0);
    free(v.p);
    if (o.bad) { free(o.p); return NULL; }
    *n = o.n; return o.p;
}
