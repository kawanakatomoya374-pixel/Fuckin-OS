/**
 * png_decoder.c - see png_decoder.h
 *
 * Layout of this file:
 *   1. CRC-32 (slice-by-8) and Adler-32
 *   2. DEFLATE core: 64-bit bit buffer, table-driven Huffman decode, 32 KiB
 *      sliding window flushed to a sink callback (so the same core serves
 *      PNG's row-streaming decoder and gzip_inflate()'s flat output)
 *   3. gzip_inflate()
 *   4. PNG chunk scan, row un-filter, colour conversion, Adam7, scaling
 */
#include "png_decoder.h"
#include "image_scale.h"
#include "memory.h"
#include "serial.h"
#include <string.h>
#include <stdint.h>

/* ============================================================================
   1. Checksums
   ========================================================================== */
static uint32_t g_crc_tab[8][256];
static volatile int g_crc_ready = 0;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tab[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = g_crc_tab[0][i];
        for (int t = 1; t < 8; ++t) {
            c = g_crc_tab[0][c & 0xFF] ^ (c >> 8);
            g_crc_tab[t][i] = c;
        }
    }
    g_crc_ready = 1;
}

static uint32_t rd_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Standard CRC-32 (init/final xor handled by the caller via ~). */
static uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
    if (!g_crc_ready) crc_init();
    crc = ~crc;
    while (n >= 8) {
        uint32_t lo = crc ^ rd_le32(p), hi = rd_le32(p + 4);
        crc = g_crc_tab[7][lo & 0xFF] ^ g_crc_tab[6][(lo >> 8) & 0xFF] ^
              g_crc_tab[5][(lo >> 16) & 0xFF] ^ g_crc_tab[4][lo >> 24] ^
              g_crc_tab[3][hi & 0xFF] ^ g_crc_tab[2][(hi >> 8) & 0xFF] ^
              g_crc_tab[1][(hi >> 16) & 0xFF] ^ g_crc_tab[0][hi >> 24];
        p += 8; n -= 8;
    }
    while (n--) crc = g_crc_tab[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint32_t adler32_update(uint32_t adler, const uint8_t* p, size_t n) {
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    while (n) {
        size_t k = n < 5552 ? n : 5552;      /* largest n with no 32-bit overflow */
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521u; b %= 65521u;
    }
    return (b << 16) | a;
}

/* ============================================================================
   2. DEFLATE core
   ========================================================================== */
#define INF_WIN     65536u       /* window buffer: 32 KiB history + 32 KiB new output */
#define INF_HIST    32768u
#define INF_SLACK   288u         /* > longest match (258) */
#define LIT_FBITS   10
#define DST_FBITS   9
#define CL_FBITS    7

#define INF_OK     0
#define INF_STOP   1
#define INF_ERR   (-1)

typedef bool (*inf_sink_fn)(void* ctx, const uint8_t* p, size_t n);

typedef struct {
    const uint8_t* in;
    const uint8_t* in_end;
    uint64_t bitbuf;
    int bitcnt;
    size_t pos, flushed;
    inf_sink_fn sink;
    void* sink_ctx;
    uint16_t lit_fast[1u << LIT_FBITS];
    uint16_t dst_fast[1u << DST_FBITS];
    uint16_t cl_fast[1u << CL_FBITS];
    uint16_t lit_cnt[16], dst_cnt[16], cl_cnt[16];
    uint16_t lit_sym[288], dst_sym[32], cl_sym[19];
    uint8_t win[INF_WIN];
} inflate_t;

static const uint16_t k_len_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  k_len_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t k_dst_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  k_dst_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static const uint8_t  k_cl_order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

static void inf_init(inflate_t* s, const uint8_t* in, size_t n, inf_sink_fn sink, void* ctx) {
    s->in = in; s->in_end = in + n;
    s->bitbuf = 0; s->bitcnt = 0;
    s->pos = s->flushed = 0;
    s->sink = sink; s->sink_ctx = ctx;
}

/* Tops the bit buffer up to >= 56 bits when at least 8 input bytes remain
 * (otherwise as many as are left).  Bits above bitcnt are either zero or an
 * exact copy of the upcoming input bits, so re-ORing the same byte is
 * idempotent. */
static inline void inf_refill(inflate_t* s) {
    if (s->in_end - s->in >= 8) {
        const uint8_t* p = s->in;
        uint64_t w = (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
        s->bitbuf |= w << s->bitcnt;
        s->in += (63 - s->bitcnt) >> 3;
        s->bitcnt |= 56;
    } else {
        while (s->bitcnt < 56 && s->in < s->in_end) {
            s->bitbuf |= (uint64_t)*s->in++ << s->bitcnt;
            s->bitcnt += 8;
        }
    }
}

static inline uint32_t bitrev(uint32_t v, int n) {
    uint32_t r = 0;
    while (n--) { r = (r << 1) | (v & 1u); v >>= 1; }
    return r;
}

/* Builds a canonical Huffman decoder.  Returns 0 for a usable code, 1 for an
 * empty code (no symbols), -1 for an over-subscribed or (except the single
 * 1-bit code RFC 1951 allows) incomplete set.  Fast-table entry =
 * (symbol << 4) | length, 0 = "longer than fbits / invalid". */
static int huff_build(const uint8_t* lens, int n, uint16_t* fast, int fbits,
                      uint16_t* cnt, uint16_t* sym, int allow_single) {
    uint16_t offs[16];
    memset(cnt, 0, 16 * sizeof(uint16_t));
    for (int i = 0; i < n; ++i) cnt[lens[i]]++;
    memset(fast, 0, ((size_t)1 << fbits) * sizeof(uint16_t));
    int total = n - cnt[0];
    cnt[0] = 0;
    if (total == 0) return 1;
    int left = 1;
    for (int l = 1; l <= 15; ++l) {
        left <<= 1;
        left -= cnt[l];
        if (left < 0) return -1;
    }
    if (left > 0 && !(allow_single && total == 1 && cnt[1] == 1)) return -1;

    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = (uint16_t)(offs[l] + cnt[l]);
    uint32_t next_code[16];
    uint32_t code = 0;
    for (int l = 1; l <= 15; ++l) {
        code = (code + cnt[l - 1]) << 1;       /* cnt[0] == 0 */
        next_code[l] = code;
    }
    for (int i = 0; i < n; ++i) {
        int l = lens[i];
        if (!l) continue;
        sym[offs[l]++] = (uint16_t)i;
        uint32_t c = next_code[l]++;
        if (l <= fbits) {
            uint32_t r = bitrev(c, l);
            uint16_t e = (uint16_t)((i << 4) | l);
            for (uint32_t k = r; k < (1u << fbits); k += (1u << l)) fast[k] = e;
        }
    }
    return 0;
}

static inline int huff_decode(inflate_t* s, const uint16_t* fast, int fbits,
                              const uint16_t* cnt, const uint16_t* sym) {
    uint16_t e = fast[s->bitbuf & ((1u << fbits) - 1u)];
    if (e) {
        int l = e & 15;
        if (l > s->bitcnt) return -1;
        s->bitbuf >>= l; s->bitcnt -= l;
        return e >> 4;
    }
    /* Slow canonical path for codes longer than fbits (and invalid codes). */
    int code = 0, first = 0, index = 0;
    uint64_t b = s->bitbuf;
    for (int len = 1; len <= 15; ++len) {
        if (len > s->bitcnt) return -1;
        code |= (int)(b & 1u); b >>= 1;
        int count = cnt[len];
        if (code - first < count) {
            s->bitbuf >>= len; s->bitcnt -= len;
            return sym[index + (code - first)];
        }
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

/* Hands win[flushed..pos) to the sink and slides the window so that the last
 * 32 KiB stay available for back-references.  Returns false if the sink asked
 * to stop. */
static bool inf_flush(inflate_t* s, bool slide) {
    size_t n = s->pos - s->flushed;
    bool more = true;
    if (n) more = s->sink(s->sink_ctx, s->win + s->flushed, n);
    s->flushed = s->pos;
    if (slide && s->pos > INF_HIST) {
        memmove(s->win, s->win + s->pos - INF_HIST, INF_HIST);
        s->pos = s->flushed = INF_HIST;
    }
    return more;
}

static int inf_codes(inflate_t* s, const uint16_t* lfast, const uint16_t* lcnt, const uint16_t* lsym,
                     const uint16_t* dfast, const uint16_t* dcnt, const uint16_t* dsym) {
    for (;;) {
        if (s->pos >= INF_WIN - INF_SLACK) {
            if (!inf_flush(s, true)) return INF_STOP;
        }
        inf_refill(s);
        int sym = huff_decode(s, lfast, LIT_FBITS, lcnt, lsym);
        if (sym < 0) return INF_ERR;
        if (sym < 256) { s->win[s->pos++] = (uint8_t)sym; continue; }
        if (sym == 256) return INF_OK;
        sym -= 257;
        if (sym >= 29) return INF_ERR;
        int eb = k_len_extra[sym];
        if (s->bitcnt < eb) return INF_ERR;
        int len = k_len_base[sym] + (int)(s->bitbuf & ((1u << eb) - 1u));
        s->bitbuf >>= eb; s->bitcnt -= eb;

        int ds = huff_decode(s, dfast, DST_FBITS, dcnt, dsym);
        if (ds < 0 || ds >= 30) return INF_ERR;
        eb = k_dst_extra[ds];
        if (s->bitcnt < eb) return INF_ERR;
        size_t dist = (size_t)k_dst_base[ds] + (size_t)(s->bitbuf & ((1u << eb) - 1u));
        s->bitbuf >>= eb; s->bitcnt -= eb;
        if (dist > s->pos) return INF_ERR;          /* reaches before the start of the data */

        uint8_t* d = s->win + s->pos;
        const uint8_t* src = d - dist;
        s->pos += (size_t)len;
        if (dist >= (size_t)len) {
            memcpy(d, src, (size_t)len);
        } else if (dist == 1) {
            memset(d, src[0], (size_t)len);
        } else {
            for (int i = 0; i < len; ++i) d[i] = src[i];
        }
    }
}

static int inf_stored(inflate_t* s) {
    int drop = s->bitcnt & 7;
    s->bitbuf >>= drop; s->bitcnt -= drop;
    /* Give back the whole bytes the bit buffer still holds. */
    s->in -= (size_t)(s->bitcnt >> 3);
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->in_end - s->in < 4) return INF_ERR;
    uint32_t len = (uint32_t)s->in[0] | ((uint32_t)s->in[1] << 8);
    uint32_t nlen = (uint32_t)s->in[2] | ((uint32_t)s->in[3] << 8);
    if ((len ^ nlen) != 0xFFFFu) return INF_ERR;
    s->in += 4;
    if ((size_t)(s->in_end - s->in) < len) return INF_ERR;   /* truncated stored block */
    while (len) {
        if (s->pos >= INF_WIN - INF_SLACK) {
            if (!inf_flush(s, true)) return INF_STOP;
        }
        size_t room = INF_WIN - s->pos;
        size_t n = len < room ? len : room;
        memcpy(s->win + s->pos, s->in, n);
        s->pos += n; s->in += n; len -= (uint32_t)n;
    }
    return INF_OK;
}

static int inf_dynamic(inflate_t* s) {
    inf_refill(s);
    if (s->bitcnt < 14) return INF_ERR;
    int hlit = (int)(s->bitbuf & 31u) + 257;
    int hdist = (int)((s->bitbuf >> 5) & 31u) + 1;
    int hclen = (int)((s->bitbuf >> 10) & 15u) + 4;
    s->bitbuf >>= 14; s->bitcnt -= 14;
    if (hlit > 286 || hdist > 30) return INF_ERR;

    uint8_t cl[19];
    memset(cl, 0, sizeof(cl));
    for (int i = 0; i < hclen; ++i) {
        inf_refill(s);
        if (s->bitcnt < 3) return INF_ERR;
        cl[k_cl_order[i]] = (uint8_t)(s->bitbuf & 7u);
        s->bitbuf >>= 3; s->bitcnt -= 3;
    }
    if (huff_build(cl, 19, s->cl_fast, CL_FBITS, s->cl_cnt, s->cl_sym, 0) != 0) return INF_ERR;

    uint8_t lens[320];
    int total = hlit + hdist, n = 0;
    while (n < total) {
        inf_refill(s);
        int sym = huff_decode(s, s->cl_fast, CL_FBITS, s->cl_cnt, s->cl_sym);
        if (sym < 0) return INF_ERR;
        if (sym < 16) { lens[n++] = (uint8_t)sym; continue; }
        int rep, val = 0, eb;
        if (sym == 16) {
            if (n == 0) return INF_ERR;
            val = lens[n - 1]; eb = 2; rep = 3;
        } else if (sym == 17) { eb = 3; rep = 3; }
        else { eb = 7; rep = 11; }
        if (s->bitcnt < eb) return INF_ERR;
        rep += (int)(s->bitbuf & ((1u << eb) - 1u));
        s->bitbuf >>= eb; s->bitcnt -= eb;
        if (n + rep > total) return INF_ERR;
        while (rep--) lens[n++] = (uint8_t)val;
    }
    if (lens[256] == 0) return INF_ERR;          /* no end-of-block code */
    if (huff_build(lens, hlit, s->lit_fast, LIT_FBITS, s->lit_cnt, s->lit_sym, 1) != 0) return INF_ERR;
    if (huff_build(lens + hlit, hdist, s->dst_fast, DST_FBITS, s->dst_cnt, s->dst_sym, 1) < 0) return INF_ERR;
    return INF_OK;
}

static int inf_fixed(inflate_t* s) {
    uint8_t lens[288];
    for (int i = 0; i < 144; ++i) lens[i] = 8;
    for (int i = 144; i < 256; ++i) lens[i] = 9;
    for (int i = 256; i < 280; ++i) lens[i] = 7;
    for (int i = 280; i < 288; ++i) lens[i] = 8;
    if (huff_build(lens, 288, s->lit_fast, LIT_FBITS, s->lit_cnt, s->lit_sym, 0) != 0) return INF_ERR;
    uint8_t dl[32];
    for (int i = 0; i < 32; ++i) dl[i] = 5;
    if (huff_build(dl, 32, s->dst_fast, DST_FBITS, s->dst_cnt, s->dst_sym, 0) != 0) return INF_ERR;
    return INF_OK;
}

/* Runs one complete DEFLATE stream.  INF_OK: final block decoded and flushed
 * (s->in / bit buffer then locate the trailer, see inf_tail()).  INF_STOP: the
 * sink stopped the stream early.  INF_ERR: corrupt/truncated. */
static int inf_stream(inflate_t* s) {
    for (;;) {
        inf_refill(s);
        if (s->bitcnt < 3) return INF_ERR;
        int final = (int)(s->bitbuf & 1u);
        int type = (int)((s->bitbuf >> 1) & 3u);
        s->bitbuf >>= 3; s->bitcnt -= 3;
        int r;
        if (type == 0) r = inf_stored(s);
        else if (type == 1) {
            r = inf_fixed(s);
            if (r == INF_OK) r = inf_codes(s, s->lit_fast, s->lit_cnt, s->lit_sym, s->dst_fast, s->dst_cnt, s->dst_sym);
        } else if (type == 2) {
            r = inf_dynamic(s);
            if (r == INF_OK) r = inf_codes(s, s->lit_fast, s->lit_cnt, s->lit_sym, s->dst_fast, s->dst_cnt, s->dst_sym);
        } else return INF_ERR;
        if (r != INF_OK) return r;
        if (final) break;
    }
    if (!inf_flush(s, false)) return INF_STOP;
    return INF_OK;
}

/* First byte after the DEFLATE stream (valid after INF_OK). */
static const uint8_t* inf_tail(inflate_t* s) {
    return s->in - (size_t)(s->bitcnt >> 3);
}

/* ============================================================================
   3. gzip
   ========================================================================== */
typedef struct {
    uint8_t* out;
    uint64_t cap, len;
    uint32_t crc;          /* running CRC of the current member */
    uint64_t member_len;
} gz_sink_t;

static bool gz_sink(void* c, const uint8_t* p, size_t n) {
    gz_sink_t* g = (gz_sink_t*)c;
    size_t take = n;
    bool more = true;
    if (g->len + take >= g->cap) { take = (size_t)(g->cap - g->len); more = false; }
    memcpy(g->out + g->len, p, take);
    g->crc = crc32_update(g->crc, p, take);
    g->len += take;
    g->member_len += take;
    return more;
}

/* Parses a gzip member header at p; returns the offset of the DEFLATE data or 0. */
static size_t gz_header(const uint8_t* d, size_t n) {
    if (n < 10 || d[0] != 0x1f || d[1] != 0x8b || d[2] != 8) return 0;
    uint8_t flg = d[3];
    if (flg & 0xE0) return 0;                       /* reserved flag bits */
    size_t pos = 10;
    if (flg & 0x04) {
        if (pos + 2 > n) return 0;
        size_t xlen = (size_t)d[pos] | ((size_t)d[pos + 1] << 8);
        pos += 2;
        if (pos + xlen > n) return 0;
        pos += xlen;
    }
    if (flg & 0x08) { while (pos < n && d[pos]) pos++; if (pos >= n) return 0; pos++; }
    if (flg & 0x10) { while (pos < n && d[pos]) pos++; if (pos >= n) return 0; pos++; }
    if (flg & 0x02) {
        if (pos + 2 > n) return 0;
        uint32_t want = (uint32_t)d[pos] | ((uint32_t)d[pos + 1] << 8);
        if ((crc32_update(0, d, pos) & 0xFFFFu) != want) return 0;   /* FHCRC */
        pos += 2;
    }
    return pos < n ? pos : 0;
}

bool gzip_inflate(const uint8_t* gzdata, uint64_t gzsize, uint8_t* out,
                  uint64_t out_cap, uint64_t* out_len) {
    if (out_len) *out_len = 0;
    if (gzdata == NULL || out == NULL || out_cap == 0 || gzsize < 18) return false;

    inflate_t* s = (inflate_t*)kmalloc(sizeof(inflate_t));
    if (!s) return false;

    gz_sink_t g;
    g.out = out; g.cap = out_cap; g.len = 0; g.crc = 0; g.member_len = 0;

    bool ok = false;
    const uint8_t* p = gzdata;
    size_t left = (size_t)gzsize;
    int members = 0;

    for (;;) {
        size_t hdr = gz_header(p, left);
        if (!hdr) { ok = members > 0; break; }
        g.crc = 0; g.member_len = 0;
        inf_init(s, p + hdr, left - hdr, gz_sink, &g);
        int r = inf_stream(s);
        if (r == INF_ERR) { ok = false; break; }
        if (r == INF_STOP) { ok = true; break; }             /* out_cap filled */
        members++;
        /* Trailer: CRC-32 + ISIZE.  Verified when present. */
        const uint8_t* t = inf_tail(s);
        size_t rest = (size_t)(p + left - t);
        if (rest >= 8) {
            if (rd_le32(t) != g.crc || rd_le32(t + 4) != (uint32_t)g.member_len) { ok = false; break; }
            t += 8; rest -= 8;
        } else {
            ok = true; break;                                 /* truncated trailer: tolerate */
        }
        ok = true;
        if (g.len >= g.cap) break;
        /* Another member? */
        if (rest < 18 || t[0] != 0x1f || t[1] != 0x8b) break;
        p = t; left = rest;
    }
    kfree(s);
    if (ok && out_len) *out_len = g.len;
    return ok;
}

/* ============================================================================
   4. PNG
   ========================================================================== */
#define PNG_MAX_DIM            65535u
#define PNG_MAX_INTERLACED_PX  (1ull << 26)     /* full BGRA copy when Adam7 + downscale */

typedef struct {
    uint32_t w, h;
    uint8_t depth, ctype, interlace;
    int channels;
    int bits_pp;                 /* bits per pixel */
    uint8_t pal[768];
    int pal_n;
    uint8_t pal_a[256];
    int pal_a_n;
    bool has_key;
    uint16_t key[3];
    uint32_t pal_bgra[256];      /* B,G,R,A bytes in memory order; built before decoding */
} png_info_t;

static const uint8_t k_png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

bool cos_png_probe(const uint8_t* data, uint64_t size, uint32_t* width, uint32_t* height) {
    if (!data || size < 33 || memcmp(data, k_png_sig, 8) != 0) return false;
    if (rd_be32(data + 8) != 13 || memcmp(data + 12, "IHDR", 4) != 0) return false;
    uint32_t w = rd_be32(data + 16), h = rd_be32(data + 20);
    if (w == 0 || h == 0) return false;
    if (width) *width = w;
    if (height) *height = h;
    return true;
}

static bool png_valid_combo(uint8_t ct, uint8_t d) {
    switch (ct) {
        case 0: return d == 1 || d == 2 || d == 4 || d == 8 || d == 16;
        case 2: return d == 8 || d == 16;
        case 3: return d == 1 || d == 2 || d == 4 || d == 8;
        case 4: return d == 8 || d == 16;
        case 6: return d == 8 || d == 16;
        default: return false;
    }
}

/* Adam7 pass geometry. */
static const uint8_t k_a7_x0[7] = {0, 4, 0, 2, 0, 1, 0};
static const uint8_t k_a7_y0[7] = {0, 0, 4, 0, 2, 0, 1};
static const uint8_t k_a7_dx[7] = {8, 8, 4, 4, 2, 2, 1};
static const uint8_t k_a7_dy[7] = {8, 8, 8, 4, 4, 2, 2};

static void pass_dims(const png_info_t* c, int pass, uint32_t* pw, uint32_t* ph) {
    if (!c->interlace) { *pw = c->w; *ph = c->h; return; }
    *pw = c->w > k_a7_x0[pass] ? (c->w - k_a7_x0[pass] + k_a7_dx[pass] - 1u) / k_a7_dx[pass] : 0;
    *ph = c->h > k_a7_y0[pass] ? (c->h - k_a7_y0[pass] + k_a7_dy[pass] - 1u) / k_a7_dy[pass] : 0;
}

static uint64_t png_raw_size(const png_info_t* c) {
    uint64_t total = 0;
    int passes = c->interlace ? 7 : 1;
    for (int p = 0; p < passes; ++p) {
        uint32_t pw, ph;
        pass_dims(c, p, &pw, &ph);
        if (!pw || !ph) continue;
        uint64_t stride = ((uint64_t)pw * (uint64_t)c->bits_pp + 7u) / 8u;
        total += (uint64_t)ph * (stride + 1u);
    }
    return total;
}

typedef struct {
    png_info_t info;
    const uint8_t* idat;        /* IDAT payload (points into the file or into idat_mem) */
    uint8_t* idat_mem;
    uint64_t idat_len;
} png_parsed_t;

/* Walks the chunk list: validates structure and CRCs, collects the palette,
 * transparency and the (concatenated) IDAT stream. */
static bool png_scan(const uint8_t* data, uint64_t size, png_parsed_t* pp) {
    png_info_t* c = &pp->info;
    memset(pp, 0, sizeof(*pp));
    if (!data || size < 8 + 25 || memcmp(data, k_png_sig, 8) != 0) return false;

    uint64_t pos = 8;
    bool seen_ihdr = false, idat_started = false, idat_ended = false;
    uint64_t idat_total = 0;
    const uint8_t* idat_first = NULL;
    uint64_t idat_first_len = 0;
    int idat_count = 0;

    while (pos + 12 <= size) {
        uint32_t clen = rd_be32(data + pos);
        const uint8_t* ctype = data + pos + 4;
        const uint8_t* cdata = data + pos + 8;
        bool is_ihdr = memcmp(ctype, "IHDR", 4) == 0;
        bool is_plte = memcmp(ctype, "PLTE", 4) == 0;
        bool is_idat = memcmp(ctype, "IDAT", 4) == 0;
        if (clen > 0x7FFFFFFFu) { serial_puts("[PNG] Chunk length out of range\n"); return false; }
        if (pos + 12u + clen > size) {
            if (is_ihdr || is_plte || is_idat) { serial_puts("[PNG] Truncated critical chunk\n"); return false; }
            break;
        }
        bool crc_known = false, crc_ok = false;
        #define CHUNK_CRC_OK() (crc_known ? crc_ok : (crc_known = true, \
            crc_ok = (crc32_update(0, ctype, 4u + clen) == rd_be32(cdata + clen))))

        if (is_ihdr) {
            if (seen_ihdr || pos != 8 || clen != 13) { serial_puts("[PNG] Bad IHDR\n"); return false; }
            if (!CHUNK_CRC_OK()) { serial_puts("[PNG] IHDR CRC mismatch\n"); return false; }
            c->w = rd_be32(cdata); c->h = rd_be32(cdata + 4);
            c->depth = cdata[8]; c->ctype = cdata[9]; c->interlace = cdata[12];
            if (c->w == 0 || c->h == 0 || c->w > PNG_MAX_DIM || c->h > PNG_MAX_DIM) {
                serial_puts("[PNG] Image dimensions invalid or too large\n"); return false;
            }
            if (!png_valid_combo(c->ctype, c->depth)) {
                serial_puts("[PNG] Invalid colour type / bit depth combination\n"); return false;
            }
            if (cdata[10] != 0 || cdata[11] != 0 || c->interlace > 1) {
                serial_puts("[PNG] Unknown compression/filter/interlace method\n"); return false;
            }
            static const uint8_t chans[7] = {1, 0, 3, 1, 2, 0, 4};
            c->channels = chans[c->ctype];
            c->bits_pp = c->channels * c->depth;
            seen_ihdr = true;
        } else if (!seen_ihdr) {
            serial_puts("[PNG] IHDR missing/not first\n");
            return false;
        } else if (is_plte) {
            if (idat_started) { /* ignored: PLTE after IDAT is a spec violation */ }
            else {
                if (!CHUNK_CRC_OK()) { serial_puts("[PNG] PLTE CRC mismatch\n"); return false; }
                if (clen == 0 || clen % 3u || clen > 768u) { serial_puts("[PNG] Bad PLTE length\n"); return false; }
                if (c->ctype == 3) {
                    int n = (int)(clen / 3u);
                    if (n > (1 << c->depth)) { serial_puts("[PNG] PLTE larger than bit depth allows\n"); return false; }
                    c->pal_n = n;
                    memcpy(c->pal, cdata, clen);
                } /* RGB/RGBA: suggested palette, gray types: illegal - ignored */
            }
        } else if (memcmp(ctype, "tRNS", 4) == 0) {
            if (!idat_started && CHUNK_CRC_OK()) {
                if (c->ctype == 3) {
                    c->pal_a_n = clen > 256u ? 256 : (int)clen;
                    memcpy(c->pal_a, cdata, (size_t)c->pal_a_n);
                } else if (c->ctype == 0 && clen == 2) {
                    uint16_t m = c->depth == 16 ? 0xFFFFu : (uint16_t)((1u << c->depth) - 1u);
                    c->key[0] = (uint16_t)(((cdata[0] << 8) | cdata[1]) & m);
                    c->has_key = true;
                } else if (c->ctype == 2 && clen == 6) {
                    uint16_t m = c->depth == 16 ? 0xFFFFu : 0xFFu;
                    for (int i = 0; i < 3; ++i)
                        c->key[i] = (uint16_t)(((cdata[i * 2] << 8) | cdata[i * 2 + 1]) & m);
                    c->has_key = true;
                } /* anything else is invalid for this colour type: ignored */
            }
        } else if (is_idat) {
            if (idat_ended) { serial_puts("[PNG] IDAT chunks are not consecutive\n"); return false; }
            if (!CHUNK_CRC_OK()) { serial_puts("[PNG] IDAT CRC mismatch\n"); return false; }
            if (!idat_started) {
                if (c->ctype == 3 && c->pal_n == 0) { serial_puts("[PNG] Palette image without PLTE\n"); return false; }
                idat_started = true;
                idat_first = cdata; idat_first_len = clen;
            }
            idat_total += clen;
            idat_count++;
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            break;
        } else if (idat_started) {
            idat_ended = true;        /* includes APNG fcTL/fdAT: only the default image is decoded */
        }
        #undef CHUNK_CRC_OK
        pos += 12u + clen;
    }

    if (!seen_ihdr || idat_total == 0) { serial_puts("[PNG] Missing IHDR or IDAT\n"); return false; }

    /* Bomb guard before allocating anything proportional to the image. */
    uint64_t raw = png_raw_size(c);
    if (raw / 1032u > idat_total + 64u) {
        serial_puts("[PNG] Header claims more pixel data than the stream can hold\n");
        return false;
    }

    if (idat_count == 1) {
        pp->idat = idat_first; pp->idat_len = idat_first_len;
    } else {
        uint8_t* mem = (uint8_t*)kmalloc((size_t)idat_total);
        if (!mem) return false;
        uint64_t off = 0;
        pos = 8;
        while (pos + 12 <= size && off < idat_total) {
            uint32_t clen = rd_be32(data + pos);
            if (pos + 12u + clen > size) break;
            if (memcmp(data + pos + 4, "IDAT", 4) == 0) {
                memcpy(mem + off, data + pos + 8, clen);
                off += clen;
            }
            pos += 12u + clen;
        }
        pp->idat_mem = mem; pp->idat = mem; pp->idat_len = idat_total;
    }

    /* Palette -> BGRA lookup, 256 entries; undefined indices are opaque black. */
    for (int i = 0; i < 256; ++i) {
        uint8_t b = 0, g = 0, r = 0, a = 255;
        if (i < c->pal_n) {
            r = c->pal[i * 3]; g = c->pal[i * 3 + 1]; b = c->pal[i * 3 + 2];
            if (i < c->pal_a_n) a = c->pal_a[i];
        }
        c->pal_bgra[i] = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24);
    }
    return true;
}

