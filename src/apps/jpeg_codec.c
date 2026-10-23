/*
 * jpeg_codec.c - see jpeg_codec.h
 *
 * Pipeline:  markers -> entropy decode (baseline / extended / progressive)
 *            -> [coefficient buffer] -> IDCT (islow or reduced) -> component
 *            planes -> per-row chroma upsampling -> colour conversion
 *            -> [area-average reduction] -> [EXIF orientation] -> BGRA out.
 *
 * Algorithms follow the IJG libjpeg reference (jdhuff.c, jdphuff.c, jidctint.c,
 * jidctred.c, jdsample.c, jdcolor.c, jdmaster.c) closely so output matches it.
 * All arithmetic that can see hostile input is done with unsigned or 64-bit
 * integers so UBSan stays quiet on corrupt files.
 */
#include "jpeg_codec.h"
#include "image_scale.h"
#include "memory.h"
#include "serial.h"
#include <string.h>

#define JD_MAX_COMP        4
#define HLOOK              9
#define JD_MAX_WORKSPACE   (256u * 1024u * 1024u)
#define JD_MAX_DIM         65535u

/* zigzag index -> natural (row-major) index, with 16 extra entries so a
 * corrupt run that overshoots 63 lands harmlessly on coefficient 63 (as in
 * libjpeg's jpeg_natural_order[]). */
static const uint8_t k_natural[80] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63,
    63, 63, 63, 63, 63, 63, 63, 63
};

/* Standard Huffman tables of T.81 Annex K (used if a stream has no DHT). */
static const uint8_t k_std_dc_lum_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t k_std_dc_lum_vals[12] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
};
static const uint8_t k_std_dc_chr_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t k_std_dc_chr_vals[12] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
};
static const uint8_t k_std_ac_lum_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125};
static const uint8_t k_std_ac_lum_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
};
static const uint8_t k_std_ac_chr_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119};
static const uint8_t k_std_ac_chr_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
    0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
};


/* ============================================================================
   Huffman tables and the bit reader
   ========================================================================== */
typedef struct {
    uint8_t  bits[17];            /* bits[1..16] = number of codes of that length */
    uint8_t  vals[256];
    uint16_t look[1u << HLOOK];   /* (len << 8) | symbol, 0 = not a short code */
    int32_t  maxcode[18];         /* largest code of each length, -1 if none */
    int32_t  valoff[17];          /* vals index = code + valoff[len] */
    uint8_t  defined;
} jhuff_t;

typedef struct {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf;
    int cnt;                      /* valid bits in buf (low bits) */
    int padded;                   /* of those, how many are zero padding past the data */
    int marker;                   /* marker met while reading (0 = none); 0xD9 at EOF */
    bool insufficient;            /* ran out of real data inside this segment */
} jbits_t;

static void bits_fill(jbits_t* b, int need) {
    while (b->cnt <= 56 && !b->marker) {
        if (b->p >= b->end) { b->marker = 0xD9; break; }
        uint32_t c = *b->p++;
        if (c == 0xFF) {
            uint32_t c2;
            do {
                if (b->p >= b->end) { b->marker = 0xD9; goto nomore; }
                c2 = *b->p++;
            } while (c2 == 0xFF);                 /* fill bytes */
            if (c2 != 0) { b->marker = (int)c2; break; }
        }
        b->buf = (b->buf << 8) | c;
        b->cnt += 8;
    }
nomore:
    if (b->cnt < need) {                          /* feed zero bits, like libjpeg */
        b->buf <<= (need - b->cnt);
        b->padded += need - b->cnt;
        b->cnt = need;
    }
}

static inline void bits_consume(jbits_t* b, int n) {
    b->cnt -= n;
    if (b->padded && b->cnt < b->padded) { b->insufficient = true; b->padded = b->cnt; }
}

static inline int bits_get(jbits_t* b, int n) {   /* 1 <= n <= 16 */
    if (b->cnt < n) bits_fill(b, n);
    int v = (int)((b->buf >> (b->cnt - n)) & ((1u << n) - 1u));
    bits_consume(b, n);
    return v;
}

