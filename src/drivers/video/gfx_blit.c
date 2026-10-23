/**
 * gfx_blit.c - see gfx_blit.h for the design rationale.
 */
#include "gfx_blit.h"
#include <stddef.h>
#include "smp.h"

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

    /* Clip left/top against the destination's write clip, sliding the source
     * origin by the same amount so the two stay in lockstep. */
    if (ddx < dst->cx0) { int d = dst->cx0 - ddx; ww -= d; ssx += d; ddx = dst->cx0; }
    if (ddy < dst->cy0) { int d = dst->cy0 - ddy; hh -= d; ssy += d; ddy = dst->cy0; }
    if (src) {
        if (ssx < 0) { ww += ssx; ddx -= ssx; ssx = 0; }
        if (ssy < 0) { hh += ssy; ddy -= ssy; ssy = 0; }
    }
    if (ww <= 0 || hh <= 0) return false;

    /* Clip right/bottom against both surfaces. */
    if (ddx + ww > dst->cx1) ww = dst->cx1 - ddx;
    if (ddy + hh > dst->cy1) hh = dst->cy1 - ddy;
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

typedef struct {
    uint32_t* dbase; const uint32_t* sbase; size_t dstride, sstride; int w, h, ntiles;
} gfx_copy_ctx_t;
static void gfx_copy_tile(void* opaque, uint32_t tile) {
    const gfx_copy_ctx_t* c = (const gfx_copy_ctx_t*)opaque;
    int r0 = (int)(((int64_t)c->h * tile) / c->ntiles), r1 = (int)(((int64_t)c->h * (tile + 1u)) / c->ntiles);
    bool use_avx2 = gfx_blit_avx2_available();      /* per-CPU capability: probed on the executing core */
    for (int row = r0; row < r1; ++row) {
        uint32_t* d = c->dbase + (size_t)row * c->dstride;
        const uint32_t* s = c->sbase + (size_t)row * c->sstride;
        if (use_avx2) gfx_blit_avx2_copy_row(d, s, c->w);
        else memcpy(d, s, (size_t)c->w * sizeof(uint32_t));
    }
}

#define GFX_COPY_PARALLEL_MIN_PIXELS 16384

void gfx_blit(gfx_surface_t* dst, int dx, int dy,
              const gfx_surface_t* src, int sx, int sy,
              int w, int h, gfx_blit_mode_t mode, uint32_t colorkey) {
    if (!src || !src->pixels) return;
    if (!gfx_clip_pair(dst, &dx, &dy, src, &sx, &sy, &w, &h)) return;

    /* Big opaque copies (cached layers, window chrome, read-backs) are spread over the idle cores. */
    if (mode == GFX_BLIT_COPY && dst->pixels != src->pixels && (int64_t)w * h >= GFX_COPY_PARALLEL_MIN_PIXELS && h >= 16 &&
        smp_online_cpu_count() > 1u && smp_current_logical_id() == 0u) {
        int nt = (int)(smp_online_cpu_count() * 4u);
        if (nt > h / 4) nt = h / 4;
        gfx_copy_ctx_t c = { dst->pixels + (size_t)dy * (size_t)dst->stride + (size_t)dx,
                             src->pixels + (size_t)sy * (size_t)src->stride + (size_t)sx,
                             (size_t)dst->stride, (size_t)src->stride, w, h, nt };
        smp_parallel_tiles(gfx_copy_tile, &c, (uint32_t)nt, 100u);
        return;
    }

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

/* One row of solid colour. `rep stosl` is a single translated/microcoded operation (fast-string stores on real
 * CPUs, one helper call under TCG) where a C loop is either a scalar store per pixel or auto-vectorised SSE that
 * TCG emulates call-by-call. Short runs use the plain loop so the setup cost is not paid for a few pixels. */
static inline void gfx_fill_row32(uint32_t* d, uint32_t c, size_t n) {
    if (n < 16) { for (size_t i = 0; i < n; ++i) d[i] = c; return; }
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(c) : "memory");
}