/* ---- filtering ---------------------------------------------------------- */
static inline int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static bool unfilter_row(int f, const uint8_t* src, uint8_t* cur, const uint8_t* prev,
                         size_t n, size_t bpp) {
    size_t i;
    switch (f) {
    case 0:
        memcpy(cur, src, n);
        return true;
    case 1:
        for (i = 0; i < bpp && i < n; ++i) cur[i] = src[i];
        for (; i < n; ++i) cur[i] = (uint8_t)(src[i] + cur[i - bpp]);
        return true;
    case 2:
        for (i = 0; i < n; ++i) cur[i] = (uint8_t)(src[i] + prev[i]);
        return true;
    case 3:
        for (i = 0; i < bpp && i < n; ++i) cur[i] = (uint8_t)(src[i] + (prev[i] >> 1));
        for (; i < n; ++i) cur[i] = (uint8_t)(src[i] + ((cur[i - bpp] + prev[i]) >> 1));
        return true;
    case 4:
        for (i = 0; i < bpp && i < n; ++i) cur[i] = (uint8_t)(src[i] + prev[i]);
        if (bpp == 4) {
            for (; i < n; ++i) cur[i] = (uint8_t)(src[i] + paeth(cur[i - 4], prev[i], prev[i - 4]));
        } else if (bpp == 3) {
            for (; i < n; ++i) cur[i] = (uint8_t)(src[i] + paeth(cur[i - 3], prev[i], prev[i - 3]));
        } else {
            for (; i < n; ++i) cur[i] = (uint8_t)(src[i] + paeth(cur[i - bpp], prev[i], prev[i - bpp]));
        }
        return true;
    default:
        return false;
    }
}

