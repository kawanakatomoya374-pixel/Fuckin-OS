#ifndef VGA_H
#define VGA_H

/* This is now the single canonical copy of vga.h - src/include/vga.h
 * used to be a second, independently-hand-maintained copy that had
 * drifted out of sync with this one (missing vga_draw_bmp/
 * vga_set_pixel/vga_reserve_physical_regions; this one was missing
 * the gradient/trig/thick-line declarations that were only added
 * here). Since -Isrc/include comes before -Isrc/drivers/video in the
 * Makefile's INCLUDES, any file outside drivers/video/ that did
 * `#include "vga.h"` was silently getting the *other*, incomplete
 * copy - a real footgun. src/include/vga.h is now just a one-line
 * redirect to this file, so there's exactly one declaration set to
 * keep in sync with vga.c from here on. */

#include "types.h"

// Screen dimensions
extern uint64_t SCREEN_W;
extern uint64_t SCREEN_H;
extern uint32_t* framebuffer;
extern uint32_t* backbuffer;

bool vga_has_framebuffer(void);

void vga_init(uint64_t multiboot_magic, uint64_t multiboot_info_addr);
void vga_reserve_physical_regions(void);
void vga_flip(void);
/* Present only a clipped backbuffer rectangle via the same BitBlt path. */
void vga_flip_rect(int x, int y, int w, int h);
void vga_wait_vblank(void);
void vga_clear(uint64_t color);
/* The BitBlt entry point: copy a w x h block from src_buf(sx,sy) onto
 * the screen at (dx,dy). src_buf is tightly packed at a natural width
 * of (sx + w) pixels - i.e. it must hold at least (sy + h) rows of
 * (sx + w) pixels each. sx=sy=0 (the common case) means src_buf is
 * exactly a w x h image; non-zero sx/sy let a caller blit a sub-rect
 * out of a bitmap it's keeping wider than what's being copied this
 * call. See gfx_blit.h for the underlying primitive this (and every
 * other drawing function below) is now implemented in terms of. */
void vga_copy_rect(int dx, int dy, int sx, int sy, int w, int h, uint32_t* src_buf);
/* Stride-aware BitBlt for a sub-rectangle of a full-sized software surface. */
void vga_copy_rect_strided(int dx, int dy, int w, int h, const uint32_t *src_buf,
                           int src_stride);

// Drawing primitives
void vga_put_pixel(int x, int y, uint64_t color);
void vga_set_pixel(int x, int y, uint64_t color);
uint64_t vga_get_pixel(int x, int y);
void vga_fill_rect(int x, int y, int w, int h, uint64_t color);
void vga_draw_rect(int x, int y, int w, int h, uint64_t color);
void vga_rect(int x, int y, int w, int h, uint64_t color);  /* alias for vga_draw_rect */
void vga_draw_line(int x0, int y0, int x1, int y1, uint64_t color);
void vga_draw_char(int x, int y, char c, uint64_t fg, uint64_t bg);
void vga_draw_string(int x, int y, const char* s, uint64_t fg, uint64_t bg);
void vga_draw_string_len(int x, int y, const char* s, int len, uint64_t fg, uint64_t bg);
void vga_set_font_scale(int scale);
int vga_get_font_scale(void);
void vga_set_font_resolution(int res);
int vga_get_font_resolution(void);
int vga_get_font_width(void);
int vga_get_font_height(void);
/* Exact horizontal advance used for one decoded Unicode scalar. */
int vga_get_codepoint_width(uint32_t codepoint);
void vga_fill_circle(int cx, int cy, int r, uint64_t color);
void vga_draw_circle(int cx, int cy, int r, uint64_t color);
void vga_fill_rounded_rect(int x, int y, int w, int h, int r, uint64_t color);
void vga_draw_rounded_rect(int x, int y, int w, int h, int r, uint64_t color);
void vga_draw_bmp(int x, int y, const uint8_t* bmp_data);

// Color helpers
uint64_t rgb(uint8_t r, uint8_t g, uint8_t b);
uint64_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
uint64_t blend(uint64_t fg, uint64_t bg, uint8_t alpha);
uint64_t darken(uint64_t color, uint8_t amount);
uint64_t lighten(uint64_t color, uint8_t amount);

// Font size
#define FONT_W (vga_get_font_width())
#define FONT_H (vga_get_font_height())

// Color constants (used by browser.c and other modules)
#define COLOR_WHITE  0x00FFFFFF
#define COLOR_BLACK  0x00000000
#define COLOR_RED    0x00FF0000
#define COLOR_GREEN  0x0000FF00
#define COLOR_BLUE   0x000000FF


/* Gradient drawing */
void vga_fill_gradient_rect(int x, int y, int w, int h,
                             uint64_t top_color, uint64_t bottom_color);
void vga_fill_gradient_rect_h(int x, int y, int w, int h,
                               uint64_t left_color, uint64_t right_color);
/* Trigonometric approximations (scaled by 1024) */
int vga_isin(int angle_deg);
int vga_icos(int angle_deg);
/* Thick line */
void vga_draw_line_thick(int x0, int y0, int x1, int y1, int thickness, uint64_t color);
#endif
