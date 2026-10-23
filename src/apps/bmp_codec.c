/*
 * bmp_codec.c - see bmp_codec.h
 */
#include "bmp_codec.h"
#include "image_scale.h"
#include "png_decoder.h"
#include "jpeg_codec.h"
#include "memory.h"
#include "serial.h"
#include <string.h>

#define BMP_MAX_DIM        65535u
#define BMP_MAX_RLE_PIXELS (1u << 25)

enum { BI_RGB_ = 0, BI_RLE8_ = 1, BI_RLE4_ = 2, BI_BITFIELDS_ = 3, BI_JPEG_ = 4, BI_PNG_ = 5,
       BI_ALPHABITFIELDS_ = 6 };

typedef struct {
    uint32_t w, h;
    bool top_down;
    int bpp;
    uint32_t comp;
    uint32_t mask[4];            /* R, G, B, A (after masking to the pixel width) */
    int shift[4];
    uint32_t mmax[4];            /* mask >> shift, 0 = channel absent */
    uint32_t pal[256];           /* B,G,R,A bytes, little-endian packed */
    uint64_t data_off;
    uint64_t stride;
    bool std32;                  /* 32-bit with the usual 8-8-8 BGR layout */
} bmp_hdr_t;

static uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool cos_bmp_is_bmp(const uint8_t* d, uint64_t size) {
    return d && size >= 26 && d[0] == 'B' && d[1] == 'M';
}

static void set_mask(bmp_hdr_t* h, int ch, uint32_t mask) {
    if (h->bpp == 16) mask &= 0xFFFFu;
    h->mask[ch] = mask;
    if (!mask) { h->shift[ch] = 0; h->mmax[ch] = 0; return; }
    int s = 0;
    while (!((mask >> s) & 1u)) s++;
    h->shift[ch] = s;
    h->mmax[ch] = mask >> s;
}