/* ---- colour conversion -------------------------------------------------- */
static inline uint8_t scale16(uint32_t v) {     /* round(v * 255 / 65535) */
    return (uint8_t)((v * 255u + 32767u) / 65535u);
}

/* Converts n pixels of one un-filtered row (bytes as stored) to BGRA. */
static void png_convert_row(const png_info_t* c, const uint8_t* row, uint32_t n, uint8_t* dst) {
    uint32_t i;
    const int key = c->has_key;
    switch (c->ctype) {
    case 0: {
        if (c->depth == 8) {
            for (i = 0; i < n; ++i) {
                uint8_t v = row[i];
                dst[0] = dst[1] = dst[2] = v;
                dst[3] = (key && v == c->key[0]) ? 0 : 255;
                dst += 4;
            }
        } else if (c->depth == 16) {
            for (i = 0; i < n; ++i) {
                uint32_t s = ((uint32_t)row[i * 2] << 8) | row[i * 2 + 1];
                uint8_t v = scale16(s);
                dst[0] = dst[1] = dst[2] = v;
                dst[3] = (key && s == c->key[0]) ? 0 : 255;
                dst += 4;
            }
        } else {
            int d = c->depth, mask = (1 << d) - 1, mul = 255 / mask;
            for (i = 0; i < n; ++i) {
                uint32_t bit = i * (uint32_t)d;
                int s = (row[bit >> 3] >> (8 - d - (int)(bit & 7))) & mask;
                uint8_t v = (uint8_t)(s * mul);
                dst[0] = dst[1] = dst[2] = v;
                dst[3] = (key && s == c->key[0]) ? 0 : 255;
                dst += 4;
            }
        }
        break;
    }
    case 2: {
        if (c->depth == 8) {
            for (i = 0; i < n; ++i) {
                const uint8_t* p = row + i * 3;
                dst[0] = p[2]; dst[1] = p[1]; dst[2] = p[0];
                dst[3] = (key && p[0] == c->key[0] && p[1] == c->key[1] && p[2] == c->key[2]) ? 0 : 255;
                dst += 4;
            }
        } else {
            for (i = 0; i < n; ++i) {
                const uint8_t* p = row + i * 6;
                uint32_t r = ((uint32_t)p[0] << 8) | p[1], g = ((uint32_t)p[2] << 8) | p[3],
                         b = ((uint32_t)p[4] << 8) | p[5];
                dst[0] = scale16(b); dst[1] = scale16(g); dst[2] = scale16(r);
                dst[3] = (key && r == c->key[0] && g == c->key[1] && b == c->key[2]) ? 0 : 255;
                dst += 4;
            }
        }
        break;
    }
    case 3: {
        if (c->depth == 8) {
            for (i = 0; i < n; ++i) {
                uint32_t v = c->pal_bgra[row[i]];
                memcpy(dst, &v, 4);          /* little-endian: B,G,R,A byte order */
                dst += 4;
            }
        } else {
            int d = c->depth, mask = (1 << d) - 1;
            for (i = 0; i < n; ++i) {
                uint32_t bit = i * (uint32_t)d;
                int s = (row[bit >> 3] >> (8 - d - (int)(bit & 7))) & mask;
                uint32_t v = c->pal_bgra[s];
                dst[0] = (uint8_t)v; dst[1] = (uint8_t)(v >> 8);
                dst[2] = (uint8_t)(v >> 16); dst[3] = (uint8_t)(v >> 24);
                dst += 4;
            }
        }
        break;
    }
    case 4: {
        if (c->depth == 8) {
            for (i = 0; i < n; ++i) {
                dst[0] = dst[1] = dst[2] = row[i * 2];
                dst[3] = row[i * 2 + 1];
                dst += 4;
            }
        } else {
            for (i = 0; i < n; ++i) {
                const uint8_t* p = row + i * 4;
                dst[0] = dst[1] = dst[2] = scale16(((uint32_t)p[0] << 8) | p[1]);
                dst[3] = scale16(((uint32_t)p[2] << 8) | p[3]);
                dst += 4;
            }
        }
        break;
    }
    default: { /* 6 */
        if (c->depth == 8) {
            for (i = 0; i < n; ++i) {
                const uint8_t* p = row + i * 4;
                dst[0] = p[2]; dst[1] = p[1]; dst[2] = p[0]; dst[3] = p[3];
                dst += 4;
            }
        } else {
            for (i = 0; i < n; ++i) {
                const uint8_t* p = row + i * 8;
                dst[0] = scale16(((uint32_t)p[4] << 8) | p[5]);
                dst[1] = scale16(((uint32_t)p[2] << 8) | p[3]);
                dst[2] = scale16(((uint32_t)p[0] << 8) | p[1]);
                dst[3] = scale16(((uint32_t)p[6] << 8) | p[7]);
                dst += 4;
            }
        }
        break;
    }
    }
}

