#ifndef GFX_BLIT_H
#define GFX_BLIT_H

/**
 * gfx_blit.h / gfx_blit.c - C-OS unified BitBlt (block image transfer) core.
 *
 * Before this module, every subsystem that needed to move a rectangle
 * of pixels onto the screen (window fills, glyphs, the wallpaper, the
 * mouse cursor, decoded JPEG/PNG/BMP images, file-manager thumbnails)
 * hand-rolled its own nested x/y pixel loop with its own bounds
 * checking. That meant N slightly-different copies of the same bug
 * surface, and no single place to reason about correctness.
 *
 * Everything now reduces to one of the primitives below, which mirror
 * the classic BitBlt(dst, dx, dy, w, h, src, sx, sy, mode) shape: a
 * block transfer between two rectangular pixel surfaces, expressed as
 * one function with one clipping implementation. vga.c's public
 * drawing API (vga_fill_rect, vga_copy_rect, vga_draw_bmp,
 * vga_draw_char, ...) is implemented in terms of this file, so every
 * caller in the GUI (apps included - they only ever go through vga.c)
 * automatically benefits without needing to change.
 *
 * All surfaces are top-left origin, 32bpp, 0x00RRGGBB (top byte
 * unused) - the same convention vga.c already uses.
 */

#include "types.h"

typedef struct {
    uint32_t* pixels;  /* pointer to the top-left pixel */
    int       width;
    int       height;
    int       stride;  /* pixels per row (>= width) */
} gfx_surface_t;

static inline gfx_surface_t gfx_surface_make(uint32_t* pixels, int width, int height, int stride) {
    gfx_surface_t s;
    s.pixels = pixels;
    s.width  = width;
    s.height = height;
    s.stride = (stride > 0) ? stride : width;
    return s;
}

/* Single-pixel write/read, bounds-checked against the surface. Every
 * other primitive in this file (and vga_put_pixel/vga_get_pixel)
 * funnels through these rather than each re-deriving the same bounds
 * check. Not a "blit" by itself (a blit is inherently a block
 * operation) - this is the scalar primitive block operations are
 * built out of. */
static inline void gfx_surface_set_pixel(gfx_surface_t* s, int x, int y, uint32_t color) {
    if (!s || !s->pixels) return;
    if (x < 0 || y < 0 || x >= s->width || y >= s->height) return;
    s->pixels[(size_t)y * (size_t)s->stride + (size_t)x] = color;
}

static inline uint32_t gfx_surface_get_pixel(const gfx_surface_t* s, int x, int y) {
    if (!s || !s->pixels) return 0;
    if (x < 0 || y < 0 || x >= s->width || y >= s->height) return 0;
    return s->pixels[(size_t)y * (size_t)s->stride + (size_t)x];
}

typedef enum {
    GFX_BLIT_COPY = 0,   /* opaque block copy - every source pixel overwrites dst */
    GFX_BLIT_COLORKEY,   /* copy, skipping any source pixel that equals colorkey */
} gfx_blit_mode_t;

/*
 * The canonical BitBlt: transfer a w x h block from src(sx,sy) to
 * dst(dx,dy), clipped against both surfaces' bounds. This is the
 * literal block-transfer primitive - everything else in this file is
 * built on top of it or follows the same shape.
 */
void gfx_blit(gfx_surface_t* dst, int dx, int dy,
              const gfx_surface_t* src, int sx, int sy,
              int w, int h, gfx_blit_mode_t mode, uint32_t colorkey);

/*
 * Solid-color block fill - the "no source surface" case of BitBlt
 * (equivalent to a PatBlt). Used for window backgrounds, chrome,
 * clearing, and glyph background cells.
 */
void gfx_blit_fill(gfx_surface_t* dst, int x, int y, int w, int h, uint32_t color);

/*
 * Nearest-neighbor scaled block copy: samples src(sx,sy,sw,sh) into
 * dst(dx,dy,dw,dh). Used wherever a decoded image needs to be drawn
 * at a different size than it was decoded at (wallpaper, thumbnails,
 * the image viewer) - previously each of those call sites hand-rolled
 * this exact sampling loop independently.
 */
void gfx_blit_scaled(gfx_surface_t* dst, int dx, int dy, int dw, int dh,
                      const gfx_surface_t* src, int sx, int sy, int sw, int sh,
                      gfx_blit_mode_t mode, uint32_t colorkey);

/*
 * Blit a 1-bit-per-pixel bitmap (MSB-first within each row byte, rows
 * padded to stride_bytes) - used for font glyphs and other small
 * monochrome sprites (the mouse cursor's outline/fill mask). Set bits
 * become fg; clear bits become bg when bg_opaque is true, otherwise
 * they're left untouched so glyphs/sprites can sit on top of whatever
 * is already drawn (transparent background). scale replicates each
 * source bit into a scale x scale block of output pixels.
 */
void gfx_blit_bitmap1(gfx_surface_t* dst, int dx, int dy,
                       const uint8_t* bits, int bit_w, int bit_h, int stride_bytes,
                       uint32_t fg, uint32_t bg, bool bg_opaque, int scale);

#endif /* GFX_BLIT_H */