/* Returns 0 ok, -1 invalid, -2 unsupported. */
static int bmp_parse(const uint8_t* d, uint64_t size, bmp_hdr_t* h) {
    memset(h, 0, sizeof(*h));
    if (!cos_bmp_is_bmp(d, size)) return -1;
    uint64_t off = rd32(d + 10);
    uint32_t hs = rd32(d + 14);
    bool core = false;
    int64_t w, hh;
    uint32_t planes, clrused = 0;

    if (hs == 12) {
        core = true;
        w = rd16(d + 18); hh = rd16(d + 20);
        planes = rd16(d + 22); h->bpp = (int)rd16(d + 24);
        h->comp = BI_RGB_;
    } else if (hs == 16 || hs >= 40) {
        if (14u + (uint64_t)(hs < 64 ? hs : 64) > size) return -1;
        w = (int32_t)rd32(d + 18); hh = (int32_t)rd32(d + 22);
        planes = rd16(d + 26); h->bpp = (int)rd16(d + 28);
        if (hs >= 20) h->comp = rd32(d + 30);
        if (hs >= 36) clrused = rd32(d + 46);
    } else {
        return -2;
    }
    if (planes != 1) return -1;
    if (w <= 0 || hh == 0) return -1;
    h->top_down = hh < 0;
    if (hh < 0) hh = -hh;
    if (w > BMP_MAX_DIM || hh > BMP_MAX_DIM) return -2;
    h->w = (uint32_t)w; h->h = (uint32_t)hh;

    uint64_t hdr_end = 14u + hs;
    uint32_t comp = h->comp;
    if (comp != BI_RGB_ && comp != BI_RLE8_ && comp != BI_RLE4_ && comp != BI_BITFIELDS_ &&
        comp != BI_JPEG_ && comp != BI_PNG_ && comp != BI_ALPHABITFIELDS_) return -2;

    if (comp == BI_JPEG_ || comp == BI_PNG_) {
        if (off > size) return -1;
        h->data_off = off;
        return 0;
    }

    int bpp = h->bpp;
    switch (bpp) {
    case 1: case 2: case 4: case 8: case 16: case 24: case 32: break;
    default: return -2;
    }
    if (core && (bpp == 2 || bpp == 16 || bpp == 32)) return -2;
    if (comp == BI_RLE8_ && bpp != 8) return -1;
    if (comp == BI_RLE4_ && bpp != 4) return -1;
    if ((comp == BI_BITFIELDS_ || comp == BI_ALPHABITFIELDS_) && bpp != 16 && bpp != 32) return -1;

    if (comp == BI_BITFIELDS_ || comp == BI_ALPHABITFIELDS_) {
        uint32_t m[4] = {0, 0, 0, 0};
        if (hs >= 52) {                                  /* masks live inside the header */
            m[0] = rd32(d + 54); m[1] = rd32(d + 58); m[2] = rd32(d + 62);
            if (hs >= 56) m[3] = rd32(d + 66);
        } else {                                         /* 40-byte header: masks follow it */
            uint64_t n = comp == BI_ALPHABITFIELDS_ ? 16 : 12;
            if (hdr_end + n > size) return -1;
            m[0] = rd32(d + hdr_end); m[1] = rd32(d + hdr_end + 4); m[2] = rd32(d + hdr_end + 8);
            if (n == 16) m[3] = rd32(d + hdr_end + 12);
            hdr_end += n;
        }
        for (int i = 0; i < 4; ++i) set_mask(h, i, m[i]);
        if (!h->mmax[0] && !h->mmax[1] && !h->mmax[2]) return -1;
    } else if (bpp == 16) {
        set_mask(h, 0, 0x7C00); set_mask(h, 1, 0x03E0); set_mask(h, 2, 0x001F); set_mask(h, 3, 0);
    } else if (bpp == 32) {
        set_mask(h, 0, 0x00FF0000u); set_mask(h, 1, 0x0000FF00u); set_mask(h, 2, 0x000000FFu); set_mask(h, 3, 0);
    }
    if (bpp == 32 && h->mask[0] == 0x00FF0000u && h->mask[1] == 0x0000FF00u && h->mask[2] == 0x000000FFu)
        h->std32 = true;

    if (off < hdr_end || off > size) { serial_puts("[BMP] Bad pixel data offset\n"); return -1; }
    h->data_off = off;

    if (bpp <= 8) {
        uint32_t declared = clrused ? clrused : (1u << bpp);
        if (declared > (1u << bpp)) declared = 1u << bpp;
        uint32_t entry = core ? 3u : 4u;
        uint64_t avail = (off - hdr_end) / entry;
        uint32_t n = declared < avail ? declared : (uint32_t)avail;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = 0xFF000000u;
            if (i < n) {
                const uint8_t* p = d + hdr_end + (uint64_t)i * entry;
                v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | 0xFF000000u;
            }
            h->pal[i] = v;
        }
    }
    h->stride = (((uint64_t)h->w * (uint64_t)bpp + 31u) / 32u) * 4u;
    if (comp == BI_RGB_ || comp == BI_BITFIELDS_ || comp == BI_ALPHABITFIELDS_) {
        if (h->data_off + h->stride * h->h > size) {
            serial_puts("[BMP] Pixel data extends past end of file\n");
            return -1;
        }
    }
    return 0;
}

bool cos_bmp_probe(const uint8_t* data, uint64_t size, uint32_t* width, uint32_t* height) {
    bmp_hdr_t h;
    if (bmp_parse(data, size, &h) != 0) return false;
    if (width) *width = h.w;
    if (height) *height = h.h;
    return true;
}

static inline uint8_t scale_ch(const bmp_hdr_t* h, int ch, uint32_t px) {
    uint32_t m = h->mmax[ch];
    if (!m) return 0;
    uint32_t v = (px & h->mask[ch]) >> h->shift[ch];
    return m == 255 ? (uint8_t)v : (uint8_t)(v * 255u / m);
}

/* Converts n pixels of one uncompressed row.  alpha_from_byte: 32-bit BI_RGB
 * whose 4th byte carries alpha. */
