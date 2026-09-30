/**
 * gfx_blit.c - see gfx_blit.h for the design rationale.
 */
#include "gfx_blit.h"
#include <stddef.h>

/* Freestanding kernel build (-ffreestanding -nostdlib, like every
 * other driver file in this tree) - use the kernel's own memcpy
 * rather than pulling in <string.h>'s declaration assumptions. */
extern void* memcpy(void* dst, const void* src, size_t n);

/* Clip a proposed (dx,dy,w,h) destination rect - with a matching
 * (sx,sy) source origin that must move in lockstep, if src is
 * non-NULL - against both surfaces' bounds. Returns false if nothing
 * is left to draw once clipped. This is the one piece of clipping
 * arithmetic every block-transfer primitive below shares. */
static bool gfx_clip_pair(const gfx_surface_t* dst, int* dx, int* dy,
                           const gfx_surface_t* src, int* sx, int* sy,
                           int* w, int* h) {
    if (!dst || !dst->pixels || !w || !h || *w <= 0 || *h <= 0) return false;

    int ddx = *dx, ddy = *dy;
    int ssx = src ? *sx : 0, ssy = src ? *sy : 0;
    int ww = *w, hh = *h;

    /* Clip left/top against the destination, sliding the source
     * origin by the same amount so the two stay in lockstep. */
    if (ddx < 0) { ww += ddx; ssx -= ddx; ddx = 0; }
    if (ddy < 0) { hh += ddy; ssy -= ddy; ddy = 0; }
    if (src) {
        if (ssx < 0) { ww += ssx; ddx -= ssx; ssx = 0; }
        if (ssy < 0) { hh += ssy; ddy -= ssy; ssy = 0; }
    }
    if (ww <= 0 || hh <= 0) return false;

    /* Clip right/bottom against both surfaces. */
    if (ddx + ww > dst->width)  ww = dst->width  - ddx;
    if (ddy + hh > dst->height) hh = dst->height - ddy;
    if (src) {
        if (ssx + ww > src->width)  ww = src->width  - ssx;
        if (ssy + hh > src->height) hh = src->height - ssy;
    }
    if (ww <= 0 || hh <= 0) return false;

    *dx = ddx; *dy = ddy;
    if (src) { *sx = ssx; *sy = ssy; }
    *w = ww; *h = hh;
    return true;
}

/* Implemented in gfx_blit_avx2.c, isolated behind function-level
 * target("avx2") attributes there (not a global -mavx2 build flag),
 * and gated on a runtime probe that actually enables and re-verifies
 * the CPU state AVX2 needs - see that file's header comment. Safe to
 * call gfx_blit_avx2_available() unconditionally; it only ever
 * returns true once every prerequisite has been checked, never
 * assumed. */
extern bool gfx_blit_avx2_available(void);
extern void gfx_blit_avx2_copy_row(uint32_t* dst, const uint32_t* src, int count);

void gfx_blit(gfx_surface_t* dst, int dx, int dy,
              const gfx_surface_t* src, int sx, int sy,
              int w, int h, gfx_blit_mode_t mode, uint32_t colorkey) {
    if (!src || !src->pixels) return;
    if (!gfx_clip_pair(dst, &dx, &dy, src, &sx, &sy, &w, &h)) return;

    bool use_avx2 = (mode == GFX_BLIT_COPY) && gfx_blit_avx2_available();

    for (int row = 0; row < h; ++row) {
        uint32_t* drow = dst->pixels + (size_t)(dy + row) * (size_t)dst->stride + (size_t)dx;
        const uint32_t* srow = src->pixels + (size_t)(sy + row) * (size_t)src->stride + (size_t)sx;

        if (mode == GFX_BLIT_COPY) {
            /* The literal block transfer: one contiguous run per row.
             * AVX2 (when available) moves it 32 bytes at a time
             * instead of relying on memcpy's own dispatch; either way
             * it's still one row-sized transfer, not a per-pixel
             * store loop. */
            if (use_avx2) {
                gfx_blit_avx2_copy_row(drow, srow, w);
            } else {
                memcpy(drow, srow, (size_t)w * sizeof(uint32_t));
            }
        } else {
            for (int col = 0; col < w; ++col) {
                uint32_t px = srow[col];
                if (px != colorkey) drow[col] = px;
            }
        }
    }
}

