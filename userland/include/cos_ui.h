/* cos_ui.h - drawing library for C-OS ring-3 applications.
 *
 * Draws into any 0x00RRGGBB pixel buffer - normally the one a window-API-v2
 * window hands you (cos_win2_create(): info.pixels / width / height /
 * stride). Shapes are anti-aliased; text is TrueType (stb_truetype) with a
 * per-size glyph cache. Two fonts are built in (Inter Regular and SemiBold,
 * Latin-1 plus a few UI symbols: arrows, play/pause/stop, note), so an app
 * works without any font files on disk; cui_font_load() takes any other TTF.
 *
 * Coordinates are pixels, origin top-left. Every call clips to the canvas
 * clip rectangle (the whole canvas unless cui_clip() narrows it).
 */
#ifndef COS_UI_H
#define COS_UI_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t *px;
    int w, h, stride;          /* stride in pixels */
    int cx0, cy0, cx1, cy1;    /* clip rectangle, exclusive right/bottom */
} cui_canvas;

void cui_canvas_init(cui_canvas *c, uint32_t *px, int w, int h, int stride);
void cui_clip(cui_canvas *c, int x, int y, int w, int h);   /* intersects with the current clip */
void cui_unclip(cui_canvas *c);

static inline uint32_t cui_rgb(int r, int g, int b) { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; }
uint32_t cui_mix(uint32_t a, uint32_t b, int t);            /* t = 0..255 : a -> b */

void cui_fill(cui_canvas *c, int x, int y, int w, int h, uint32_t color);
void cui_fill_alpha(cui_canvas *c, int x, int y, int w, int h, uint32_t color, int alpha);   /* alpha 0..255 */
void cui_vgradient(cui_canvas *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void cui_round_rect(cui_canvas *c, int x, int y, int w, int h, int radius, uint32_t color);
void cui_round_rect_alpha(cui_canvas *c, int x, int y, int w, int h, int radius, uint32_t color, int alpha);
void cui_round_rect_outline(cui_canvas *c, int x, int y, int w, int h, int radius, float thickness, uint32_t color);
void cui_circle(cui_canvas *c, float cx, float cy, float r, uint32_t color);
void cui_ring(cui_canvas *c, float cx, float cy, float r, float thickness, uint32_t color);
void cui_line(cui_canvas *c, float x0, float y0, float x1, float y1, float width, uint32_t color);
void cui_triangle(cui_canvas *c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color);
void cui_blit(cui_canvas *c, int x, int y, const uint32_t *src, int w, int h, int src_stride);

/* ---- text ---- */
typedef struct cui_font cui_font;
cui_font *cui_font_default(int px);   /* Inter Regular at `px` pixels (cached per size) */
cui_font *cui_font_bold(int px);      /* Inter SemiBold */
cui_font *cui_font_load(const void *ttf, int px);   /* ttf must stay valid */
void cui_font_set_fallback(cui_font *f, cui_font *fallback);   /* glyphs f lacks come from `fallback` (same px) */
int  cui_font_height(const cui_font *f);    /* line height */
int  cui_font_ascent(const cui_font *f);
/* Draws UTF-8 text with its line box's top-left at (x, y). Returns the advance width. */
int  cui_text(cui_canvas *c, cui_font *f, int x, int y, const char *utf8, uint32_t color);
int  cui_text_width(cui_font *f, const char *utf8);
/* Like cui_text, but truncates with "..." to fit max_w. */
int  cui_text_fit(cui_canvas *c, cui_font *f, int x, int y, int max_w, const char *utf8, uint32_t color);
/* Draws text centred inside the rectangle. */
void cui_text_center(cui_canvas *c, cui_font *f, int x, int y, int w, int h, const char *utf8, uint32_t color);

#endif