static void conv_row(const bmp_hdr_t* h, const uint8_t* row, uint32_t n, uint8_t* dst, bool alpha_from_byte) {
    uint32_t i;
    switch (h->bpp) {
    case 24:
        for (i = 0; i < n; ++i) {
            dst[0] = row[i * 3]; dst[1] = row[i * 3 + 1]; dst[2] = row[i * 3 + 2]; dst[3] = 255;
            dst += 4;
        }
        break;
    case 32:
        if (h->std32 && !h->mmax[3]) {
            for (i = 0; i < n; ++i) {
                dst[0] = row[i * 4]; dst[1] = row[i * 4 + 1]; dst[2] = row[i * 4 + 2];
                dst[3] = alpha_from_byte ? row[i * 4 + 3] : 255;
                dst += 4;
            }
        } else {
            for (i = 0; i < n; ++i) {
                uint32_t px = rd32(row + (size_t)i * 4);
                dst[0] = scale_ch(h, 2, px); dst[1] = scale_ch(h, 1, px); dst[2] = scale_ch(h, 0, px);
                dst[3] = h->mmax[3] ? scale_ch(h, 3, px) : 255;
                dst += 4;
            }
        }
        break;
    case 16:
        for (i = 0; i < n; ++i) {
            uint32_t px = rd16(row + (size_t)i * 2);
            dst[0] = scale_ch(h, 2, px); dst[1] = scale_ch(h, 1, px); dst[2] = scale_ch(h, 0, px);
            dst[3] = h->mmax[3] ? scale_ch(h, 3, px) : 255;
            dst += 4;
        }
        break;
    default: { /* 1, 2, 4, 8 */
        int bpp = h->bpp, mask = (1 << bpp) - 1;
        for (i = 0; i < n; ++i) {
            uint32_t bit = i * (uint32_t)bpp;
            uint32_t v = h->pal[(row[bit >> 3] >> (8 - bpp - (int)(bit & 7))) & mask];
            dst[0] = (uint8_t)v; dst[1] = (uint8_t)(v >> 8); dst[2] = (uint8_t)(v >> 16); dst[3] = (uint8_t)(v >> 24);
            dst += 4;
        }
        break;
    }
    }
}

/* ---- RLE ---------------------------------------------------------------- */
#define RLE_UNDEF 0xFFFFu

/* Decodes into plane (w*h uint16: palette index or RLE_UNDEF), in FILE row
 * order (row 0 = first row stored in the file).  Never writes outside it. */
static void rle_decode(const uint8_t* p, size_t n, uint32_t w, uint32_t h, bool four, uint16_t* plane) {
    uint64_t x = 0, y = 0;
    size_t i = 0;
    #define PUT(v) do { if (x < w && y < h) plane[y * w + x] = (uint16_t)(v); x++; } while (0)
    while (i + 2 <= n && y < h) {
        uint32_t c = p[i], v = p[i + 1];
        i += 2;
        if (c) {
            if (!four) { for (uint32_t k = 0; k < c; ++k) PUT(v); }
            else       { for (uint32_t k = 0; k < c; ++k) PUT((k & 1) ? (v & 15) : (v >> 4)); }
        } else if (v == 0) { x = 0; y++; }
        else if (v == 1) { break; }
        else if (v == 2) {
            if (i + 2 > n) break;
            x += p[i]; y += p[i + 1];
            i += 2;
        } else {
            size_t bytes = four ? ((size_t)v + 1) / 2 : (size_t)v;
            size_t padded = bytes + (bytes & 1);
            if (i + bytes > n) bytes = n - i, padded = bytes;        /* truncated run: use what exists */
            if (!four) { for (uint32_t k = 0; k < v && k < bytes; ++k) PUT(p[i + k]); }
            else {
                for (uint32_t k = 0; k < v && (k >> 1) < bytes; ++k)
                    PUT((k & 1) ? (p[i + (k >> 1)] & 15) : (p[i + (k >> 1)] >> 4));
            }
            i += padded;
        }
    }
    #undef PUT
}

static void rle_row(const bmp_hdr_t* h, const uint16_t* prow, uint32_t n, uint8_t* dst) {
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t v = prow[i];
        if (v == RLE_UNDEF) { dst[0] = dst[1] = dst[2] = dst[3] = 0; }
        else {
            uint32_t c = h->pal[v & 255];
            dst[0] = (uint8_t)c; dst[1] = (uint8_t)(c >> 8); dst[2] = (uint8_t)(c >> 16); dst[3] = (uint8_t)(c >> 24);
        }
        dst += 4;
    }
}