/* ---- row sink fed by the inflater --------------------------------------- */
enum { OUT_DIRECT = 0, OUT_SCALED = 1 };

typedef struct {
    const png_info_t* c;
    int pass;                       /* current pass (0 for non-interlaced) */
    uint32_t pw, ph, row;           /* geometry of the current pass / next row index */
    size_t stride, rowlen, fill;
    size_t bpp;
    uint8_t *fbuf, *cur, *prev, *line;
    /* destination */
    uint8_t* out;                   /* ow*oh*4 */
    uint32_t ow, oh;
    uint8_t* full;                  /* interlaced + scaling: sw*sh*4 intermediate, else NULL */
    cos_img_scaler_t* scaler;           /* non-interlaced + scaling */
    /* state */
    bool all_rows, early, extra, error;
    uint32_t adler;
} png_rows_t;

static bool rows_next_pass(png_rows_t* r) {
    const png_info_t* c = r->c;
    int passes = c->interlace ? 7 : 1;
    for (int p = r->pass + 1; p < passes; ++p) {
        uint32_t pw, ph;
        pass_dims(c, p, &pw, &ph);
        if (!pw || !ph) continue;
        r->pass = p; r->pw = pw; r->ph = ph; r->row = 0;
        r->stride = ((size_t)pw * (size_t)c->bits_pp + 7u) / 8u;
        r->rowlen = r->stride + 1u;
        r->fill = 0;
        memset(r->prev, 0, r->stride);
        return true;
    }
    return false;
}