typedef struct { uint32_t* base; size_t stride; int w, h, ntiles; uint32_t color; } gfx_fill_ctx_t;
static void gfx_fill_tile(void* opaque, uint32_t tile) {
    const gfx_fill_ctx_t* c = (const gfx_fill_ctx_t*)opaque;
    int r0 = (int)(((int64_t)c->h * tile) / c->ntiles), r1 = (int)(((int64_t)c->h * (tile + 1u)) / c->ntiles);
    for (int row = r0; row < r1; ++row) gfx_fill_row32(c->base + (size_t)row * c->stride, c->color, (size_t)c->w);
}

/* Large solid fills (window bodies, app backgrounds, wallpaper bands) are cut into row bands and pulled by every
 * idle core. Below the threshold the dispatch costs more than the stores. Only from CPU 0: an AP worker must not
 * start a nested parallel section. */
#define GFX_FILL_PARALLEL_MIN_PIXELS 16384

void gfx_blit_fill(gfx_surface_t* dst, int x, int y, int w, int h, uint32_t color) {
    if (!gfx_clip_pair(dst, &x, &y, NULL, NULL, NULL, &w, &h)) return;

    uint32_t* base = dst->pixels + (size_t)y * (size_t)dst->stride + (size_t)x;
    if ((int64_t)w * h >= GFX_FILL_PARALLEL_MIN_PIXELS && h >= 16 && smp_online_cpu_count() > 1u &&
        smp_current_logical_id() == 0u) {
        int nt = (int)(smp_online_cpu_count() * 4u);
        if (nt > h / 4) nt = h / 4;
        gfx_fill_ctx_t c = { base, (size_t)dst->stride, w, h, nt, color };
        smp_parallel_tiles(gfx_fill_tile, &c, (uint32_t)nt, 100u);
        return;
    }
    for (int row = 0; row < h; ++row) {
        gfx_fill_row32(base + (size_t)row * (size_t)dst->stride, color, (size_t)w);
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
    if (x0 < dst->cx0) { skip_x = dst->cx0 - x0; w -= skip_x; x0 = dst->cx0; }
    if (y0 < dst->cy0) { skip_y = dst->cy0 - y0; h -= skip_y; y0 = dst->cy0; }
    if (w <= 0 || h <= 0) return;
    if (x0 + w > dst->cx1) w = dst->cx1 - x0;
    if (y0 + h > dst->cy1) h = dst->cy1 - y0;
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
    if (dx + bit_w * scale <= dst->cx0 || dy + bit_h * scale <= dst->cy0) return;
    if (dx >= dst->cx1 || dy >= dst->cy1) return;

    /* Fast path: the whole glyph/sprite lands fully inside the
     * destination surface (true for the overwhelming majority of text
     * and icon drawing - anything not scrolled or dragged half off an
     * edge). This is the hot path for every character of every string
     * the GUI draws, so it is worth skipping gfx_surface_set_pixel()'s
     * four-comparison bounds check on every single set bit: compute
     * the row/column stride once, and write straight through raw
     * pointers instead. Anything that doesn't fit falls through to the
     * general clipped loop below unchanged. */
    if (dx >= dst->cx0 && dy >= dst->cy0 &&
        dx + bit_w * scale <= dst->cx1 && dy + bit_h * scale <= dst->cy1) {
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
        if (by + scale <= dst->cy0 || by >= dst->cy1) continue;

        for (int col = 0; col < bit_w; ++col) {
            int byte_idx = col >> 3;
            int bit_idx = 7 - (col & 7);
            bool on = (rowbits[byte_idx] >> bit_idx) & 1u;
            if (!on && !bg_opaque) continue;

            uint32_t color = on ? fg : bg;
            int bx = dx + col * scale;
            if (bx + scale <= dst->cx0 || bx >= dst->cx1) continue;

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