static inline int recv_ext(jbits_t* b, int s) {   /* 1 <= s <= 16 */
    int v = bits_get(b, s);
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

/* Returns the decoded symbol, or -1 for a code that is not in the table. */
static inline int huff_dec(jbits_t* b, const jhuff_t* t) {
    if (b->cnt < 16) bits_fill(b, 16);
    uint32_t peek = (uint32_t)(b->buf >> (b->cnt - 16)) & 0xFFFFu;
    uint16_t e = t->look[peek >> (16 - HLOOK)];
    if (e) { bits_consume(b, e >> 8); return e & 0xFF; }
    for (int l = HLOOK + 1; l <= 16; ++l) {
        int32_t code = (int32_t)(peek >> (16 - l));
        if (code <= t->maxcode[l]) {
            int32_t idx = code + t->valoff[l];
            bits_consume(b, l);
            return (idx >= 0 && idx < 256) ? t->vals[idx] : -1;
        }
    }
    bits_consume(b, 16);
    return -1;
}

/* Builds decode tables from bits[]/vals[].  Fails for over-subscribed codes
 * (more codes of a length than the code space allows), more than 256 symbols,
 * or DC symbols above 15. */
static int huff_build(jhuff_t* t, bool is_dc) {
    int total = 0;
    for (int l = 1; l <= 16; ++l) total += t->bits[l];
    if (total > 256) return -1;
    memset(t->look, 0, sizeof(t->look));
    int32_t code = 0;
    int k = 0;
    for (int l = 1; l <= 16; ++l) {
        t->valoff[l] = k - code;
        for (int i = 0; i < t->bits[l]; ++i) {
            if (code >= (1 << l)) return -1;
            if (l <= HLOOK) {
                uint32_t first = (uint32_t)code << (HLOOK - l);
                uint32_t n = 1u << (HLOOK - l);
                for (uint32_t j = 0; j < n; ++j)
                    t->look[first + j] = (uint16_t)((l << 8) | t->vals[k]);
            }
            code++; k++;
        }
        t->maxcode[l] = t->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFF;
    if (is_dc) for (int i = 0; i < total; ++i) if (t->vals[i] > 15) return -1;
    t->defined = 1;
    return 0;
}

static void huff_load_std(jhuff_t* t, bool ac, int id) {
    const uint8_t* bits; const uint8_t* vals; int n;
    if (!ac) {
        bits = id ? k_std_dc_chr_bits : k_std_dc_lum_bits;
        vals = id ? k_std_dc_chr_vals : k_std_dc_lum_vals; n = 12;
    } else {
        bits = id ? k_std_ac_chr_bits : k_std_ac_lum_bits;
        vals = id ? k_std_ac_chr_vals : k_std_ac_lum_vals; n = 162;
    }
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 16; ++i) t->bits[i + 1] = bits[i];
    memcpy(t->vals, vals, (size_t)n);
    huff_build(t, !ac);
}

/* ============================================================================
   IDCT: jidctint.c "islow" and the jidctred.c reduced-size variants.
   Work is done in int64_t so hostile coefficients cannot overflow; for valid
   data the results are identical to libjpeg's 32-bit arithmetic.
   ========================================================================== */
#define CONST_BITS 13
#define F_0_298631336 2446
#define F_0_390180644 3196
#define F_0_541196100 4433
#define F_0_765366865 6270
#define F_0_899976223 7373
#define F_1_175875602 9633
#define F_1_501321110 12299
#define F_1_847759065 15137
#define F_1_961570560 16069
#define F_2_053119869 16819
#define F_2_562915447 20995
#define F_3_072711026 25172
/* jidctred.c extras */
#define F_0_211164243 1730
#define F_0_509795579 4176
#define F_0_601344887 4926
#define F_0_720959822 5906
#define F_0_850430095 6967
#define F_1_061594337 8697
#define F_1_272758580 10426
#define F_1_451774981 11893
#define F_2_172734803 17799
#define F_3_624509785 29692

typedef struct {
    int pass1;        /* PASS1_BITS: 2 for 8-bit, 1 for 12-bit samples */
    int center;       /* level shift: 128 or 2048 */
    int maxv;         /* 255 or 4095 */
    int oshift;       /* 0 or 4: reduce 12-bit results to 8 bit */
} idct_cfg_t;

static inline int64_t descale(int64_t x, int n) { return (x + ((int64_t)1 << (n - 1))) >> n; }

static inline int32_t deq(int16_t c, uint16_t q) {
    int32_t v = (int32_t)c * (int32_t)q;
    return v > 65535 ? 65535 : (v < -65535 ? -65535 : v);
}

static inline uint8_t out_px(int64_t v, const idct_cfg_t* g) {
    v += g->center;
    if (v < 0) v = 0; else if (v > g->maxv) v = g->maxv;
    if (g->oshift) { v = (v + (1 << (g->oshift - 1))) >> g->oshift; if (v > 255) v = 255; }
    return (uint8_t)v;
}

static void idct_8x8(const int16_t* in, const uint16_t* qt, uint8_t* out, size_t stride, const idct_cfg_t* g) {
    int32_t ws[64];
    const int pb = g->pass1;
    for (int c = 0; c < 8; ++c) {
        const int16_t* ip = in + c;
        const uint16_t* qp = qt + c;
        if (!ip[8] && !ip[16] && !ip[24] && !ip[32] && !ip[40] && !ip[48] && !ip[56]) {
            int32_t dc = (int32_t)((int64_t)deq(ip[0], qp[0]) * (1 << pb));
            for (int r = 0; r < 8; ++r) ws[r * 8 + c] = dc;
            continue;
        }
        int64_t z2 = deq(ip[16], qp[16]), z3 = deq(ip[48], qp[48]);
        int64_t z1 = (z2 + z3) * F_0_541196100;
        int64_t tmp2 = z1 + z3 * -F_1_847759065;
        int64_t tmp3 = z1 + z2 * F_0_765366865;
        z2 = deq(ip[0], qp[0]); z3 = deq(ip[32], qp[32]);
        int64_t tmp0 = (z2 + z3) * (1 << CONST_BITS), tmp1 = (z2 - z3) * (1 << CONST_BITS);
        int64_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = deq(ip[56], qp[56]); tmp1 = deq(ip[40], qp[40]);
        tmp2 = deq(ip[24], qp[24]); tmp3 = deq(ip[8], qp[8]);
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2;
        int64_t z4 = tmp1 + tmp3;
        int64_t z5 = (z3 + z4) * F_1_175875602;
        tmp0 *= F_0_298631336; tmp1 *= F_2_053119869; tmp2 *= F_3_072711026; tmp3 *= F_1_501321110;
        z1 *= -F_0_899976223; z2 *= -F_2_562915447; z3 *= -F_1_961570560; z4 *= -F_0_390180644;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        const int sh = CONST_BITS - pb;
        ws[0 * 8 + c] = (int32_t)descale(tmp10 + tmp3, sh);
        ws[7 * 8 + c] = (int32_t)descale(tmp10 - tmp3, sh);
        ws[1 * 8 + c] = (int32_t)descale(tmp11 + tmp2, sh);
        ws[6 * 8 + c] = (int32_t)descale(tmp11 - tmp2, sh);
        ws[2 * 8 + c] = (int32_t)descale(tmp12 + tmp1, sh);
        ws[5 * 8 + c] = (int32_t)descale(tmp12 - tmp1, sh);
        ws[3 * 8 + c] = (int32_t)descale(tmp13 + tmp0, sh);
        ws[4 * 8 + c] = (int32_t)descale(tmp13 - tmp0, sh);
    }
    for (int r = 0; r < 8; ++r) {
        const int32_t* w = ws + r * 8;
        uint8_t* o = out + (size_t)r * stride;
        if (!w[1] && !w[2] && !w[3] && !w[4] && !w[5] && !w[6] && !w[7]) {
            uint8_t v = out_px(descale(w[0], pb + 3), g);
            for (int i = 0; i < 8; ++i) o[i] = v;
            continue;
        }
        int64_t z2 = w[2], z3 = w[6];
        int64_t z1 = (z2 + z3) * F_0_541196100;
        int64_t tmp2 = z1 + z3 * -F_1_847759065;
        int64_t tmp3 = z1 + z2 * F_0_765366865;
        int64_t tmp0 = ((int64_t)w[0] + w[4]) * (1 << CONST_BITS);
        int64_t tmp1 = ((int64_t)w[0] - w[4]) * (1 << CONST_BITS);
        int64_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = w[7]; tmp1 = w[5]; tmp2 = w[3]; tmp3 = w[1];
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2;
        int64_t z4 = tmp1 + tmp3;
        int64_t z5 = (z3 + z4) * F_1_175875602;
        tmp0 *= F_0_298631336; tmp1 *= F_2_053119869; tmp2 *= F_3_072711026; tmp3 *= F_1_501321110;
        z1 *= -F_0_899976223; z2 *= -F_2_562915447; z3 *= -F_1_961570560; z4 *= -F_0_390180644;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        const int sh = CONST_BITS + pb + 3;
        o[0] = out_px(descale(tmp10 + tmp3, sh), g);
        o[7] = out_px(descale(tmp10 - tmp3, sh), g);
        o[1] = out_px(descale(tmp11 + tmp2, sh), g);
        o[6] = out_px(descale(tmp11 - tmp2, sh), g);
        o[2] = out_px(descale(tmp12 + tmp1, sh), g);
        o[5] = out_px(descale(tmp12 - tmp1, sh), g);
        o[3] = out_px(descale(tmp13 + tmp0, sh), g);
        o[4] = out_px(descale(tmp13 - tmp0, sh), g);
    }
}

static void idct_4x4(const int16_t* in, const uint16_t* qt, uint8_t* out, size_t stride, const idct_cfg_t* g) {
    int32_t ws[8 * 4];
    const int pb = g->pass1;
    for (int c = 0; c < 8; ++c) {
        if (c == 4) continue;
        const int16_t* ip = in + c;
        const uint16_t* qp = qt + c;
        if (!ip[8] && !ip[16] && !ip[24] && !ip[40] && !ip[48] && !ip[56]) {
            int32_t dc = (int32_t)((int64_t)deq(ip[0], qp[0]) * (1 << pb));
            for (int r = 0; r < 4; ++r) ws[r * 8 + c] = dc;
            continue;
        }
        int64_t tmp0 = (int64_t)deq(ip[0], qp[0]) * (1 << (CONST_BITS + 1));
        int64_t z2 = deq(ip[16], qp[16]), z3 = deq(ip[48], qp[48]);
        int64_t tmp2 = z2 * F_1_847759065 + z3 * -F_0_765366865;
        int64_t tmp10 = tmp0 + tmp2, tmp12 = tmp0 - tmp2;
        int64_t z1 = deq(ip[56], qp[56]); z2 = deq(ip[40], qp[40]);
        z3 = deq(ip[24], qp[24]); int64_t z4 = deq(ip[8], qp[8]);
        tmp0 = z1 * -F_0_211164243 + z2 * F_1_451774981 + z3 * -F_2_172734803 + z4 * F_1_061594337;
        tmp2 = z1 * -F_0_509795579 + z2 * -F_0_601344887 + z3 * F_0_899976223 + z4 * F_2_562915447;
        const int sh = CONST_BITS - pb + 1;
        ws[0 * 8 + c] = (int32_t)descale(tmp10 + tmp2, sh);
        ws[3 * 8 + c] = (int32_t)descale(tmp10 - tmp2, sh);
        ws[1 * 8 + c] = (int32_t)descale(tmp12 + tmp0, sh);
        ws[2 * 8 + c] = (int32_t)descale(tmp12 - tmp0, sh);
    }
    for (int r = 0; r < 4; ++r) {
        const int32_t* w = ws + r * 8;
        uint8_t* o = out + (size_t)r * stride;
        if (!w[1] && !w[2] && !w[3] && !w[5] && !w[6] && !w[7]) {
            uint8_t v = out_px(descale(w[0], pb + 3), g);
            o[0] = o[1] = o[2] = o[3] = v;
            continue;
        }
        int64_t tmp0 = (int64_t)w[0] * (1 << (CONST_BITS + 1));
        int64_t tmp2 = (int64_t)w[2] * F_1_847759065 + (int64_t)w[6] * -F_0_765366865;
        int64_t tmp10 = tmp0 + tmp2, tmp12 = tmp0 - tmp2;
        int64_t z1 = w[7], z2 = w[5], z3 = w[3], z4 = w[1];
        tmp0 = z1 * -F_0_211164243 + z2 * F_1_451774981 + z3 * -F_2_172734803 + z4 * F_1_061594337;
        tmp2 = z1 * -F_0_509795579 + z2 * -F_0_601344887 + z3 * F_0_899976223 + z4 * F_2_562915447;
        const int sh = CONST_BITS + pb + 3 + 1;
        o[0] = out_px(descale(tmp10 + tmp2, sh), g);
        o[3] = out_px(descale(tmp10 - tmp2, sh), g);
        o[1] = out_px(descale(tmp12 + tmp0, sh), g);
        o[2] = out_px(descale(tmp12 - tmp0, sh), g);
    }
}

static void idct_2x2(const int16_t* in, const uint16_t* qt, uint8_t* out, size_t stride, const idct_cfg_t* g) {
    int32_t ws[8 * 2];
    const int pb = g->pass1;
    for (int c = 0; c < 8; ++c) {
        if (c == 2 || c == 4 || c == 6) continue;
        const int16_t* ip = in + c;
        const uint16_t* qp = qt + c;
        if (!ip[8] && !ip[24] && !ip[40] && !ip[56]) {
            int32_t dc = (int32_t)((int64_t)deq(ip[0], qp[0]) * (1 << pb));
            ws[c] = dc; ws[8 + c] = dc;
            continue;
        }
        int64_t tmp10 = (int64_t)deq(ip[0], qp[0]) * (1 << (CONST_BITS + 2));
        int64_t tmp0 = (int64_t)deq(ip[56], qp[56]) * -F_0_720959822
                     + (int64_t)deq(ip[40], qp[40]) * F_0_850430095
                     + (int64_t)deq(ip[24], qp[24]) * -F_1_272758580
                     + (int64_t)deq(ip[8], qp[8]) * F_3_624509785;
        const int sh = CONST_BITS - pb + 2;
        ws[c] = (int32_t)descale(tmp10 + tmp0, sh);
        ws[8 + c] = (int32_t)descale(tmp10 - tmp0, sh);
    }
    for (int r = 0; r < 2; ++r) {
        const int32_t* w = ws + r * 8;
        uint8_t* o = out + (size_t)r * stride;
        if (!w[1] && !w[3] && !w[5] && !w[7]) {
            uint8_t v = out_px(descale(w[0], pb + 3), g);
            o[0] = o[1] = v;
            continue;
        }
        int64_t tmp10 = (int64_t)w[0] * (1 << (CONST_BITS + 2));
        int64_t tmp0 = (int64_t)w[7] * -F_0_720959822 + (int64_t)w[5] * F_0_850430095
                     + (int64_t)w[3] * -F_1_272758580 + (int64_t)w[1] * F_3_624509785;
        const int sh = CONST_BITS + pb + 3 + 2;
        o[0] = out_px(descale(tmp10 + tmp0, sh), g);
        o[1] = out_px(descale(tmp10 - tmp0, sh), g);
    }
}

static void idct_1x1(const int16_t* in, const uint16_t* qt, uint8_t* out, size_t stride, const idct_cfg_t* g) {
    (void)stride;
    out[0] = out_px(descale(deq(in[0], qt[0]), 3), g);
}

/* ============================================================================
   Decoder state
   ========================================================================== */
enum { UP_FULL = 0, UP_H2V1F, UP_H2V1, UP_H1V2F, UP_H2V2F, UP_H2V2, UP_INT, UP_NEAREST };
enum { SC_SEQ = 0, SC_DC_FIRST, SC_DC_REFINE, SC_AC_FIRST, SC_AC_REFINE };

typedef struct {
    uint8_t id, h, v, tq;
    uint8_t dc_tbl, ac_tbl;
    uint8_t ssize;                      /* DCT scaled size 1/2/4/8 (libjpeg DCT_scaled_size) */
    uint8_t latched;
    uint8_t up_mode, hexp, vexp, in_scan;
    uint32_t wblocks, hblocks;          /* blocks that cover the real component */
    uint32_t mcu_wb, mcu_hb;            /* blocks including MCU padding */
    uint32_t dw, dh;                    /* real component size in (scaled) samples */
    uint32_t pw, ph;                    /* plane size in samples (padded) */
    int32_t pred;
    uint8_t* plane;
    int16_t* coef;                      /* multi-scan files only */
    uint16_t qt[64];
} jcomp_t;

typedef struct {
    int ncs;
    int idx[JD_MAX_COMP];
    int ss, se, ah, al;
} jscan_t;

typedef struct {
    const uint8_t* data;
    size_t size;
    uint64_t max_w, max_h;
    unsigned flags;

    uint16_t qt[4][64];                 /* natural order */
    uint8_t qt_def[4];
    jhuff_t dc[4], ac[4];

    jcomp_t comp[JD_MAX_COMP];
    int ncomp;
    uint32_t width, height;
    int precision;
    int sof;                            /* 0 baseline, 1 extended, 2 progressive */
    int restart_interval;
    bool got_sof;
    bool saw_jfif, saw_adobe;
    int adobe_transform;
    int orientation;                    /* 1..8 */
    int hmax, vmax;
    uint32_t mcux, mcuy;

    bool setup_done, buffered, truncated;
    int scans_done;
    int den;                            /* DCT scale denominator 1/2/4/8 */
    int cs;
    idct_cfg_t cfg;

    uint8_t* ws;
    size_t ws_bytes;
    uint8_t* urow[JD_MAX_COMP];
    uint8_t* line;                      /* BGRA scratch row */
    uint32_t ubw;
    uint32_t sw, sh;                    /* size after DCT scaling, before rotation */
    int orient;                         /* orientation actually applied (1 = none) */
} jdec_t;

static uint32_t ceil_div(uint64_t a, uint64_t b) { return (uint32_t)((a + b - 1) / b); }
static inline uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }

/* ============================================================================
   Marker parsing
   ========================================================================== */
/* Finds the next marker at or after *pos (skipping garbage, 0xFF fill and
 * stuffed 0xFF00).  Returns the marker code and leaves *pos after it, or -1. */
static int next_marker(const uint8_t* data, size_t size, size_t* pos) {
    size_t p = *pos;
    for (;;) {
        while (p < size && data[p] != 0xFF) p++;
        while (p < size && data[p] == 0xFF) p++;
        if (p >= size) { *pos = size; return -1; }
        int m = data[p++];
        if (m != 0) { *pos = p; return m; }
    }
}

static int parse_dqt(jdec_t* d, const uint8_t* s, size_t n) {
    while (n > 0) {
        int pq = s[0] >> 4, tq = s[0] & 15;
        s++; n--;
        if (pq > 1 || tq > 3) return -1;
        size_t need = pq ? 128 : 64;
        if (n < need) return -1;
        for (int i = 0; i < 64; ++i)
            d->qt[tq][k_natural[i]] = pq ? (uint16_t)be16(s + 2 * i) : s[i];
        d->qt_def[tq] = 1;
        s += need; n -= need;
    }
    return 0;
}

static int parse_dht(jdec_t* d, const uint8_t* s, size_t n) {
    while (n > 0) {
        if (n < 17) return -1;
        int tc = s[0] >> 4, th = s[0] & 15;
        if (tc > 1 || th > 3) return -1;
        jhuff_t* t = tc ? &d->ac[th] : &d->dc[th];
        memset(t, 0, sizeof(*t));
        int total = 0;
        for (int i = 1; i <= 16; ++i) { t->bits[i] = s[i]; total += s[i]; }
        if (total > 256 || (size_t)(17 + total) > n) return -1;
        memcpy(t->vals, s + 17, (size_t)total);
        if (huff_build(t, tc == 0) != 0) { t->defined = 0; return -1; }
        s += 17 + total; n -= (size_t)(17 + total);
    }
    return 0;
}

static int parse_sof(jdec_t* d, int marker, const uint8_t* s, size_t n) {
    if (d->got_sof) return -1;                       /* duplicate SOF */
    if (n < 6) return -1;
    d->precision = s[0];
    d->height = be16(s + 1);
    d->width = be16(s + 3);
    d->ncomp = s[5];
    if (d->precision != 8 && d->precision != 12) return -2;
    if (marker == 0xC0 && d->precision != 8) return -2;
    if (d->ncomp < 1 || d->ncomp > JD_MAX_COMP) return -2;
    if (d->width == 0) return -1;
    if (n < 6 + 3u * (size_t)d->ncomp) return -1;
    d->hmax = d->vmax = 1;
    for (int i = 0; i < d->ncomp; ++i) {
        jcomp_t* c = &d->comp[i];
        c->id = s[6 + i * 3];
        c->h = s[7 + i * 3] >> 4;
        c->v = s[7 + i * 3] & 15;
        c->tq = s[8 + i * 3];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3) return -1;
        if (c->h > d->hmax) d->hmax = c->h;
        if (c->v > d->vmax) d->vmax = c->v;
    }
    d->sof = marker == 0xC2 ? 2 : (marker == 0xC1 ? 1 : 0);
    d->got_sof = true;
    return 0;
}