static void rows_deliver(png_rows_t* r) {
    const png_info_t* c = r->c;
    uint32_t y = c->interlace ? (uint32_t)k_a7_y0[r->pass] + r->row * k_a7_dy[r->pass] : r->row;
    if (!c->interlace) {
        if (r->scaler) {
            png_convert_row(c, r->cur, r->pw, r->line);
            cos_img_scaler_push_row(r->scaler, r->line);
        } else if (y < r->oh) {
            uint32_t n = r->pw < r->ow ? r->pw : r->ow;
            png_convert_row(c, r->cur, n, r->out + (size_t)y * r->ow * 4u);
        }
        /* Cropping: once the rows we keep are done, stop inflating (the rest of
         * the stream is then not checksummed).  Never when nothing is cropped,
         * so ordinary images still get their Adler-32 verified. */
        if (!r->scaler && r->oh < c->h && r->row + 1 >= r->oh) r->early = true;
        return;
    }
    /* Adam7: scatter the pass row into the destination. */
    uint8_t* base; uint32_t bw, bh;
    if (r->full) { base = r->full; bw = c->w; bh = c->h; }
    else         { base = r->out;  bw = r->ow; bh = r->oh; }
    if (y >= bh) return;
    uint32_t x0 = k_a7_x0[r->pass], dx = k_a7_dx[r->pass];
    uint32_t n = bw > x0 ? (bw - x0 + dx - 1u) / dx : 0;
    if (n > r->pw) n = r->pw;
    if (!n) return;
    png_convert_row(c, r->cur, n, r->line);
    uint8_t* d = base + ((size_t)y * bw + x0) * 4u;
    for (uint32_t i = 0; i < n; ++i) {
        memcpy(d, r->line + (size_t)i * 4u, 4);
        d += (size_t)dx * 4u;
    }
}