void gfx_blit_fill(gfx_surface_t* dst, int x, int y, int w, int h, uint32_t color) {
    if (!gfx_clip_pair(dst, &x, &y, NULL, NULL, NULL, &w, &h)) return;

    for (int row = 0; row < h; ++row) {
        uint32_t* drow = dst->pixels + (size_t)(y + row) * (size_t)dst->stride + (size_t)x;
        for (int col = 0; col < w; ++col) drow[col] = color;
    }
}

/* Above this, a per-destination-column source-x table would rather be
 * recomputed per call than risk a stack array this large; gfx_blit_scaled()
 * falls back to the original per-pixel division for anything wider
 * (a destination rect wider than 4K pixels, which nothing in this GUI
 * ever draws - wallpaper, thumbnails, and the image viewer all target
 * ordinary desktop resolutions). */
#define GFX_BLIT_SCALED_LUT_MAX 2048

void gfx_blit_scaled(gfx_surface_t* dst, int dx, int dy, int dw, int dh,
                      const gfx_surface_t* src, int sx, int sy, int sw, int sh,
                      gfx_blit_mode_t mode, uint32_t colorkey) {
    if (!dst || !dst->pixels || !src || !src->pixels) return;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;

    /* Clip the *destination* rect against the destination surface;
     * skip_x/skip_y track how many destination columns/rows got
     * trimmed off the top-left so the source sampling below stays in
     * lockstep with the un-clipped mapping. */
    int x0 = dx, y0 = dy, w = dw, h = dh;
    int skip_x = 0, skip_y = 0;
    if (x0 < 0) { skip_x = -x0; w += x0; x0 = 0; }
    if (y0 < 0) { skip_y = -y0; h += y0; y0 = 0; }
    if (w <= 0 || h <= 0) return;
    if (x0 + w > dst->width)  w = dst->width  - x0;
    if (y0 + h > dst->height) h = dst->height - y0;
    if (w <= 0 || h <= 0) return;

    /* Precompute each destination column's source-x once, instead of
     * a 64-bit multiply+divide per pixel (i.e. w*h divisions -
     * every pixel of every scaled draw, which for a full-window
     * wallpaper repaint is hundreds of thousands of them). A row's
     * mapping never depends on which row it is, only on col/skip_x/
     * sw/dw, so this table is built exactly once per call and reused
     * for every row - cutting the division count from w*h to w (plus
     * h for the per-row y mapping, which was already only computed
     * once per row). */
    int srcx_lut[GFX_BLIT_SCALED_LUT_MAX];
    bool use_lut = (w <= GFX_BLIT_SCALED_LUT_MAX);
    if (use_lut) {
        for (int col = 0; col < w; ++col) {
            uint64_t sxx = (uint64_t)(skip_x + col) * (uint64_t)sw / (uint64_t)dw;
            if (sxx >= (uint64_t)sw) sxx = (uint64_t)sw - 1;
            srcx_lut[col] = sx + (int)sxx;
        }
    }

    for (int row = 0; row < h; ++row) {
        int dyy = y0 + row;
        uint64_t syy = (uint64_t)(skip_y + row) * (uint64_t)sh / (uint64_t)dh;
        if (syy >= (uint64_t)sh) syy = (uint64_t)sh - 1;
        int srcy = sy + (int)syy;
        if (srcy < 0 || srcy >= src->height) continue;

        uint32_t* drow = dst->pixels + (size_t)dyy * (size_t)dst->stride + (size_t)x0;
        const uint32_t* srow = src->pixels + (size_t)srcy * (size_t)src->stride;

        for (int col = 0; col < w; ++col) {
            int srcx;
            if (use_lut) {
                srcx = srcx_lut[col];
            } else {
                uint64_t sxx = (uint64_t)(skip_x + col) * (uint64_t)sw / (uint64_t)dw;
                if (sxx >= (uint64_t)sw) sxx = (uint64_t)sw - 1;
                srcx = sx + (int)sxx;
            }
            if (srcx < 0 || srcx >= src->width) continue;

            uint32_t px = srow[srcx];
            if (mode == GFX_BLIT_COLORKEY && px == colorkey) continue;
            if (mode == GFX_BLIT_ALPHA) {
                uint32_t a = (px >> 24) & 0xFF;
                if (a == 0) continue; /* fully transparent - dst pixel untouched */
                if (a == 255) { drow[col] = px; continue; }
                uint32_t dst_px = drow[col];
                uint32_t sr = (px >> 16) & 0xFF, sg = (px >> 8) & 0xFF, sb = px & 0xFF;
                uint32_t dr = (dst_px >> 16) & 0xFF, dg = (dst_px >> 8) & 0xFF, db = dst_px & 0xFF;
                uint32_t r = (sr * a + dr * (255 - a)) / 255;
                uint32_t g = (sg * a + dg * (255 - a)) / 255;
                uint32_t b = (sb * a + db * (255 - a)) / 255;
                drow[col] = (r << 16) | (g << 8) | b;
                continue;
            }
            drow[col] = px;
        }
    }
}