static int parse_sos(jdec_t* d, const uint8_t* s, size_t n, jscan_t* sc) {
    if (n < 1) return -1;
    int ns = s[0];
    if (ns < 1 || ns > d->ncomp || n < 4u + 2u * (size_t)ns) return -1;
    sc->ncs = ns;
    for (int i = 0; i < ns; ++i) {
        int cs = s[1 + 2 * i], tt = s[2 + 2 * i];
        int found = -1;
        for (int j = 0; j < d->ncomp && found < 0; ++j) {
            bool used = false;
            for (int k = 0; k < i; ++k) if (sc->idx[k] == j) used = true;
            if (!used && d->comp[j].id == cs) found = j;
        }
        if (found < 0) return -1;
        sc->idx[i] = found;
        d->comp[found].dc_tbl = (uint8_t)(tt >> 4);
        d->comp[found].ac_tbl = (uint8_t)(tt & 15);
    }
    sc->ss = s[1 + 2 * ns];
    sc->se = s[2 + 2 * ns];
    sc->ah = s[3 + 2 * ns] >> 4;
    sc->al = s[3 + 2 * ns] & 15;
    return 0;
}

static int exif_orientation(const uint8_t* p, size_t n) {
    if (n < 14 || memcmp(p, "Exif\0\0", 6) != 0) return 0;
    const uint8_t* t = p + 6;
    size_t tn = n - 6;
    bool le;
    if (t[0] == 'I' && t[1] == 'I') le = true;
    else if (t[0] == 'M' && t[1] == 'M') le = false;
    else return 0;
    #define R16(o) (le ? ((uint32_t)t[o] | ((uint32_t)t[(o) + 1] << 8)) : (((uint32_t)t[o] << 8) | t[(o) + 1]))
    #define R32(o) (le ? ((uint32_t)t[o] | ((uint32_t)t[(o)+1] << 8) | ((uint32_t)t[(o)+2] << 16) | ((uint32_t)t[(o)+3] << 24)) \
                       : (((uint32_t)t[o] << 24) | ((uint32_t)t[(o)+1] << 16) | ((uint32_t)t[(o)+2] << 8) | t[(o)+3]))
    if (R16(2) != 42) return 0;
    uint64_t ifd = R32(4);
    if (ifd + 2 > tn) return 0;
    uint32_t cnt = R16(ifd);
    for (uint32_t i = 0; i < cnt; ++i) {
        uint64_t o = ifd + 2 + (uint64_t)i * 12;
        if (o + 12 > tn) break;
        if (R16(o) == 0x0112) {
            uint32_t type = R16(o + 2), count = R32(o + 4);
            if (type == 3 && count >= 1) {
                uint32_t v = R16(o + 8);
                return (v >= 1 && v <= 8) ? (int)v : 0;
            }
            return 0;
        }
    }
    #undef R16
    #undef R32
    return 0;
}

static void parse_app(jdec_t* d, int marker, const uint8_t* s, size_t n) {
    if (marker == 0xE0) {
        if (n >= 5 && memcmp(s, "JFIF\0", 5) == 0) d->saw_jfif = true;
    } else if (marker == 0xE1) {
        if (d->orientation == 1) {
            int o = exif_orientation(s, n);
            if (o) d->orientation = o;
        }
    } else if (marker == 0xEE) {
        if (n >= 12 && memcmp(s, "Adobe", 5) == 0) {
            d->saw_adobe = true;
            d->adobe_transform = s[11];
        }
    }
}

static int pick_colorspace(const jdec_t* d) {
    switch (d->ncomp) {
    case 1: return JPEG_CS_GRAY;
    case 3:
        if (d->saw_jfif) return JPEG_CS_YCBCR;
        if (d->saw_adobe) return d->adobe_transform == 0 ? JPEG_CS_RGB : JPEG_CS_YCBCR;
        if (d->comp[0].id == 'R' && d->comp[1].id == 'G' && d->comp[2].id == 'B') return JPEG_CS_RGB;
        return JPEG_CS_YCBCR;
    case 4:
        if (d->saw_adobe) return d->adobe_transform == 0 ? JPEG_CS_CMYK : JPEG_CS_YCCK;
        return JPEG_CS_CMYK;
    default: return JPEG_CS_UNKNOWN;
    }
}

/* DNL (define number of lines) directly after the first scan supplies the
 * height when the frame header says 0. */
static uint32_t find_dnl(const jdec_t* d, size_t pos) {
    const uint8_t* data = d->data;
    for (;;) {
        int m = next_marker(data, d->size, &pos);
        if (m < 0) return 0;
        if (m >= 0xD0 && m <= 0xD7) continue;
        if (m == 0xDC) {
            if (pos + 4 <= d->size && be16(data + pos) == 4) return be16(data + pos + 2);
            return 0;
        }
        return 0;
    }
}

/* ============================================================================
   Frame setup at the first SOS: scale, plane geometry, one workspace block
   ========================================================================== */
#define WS_MAGIC 0x4A50454743414E41ull   /* canary behind the workspace */

static size_t align16(size_t v) { return (v + 15u) & ~(size_t)15u; }

static void setup_upsample(jdec_t* d, int N) {
    bool fancy = N > 1;
    for (int i = 0; i < d->ncomp; ++i) {
        jcomp_t* c = &d->comp[i];
        int hin = c->h * c->ssize / N, vin = c->v * c->ssize / N;
        int hout = d->hmax, vout = d->vmax;
        c->hexp = c->vexp = 1;
        if (hin == hout && vin == vout) c->up_mode = UP_FULL;
        else if (hin * 2 == hout && vin == vout) {
            c->up_mode = (fancy && c->dw > 2) ? UP_H2V1F : UP_H2V1; c->hexp = 2;
        } else if (hin == hout && vin * 2 == vout && fancy) {
            c->up_mode = UP_H1V2F; c->vexp = 2;
        } else if (hin * 2 == hout && vin * 2 == vout) {
            c->up_mode = (fancy && c->dw > 2) ? UP_H2V2F : UP_H2V2; c->hexp = c->vexp = 2;
        } else if (hout % hin == 0 && vout % vin == 0) {
            c->up_mode = UP_INT; c->hexp = (uint8_t)(hout / hin); c->vexp = (uint8_t)(vout / vin);
        } else {
            c->up_mode = UP_NEAREST;
        }
    }
}