static bool rows_process(png_rows_t* r, const uint8_t* src) {
    int f = src[0];
    if (!unfilter_row(f, src + 1, r->cur, r->prev, r->stride, r->bpp)) {
        serial_puts("[PNG] Unknown scanline filter type (corrupt data)\n");
        r->error = true;
        return false;
    }
    rows_deliver(r);
    uint8_t* t = r->cur; r->cur = r->prev; r->prev = t;
    if (++r->row >= r->ph) {
        if (!rows_next_pass(r)) r->all_rows = true;
    }
    return true;
}

static bool png_sink(void* ctx, const uint8_t* p, size_t n) {
    png_rows_t* r = (png_rows_t*)ctx;
    r->adler = adler32_update(r->adler, p, n);
    while (n > 0) {
        if (r->all_rows) { r->extra = true; return false; }
        const uint8_t* src;
        if (r->fill == 0 && n >= r->rowlen) {
            src = p; p += r->rowlen; n -= r->rowlen;
        } else {
            size_t take = r->rowlen - r->fill;
            if (take > n) take = n;
            memcpy(r->fbuf + r->fill, p, take);
            r->fill += take; p += take; n -= take;
            if (r->fill < r->rowlen) break;
            r->fill = 0;
            src = r->fbuf;
        }
        if (!rows_process(r, src)) return false;
        if (r->early) return false;
    }
    return true;
}