/* ---- driver ------------------------------------------------------------- */
static bool bmp_decode_impl(const uint8_t* data, uint64_t size, uint8_t* out, uint64_t max_w, uint64_t max_h,
                            bool fit, uint64_t* out_w, uint64_t* out_h, uint64_t* src_w, uint64_t* src_h) {
    if (!data || !out || max_w == 0 || max_h == 0) return false;
    bmp_hdr_t hd;
    int rc = bmp_parse(data, size, &hd);
    if (rc != 0) { serial_puts("[BMP] Unsupported or invalid header\n"); return false; }

    if (hd.comp == BI_JPEG_ || hd.comp == BI_PNG_) {
        uint64_t avail = size - hd.data_off;
        const uint8_t* payload = data + hd.data_off;
        if (hd.comp == BI_PNG_) {
            bool ok = fit ? cos_png_decode_fit(payload, avail, out, max_w, max_h, out_w, out_h, src_w, src_h)
                          : png_decode(payload, avail, out, max_w, max_h, out_w, out_h);
            if (ok && !fit) { if (src_w) *src_w = hd.w; if (src_h) *src_h = hd.h; }
            return ok;
        }
        jpeg_info_t info;
        int jr = cos_jpeg_codec_decode(payload, avail, out, max_w, max_h, fit ? JPEG_DEC_FIT : 0, &info);
        if (jr != JPEG_ERROR_OK) return false;
        if (out_w) *out_w = info.width;
        if (out_h) *out_h = info.height;
        if (src_w) *src_w = info.src_width;
        if (src_h) *src_h = info.src_height;
        return true;
    }

    const uint32_t w = hd.w, h = hd.h;
    uint64_t ow, oh;
    bool scale = false;
    if (fit && (w > max_w || h > max_h)) { cos_img_fit_dims(w, h, max_w, max_h, &ow, &oh); scale = true; }
    else { ow = w < max_w ? w : max_w; oh = h < max_h ? h : max_h; }

    bool ok = false;
    uint8_t* line = NULL;
    uint16_t* plane = NULL;
    cos_img_scaler_t* sc = NULL;
    bool rle = hd.comp == BI_RLE8_ || hd.comp == BI_RLE4_;

    if (rle) {
        if ((uint64_t)w * h > BMP_MAX_RLE_PIXELS) { serial_puts("[BMP] RLE image too large\n"); return false; }
        plane = (uint16_t*)kmalloc((size_t)w * h * 2u);
        if (!plane) return false;
        memset(plane, 0xFF, (size_t)w * h * 2u);
        rle_decode(data + hd.data_off, (size_t)(size - hd.data_off), w, h, hd.comp == BI_RLE4_, plane);
    }
    if (scale || rle) {
        /* rows are converted into a scratch line first (scaler input / RLE) */
        line = (uint8_t*)kmalloc((size_t)w * 4u);
        if (!line) goto done;
    }
    if (scale) {
        sc = cos_img_scaler_create(w, h, (uint32_t)ow, (uint32_t)oh, out);
        if (!sc) goto done;
    }

    bool alpha_byte = false;
    if (!rle && hd.bpp == 32 && hd.std32 && !hd.mmax[3] && hd.comp == BI_RGB_) {
        for (uint32_t y = 0; y < h && !alpha_byte; ++y) {
            const uint8_t* row = data + hd.data_off + (uint64_t)y * hd.stride;
            for (uint32_t x = 0; x < w; ++x) if (row[x * 4 + 3]) { alpha_byte = true; break; }
        }
    }

    uint32_t rows = scale ? h : (uint32_t)oh;
    for (uint32_t j = 0; j < rows; ++j) {
        uint32_t frow = hd.top_down ? j : h - 1 - j;           /* row index in file order */
        uint32_t n = scale ? w : (uint32_t)ow;
        uint8_t* dst = scale ? line : (rle ? line : out + (size_t)j * ow * 4u);
        if (rle) rle_row(&hd, plane + (size_t)frow * w, n, dst);
        else conv_row(&hd, data + hd.data_off + (uint64_t)frow * hd.stride, n, dst, alpha_byte);
        if (scale) cos_img_scaler_push_row(sc, line);
        else if (rle) memcpy(out + (size_t)j * ow * 4u, line, (size_t)ow * 4u);
    }
    ok = true;
done:
    if (sc) cos_img_scaler_destroy(sc);
    if (line) kfree(line);
    if (plane) kfree(plane);
    if (ok) {
        if (out_w) *out_w = ow;
        if (out_h) *out_h = oh;
        if (src_w) *src_w = w;
        if (src_h) *src_h = h;
    }
    return ok;
}

bool cos_bmp_decode(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                uint64_t max_w, uint64_t max_h, uint64_t* out_w, uint64_t* out_h) {
    return bmp_decode_impl(data, size, out_bgra, max_w, max_h, false, out_w, out_h, NULL, NULL);
}

bool cos_bmp_decode_fit(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                    uint64_t max_w, uint64_t max_h, uint64_t* out_w, uint64_t* out_h,
                    uint64_t* src_w, uint64_t* src_h) {
    return bmp_decode_impl(data, size, out_bgra, max_w, max_h, true, out_w, out_h, src_w, src_h);
}