static int jd_setup(jdec_t* d, const jscan_t* first, uint32_t dnl_height, jpeg_info_t* info) {
    if (d->height == 0) d->height = dnl_height;
    if (d->height == 0 || d->width == 0) return JPEG_ERROR_INVALID_FORMAT;
    d->cs = pick_colorspace(d);

    /* Cheap bomb guard: even at the best case of ~2 bits per block, a frame
     * cannot have more blocks than ~(file size * 4); allow 64x slack for
     * truncated files, reject frames that claim far more. */
    uint64_t total_blocks = 0;
    for (int i = 0; i < d->ncomp; ++i) {
        jcomp_t* c = &d->comp[i];
        c->wblocks = ceil_div((uint64_t)d->width * c->h, (uint64_t)d->hmax * 8);
        c->hblocks = ceil_div((uint64_t)d->height * c->v, (uint64_t)d->vmax * 8);
        total_blocks += (uint64_t)c->wblocks * c->hblocks;
    }
    if (total_blocks > ((uint64_t)d->size + 4096u) * 256u) {
        serial_puts("[JPEG] Frame claims far more data than the file holds\n");
        return JPEG_ERROR_INVALID_FORMAT;
    }
    d->mcux = ceil_div(d->width, (uint64_t)d->hmax * 8);
    d->mcuy = ceil_div(d->height, (uint64_t)d->vmax * 8);

    d->orient = (d->flags & JPEG_DEC_NO_ORIENT) ? 1 : d->orientation;

    /* DCT-domain scale: smallest divisor whose (rotated) result fits. */
    bool fit = (d->flags & JPEG_DEC_FIT) != 0;
    int den = 0;
    for (int dv = 1; dv <= 8; dv *= 2) {
        uint64_t w = ceil_div(d->width, (uint64_t)dv), h = ceil_div(d->height, (uint64_t)dv);
        uint64_t ow = d->orient >= 5 ? h : w, oh = d->orient >= 5 ? w : h;
        if (ow <= d->max_w && oh <= d->max_h) { den = dv; break; }
        if (!fit) break;
    }
    if (den == 0) {
        if (!fit) {
            if (info) {
                bool sw_ = d->orient >= 5;
                info->width = sw_ ? d->height : d->width;
                info->height = sw_ ? d->width : d->height;
            }
            return JPEG_ERROR_BUFFER_OVERFLOW;
        }
        den = 8;
    }
    d->den = den;
    int N = 8 / den;
    d->sw = ceil_div(d->width, (uint64_t)den);
    d->sh = ceil_div(d->height, (uint64_t)den);

    d->cfg.pass1 = d->precision == 12 ? 1 : 2;
    d->cfg.center = d->precision == 12 ? 2048 : 128;
    d->cfg.maxv = d->precision == 12 ? 4095 : 255;
    d->cfg.oshift = d->precision == 12 ? 4 : 0;

    d->buffered = d->sof == 2 || first->ncs < d->ncomp;

    uint64_t total = 0;
    for (int i = 0; i < d->ncomp; ++i) {
        jcomp_t* c = &d->comp[i];
        int ss = N;
        while (ss < 8 && (d->hmax * N) % (c->h * ss * 2) == 0 && (d->vmax * N) % (c->v * ss * 2) == 0) ss *= 2;
        c->ssize = (uint8_t)ss;
        c->mcu_wb = d->mcux * c->h;
        c->mcu_hb = d->mcuy * c->v;
        c->dw = ceil_div((uint64_t)d->width * c->h * (uint64_t)ss, (uint64_t)d->hmax * 8);
        c->dh = ceil_div((uint64_t)d->height * c->v * (uint64_t)ss, (uint64_t)d->vmax * 8);
        c->pw = c->mcu_wb * (uint32_t)ss;
        c->ph = c->mcu_hb * (uint32_t)ss;
        total += align16((size_t)((uint64_t)c->pw * c->ph));
        if (d->buffered) total += align16((size_t)((uint64_t)c->mcu_wb * c->mcu_hb * 64u * 2u));
        if (total > JD_MAX_WORKSPACE) {
            serial_puts("[JPEG] Image too large for the decoder workspace\n");
            return JPEG_ERROR_BUFFER_OVERFLOW;
        }
    }
    setup_upsample(d, N);
    uint32_t ubw = d->sw;
    for (int i = 0; i < d->ncomp; ++i) {
        jcomp_t* c = &d->comp[i];
        uint32_t w = c->up_mode == UP_FULL ? 0 : (c->up_mode == UP_NEAREST ? d->sw : c->dw * c->hexp);
        if (w > ubw) ubw = w;
    }
    d->ubw = ubw + 32;
    total += (size_t)d->ubw * (size_t)d->ncomp + (size_t)d->sw * 4u + 64u + 16u;
    if (total > JD_MAX_WORKSPACE) return JPEG_ERROR_BUFFER_OVERFLOW;

    d->ws_bytes = (size_t)total;
    d->ws = (uint8_t*)kmalloc(d->ws_bytes);
    if (!d->ws) return JPEG_ERROR_BUFFER_OVERFLOW;
    uint8_t* p = d->ws;
    for (int i = 0; i < d->ncomp; ++i) {
        jcomp_t* c = &d->comp[i];
        size_t pb = (size_t)((uint64_t)c->pw * c->ph);
        c->plane = p; memset(p, 128, pb);                 /* missing data stays gray */
        p += align16(pb);
        if (d->buffered) {
            size_t cb = (size_t)((uint64_t)c->mcu_wb * c->mcu_hb * 64u * 2u);
            c->coef = (int16_t*)(void*)p; memset(p, 0, cb);
            p += align16(cb);
        }
    }
    for (int i = 0; i < d->ncomp; ++i) { d->urow[i] = p; p += d->ubw; }
    d->line = p; p += (size_t)d->sw * 4u + 64u;
    (void)p;
    /* canary: last 8 bytes of the block (16 bytes were reserved for it) */
    uint64_t magic = WS_MAGIC;
    memcpy(d->ws + d->ws_bytes - 8, &magic, 8);

    d->setup_done = true;
    return JPEG_ERROR_OK;
}

static void jd_free_workspace(jdec_t* d) {
    if (!d->ws) return;
    uint64_t magic = 0;
    memcpy(&magic, d->ws + d->ws_bytes - 8, 8);
    if (magic != WS_MAGIC) serial_puts("[JPEG] WARNING: workspace canary overwritten\n");
    kfree(d->ws);
    d->ws = NULL;
}

/* ============================================================================
   Entropy decoding
   ========================================================================== */
static int br_next_marker(jbits_t* b) {
    for (;;) {
        while (b->p < b->end && *b->p != 0xFF) b->p++;
        while (b->p < b->end && *b->p == 0xFF) b->p++;
        if (b->p >= b->end) return 0xD9;
        int m = *b->p++;
        if (m != 0) return m;
    }
}

/* libjpeg's jdmarker.c jpeg_resync_to_restart() decision procedure. */
static void resync_to_restart(jbits_t* b, int desired) {
    int marker = b->marker;
    for (;;) {
        int action;
        if (marker < 0xC0) action = 2;
        else if (marker < 0xD0 || marker > 0xD7) action = 3;
        else if (marker == 0xD0 + ((desired + 1) & 7) || marker == 0xD0 + ((desired + 2) & 7)) action = 3;
        else if (marker == 0xD0 + ((desired - 1) & 7) || marker == 0xD0 + ((desired - 2) & 7)) action = 2;
        else action = 1;
        if (action == 1) { b->marker = 0; return; }
        if (action == 3) { b->marker = marker; return; }
        marker = br_next_marker(b);
    }
}

static void process_restart(jbits_t* b, int* next_rst) {
    b->cnt = 0; b->buf = 0; b->padded = 0;          /* discard the remaining bits (byte align) */
    if (b->marker == 0) b->marker = br_next_marker(b);
    if (b->marker == 0xD0 + *next_rst) b->marker = 0;
    else resync_to_restart(b, *next_rst);
    *next_rst = (*next_rst + 1) & 7;
    if (b->marker == 0) b->insufficient = false;
}

static void blk_seq(jbits_t* b, jcomp_t* c, const jhuff_t* dc, const jhuff_t* ac, int16_t* blk) {
    int s = huff_dec(b, dc);
    if (s < 0) s = 0;
    int diff = s ? recv_ext(b, s) : 0;
    c->pred = (int32_t)((uint32_t)c->pred + (uint32_t)diff);
    blk[0] = (int16_t)c->pred;
    for (int k = 1; k < 64; ++k) {
        int rs = huff_dec(b, ac);
        if (rs < 0) rs = 0;
        int r = rs >> 4, sz = rs & 15;
        if (sz) {
            k += r;
            blk[k_natural[k]] = (int16_t)recv_ext(b, sz);
        } else {
            if (r != 15) break;
            k += 15;
        }
    }
}

static void blk_dc_first(jbits_t* b, jcomp_t* c, const jhuff_t* dc, int16_t* blk, int al) {
    int s = huff_dec(b, dc);
    if (s < 0) s = 0;
    int diff = s ? recv_ext(b, s) : 0;
    c->pred = (int32_t)((uint32_t)c->pred + (uint32_t)diff);
    blk[0] = (int16_t)((uint32_t)c->pred << al);
}

static void blk_dc_refine(jbits_t* b, int16_t* blk, int al) {
    if (bits_get(b, 1)) blk[0] = (int16_t)(blk[0] | (1 << al));
}

static void blk_ac_first(jbits_t* b, const jhuff_t* ac, int16_t* blk, int ss, int se, int al, int32_t* eobrun) {
    if (*eobrun > 0) { (*eobrun)--; return; }
    for (int k = ss; k <= se; ++k) {
        int rs = huff_dec(b, ac);
        if (rs < 0) rs = 0;
        int r = rs >> 4, s = rs & 15;
        if (s) {
            k += r;
            int v = recv_ext(b, s);
            blk[k_natural[k]] = (int16_t)(v * (1 << al));
        } else if (r == 15) {
            k += 15;
        } else {
            *eobrun = 1 << r;
            if (r) *eobrun += bits_get(b, r);
            (*eobrun)--;
            break;
        }
    }
}

