/* cosarc.c - CRC-32, a streaming inflate and a streaming deflate. See cosarc.h. */
#include <stdlib.h>
#include <string.h>
#include "cosarc.h"
#include "cosarc_in.h"

/* ---- CRC-32 -------------------------------------------------------------------------------------------------------------------------------------- */
static uint32_t g_crc_tab[256]; static int g_crc_ready;
uint32_t arc_crc32(uint32_t crc, const void *buf, size_t n) {
    if (!g_crc_ready) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; g_crc_tab[i] = c; }
        g_crc_ready = 1;
    }
    const uint8_t *p = (const uint8_t *)buf; crc = ~crc;
    while (n--) crc = g_crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- buffered input (keeps the last 8 bytes across a refill so that an inflate can give back the bytes it read but did not use) ----------------------- */
void arc_in_init(arc_in *in, arc_read_fn rd, void *ctx) { in->rd = rd; in->ctx = ctx; in->len = in->pos = 0; in->eof = 0; }
static int arc_in_refill(arc_in *in) {
    if (in->eof) return 0;
    size_t keep = in->len < 8 ? in->len : 8;
    memmove(in->buf, in->buf + in->len - keep, keep);
    size_t n = in->rd(in->ctx, in->buf + keep, ARC_IN_BUF - keep);
    in->len = keep + n; in->pos = keep;
    if (n == 0) { in->eof = 1; return 0; }
    return 1;
}
int arc_in_getc(arc_in *in) {
    if (in->pos >= in->len && !arc_in_refill(in)) return -1;
    return in->buf[in->pos++];
}
size_t arc_in_read(arc_in *in, void *dst, size_t n) {
    uint8_t *d = (uint8_t *)dst; size_t got = 0;
    while (got < n) {
        if (in->pos >= in->len && !arc_in_refill(in)) break;
        size_t k = in->len - in->pos; if (k > n - got) k = n - got;
        memcpy(d + got, in->buf + in->pos, k); in->pos += k; got += k;
    }
    return got;
}

/* ---- inflate ------------------------------------------------------------------------------------------------------------------------------------- */
#define WIN 32768u
typedef struct {
    arc_in *in; arc_write_fn wr; void *wctx;
    uint64_t bitbuf; int bitcnt, pad;                    /* pad: zero bytes appended after the end of the input (the table lookup looks ahead) */
    uint8_t *win; uint32_t pos; uint64_t total; uint32_t crc;
    int err;
    uint16_t *lit, *dst; int litbits, dstbits;
} infl_t;

static void inf_fill(infl_t *s) {
    while (s->bitcnt <= 56) {
        int c = arc_in_getc(s->in);
        if (c < 0) { c = 0; s->pad++; }
        s->bitbuf |= (uint64_t)(unsigned)c << s->bitcnt; s->bitcnt += 8;
    }
}
static uint32_t inf_bits(infl_t *s, int n) {
    if (n == 0) return 0;
    if (s->bitcnt < n) inf_fill(s);
    uint32_t v = (uint32_t)(s->bitbuf & ((1ull << n) - 1));
    s->bitbuf >>= n; s->bitcnt -= n;
    if (s->pad * 8 > s->bitcnt) s->err = 1;              /* consumed bits that were not in the input */
    return v;
}
static int inf_flush(infl_t *s, uint32_t from, uint32_t to) {        /* writes win[from..to) */
    if (to <= from) return 0;
    s->crc = arc_crc32(s->crc, s->win + from, to - from);
    s->total += to - from;
    return s->wr(s->wctx, s->win + from, to - from);
}
static uint32_t rev_bits(uint32_t v, int n) { uint32_t r = 0; while (n--) { r = (r << 1) | (v & 1); v >>= 1; } return r; }