static bool png_decode_impl(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                            uint64_t max_w, uint64_t max_h, bool fit,
                            uint64_t* out_w, uint64_t* out_h,
                            uint64_t* src_w, uint64_t* src_h) {
    if (!data || !out_bgra || max_w == 0 || max_h == 0) return false;
    png_parsed_t pp;
    if (!png_scan(data, size, &pp)) { if (pp.idat_mem) kfree(pp.idat_mem); return false; }
    const png_info_t* c = &pp.info;

    uint64_t ow, oh;
    bool scale = false;
    if (fit && (c->w > max_w || c->h > max_h)) {
        cos_img_fit_dims(c->w, c->h, max_w, max_h, &ow, &oh);
        scale = true;
    } else {
        ow = c->w < max_w ? c->w : max_w;
        oh = c->h < max_h ? c->h : max_h;
    }

    bool ok = false;
    inflate_t* inf = NULL;
    uint8_t* work = NULL;
    uint8_t* full = NULL;
    cos_img_scaler_t* scaler = NULL;
    png_rows_t r;
    memset(&r, 0, sizeof(r));

    /* zlib header (RFC 1950): deflate, window <= 32K, no preset dictionary. */
    if (pp.idat_len < 2 || (pp.idat[0] & 0x0F) != 8 || (pp.idat[0] >> 4) > 7 ||
        (((unsigned)pp.idat[0] << 8) | pp.idat[1]) % 31u != 0 || (pp.idat[1] & 0x20)) {
        serial_puts("[PNG] Bad zlib header\n");
        goto done;
    }

    if (c->interlace && scale) {
        if ((uint64_t)c->w * c->h > PNG_MAX_INTERLACED_PX) {
            serial_puts("[PNG] Interlaced image too large to downscale\n");
            goto done;
        }
        full = (uint8_t*)kmalloc((size_t)c->w * c->h * 4u);
        if (!full) goto done;
    } else if (scale) {
        scaler = cos_img_scaler_create(c->w, c->h, (uint32_t)ow, (uint32_t)oh, out_bgra);
        if (!scaler) goto done;
    }

    size_t max_stride = ((size_t)c->w * (size_t)c->bits_pp + 7u) / 8u;
    size_t line_bytes = (size_t)c->w * 4u;
    work = (uint8_t*)kmalloc((max_stride + 1u) + max_stride * 2u + line_bytes + 16u);
    inf = (inflate_t*)kmalloc(sizeof(inflate_t));
    if (!work || !inf) goto done;

    r.c = c;
    r.fbuf = work;
    r.cur = work + (max_stride + 1u);
    r.prev = r.cur + max_stride;
    r.line = r.prev + max_stride;
    r.out = out_bgra; r.ow = (uint32_t)ow; r.oh = (uint32_t)oh;
    r.full = full; r.scaler = scaler;
    r.bpp = (size_t)((c->bits_pp + 7) / 8);
    r.pass = -1;
    r.adler = 1;
    if (!rows_next_pass(&r)) goto done;

    inf_init(inf, pp.idat + 2, (size_t)pp.idat_len - 2u, png_sink, &r);
    int rc = inf_stream(inf);

    if (r.error) goto done;
    if (rc == INF_ERR) {
        serial_puts("[PNG] DEFLATE decompression failed (corrupt or truncated data)\n");
        goto done;
    }
    if (rc == INF_OK) {
        if (!r.all_rows) { serial_puts("[PNG] Not enough image data\n"); goto done; }
        const uint8_t* t = inf_tail(inf);
        size_t rest = (size_t)(pp.idat + pp.idat_len - t);
        if (rest >= 4 && rd_be32(t) != r.adler) {
            serial_puts("[PNG] Adler-32 mismatch\n");
            goto done;
        }
    } else {
        /* Stopped by the sink: either the rows we need are done (crop) or the
         * stream carries more data than the image needs - both fine. */
        if (!r.early && !r.extra) goto done;
        if (r.extra && !r.all_rows) goto done;
    }

    if (full) {
        /* Adam7 + downscale: stream the assembled image through the scaler. */
        scaler = cos_img_scaler_create(c->w, c->h, (uint32_t)ow, (uint32_t)oh, out_bgra);
        if (!scaler) goto done;
        for (uint32_t y = 0; y < c->h; ++y)
            cos_img_scaler_push_row(scaler, full + (size_t)y * c->w * 4u);
    }
    ok = true;

done:
    if (scaler) cos_img_scaler_destroy(scaler);
    if (full) kfree(full);
    if (inf) kfree(inf);
    if (work) kfree(work);
    if (pp.idat_mem) kfree(pp.idat_mem);
    if (ok) {
        if (out_w) *out_w = ow;
        if (out_h) *out_h = oh;
        if (src_w) *src_w = c->w;
        if (src_h) *src_h = c->h;
    }
    return ok;
}

bool png_decode(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                uint64_t max_out_w, uint64_t max_out_h,
                uint64_t* out_w, uint64_t* out_h) {
    return png_decode_impl(data, size, out_bgra, max_out_w, max_out_h, false,
                           out_w, out_h, NULL, NULL);
}

bool cos_png_decode_fit(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                    uint64_t max_out_w, uint64_t max_out_h,
                    uint64_t* out_w, uint64_t* out_h,
                    uint64_t* src_w, uint64_t* src_h) {
    return png_decode_impl(data, size, out_bgra, max_out_w, max_out_h, true,
                           out_w, out_h, src_w, src_h);
}