static void blk_ac_refine(jbits_t* b, const jhuff_t* ac, int16_t* blk, int ss, int se, int al, int32_t* eobrun) {
    int p1 = 1 << al, m1 = -(1 << al);
    int k = ss;
    if (*eobrun == 0) {
        for (; k <= se; ++k) {
            int rs = huff_dec(b, ac);
            if (rs < 0) rs = 0;
            int r = rs >> 4, s = rs & 15;
            if (s) {
                s = bits_get(b, 1) ? p1 : m1;       /* size of a new coefficient is always 1 */
            } else if (r != 15) {
                *eobrun = 1 << r;
                if (r) *eobrun += bits_get(b, r);
                break;
            }
            do {
                int16_t* tc = &blk[k_natural[k]];
                if (*tc != 0) {
                    if (bits_get(b, 1) && (*tc & p1) == 0)
                        *tc = (int16_t)(*tc + (*tc >= 0 ? p1 : m1));
                } else if (--r < 0) {
                    break;
                }
                k++;
            } while (k <= se);
            if (s) blk[k_natural[k]] = (int16_t)s;
        }
    }
    if (*eobrun > 0) {
        for (; k <= se; ++k) {
            int16_t* tc = &blk[k_natural[k]];
            if (*tc != 0 && bits_get(b, 1) && (*tc & p1) == 0)
                *tc = (int16_t)(*tc + (*tc >= 0 ? p1 : m1));
        }
        (*eobrun)--;
    }
}

static void idct_block(const jdec_t* d, const jcomp_t* c, const int16_t* blk, uint8_t* dst) {
    switch (c->ssize) {
    case 8: idct_8x8(blk, c->qt, dst, c->pw, &d->cfg); break;
    case 4: idct_4x4(blk, c->qt, dst, c->pw, &d->cfg); break;
    case 2: idct_2x2(blk, c->qt, dst, c->pw, &d->cfg); break;
    default: idct_1x1(blk, c->qt, dst, c->pw, &d->cfg); break;
    }
}

static jhuff_t* ensure_table(jdec_t* d, bool ac, int id) {
    if (id > 3) return NULL;
    jhuff_t* t = ac ? &d->ac[id] : &d->dc[id];
    if (!t->defined) {
        if (id > 1) return NULL;
        huff_load_std(t, ac, id);
    }
    return t;
}

/* Decodes one scan starting at byte offset `start`.  Returns 0, or <0 if the
 * scan parameters/tables are unusable (nothing decoded).  On return *endpos /
 * *pending locate the marker that ended the scan (pending = -1: search). */
static int run_scan(jdec_t* d, const jscan_t* sc, size_t start, size_t* endpos, int* pending) {
    int kind = SC_SEQ;
    if (d->sof == 2) {
        if (sc->ss == 0) {
            if (sc->se != 0) return -1;
            kind = sc->ah == 0 ? SC_DC_FIRST : SC_DC_REFINE;
        } else {
            if (sc->se < sc->ss || sc->se > 63 || sc->ncs != 1) return -1;
            kind = sc->ah == 0 ? SC_AC_FIRST : SC_AC_REFINE;
        }
        if (sc->al > 13 || (sc->ah != 0 && sc->al != sc->ah - 1)) return -1;
    }
    const jhuff_t* dct[JD_MAX_COMP] = {0};
    const jhuff_t* act[JD_MAX_COMP] = {0};
    for (int i = 0; i < sc->ncs; ++i) {
        jcomp_t* c = &d->comp[sc->idx[i]];
        if (!c->latched) {
            if (!d->qt_def[c->tq]) return -1;
            memcpy(c->qt, d->qt[c->tq], sizeof(c->qt));
            c->latched = 1;
        }
        if (kind == SC_SEQ || kind == SC_DC_FIRST) {
            dct[i] = ensure_table(d, false, c->dc_tbl);
            if (!dct[i]) return -1;
        }
        if (kind == SC_SEQ || kind == SC_AC_FIRST || kind == SC_AC_REFINE) {
            act[i] = ensure_table(d, true, c->ac_tbl);
            if (!act[i]) return -1;
        }
        c->pred = 0;
        c->in_scan = 1;
    }

    jbits_t b;
    memset(&b, 0, sizeof(b));
    b.p = d->data + start;
    b.end = d->data + d->size;

    uint32_t mcus_x, mcus_y;
    if (sc->ncs == 1) {
        const jcomp_t* c = &d->comp[sc->idx[0]];
        mcus_x = c->wblocks; mcus_y = c->hblocks;
    } else {
        mcus_x = d->mcux; mcus_y = d->mcuy;
    }

    const int ri = d->restart_interval;
    int togo = ri, next_rst = 0;
    int32_t eobrun = 0;

    for (uint32_t my = 0; my < mcus_y; ++my) {
        for (uint32_t mx = 0; mx < mcus_x; ++mx) {
            if (ri && togo == 0) {
                process_restart(&b, &next_rst);
                for (int i = 0; i < sc->ncs; ++i) d->comp[sc->idx[i]].pred = 0;
                eobrun = 0;
                togo = ri;
            }
            if (!b.insufficient) {
                for (int i = 0; i < sc->ncs; ++i) {
                    jcomp_t* c = &d->comp[sc->idx[i]];
                    int nh = sc->ncs == 1 ? 1 : c->h, nv = sc->ncs == 1 ? 1 : c->v;
                    for (int sy = 0; sy < nv; ++sy) {
                        for (int sx = 0; sx < nh; ++sx) {
                            uint32_t bx = sc->ncs == 1 ? mx : mx * c->h + (uint32_t)sx;
                            uint32_t by = sc->ncs == 1 ? my : my * c->v + (uint32_t)sy;
                            int16_t tmp[64];
                            int16_t* blk;
                            if (c->coef) blk = c->coef + ((size_t)by * c->mcu_wb + bx) * 64u;
                            else { memset(tmp, 0, sizeof(tmp)); blk = tmp; }
                            switch (kind) {
                            case SC_SEQ:
                                if (c->coef) memset(blk, 0, 64 * sizeof(int16_t));
                                blk_seq(&b, c, dct[i], act[i], blk);
                                if (!c->coef && bx < c->wblocks && by < c->hblocks)
                                    idct_block(d, c, blk, c->plane + ((size_t)by * c->ssize) * c->pw + (size_t)bx * c->ssize);
                                break;
                            case SC_DC_FIRST:  blk_dc_first(&b, c, dct[i], blk, sc->al); break;
                            case SC_DC_REFINE: blk_dc_refine(&b, blk, sc->al); break;
                            case SC_AC_FIRST:  blk_ac_first(&b, act[i], blk, sc->ss, sc->se, sc->al, &eobrun); break;
                            default:           blk_ac_refine(&b, act[i], blk, sc->ss, sc->se, sc->al, &eobrun); break;
                            }
                        }
                    }
                }
            } else if (!ri) {
                goto finish;                       /* no restart can ever resync: rest stays gray */
            }
            if (ri) togo--;
        }
    }
finish:
    if (b.insufficient) d->truncated = true;
    *endpos = (size_t)(b.p - d->data);
    *pending = b.marker ? b.marker : -1;
    return 0;
}

/* Multi-scan files: IDCT every buffered block once all scans are in. */
static void finalize_coefficients(jdec_t* d) {
    for (int i = 0; i < d->ncomp; ++i) {
        jcomp_t* c = &d->comp[i];
        if (!c->coef) continue;
        if (!c->latched) {
            if (d->qt_def[c->tq]) memcpy(c->qt, d->qt[c->tq], sizeof(c->qt));
            else for (int k = 0; k < 64; ++k) c->qt[k] = 1;
            c->latched = 1;
        }
        for (uint32_t by = 0; by < c->hblocks; ++by)
            for (uint32_t bx = 0; bx < c->wblocks; ++bx)
                idct_block(d, c, c->coef + ((size_t)by * c->mcu_wb + bx) * 64u,
                           c->plane + ((size_t)by * c->ssize) * c->pw + (size_t)bx * c->ssize);
    }
}

/* ============================================================================
   Output: upsampling (jdsample.c), colour conversion (jdcolor.c), rotation
   ========================================================================== */
static inline const uint8_t* crow(const jcomp_t* c, int64_t r) {
    if (r < 0) r = 0;
    else if (r >= (int64_t)c->dh) r = (int64_t)c->dh - 1;
    return c->plane + (size_t)r * c->pw;
}