void gfx_blit_bitmap1(gfx_surface_t* dst, int dx, int dy,
                       const uint8_t* bits, int bit_w, int bit_h, int stride_bytes,
                       uint32_t fg, uint32_t bg, bool bg_opaque, int scale) {
    if (!dst || !dst->pixels || !bits || bit_w <= 0 || bit_h <= 0) return;
    if (scale < 1) scale = 1;

    /* Cheap whole-glyph reject before touching any bits. */
    if (dx + bit_w * scale <= 0 || dy + bit_h * scale <= 0) return;
    if (dx >= dst->width || dy >= dst->height) return;

    /* Fast path: the whole glyph/sprite lands fully inside the
     * destination surface (true for the overwhelming majority of text
     * and icon drawing - anything not scrolled or dragged half off an
     * edge). This is the hot path for every character of every string
     * the GUI draws, so it is worth skipping gfx_surface_set_pixel()'s
     * four-comparison bounds check on every single set bit: compute
     * the row/column stride once, and write straight through raw
     * pointers instead. Anything that doesn't fit falls through to the
     * general clipped loop below unchanged. */
    if (dx >= 0 && dy >= 0 &&
        dx + bit_w * scale <= dst->width && dy + bit_h * scale <= dst->height) {
        for (int row = 0; row < bit_h; ++row) {
            const uint8_t* rowbits = bits + (size_t)row * (size_t)stride_bytes;
            uint32_t* drow0 = dst->pixels + (size_t)(dy + row * scale) * (size_t)dst->stride + (size_t)dx;

            for (int col = 0; col < bit_w; ++col) {
                int byte_idx = col >> 3;
                int bit_idx = 7 - (col & 7);
                bool on = (rowbits[byte_idx] >> bit_idx) & 1u;
                if (!on && !bg_opaque) continue;

                uint32_t color = on ? fg : bg;
                uint32_t* px0 = drow0 + (size_t)col * (size_t)scale;
                if (scale == 1) {
                    *px0 = color;
                } else {
                    for (int sy2 = 0; sy2 < scale; ++sy2) {
                        uint32_t* prow = px0 + (size_t)sy2 * (size_t)dst->stride;
                        for (int sx2 = 0; sx2 < scale; ++sx2) prow[sx2] = color;
                    }
                }
            }
        }
        return;
    }

    for (int row = 0; row < bit_h; ++row) {
        const uint8_t* rowbits = bits + (size_t)row * (size_t)stride_bytes;
        int by = dy + row * scale;
        if (by + scale <= 0 || by >= dst->height) continue;

        for (int col = 0; col < bit_w; ++col) {
            int byte_idx = col >> 3;
            int bit_idx = 7 - (col & 7);
            bool on = (rowbits[byte_idx] >> bit_idx) & 1u;
            if (!on && !bg_opaque) continue;

            uint32_t color = on ? fg : bg;
            int bx = dx + col * scale;
            if (bx + scale <= 0 || bx >= dst->width) continue;

            if (scale == 1) {
                gfx_surface_set_pixel(dst, bx, by, color);
            } else {
                for (int sy2 = 0; sy2 < scale; ++sy2) {
                    for (int sx2 = 0; sx2 < scale; ++sx2) {
                        gfx_surface_set_pixel(dst, bx + sx2, by + sy2, color);
                    }
                }
            }
        }
    }
}
