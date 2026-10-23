/*
 * image_scale.c - see image_scale.h
 *
 * Geometry (all integer, exact): the source row/column y covers the interval
 * [y*dst, (y+1)*dst) and the destination cell j covers [j*src, (j+1)*src) on a
 * common axis of length src*dst.  The overlap of a source pixel with a
 * destination cell is its weight along that axis; the 2-D weight is the
 * product, and the weights of one cell sum to src_w*src_h.
 */
#include "image_scale.h"
#include "memory.h"
#include <string.h>

struct cos_img_scaler {
    uint32_t sw, sh, dw, dh;
    uint8_t* out;
    uint32_t src_row;          /* next source row index */
    uint32_t dst_row;          /* next destination row to emit */
    uint64_t* hrow;            /* dw*4: horizontally accumulated current row  */
    uint64_t* acc;             /* dw*4: vertical accumulator of the destination row being built */
    uint8_t* mem;
};

void cos_img_fit_dims(uint64_t sw, uint64_t sh, uint64_t mw, uint64_t mh,
                  uint64_t* dw, uint64_t* dh) {
    uint64_t w = sw, h = sh;
    if (sw == 0 || sh == 0 || mw == 0 || mh == 0) { w = 1; h = 1; }
    else if (sw > mw || sh > mh) {
        /* Compare sw/mw with sh/mh by cross-multiplication. */
        if (sw * mh >= sh * mw) { w = mw; h = (sh * mw) / sw; }
        else                    { h = mh; w = (sw * mh) / sh; }
        if (w == 0) w = 1;
        if (h == 0) h = 1;
        if (w > mw) w = mw;
        if (h > mh) h = mh;
    }
    if (dw) *dw = w;
    if (dh) *dh = h;
}

cos_img_scaler_t* cos_img_scaler_create(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, uint8_t* out) {
    if (!out || sw == 0 || sh == 0 || dw == 0 || dh == 0 || dw > sw || dh > sh) return NULL;
    size_t arr = (size_t)dw * 4u * sizeof(uint64_t);
    size_t total = sizeof(cos_img_scaler_t) + arr * 2u;
    /* Overflow cannot happen (dw < 2^32, arr < 2^37) but keep it explicit. */
    if (arr / sizeof(uint64_t) / 4u != dw) return NULL;
    uint8_t* mem = (uint8_t*)kmalloc(total);
    if (!mem) return NULL;
    memset(mem, 0, total);
    cos_img_scaler_t* s = (cos_img_scaler_t*)mem;
    s->mem = mem;
    s->sw = sw; s->sh = sh; s->dw = dw; s->dh = dh; s->out = out;
    uint8_t* p = mem + sizeof(cos_img_scaler_t);
    /* sizeof(cos_img_scaler_t) is a multiple of 8, so uint64_t alignment holds. */
    s->hrow = (uint64_t*)(void*)p;           p += arr;
    s->acc = (uint64_t*)(void*)p;
    return s;
}

void cos_img_scaler_destroy(cos_img_scaler_t* s) {
    if (s) kfree(s->mem);
}

bool cos_img_scaler_done(const cos_img_scaler_t* s) {
    return s->dst_row >= s->dh;
}

static void emit_row(cos_img_scaler_t* s, const uint64_t* acc) {
    uint8_t* dst = s->out + (size_t)s->dst_row * s->dw * 4u;
    const uint64_t W = (uint64_t)s->sw * (uint64_t)s->sh;
    for (uint32_t i = 0; i < s->dw; ++i) {
        const uint64_t* a = acc + (size_t)i * 4u;
        uint64_t ta = a[3];
        if (ta == 0) {
            dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = dst[i * 4 + 3] = 0;
            continue;
        }
        uint64_t half = ta >> 1;
        dst[i * 4 + 0] = (uint8_t)((a[0] + half) / ta);
        dst[i * 4 + 1] = (uint8_t)((a[1] + half) / ta);
        dst[i * 4 + 2] = (uint8_t)((a[2] + half) / ta);
        dst[i * 4 + 3] = (uint8_t)((ta + (W >> 1)) / W);
    }
}

void cos_img_scaler_push_row(cos_img_scaler_t* s, const uint8_t* row) {
    if (!s || !row || s->src_row >= s->sh || s->dst_row >= s->dh) return;
    const uint32_t sw = s->sw, dw = s->dw;

    /* ---- horizontal pass: row -> hrow[dw][4] (premultiplied, weighted) ---- */
    memset(s->hrow, 0, (size_t)dw * 4u * sizeof(uint64_t));
    {
        uint64_t* h = s->hrow;
        uint64_t boundary = sw;                 /* end of dest cell i on the axis */
        uint32_t i = 0;
        for (uint32_t x = 0; x < sw; ++x) {
            uint64_t pos = (uint64_t)x * dw, hi = pos + dw;
            uint32_t a = row[x * 4 + 3];
            uint64_t pb = (uint64_t)row[x * 4 + 0] * a;
            uint64_t pg = (uint64_t)row[x * 4 + 1] * a;
            uint64_t pr = (uint64_t)row[x * 4 + 2] * a;
            while (pos < hi) {
                uint64_t end = hi < boundary ? hi : boundary;
                uint64_t w = end - pos;
                uint64_t* c = h + (size_t)i * 4u;
                c[0] += w * pb; c[1] += w * pg; c[2] += w * pr; c[3] += w * a;
                pos = end;
                if (pos == boundary) { ++i; boundary += sw; }
            }
        }
    }

    /* ---- vertical pass: distribute this source row over dst rows ---- */
    {
        uint64_t pos = (uint64_t)s->src_row * s->dh, hi = pos + s->dh;
        uint64_t boundary = ((uint64_t)s->dst_row + 1u) * s->sh;
        while (pos < hi) {
            uint64_t end = hi < boundary ? hi : boundary;
            uint64_t w = end - pos;
            uint64_t* a = s->acc;
            const uint64_t* h = s->hrow;
            for (size_t k = 0, n = (size_t)dw * 4u; k < n; ++k) a[k] += h[k] * w;
            pos = end;
            if (pos == boundary) {
                /* Destination row complete; whatever is left of this source
                 * row belongs to the next one. */
                emit_row(s, s->acc);
                memset(s->acc, 0, (size_t)dw * 4u * sizeof(uint64_t));
                ++s->dst_row;
                boundary += s->sh;
                if (s->dst_row >= s->dh) break;
            }
        }
    }
    ++s->src_row;
}