static void up_h2v1_fancy(const uint8_t* in, uint32_t dw, uint8_t* out) {
    uint32_t v = in[0];
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)((v * 3 + in[1] + 2) >> 2);
    for (uint32_t i = 1; i + 1 < dw; ++i) {
        v = in[i] * 3u;
        out[2 * i]     = (uint8_t)((v + in[i - 1] + 1) >> 2);
        out[2 * i + 1] = (uint8_t)((v + in[i + 1] + 2) >> 2);
    }
    v = in[dw - 1];
    out[2 * (dw - 1)]     = (uint8_t)((v * 3 + in[dw - 2] + 1) >> 2);
    out[2 * (dw - 1) + 1] = (uint8_t)v;
}

static void up_h2v2_fancy(const uint8_t* in0, const uint8_t* in1, uint32_t dw, uint8_t* out) {
    int32_t cur = in0[0] * 3 + in1[0], next = in0[1] * 3 + in1[1];
    out[0] = (uint8_t)((cur * 4 + 8) >> 4);
    out[1] = (uint8_t)((cur * 3 + next + 7) >> 4);
    int32_t last = cur; cur = next;
    for (uint32_t i = 1; i + 1 < dw; ++i) {
        next = in0[i + 1] * 3 + in1[i + 1];
        out[2 * i]     = (uint8_t)((cur * 3 + last + 8) >> 4);
        out[2 * i + 1] = (uint8_t)((cur * 3 + next + 7) >> 4);
        last = cur; cur = next;
    }
    out[2 * (dw - 1)]     = (uint8_t)((cur * 3 + last + 8) >> 4);
    out[2 * (dw - 1) + 1] = (uint8_t)((cur * 4 + 7) >> 4);
}

/* Returns a pointer to output row y of component c at full output width. */
static const uint8_t* upsample_row(const jdec_t* d, const jcomp_t* c, uint32_t y, uint8_t* buf) {
    switch (c->up_mode) {
    case UP_FULL:
        return crow(c, y);
    case UP_H2V1F:
        up_h2v1_fancy(crow(c, y), c->dw, buf);
        return buf;
    case UP_H2V1: {
        const uint8_t* in = crow(c, y);
        for (uint32_t i = 0; i < c->dw; ++i) buf[2 * i] = buf[2 * i + 1] = in[i];
        return buf;
    }
    case UP_H1V2F: {
        int64_t r = y >> 1;
        const uint8_t* in0 = crow(c, r);
        const uint8_t* in1 = crow(c, (y & 1) ? r + 1 : r - 1);
        int bias = (y & 1) ? 2 : 1;
        for (uint32_t i = 0; i < c->dw; ++i) buf[i] = (uint8_t)((in0[i] * 3 + in1[i] + bias) >> 2);
        return buf;
    }
    case UP_H2V2F: {
        int64_t r = y >> 1;
        up_h2v2_fancy(crow(c, r), crow(c, (y & 1) ? r + 1 : r - 1), c->dw, buf);
        return buf;
    }
    case UP_H2V2: {
        const uint8_t* in = crow(c, y >> 1);
        for (uint32_t i = 0; i < c->dw; ++i) buf[2 * i] = buf[2 * i + 1] = in[i];
        return buf;
    }
    case UP_INT: {
        const uint8_t* in = crow(c, y / c->vexp);
        uint32_t he = c->hexp, o = 0;
        for (uint32_t i = 0; i < c->dw; ++i)
            for (uint32_t k = 0; k < he; ++k) buf[o++] = in[i];
        return buf;
    }
    default: { /* fractional sampling ratios (libjpeg refuses these): nearest neighbour */
        const uint8_t* in = crow(c, (int64_t)(((uint64_t)y * c->dh) / d->sh));
        for (uint32_t x = 0; x < d->sw; ++x) buf[x] = in[((uint64_t)x * c->dw) / d->sw];
        return buf;
    }
    }
}

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

/* libjpeg jdcolor.c: FIX(x) = x * 65536 + 0.5 */
#define Y_CR_R   91881
#define Y_CB_B  116130
#define Y_CB_G  (-22554)
#define Y_CR_G  (-46802)

static inline void ycc_to_rgb(int y, int cb, int cr, int* r, int* g, int* b) {
    cb -= 128; cr -= 128;
    *r = y + ((Y_CR_R * cr + 32768) >> 16);
    *g = y + ((Y_CB_G * cb + Y_CR_G * cr + 32768) >> 16);
    *b = y + ((Y_CB_B * cb + 32768) >> 16);
}

static inline int mul255(int a, int b) {           /* round(a*b/255), as Pillow's MULDIV255 */
    int t = a * b + 128;
    return ((t >> 8) + t) >> 8;
}

static void convert_row(const jdec_t* d, const uint8_t* const* rp, uint8_t* dst, uint32_t n) {
    switch (d->cs) {
    case JPEG_CS_YCBCR:
        for (uint32_t x = 0; x < n; ++x) {
            int y = rp[0][x], r, g, b;
            ycc_to_rgb(y, rp[1][x], rp[2][x], &r, &g, &b);
            dst[0] = clamp8(b); dst[1] = clamp8(g); dst[2] = clamp8(r); dst[3] = 255;
            dst += 4;
        }
        break;
    case JPEG_CS_RGB:
        for (uint32_t x = 0; x < n; ++x) {
            dst[0] = rp[2][x]; dst[1] = rp[1][x]; dst[2] = rp[0][x]; dst[3] = 255;
            dst += 4;
        }
        break;
    case JPEG_CS_CMYK:
    case JPEG_CS_YCCK: {
        const bool ycck = d->cs == JPEG_CS_YCCK;
        const bool inv = d->saw_adobe;               /* Adobe stores CMYK inverted */
        for (uint32_t x = 0; x < n; ++x) {
            int s0, s1, s2, s3 = rp[3][x];
            if (ycck) {
                int r, g, b;
                ycc_to_rgb(rp[0][x], rp[1][x], rp[2][x], &r, &g, &b);
                s0 = 255 - clamp8(r); s1 = 255 - clamp8(g); s2 = 255 - clamp8(b);
            } else {
                s0 = rp[0][x]; s1 = rp[1][x]; s2 = rp[2][x];
            }
            int c = inv ? 255 - s0 : s0, m = inv ? 255 - s1 : s1, ye = inv ? 255 - s2 : s2;
            int k = inv ? 255 - s3 : s3;
            int nk = 255 - k;
            dst[0] = clamp8(nk - mul255(ye, nk));
            dst[1] = clamp8(nk - mul255(m, nk));
            dst[2] = clamp8(nk - mul255(c, nk));
            dst[3] = 255;
            dst += 4;
        }
        break;
    }
    default: /* gray (also the 2-component case: first component) */
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t v = rp[0][x];
            dst[0] = dst[1] = dst[2] = v; dst[3] = 255;
            dst += 4;
        }
        break;
    }
}

/* Writes source row y (tw pixels) of a tw x th image into `out` rotated /
 * flipped according to the EXIF orientation.  (Mappings: 2 mirror, 3 rot180,
 * 4 flip, 5 transpose, 6 rot90cw, 7 transverse, 8 rot90ccw.) */
static void put_row(uint8_t* out, uint32_t tw, uint32_t th, int orient, uint32_t y, const uint8_t* row) {
    const size_t Wd = orient >= 5 ? th : tw;
    switch (orient) {
    case 2:
        for (uint32_t x = 0; x < tw; ++x)
            memcpy(out + ((size_t)y * Wd + (tw - 1 - x)) * 4, row + (size_t)x * 4, 4);
        break;
    case 3:
        for (uint32_t x = 0; x < tw; ++x)
            memcpy(out + ((size_t)(th - 1 - y) * Wd + (tw - 1 - x)) * 4, row + (size_t)x * 4, 4);
        break;
    case 4:
        memcpy(out + (size_t)(th - 1 - y) * Wd * 4, row, (size_t)tw * 4);
        break;
    case 5:
        for (uint32_t x = 0; x < tw; ++x) memcpy(out + ((size_t)x * Wd + y) * 4, row + (size_t)x * 4, 4);
        break;
    case 6:
        for (uint32_t x = 0; x < tw; ++x)
            memcpy(out + ((size_t)x * Wd + (th - 1 - y)) * 4, row + (size_t)x * 4, 4);
        break;
    case 7:
        for (uint32_t x = 0; x < tw; ++x)
            memcpy(out + ((size_t)(tw - 1 - x) * Wd + (th - 1 - y)) * 4, row + (size_t)x * 4, 4);
        break;
    case 8:
        for (uint32_t x = 0; x < tw; ++x)
            memcpy(out + ((size_t)(tw - 1 - x) * Wd + y) * 4, row + (size_t)x * 4, 4);
        break;
    default:
        memcpy(out + (size_t)y * tw * 4, row, (size_t)tw * 4);
        break;
    }
}