/* a lookup table indexed by the next `bits` bits of the stream: entry = (length << 9) | symbol, 0 = no such code */
static int huff_build(uint16_t *lut, int *bits, const uint8_t *lens, int n) {
    int count[16] = { 0 }, maxlen = 0;
    for (int i = 0; i < n; i++) { count[lens[i]]++; if (lens[i] > maxlen) maxlen = lens[i]; }
    count[0] = 0;
    if (maxlen == 0) { *bits = 1; lut[0] = lut[1] = 0; return 0; }
    int left = 1; for (int l = 1; l <= 15; l++) { left <<= 1; left -= count[l]; if (left < 0) return -1; }      /* over-subscribed */
    uint32_t next[16]; uint32_t code = 0;
    for (int l = 1; l <= 15; l++) { code = (code + (uint32_t)count[l - 1]) << 1; next[l] = code; }
    *bits = maxlen; memset(lut, 0, ((size_t)1 << maxlen) * sizeof lut[0]);
    for (int i = 0; i < n; i++) {
        int l = lens[i]; if (!l) continue;
        uint32_t r = rev_bits(next[l]++, l);
        for (uint32_t k = r; k < (1u << maxlen); k += 1u << l) lut[k] = (uint16_t)((l << 9) | i);
    }
    return 0;
}
static int inf_sym(infl_t *s, const uint16_t *lut, int bits) {
    if (s->bitcnt < 15) inf_fill(s);
    uint16_t e = lut[s->bitbuf & ((1u << bits) - 1)];
    int l = e >> 9; if (!l) { s->err = 2; return -1; }
    s->bitbuf >>= l; s->bitcnt -= l;
    if (s->pad * 8 > s->bitcnt) { s->err = 1; return -1; }
    return e & 511;
}

