/* inflate.h - a small zlib/deflate decoder (RFC 1950/1951) for the Linux pack installer. After Mark Adler's "puff" idea: canonical Huffman codes decoded bit by bit
 * with count/symbol tables; slow-ish but short, with no allocation and no table that depends on the input's size. Everything is bounds checked: a damaged or hostile
 * stream ends with an error, never a write outside `dst`.
 *
 *   int cos_inflate_zlib(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstcap, size_t *outlen);     0 = ok, else an error code */
#ifndef COS_INFLATE_H
#define COS_INFLATE_H
#include <stdint.h>
#include <stddef.h>

typedef struct { const uint8_t *in; size_t inlen, inpos; uint32_t bitbuf; int bitcnt; uint8_t *out; size_t outcap, outpos; int err; } inflate_t;
typedef struct { short count[16]; short symbol[288]; } inflate_huff_t;

static int inf_bits(inflate_t *s, int need) {
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) { s->err = 1; return 0; }
        v |= (uint32_t)s->in[s->inpos++] << s->bitcnt; s->bitcnt += 8;
    }
    s->bitbuf = v >> need; s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}
static int inf_decode(inflate_t *s, const inflate_huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= inf_bits(s, 1); if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -10;
}
static int inf_construct(inflate_huff_t *h, const short *length, int n) {
    for (int len = 0; len <= 15; ++len) h->count[len] = 0;
    for (int sym = 0; sym < n; ++sym) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; ++len) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    short offs[16]; offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; ++sym) if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}
static int inf_codes(inflate_t *s, const inflate_huff_t *lc, const inflate_huff_t *dc) {
    static const short lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int sym = inf_decode(s, lc); if (sym < 0 || s->err) return 2;
        if (sym < 256) { if (s->outpos >= s->outcap) return 3; s->out[s->outpos++] = (uint8_t)sym; }
        else if (sym == 256) return 0;
        else {
            sym -= 257; if (sym >= 29) return 4;
            int len = lens[sym] + inf_bits(s, lext[sym]); if (s->err) return 2;
            int ds = inf_decode(s, dc); if (ds < 0 || s->err || ds >= 30) return 5;
            size_t dist = (size_t)dists[ds] + (size_t)inf_bits(s, dext[ds]); if (s->err) return 2;
            if (dist > s->outpos) return 6;
            if (s->outpos + (size_t)len > s->outcap) return 3;
            for (int i = 0; i < len; ++i) { s->out[s->outpos] = s->out[s->outpos - dist]; s->outpos++; }
        }
    }
}
static int inf_stored(inflate_t *s) {
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen) return 7;
    unsigned len = s->in[s->inpos] | (unsigned)s->in[s->inpos + 1] << 8, nlen = s->in[s->inpos + 2] | (unsigned)s->in[s->inpos + 3] << 8; s->inpos += 4;
    if (len != (~nlen & 0xFFFFu)) return 8;
    if (s->inpos + len > s->inlen) return 7;
    if (s->outpos + len > s->outcap) return 3;
    for (unsigned i = 0; i < len; ++i) s->out[s->outpos++] = s->in[s->inpos++];
    return 0;
}
static int inf_fixed(inflate_t *s) {
    static inflate_huff_t lc, dc; static int built;
    if (!built) {
        short lengths[288]; int i;
        for (i = 0; i < 144; ++i) lengths[i] = 8; for (; i < 256; ++i) lengths[i] = 9; for (; i < 280; ++i) lengths[i] = 7; for (; i < 288; ++i) lengths[i] = 8;
        inf_construct(&lc, lengths, 288);
        for (i = 0; i < 30; ++i) lengths[i] = 5;
        inf_construct(&dc, lengths, 30); built = 1;
    }
    return inf_codes(s, &lc, &dc);
}
static int inf_dynamic(inflate_t *s) {
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[320]; inflate_huff_t lc, dc;
    int nlen = inf_bits(s, 5) + 257, ndist = inf_bits(s, 5) + 1, ncode = inf_bits(s, 4) + 4; if (s->err) return 2;
    if (nlen > 286 || ndist > 30) return 9;
    int idx;
    for (idx = 0; idx < ncode; ++idx) lengths[order[idx]] = (short)inf_bits(s, 3);
    for (; idx < 19; ++idx) lengths[order[idx]] = 0;
    if (s->err) return 2;
    if (inf_construct(&lc, lengths, 19) != 0) return 10;
    idx = 0;
    while (idx < nlen + ndist) {
        int sym = inf_decode(s, &lc); if (sym < 0 || s->err) return 2;
        if (sym < 16) lengths[idx++] = (short)sym;
        else {
            int len = 0, rep;
            if (sym == 16) { if (idx == 0) return 11; len = lengths[idx - 1]; rep = 3 + inf_bits(s, 2); }
            else if (sym == 17) rep = 3 + inf_bits(s, 3); else rep = 11 + inf_bits(s, 7);
            if (s->err) return 2;
            if (idx + rep > nlen + ndist) return 12;
            while (rep--) lengths[idx++] = (short)len;
        }
    }
    if (lengths[256] == 0) return 13;
    int e = inf_construct(&lc, lengths, nlen); if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) return 14;
    e = inf_construct(&dc, lengths + nlen, ndist); if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) return 15;
    return inf_codes(s, &lc, &dc);
}
static int cos_inflate_zlib(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstcap, size_t *outlen) {
    if (srclen < 6) return 20;
    if ((src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31 != 0 || (src[1] & 0x20)) return 21;          /* deflate, a valid header, no preset dictionary */
    inflate_t s = { src + 2, srclen - 2, 0, 0, 0, dst, dstcap, 0, 0 };
    int last, err = 0;
    do {
        last = inf_bits(&s, 1); int type = inf_bits(&s, 2); if (s.err) return 2;
        err = type == 0 ? inf_stored(&s) : type == 1 ? inf_fixed(&s) : type == 2 ? inf_dynamic(&s) : 16;
        if (err) return err;
    } while (!last);
    *outlen = s.outpos; return 0;
}
#endif