static int jd_output(jdec_t* d, uint8_t* out, jpeg_info_t* info) {
    const uint32_t sw = d->sw, sh = d->sh;
    const int orient = d->orient;
    const bool swap = orient >= 5;
    uint32_t ow = swap ? sh : sw, oh = swap ? sw : sh;
    uint32_t tw = sw, th = sh;
    cos_img_scaler_t* sc = NULL;
    uint8_t* tmp = NULL;

    if (ow > d->max_w || oh > d->max_h) {              /* only with FIT and 1/8 still too big */
        uint64_t fw, fh;
        cos_img_fit_dims(ow, oh, d->max_w, d->max_h, &fw, &fh);
        tw = (uint32_t)(swap ? fh : fw);
        th = (uint32_t)(swap ? fw : fh);
        uint8_t* sout = out;
        if (orient != 1) {
            tmp = (uint8_t*)kmalloc((size_t)tw * th * 4u);
            if (!tmp) return JPEG_ERROR_BUFFER_OVERFLOW;
            sout = tmp;
        }
        sc = cos_img_scaler_create(sw, sh, tw, th, sout);
        if (!sc) { if (tmp) kfree(tmp); return JPEG_ERROR_BUFFER_OVERFLOW; }
        ow = (uint32_t)fw; oh = (uint32_t)fh;
    }

    for (uint32_t y = 0; y < sh; ++y) {
        const uint8_t* rp[JD_MAX_COMP] = {0};
        for (int i = 0; i < d->ncomp; ++i) rp[i] = upsample_row(d, &d->comp[i], y, d->urow[i]);
        if (d->ncomp < 4) for (int i = d->ncomp; i < JD_MAX_COMP; ++i) rp[i] = rp[0];
        convert_row(d, rp, d->line, sw);
        if (sc) cos_img_scaler_push_row(sc, d->line);
        else put_row(out, tw, th, orient, y, d->line);
    }
    if (sc) {
        cos_img_scaler_destroy(sc);
        if (tmp) {
            for (uint32_t y = 0; y < th; ++y) put_row(out, tw, th, orient, y, tmp + (size_t)y * tw * 4u);
            kfree(tmp);
        }
    }
    if (info) {
        info->width = ow;
        info->height = oh;
        info->orientation_applied = orient != 1;
    }
    return JPEG_ERROR_OK;
}

/* ============================================================================
   Driver
   ========================================================================== */
static void fill_info(const jdec_t* d, jpeg_info_t* info) {
    if (!info || !d->got_sof) return;
    info->src_width = d->width;
    info->src_height = d->height;
    info->ncomp = (uint8_t)d->ncomp;
    info->precision = (uint8_t)d->precision;
    info->progressive = d->sof == 2;
    info->orientation = (uint8_t)d->orientation;
    info->colorspace = (uint8_t)pick_colorspace(d);
    info->scale_denom = (uint8_t)(d->den ? d->den : 1);
    info->truncated = d->truncated;
}

static int jd_main(jdec_t* d, bool probe, uint8_t* out, jpeg_info_t* info) {
    const uint8_t* data = d->data;
    const size_t size = d->size;
    size_t pos = 2;
    int pending = -1;
    bool done = false;

    while (!done) {
        int m;
        if (pending >= 0) { m = pending; pending = -1; }
        else m = next_marker(data, size, &pos);
        if (m < 0 || m == 0xD9) break;
        if (m == 0x01 || m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (pos + 2 > size) break;
        size_t len = be16(data + pos);
        if (len < 2) continue;
        if (pos + len > size) break;                       /* truncated segment */
        const uint8_t* seg = data + pos + 2;
        const size_t sl = len - 2;
        const size_t next = pos + len;

        switch (m) {
        case 0xC0: case 0xC1: case 0xC2: {
            int rc = parse_sof(d, m, seg, sl);
            if (rc == -2) return JPEG_ERROR_UNSUPPORTED;
            if (rc < 0) return JPEG_ERROR_INVALID_FORMAT;
            break;
        }
        case 0xC3: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            /* lossless, hierarchical, arithmetic coding: not implemented */
            serial_puts("[JPEG] Unsupported JPEG process (arithmetic/lossless/hierarchical)\n");
            return JPEG_ERROR_UNSUPPORTED;
        case 0xC4:
            if (parse_dht(d, seg, sl) < 0) {
                if (d->setup_done) { d->truncated = true; done = true; break; }
                return JPEG_ERROR_INVALID_FORMAT;
            }
            break;
        case 0xDB:
            if (parse_dqt(d, seg, sl) < 0) {
                if (d->setup_done) { d->truncated = true; done = true; break; }
                return JPEG_ERROR_INVALID_FORMAT;
            }
            break;
        case 0xDD:
            if (sl >= 2) d->restart_interval = (int)be16(seg);
            break;
        case 0xDA: {
            if (!d->got_sof) return JPEG_ERROR_INVALID_FORMAT;
            if (probe) { done = true; break; }
            jscan_t sc;
            if (parse_sos(d, seg, sl, &sc) < 0) {
                if (d->setup_done) { d->truncated = true; done = true; break; }
                return JPEG_ERROR_INVALID_FORMAT;
            }
            if (!d->setup_done) {
                uint32_t dnl = d->height == 0 ? find_dnl(d, next) : 0;
                int rc = jd_setup(d, &sc, dnl, info);
                if (rc != JPEG_ERROR_OK) return rc;
            }
            size_t endpos = next;
            int pend = -1;
            if (run_scan(d, &sc, next, &endpos, &pend) < 0) {
                if (d->scans_done == 0) return JPEG_ERROR_INVALID_FORMAT;
                d->truncated = true;
                done = true;
                break;
            }
            d->scans_done++;
            if (!d->buffered) { done = true; break; }       /* single-scan: the rest is ignored */
            pos = endpos; pending = pend;
            continue;
        }
        default:
            if (m >= 0xE0 && m <= 0xEF) parse_app(d, m, seg, sl);
            break;
        }
        pos = next;
    }

    if (!d->got_sof) return JPEG_ERROR_INVALID_FORMAT;
    if (probe) return JPEG_ERROR_OK;
    if (!d->setup_done) return JPEG_ERROR_INVALID_FORMAT;   /* no scan at all */
    if (d->buffered) finalize_coefficients(d);
    return jd_output(d, out, info);
}

static void jd_init(jdec_t* d, const uint8_t* data, uint64_t size, uint64_t mw, uint64_t mh, unsigned flags) {
    memset(d, 0, sizeof(*d));
    d->data = data;
    d->size = (size_t)size;
    d->max_w = mw; d->max_h = mh;
    d->flags = flags;
    d->orientation = 1;
}

int cos_jpeg_codec_probe(const uint8_t* data, uint64_t size, jpeg_info_t* info) {
    if (info) memset(info, 0, sizeof(*info));
    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8) return JPEG_ERROR_INVALID_FORMAT;
    jdec_t* d = (jdec_t*)kmalloc(sizeof(jdec_t));
    if (!d) return JPEG_ERROR_BUFFER_OVERFLOW;
    jd_init(d, data, size, ~(uint64_t)0, ~(uint64_t)0, 0);
    int rc = jd_main(d, true, NULL, info);
    if (rc == JPEG_ERROR_OK && info) {
        fill_info(d, info);
        bool swap = d->orientation >= 5;
        info->width = swap ? d->height : d->width;
        info->height = swap ? d->width : d->height;
    }
    kfree(d);
    return rc;
}

int cos_jpeg_codec_decode(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                      uint64_t max_w, uint64_t max_h, unsigned flags, jpeg_info_t* info) {
    if (info) memset(info, 0, sizeof(*info));
    if (!data || !out_bgra || max_w == 0 || max_h == 0 || size < 4 ||
        data[0] != 0xFF || data[1] != 0xD8) return JPEG_ERROR_INVALID_FORMAT;
    jdec_t* d = (jdec_t*)kmalloc(sizeof(jdec_t));
    if (!d) return JPEG_ERROR_BUFFER_OVERFLOW;
    jd_init(d, data, size, max_w, max_h, flags);
    int rc = jd_main(d, false, out_bgra, info);
    fill_info(d, info);                       /* width/height were set by jd_setup/jd_output */
    jd_free_workspace(d);
    kfree(d);
    return rc;
}