static const uint16_t k_lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  k_lext[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t k_dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  k_dext[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inf_codes(infl_t *s, uint32_t *flushed) {
    for (;;) {
        int sym = inf_sym(s, s->lit, s->litbits); if (s->err) return s->err;
        if (sym < 256) {
            s->win[s->pos++ & (WIN - 1)] = (uint8_t)sym;
            if ((s->pos & (WIN - 1)) == 0) { if (inf_flush(s, 0, WIN)) return 3; *flushed = s->pos; }
            continue;
        }
        if (sym == 256) return 0;
        sym -= 257; if (sym >= 29) return 2;
        uint32_t len = k_lbase[sym] + inf_bits(s, k_lext[sym]);
        int ds = inf_sym(s, s->dst, s->dstbits); if (s->err) return s->err;
        if (ds >= 30) return 2;
        uint32_t dist = k_dbase[ds] + inf_bits(s, k_dext[ds]); if (s->err) return s->err;
        if ((uint64_t)dist > s->total + (s->pos - *flushed)) return 2;                       /* before the start of the output */
        while (len--) {
            s->win[s->pos & (WIN - 1)] = s->win[(s->pos - dist) & (WIN - 1)]; s->pos++;
            if ((s->pos & (WIN - 1)) == 0) { if (inf_flush(s, 0, WIN)) return 3; *flushed = s->pos; }
        }
    }
}

int arc_inflate_in(arc_in *in, arc_write_fn wr, void *wctx, uint32_t *crc, uint64_t *size) {
    infl_t *s = (infl_t *)calloc(1, sizeof *s); if (!s) return 4;
    s->win = (uint8_t *)malloc(WIN); s->lit = (uint16_t *)malloc(sizeof(uint16_t) << 15); s->dst = (uint16_t *)malloc(sizeof(uint16_t) << 15);
    if (!s->win || !s->lit || !s->dst) { free(s->win); free(s->lit); free(s->dst); free(s); return 4; }
    s->in = in; s->wr = wr; s->wctx = wctx;
    uint32_t flushed = 0; int rc = 0, last = 0;
    while (!last && !rc) {
        last = (int)inf_bits(s, 1); int type = (int)inf_bits(s, 2);
        if (s->err) { rc = s->err; break; }
        if (type == 0) {
            int drop = s->bitcnt & 7; inf_bits(s, drop);
            uint32_t len = inf_bits(s, 16), nlen = inf_bits(s, 16);
            if (s->err) { rc = s->err; break; }
            if ((len ^ 0xFFFF) != nlen) { rc = 2; break; }
            while (len-- && !s->err) {
                s->win[s->pos++ & (WIN - 1)] = (uint8_t)inf_bits(s, 8);
                if ((s->pos & (WIN - 1)) == 0) { if (inf_flush(s, 0, WIN)) { rc = 3; break; } flushed = s->pos; }
            }
            if (!rc) rc = s->err;
        } else if (type == 1) {
            uint8_t l[288]; for (int i = 0; i < 144; i++) l[i] = 8; for (int i = 144; i < 256; i++) l[i] = 9; for (int i = 256; i < 280; i++) l[i] = 7; for (int i = 280; i < 288; i++) l[i] = 8;
            uint8_t d[30]; memset(d, 5, sizeof d);
            if (huff_build(s->lit, &s->litbits, l, 288) || huff_build(s->dst, &s->dstbits, d, 30)) { rc = 2; break; }
            rc = inf_codes(s, &flushed);
        } else if (type == 2) {
            int nlen = (int)inf_bits(s, 5) + 257, ndist = (int)inf_bits(s, 5) + 1, ncode = (int)inf_bits(s, 4) + 4;
            if (s->err) { rc = s->err; break; }
            if (nlen > 286 || ndist > 30) { rc = 2; break; }
            static const uint8_t order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            uint8_t cl[19]; memset(cl, 0, sizeof cl);
            for (int i = 0; i < ncode; i++) cl[order[i]] = (uint8_t)inf_bits(s, 3);
            if (s->err) { rc = s->err; break; }
            int cbits; if (huff_build(s->dst, &cbits, cl, 19)) { rc = 2; break; }
            uint8_t lens[320]; int idx = 0;
            while (idx < nlen + ndist && !rc) {
                int sym = inf_sym(s, s->dst, cbits); if (s->err) { rc = s->err; break; }
                if (sym < 16) lens[idx++] = (uint8_t)sym;
                else {
                    int rep, val = 0;
                    if (sym == 16) { if (idx == 0) { rc = 2; break; } val = lens[idx - 1]; rep = 3 + (int)inf_bits(s, 2); }
                    else if (sym == 17) rep = 3 + (int)inf_bits(s, 3);
                    else rep = 11 + (int)inf_bits(s, 7);
                    if (idx + rep > nlen + ndist) { rc = 2; break; }
                    while (rep--) lens[idx++] = (uint8_t)val;
                }
            }
            if (rc || s->err) { if (!rc) rc = s->err; break; }
            if (lens[256] == 0) { rc = 2; break; }
            if (huff_build(s->lit, &s->litbits, lens, nlen) || huff_build(s->dst, &s->dstbits, lens + nlen, ndist)) { rc = 2; break; }
            rc = inf_codes(s, &flushed);
        } else rc = 2;
    }
    if (!rc && s->err) rc = s->err;
    if (!rc && inf_flush(s, 0, s->pos - flushed)) rc = 3;                        /* what is left in the window */
    if (!rc) {                                                                          /* give back the whole bytes still in the bit buffer */
        s->bitcnt -= s->bitcnt & 7;
        int unread = s->bitcnt / 8 - s->pad;
        if (unread > 0 && (size_t)unread <= in->pos) in->pos -= (size_t)unread;
    }
    if (crc) *crc = s->crc;
    if (size) *size = s->total;
    free(s->win); free(s->lit); free(s->dst); free(s);
    return rc;
}

int arc_inflate(arc_read_fn rd, void *rctx, arc_write_fn wr, void *wctx, uint32_t *crc, uint64_t *size) {
    arc_in *in = (arc_in *)malloc(sizeof *in); if (!in) return 4;
    arc_in_init(in, rd, rctx);
    int rc = arc_inflate_in(in, wr, wctx, crc, size);
    free(in); return rc;
}

/* ---- deflate: LZ77 with hash chains, then the fixed Huffman code --------------------------------------------------------------------------------- */
#define DBUF 131072
#define HBITS 15
typedef struct {
    arc_write_fn wr; void *wctx; uint8_t out[65536]; size_t outn; uint64_t bitbuf; int bitcnt; uint64_t csize; int err;
    uint16_t lcode[288]; uint8_t llen[288];
} defl_t;

static void def_flushout(defl_t *d) { if (d->outn && !d->err && d->wr(d->wctx, d->out, d->outn)) d->err = 3; d->csize += d->outn; d->outn = 0; }
static void def_put(defl_t *d, uint32_t v, int n) {
    d->bitbuf |= (uint64_t)v << d->bitcnt; d->bitcnt += n;
    while (d->bitcnt >= 8) { d->out[d->outn++] = (uint8_t)d->bitbuf; d->bitbuf >>= 8; d->bitcnt -= 8; if (d->outn == sizeof d->out) def_flushout(d); }
}
static int len_index(int len) { int i = 28; while (k_lbase[i] > len) i--; return i; }
static int dist_index(int dist) { int i = 29; while (k_dbase[i] > dist) i--; return i; }

int arc_deflate_in(arc_in *in, arc_write_fn wr, void *wctx, uint32_t *crc, uint64_t *size, uint64_t *csize, int level) {
    defl_t *d = (defl_t *)calloc(1, sizeof *d);
    uint8_t *buf = (uint8_t *)malloc(DBUF);
    int32_t *head = (int32_t *)malloc(sizeof(int32_t) << HBITS), *prev = (int32_t *)malloc(sizeof(int32_t) * 32768);
    if (!d || !buf || !head || !prev) { free(d); free(buf); free(head); free(prev); return 4; }
    d->wr = wr; d->wctx = wctx;
    for (int i = 0; i < 288; i++) {                                                  /* the fixed code of RFC 1951 3.2.6, bits reversed for an LSB-first stream */
        uint32_t c; int l;
        if (i < 144) { c = 0x30 + i; l = 8; } else if (i < 256) { c = 0x190 + (i - 144); l = 9; } else if (i < 280) { c = (uint32_t)(i - 256); l = 7; } else { c = 0xC0 + (i - 280); l = 8; }
        d->lcode[i] = (uint16_t)rev_bits(c, l); d->llen[i] = (uint8_t)l;
    }
    if (level < 1) level = 6; if (level > 9) level = 9;
    int max_chain = level <= 1 ? 4 : level <= 3 ? 12 : level <= 6 ? 40 : level <= 8 ? 128 : 400, nice = level <= 3 ? 32 : 258;
    for (int i = 0; i < (1 << HBITS); i++) head[i] = -1;
    memset(prev, 0xFF, sizeof(int32_t) * 32768);
    uint32_t c32 = 0; uint64_t total = 0;
    int have = 0, pos = 0, eof = 0;
    def_put(d, 3, 3);                                                                /* BFINAL = 1, BTYPE = 01 (fixed Huffman) */
    while (!d->err) {
        if (have - pos < 262 && !eof) {
            while (pos >= 65536) {                                                    /* slide by 32768: positions keep their value modulo 32768 */
                memmove(buf, buf + 32768, (size_t)(have - 32768)); have -= 32768; pos -= 32768;
                for (int i = 0; i < (1 << HBITS); i++) head[i] = head[i] >= 32768 ? head[i] - 32768 : -1;
                for (int i = 0; i < 32768; i++) prev[i] = prev[i] >= 32768 ? prev[i] - 32768 : -1;
            }
            size_t n = arc_in_read(in, buf + have, (size_t)(DBUF - have));
            if (n == 0) eof = 1;
            else { c32 = arc_crc32(c32, buf + have, n); total += n; have += (int)n; }
            continue;
        }
        if (pos >= have) break;
        int avail = have - pos, best = 0, bestd = 0;
        if (avail >= 3) {
            uint32_t h = (((uint32_t)buf[pos] << 10) ^ ((uint32_t)buf[pos + 1] << 5) ^ buf[pos + 2]) & ((1u << HBITS) - 1);
            int cand = head[h], chain = max_chain, maxl = avail < 258 ? avail : 258;
            while (cand >= 0 && chain-- > 0 && pos - cand <= 32768 && cand < pos) {
                if (buf[cand + best] == buf[pos + best] || best == 0) {
                    int l = 0; while (l < maxl && buf[cand + l] == buf[pos + l]) l++;
                    if (l > best) { best = l; bestd = pos - cand; if (l >= nice || l == maxl) break; }
                }
                cand = prev[cand & 32767];
            }
        }
        int adv;
        if (best >= 3 && !(best == 3 && bestd > 4096)) {
            int li = len_index(best), di = dist_index(bestd);
            def_put(d, d->lcode[257 + li], d->llen[257 + li]); if (k_lext[li]) def_put(d, (uint32_t)(best - k_lbase[li]), k_lext[li]);
            def_put(d, rev_bits((uint32_t)di, 5), 5); if (k_dext[di]) def_put(d, (uint32_t)(bestd - k_dbase[di]), k_dext[di]);
            adv = best;
        } else { def_put(d, d->lcode[buf[pos]], d->llen[buf[pos]]); adv = 1; }
        for (int k = 0; k < adv; k++, pos++) {                                         /* every position gets into the chains */
            if (have - pos >= 3) {
                uint32_t h = (((uint32_t)buf[pos] << 10) ^ ((uint32_t)buf[pos + 1] << 5) ^ buf[pos + 2]) & ((1u << HBITS) - 1);
                prev[pos & 32767] = head[h]; head[h] = pos;
            }
        }
    }
    def_put(d, d->lcode[256], d->llen[256]);                                          /* end of block */
    if (d->bitcnt > 0) def_put(d, 0, 8 - d->bitcnt);
    def_flushout(d);
    int rc = d->err;
    if (crc) *crc = c32; if (size) *size = total; if (csize) *csize = d->csize;
    free(d); free(buf); free(head); free(prev);
    return rc;
}

int arc_deflate(arc_read_fn rd, void *rctx, arc_write_fn wr, void *wctx, uint32_t *crc, uint64_t *size, uint64_t *csize, int level) {
    arc_in *in = (arc_in *)malloc(sizeof *in); if (!in) return 4;
    arc_in_init(in, rd, rctx);
    int rc = arc_deflate_in(in, wr, wctx, crc, size, csize, level);
    free(in); return rc;
}
