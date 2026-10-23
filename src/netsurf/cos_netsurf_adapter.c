/* C-OS NetSurf frontend primitives: UTF-8 layout metrics and 32bpp bitmap backing. */
/* Build flags define this historical name as a numeric macro. NetSurf's
 * plot_style.h declares it as an enum value, so expose the real enum here. */
#undef PLOT_FONT_FAMILY_SANS_SERIF
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <emmintrin.h>
#include <immintrin.h>
#include "memory.h"
#include "serial.h"
#include "cos_ns_font.h"

/* Upper bound on edge crossings per scanline. NetSurf's border mitres
 * are quads; SVG can be far more complex, and a cap keeps the crossing
 * list on the stack. A polygon exceeding it is filled from the
 * crossings that fit rather than dropped entirely. */
#define COS_NS_POLY_MAX_CROSSINGS 64u
#include "vga.h"
#include "gui.h"
#include "utils/errors.h"
#include "netsurf/layout.h"
#include "netsurf/bitmap.h"

struct cos_ns_bitmap {
    int width;
    int height;
    bool opaque;
    uint8_t *pixels;
};

static size_t cos_ns_utf8_step(const char *s, size_t remaining)
{
    if (remaining == 0 || s == NULL) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0 && remaining >= 2) return 2;
    if ((c & 0xF0) == 0xE0 && remaining >= 3) return 3;
    if ((c & 0xF8) == 0xF0 && remaining >= 4) return 4;
    return 1;
}

static uint32_t cos_ns_utf8_decode(const char *s, size_t remaining,
                                   size_t *consumed)
{
    if (consumed != NULL) *consumed = 0;
    if (s == NULL || remaining == 0) return 0;
    unsigned char c = (unsigned char)s[0];
    size_t step = cos_ns_utf8_step(s, remaining);
    if (step == 1) {
        if (consumed != NULL) *consumed = 1;
        return c;
    }
    for (size_t i = 1; i < step; ++i) {
        if (((unsigned char)s[i] & 0xC0u) != 0x80u) {
            if (consumed != NULL) *consumed = 1;
            return c;
        }
    }
    uint32_t value = (step == 2) ? (uint32_t)(c & 0x1Fu) :
                     (step == 3) ? (uint32_t)(c & 0x0Fu) :
                                   (uint32_t)(c & 0x07u);
    for (size_t i = 1; i < step; ++i) {
        value = (value << 6) | ((unsigned char)s[i] & 0x3Fu);
    }
    if (consumed != NULL) *consumed = step;
    return value;
}

/* ---- text measurement ---------------------------------------------------
 *
 * All three of these used to begin `(void)style;` and measure every
 * character at the one fixed 8x16 cell. libcss computed font-size,
 * font-weight and font-style correctly, NetSurf's select handler
 * resolved them, and layout.c carried them here faithfully - and then
 * they were discarded one function short of having any effect. Every
 * page laid out in a single uniform font: an <h1> occupied exactly as
 * much width as body text, so it wrapped in the wrong place, centred
 * off-centre and overflowed its box.
 *
 * They now go through cos_ns_font.c, which is also what the text
 * plotter uses - the two MUST agree on every advance or text drifts out
 * of the box the engine reserved for it. */

static int cos_ns_layout_advance(const cos_ns_font_t *font, const char *s,
                                 size_t remaining, size_t *step)
{
    uint32_t codepoint = cos_ns_font_decode(s, remaining, step);
    return cos_ns_font_advance(font, codepoint);
}

static nserror cos_ns_layout_width(const struct plot_font_style *style,
        const char *string, size_t length, int *width)
{
    if (width == NULL || (string == NULL && length != 0)) return NSERROR_BAD_PARAMETER;
    cos_ns_font_t font;
    cos_ns_font_from_style(style, &font);
    *width = cos_ns_font_string_width(&font, string, length);
    return NSERROR_OK;
}

static nserror cos_ns_layout_position(const struct plot_font_style *style,
        const char *string, size_t length, int x, size_t *char_offset, int *actual_x)
{
    if (char_offset == NULL || actual_x == NULL) return NSERROR_BAD_PARAMETER;
    cos_ns_font_t font;
    cos_ns_font_from_style(style, &font);

    size_t off = 0;
    int px = 0;
    if (x < 0) x = 0;
    while (off < length) {
        size_t step = 0;
        int advance = cos_ns_layout_advance(&font, string + off, length - off, &step);
        if (step == 0 || px + advance > x) break;
        off += step;
        px += advance;
    }
    *char_offset = off;
    *actual_x = px;
    return NSERROR_OK;
}

static nserror cos_ns_layout_split(const struct plot_font_style *style,
        const char *string, size_t length, int x, size_t *char_offset, int *actual_x)
{
    if (char_offset == NULL || actual_x == NULL) return NSERROR_BAD_PARAMETER;
    cos_ns_font_t font;
    cos_ns_font_from_style(style, &font);

    /* NetSurf asks for the last word boundary that fits in `x`, so this
     * is not simply "where does the pixel budget run out" - breaking
     * mid-word would be visibly wrong. Walk to the budget, then back up
     * to the most recent space. */
    size_t off = 0, last_space = 0;
    int px = 0, last_space_px = 0;
    bool found_space = false;

    while (off < length) {
        size_t step = 0;
        int advance = cos_ns_layout_advance(&font, string + off, length - off, &step);
        if (step == 0) break;
        if (string[off] == ' ') {
            last_space = off;
            last_space_px = px;
            found_space = true;
        }
        if (px + advance > x) break;
        off += step;
        px += advance;
    }

    if (off >= length) {
        /* The whole run fits. */
        *char_offset = length;
        *actual_x = px;
        return NSERROR_OK;
    }

    if (found_space && last_space > 0) {
        *char_offset = last_space;
        *actual_x = last_space_px;
        return NSERROR_OK;
    }

    /* A single word longer than the line. Emitting a zero-length split
     * here makes layout.c loop forever on it, so at least one character
     * must always be consumed. */
    if (off == 0 && length != 0) {
        size_t step = 0;
        int advance = cos_ns_layout_advance(&font, string, length, &step);
        *char_offset = (step != 0) ? step : 1;
        *actual_x = advance;
        return NSERROR_OK;
    }

    *char_offset = off;
    *actual_x = px;
    return NSERROR_OK;
}

static void *cos_ns_bitmap_create(int width, int height, enum gui_bitmap_flags flags)
{
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return NULL;
    struct cos_ns_bitmap *b = kmalloc(sizeof(*b));
    if (b == NULL) return NULL;
    size_t bytes = (size_t)width * (size_t)height * 4u;
    b->pixels = kmalloc(bytes);
    if (b->pixels == NULL) { kfree(b); return NULL; }
    if (flags & BITMAP_CLEAR) {
        for (size_t i = 0; i < bytes; ++i) b->pixels[i] = 0;
    }
    b->width = width; b->height = height; b->opaque = (flags & BITMAP_OPAQUE) != 0;
    return b;
}
static void cos_ns_bitmap_destroy(void *bitmap) { struct cos_ns_bitmap *b = bitmap; if (b) { if (b->pixels) kfree(b->pixels); kfree(b); } }
static void cos_ns_bitmap_set_opaque(void *bitmap, bool opaque) { if (bitmap) ((struct cos_ns_bitmap *)bitmap)->opaque = opaque; }
static bool cos_ns_bitmap_get_opaque(void *bitmap) { return bitmap ? ((struct cos_ns_bitmap *)bitmap)->opaque : false; }
static unsigned char *cos_ns_bitmap_buffer(void *bitmap) { return bitmap ? ((struct cos_ns_bitmap *)bitmap)->pixels : NULL; }
static size_t cos_ns_bitmap_rowstride(void *bitmap) { return bitmap ? (size_t)((struct cos_ns_bitmap *)bitmap)->width * 4u : 0; }
static int cos_ns_bitmap_width(void *bitmap) { return bitmap ? ((struct cos_ns_bitmap *)bitmap)->width : 0; }
static int cos_ns_bitmap_height(void *bitmap) { return bitmap ? ((struct cos_ns_bitmap *)bitmap)->height : 0; }
static void cos_ns_bitmap_modified(void *bitmap) { (void)bitmap; }
static nserror cos_ns_bitmap_render(struct bitmap *bitmap, struct hlcache_handle *content) { (void)bitmap; (void)content; return NSERROR_NOT_IMPLEMENTED; }

static struct gui_layout_table cos_ns_layout_table = { cos_ns_layout_width, cos_ns_layout_position, cos_ns_layout_split };
static struct gui_bitmap_table cos_ns_bitmap_table = {
    cos_ns_bitmap_create, cos_ns_bitmap_destroy, cos_ns_bitmap_set_opaque,
    cos_ns_bitmap_get_opaque, cos_ns_bitmap_buffer, cos_ns_bitmap_rowstride,
    cos_ns_bitmap_width, cos_ns_bitmap_height, cos_ns_bitmap_modified, cos_ns_bitmap_render
};
struct gui_layout_table *cos_netsurf_layout_table(void) { return &cos_ns_layout_table; }
struct gui_bitmap_table *cos_netsurf_bitmap_table(void) { return &cos_ns_bitmap_table; }

#include "netsurf/window.h"
#include "netsurf/mouse.h"
#include "utils/nsurl.h"
#include "utils/nsoption.h"
#include "utils/log.h"

/* A C-OS GUI window owns the real pixels; this frontend context records the
 * associated NetSurf browser window and its viewport.  Browser-app glue will
 * set the viewport before browser_window_reformat/redraw is enabled. */
struct gui_window {
    struct browser_window *bw;
    int width, height;
    int scroll_x, scroll_y;
    bool invalidated;
    /* Increments whenever NetSurf says the visible viewport changed.  The
     * GUI uses it to reuse a captured BitBlt surface only while the rendered
     * browser pixels are still authoritative. */
    uint32_t paint_generation;
};

static struct gui_window *g_cos_ns_active_window;

static struct gui_window *cos_ns_window_create(struct browser_window *bw,
        struct gui_window *existing, gui_window_create_flags flags)
{
    (void)existing; (void)flags;
    struct gui_window *gw = kmalloc(sizeof(*gw));
    if (gw == NULL) return NULL;
    gw->bw = bw; gw->width = 760; gw->height = 500;
    gw->scroll_x = 0; gw->scroll_y = 0; gw->invalidated = true;
    gw->paint_generation = 1;
    g_cos_ns_active_window = gw;
    return gw;
}
static void cos_ns_window_destroy(struct gui_window *gw)
{
    if (gw == g_cos_ns_active_window) g_cos_ns_active_window = NULL;
    if (gw) kfree(gw);
}

/* The browser bridge owns a single foreground NetSurf window.  The upstream
 * content callbacks mark it invalid when an asynchronous fetch completes;
 * consume that edge exactly once so a newly-opened HTML document is
 * reformatted before its next standard browser_window_redraw(). */
bool cos_netsurf_window_take_invalidated(void)
{
    if (g_cos_ns_active_window == NULL || !g_cos_ns_active_window->invalidated) {
        return false;
    }
    g_cos_ns_active_window->invalidated = false;
    return true;
}

uint32_t cos_netsurf_window_paint_generation(void)
{
    return g_cos_ns_active_window ? g_cos_ns_active_window->paint_generation : 0;
}

void cos_netsurf_window_set_viewport(int width, int height)
{
    if (g_cos_ns_active_window == NULL || width < 1 || height < 1) return;
    if (g_cos_ns_active_window->width != width ||
        g_cos_ns_active_window->height != height) {
        g_cos_ns_active_window->width = width;
        g_cos_ns_active_window->height = height;
        g_cos_ns_active_window->invalidated = true;
        ++g_cos_ns_active_window->paint_generation;
        if (g_cos_ns_active_window->paint_generation == 0) {
            g_cos_ns_active_window->paint_generation = 1;
        }
    }
}
static void cos_ns_window_mark_invalidated(struct gui_window *gw)
{
    if (gw == NULL) return;
    gw->invalidated = true;
    ++gw->paint_generation;
    if (gw->paint_generation == 0) gw->paint_generation = 1;
    /* This bridge runs inside NetSurf's cooperative fetch/schedule pump.
     * Propagate its paint invalidation to the C-OS lifecycle so the next
     * frame executes the normal full draw plus BitBlt flip rather than only
     * refreshing the idle FPS overlay. */
    gui_request_redraw();
}

/* Script-driven DOM replacement can rebuild boxes without an upstream
 * CONTENT_MSG redraw edge. Advance the same generation used by the GUI's
 * BitBlt cache so that its next frame cannot reuse stale page pixels. */
void cos_netsurf_window_force_invalidate(void)
{
    cos_ns_window_mark_invalidated(g_cos_ns_active_window);
}

static nserror cos_ns_window_invalidate(struct gui_window *gw, const struct rect *rect)
{ (void)rect; cos_ns_window_mark_invalidated(gw); return NSERROR_OK; }
static bool cos_ns_window_get_scroll(struct gui_window *gw, int *sx, int *sy)
{ if (!gw || !sx || !sy) return false; *sx = gw->scroll_x; *sy = gw->scroll_y; return true; }
static nserror cos_ns_window_set_scroll(struct gui_window *gw, const struct rect *r)
{ if (!gw || !r) return NSERROR_BAD_PARAMETER; gw->scroll_x = r->x0; gw->scroll_y = r->y0; cos_ns_window_mark_invalidated(gw); return NSERROR_OK; }

void cos_netsurf_window_get_scroll_offsets(int *scroll_x, int *scroll_y)
{
    if (scroll_x) *scroll_x = g_cos_ns_active_window ? g_cos_ns_active_window->scroll_x : 0;
    if (scroll_y) *scroll_y = g_cos_ns_active_window ? g_cos_ns_active_window->scroll_y : 0;
}

bool cos_netsurf_window_scroll_by(int delta_x, int delta_y,
                                  int content_width, int content_height)
{
    struct gui_window *gw = g_cos_ns_active_window;
    if (gw == NULL) return false;
    int max_x = content_width > gw->width ? content_width - gw->width : 0;
    int max_y = content_height > gw->height ? content_height - gw->height : 0;
    int next_x = gw->scroll_x + delta_x;
    int next_y = gw->scroll_y + delta_y;
    if (next_x < 0) next_x = 0;
    if (next_y < 0) next_y = 0;
    if (next_x > max_x) next_x = max_x;
    if (next_y > max_y) next_y = max_y;
    if (next_x == gw->scroll_x && next_y == gw->scroll_y) return false;
    gw->scroll_x = next_x;
    gw->scroll_y = next_y;
    cos_ns_window_mark_invalidated(gw);
    return true;
}

static nserror cos_ns_window_dimensions(struct gui_window *gw, int *w, int *h)
{ if (!gw || !w || !h) return NSERROR_BAD_PARAMETER; *w = gw->width; *h = gw->height; return NSERROR_OK; }
static nserror cos_ns_window_event(struct gui_window *gw, enum gui_window_event event)
{ (void)gw; (void)event; return NSERROR_OK; }
static void cos_ns_window_title(struct gui_window *gw, const char *title)
{ (void)gw; (void)title; }
static nserror cos_ns_window_url(struct gui_window *gw, struct nsurl *url)
{ (void)gw; (void)url; return NSERROR_OK; }

/* browser_window.c calls these hooks unconditionally as navigation state
 * changes. C-OS owns the visible chrome in gui_apps_browser.c, so they only
 * mark the NetSurf viewport dirty; leaving a NULL callback would fault. */
static void cos_ns_window_icon(struct gui_window *gw, struct hlcache_handle *icon)
{ (void)icon; cos_ns_window_mark_invalidated(gw); }
static void cos_ns_window_status(struct gui_window *gw, const char *text)
{ (void)text; cos_ns_window_mark_invalidated(gw); }
static void cos_ns_window_pointer(struct gui_window *gw, enum gui_pointer_shape shape)
{ (void)shape; cos_ns_window_mark_invalidated(gw); }
static void cos_ns_window_caret(struct gui_window *gw, int x, int y, int height,
                                const struct rect *clip)
{ (void)x; (void)y; (void)height; (void)clip; cos_ns_window_mark_invalidated(gw); }
static bool cos_ns_window_drag_start(struct gui_window *gw, gui_drag_type type,
                                     const struct rect *rect)
{ (void)type; (void)rect; cos_ns_window_mark_invalidated(gw); return false; }

static struct gui_window_table cos_ns_window_table = {
    .create = cos_ns_window_create, .destroy = cos_ns_window_destroy,
    .invalidate = cos_ns_window_invalidate, .get_scroll = cos_ns_window_get_scroll,
    .set_scroll = cos_ns_window_set_scroll, .get_dimensions = cos_ns_window_dimensions,
    .event = cos_ns_window_event, .set_title = cos_ns_window_title, .set_url = cos_ns_window_url,
    .set_icon = cos_ns_window_icon, .set_status = cos_ns_window_status,
    .set_pointer = cos_ns_window_pointer, .place_caret = cos_ns_window_caret,
    .drag_start = cos_ns_window_drag_start
};
struct gui_window_table *cos_netsurf_window_table(void) { return &cos_ns_window_table; }

#include "netsurf/plotters.h"
#include "netsurf/types.h"

/* NetSurf colours are 0xXXBBGGRR; vga.* uses 0x00RRGGBB. */
static uint64_t cos_ns_plot_colour(colour c)
{
    return ((uint64_t)(c & 0xffu) << 16) |
           ((uint64_t)((c >> 8) & 0xffu) << 8) |
           (uint64_t)((c >> 16) & 0xffu);
}

/* The standard redraw contract maintains a single active clip for every
 * plot operation. C-OS uses a synchronous display renderer, so a compact
 * frontend-global clip is sufficient until per-window redraw scheduling is
 * enabled. */
static int cos_ns_clip_x0;
static int cos_ns_clip_y0;
static int cos_ns_clip_x1;
static int cos_ns_clip_y1;
static bool cos_ns_clip_active;
/* Compact redraw diagnostics: a successful browser_window_redraw() alone does
 * not prove the page produced visible draw operations. Keep counters in the
 * frontend plotter so the bridge can distinguish an empty box tree from a
 * coordinate/paint problem without logging every primitive. */
static uint32_t cos_ns_plot_rect_count;
static uint32_t cos_ns_plot_text_count;
static uint32_t cos_ns_plot_bitmap_count;
/* One-shot field diagnostics for real NetSurf text plotting.  This is not
 * reset per redraw: a loading page can request many redraws before stable
 * layout, and serial output must never become the next rendering bottleneck. */
static uint32_t cos_ns_text_plot_diag_remaining = 10;

void cos_netsurf_plot_stats_reset(void)
{
    cos_ns_plot_rect_count = 0;
    cos_ns_plot_text_count = 0;
    cos_ns_plot_bitmap_count = 0;
}

void cos_netsurf_plot_stats_get(uint32_t *rects, uint32_t *texts, uint32_t *bitmaps)
{
    if (rects) *rects = cos_ns_plot_rect_count;
    if (texts) *texts = cos_ns_plot_text_count;
    if (bitmaps) *bitmaps = cos_ns_plot_bitmap_count;
}

static bool cos_ns_plot_clip_rect(int *x, int *y, int *w, int *h)
{
    int x0 = *x, y0 = *y, x1 = x0 + *w, y1 = y0 + *h;
    if (cos_ns_clip_active) {
        if (x0 < cos_ns_clip_x0) x0 = cos_ns_clip_x0;
        if (y0 < cos_ns_clip_y0) y0 = cos_ns_clip_y0;
        if (x1 > cos_ns_clip_x1) x1 = cos_ns_clip_x1;
        if (y1 > cos_ns_clip_y1) y1 = cos_ns_clip_y1;
    }
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}

static nserror cos_ns_plot_clip(const struct redraw_context *ctx,
                                const struct rect *clip)
{
    (void)ctx;
    if (clip == NULL) {
        cos_ns_clip_active = false;
        return NSERROR_OK;
    }
    cos_ns_clip_x0 = clip->x0; cos_ns_clip_y0 = clip->y0;
    cos_ns_clip_x1 = clip->x1; cos_ns_clip_y1 = clip->y1;
    cos_ns_clip_active = true;
    return NSERROR_OK;
}

static nserror cos_ns_plot_arc(const struct redraw_context *ctx,
                               const plot_style_t *s, int x, int y,
                               int radius, int angle1, int angle2)
{
    (void)ctx; (void)s; (void)x; (void)y; (void)radius;
    (void)angle1; (void)angle2;
    /* Rounded CSS corners normally arrive as paths; omitting isolated arcs
     * is visually safe and must not abort the page redraw. */
    return NSERROR_OK;
}

static nserror cos_ns_plot_disc(const struct redraw_context *ctx,
                                const plot_style_t *s, int x, int y,
                                int radius)
{
    (void)ctx;
    if (s != NULL && radius > 0 && s->fill_type != PLOT_OP_TYPE_NONE) {
        vga_fill_circle(x, y, radius, cos_ns_plot_colour(s->fill_colour));
    }
    return NSERROR_OK;
}

/* Draws one horizontal or vertical run honouring the stroke pattern.
 *
 * PLOT_OP_TYPE_DOT and _DASH are what CSS `border-style: dotted` and
 * `dashed` become by the time redraw_border.c is done with them. Both
 * plot callbacks used to test only `!= PLOT_OP_TYPE_NONE` and then draw
 * solid, so every dotted and dashed border in every page rendered as a
 * solid line - CSS computed correctly, and discarded at the last step.
 *
 * The pattern periods (2 on/2 off for dots, 6/3 for dashes) match what
 * NetSurf's own framebuffer frontend uses, so a page looks the same here
 * as it does there.
 *
 * Defined before cos_ns_plot_line/cos_ns_plot_rect, both of which call
 * it - C has no forward-reference for a static function without a
 * prototype, and adding a prototype just to place the definition after
 * its only callers would be pure churn. */
static void cos_ns_stroke_span(int x, int y, int len, bool vertical,
                               plot_operation_type_t op, uint64_t colour)
{
    int on = 1, period = 1;
    if (op == PLOT_OP_TYPE_DOT)       { on = 2; period = 4; }
    else if (op == PLOT_OP_TYPE_DASH) { on = 6; period = 9; }

    if (period == 1) {
        if (vertical) vga_fill_rect(x, y, 1, len, colour);
        else          vga_fill_rect(x, y, len, 1, colour);
        return;
    }
    for (int i = 0; i < len; ) {
        int run = on;
        if (i + run > len) run = len - i;
        if (vertical) vga_fill_rect(x, y + i, 1, run, colour);
        else          vga_fill_rect(x + i, y, run, 1, colour);
        i += period;
    }
}

/* A rectangle outline `width` pixels thick, with the stroke pattern. */
static void cos_ns_stroke_rect(int x, int y, int w, int h, int width,
                               plot_operation_type_t op, uint64_t colour)
{
    if (w <= 0 || h <= 0) return;
    if (width < 1) width = 1;
    for (int i = 0; i < width && i * 2 < w && i * 2 < h; ++i) {
        cos_ns_stroke_span(x + i,         y + i,         w - 2 * i, false, op, colour);
        cos_ns_stroke_span(x + i,         y + h - 1 - i, w - 2 * i, false, op, colour);
        cos_ns_stroke_span(x + i,         y + i,         h - 2 * i, true,  op, colour);
        cos_ns_stroke_span(x + w - 1 - i, y + i,         h - 2 * i, true,  op, colour);
    }
}

static nserror cos_ns_plot_line(const struct redraw_context *ctx,
                                const plot_style_t *s, const struct rect *r)
{
    (void)ctx;
    if (s == NULL || r == NULL || s->stroke_type == PLOT_OP_TYPE_NONE) {
        return NSERROR_OK;
    }
    int width = plot_style_fixed_to_int(s->stroke_width);
    if (width < 1) width = 1;
    uint64_t colour = cos_ns_plot_colour(s->stroke_colour);

    /* Axis-aligned lines - which is what borders, <hr> and table rules
     * all are - go through the pattern-aware span so dotted and dashed
     * strokes are honoured. Diagonals fall back to a solid thick line:
     * patterning one correctly needs arc-length stepping, and drawing a
     * wrong pattern would be worse than drawing none. */
    if (s->stroke_type != PLOT_OP_TYPE_SOLID) {
        if (r->y0 == r->y1) {
            int x0 = (r->x0 < r->x1) ? r->x0 : r->x1;
            int len = (r->x1 > r->x0) ? (r->x1 - r->x0) : (r->x0 - r->x1);
            for (int i = 0; i < width; ++i) {
                cos_ns_stroke_span(x0, r->y0 + i, len, false, s->stroke_type, colour);
            }
            return NSERROR_OK;
        }
        if (r->x0 == r->x1) {
            int y0 = (r->y0 < r->y1) ? r->y0 : r->y1;
            int len = (r->y1 > r->y0) ? (r->y1 - r->y0) : (r->y0 - r->y1);
            for (int i = 0; i < width; ++i) {
                cos_ns_stroke_span(r->x0 + i, y0, len, true, s->stroke_type, colour);
            }
            return NSERROR_OK;
        }
    }

    vga_draw_line_thick(r->x0, r->y0, r->x1, r->y1, width, colour);
    return NSERROR_OK;
}

static nserror cos_ns_plot_rect(const struct redraw_context *ctx,
                                const plot_style_t *s, const struct rect *r)
{
    (void)ctx;
    ++cos_ns_plot_rect_count;
    if (s == NULL || r == NULL) return NSERROR_OK;
    int x = r->x0, y = r->y0, w = r->x1 - r->x0, h = r->y1 - r->y0;
    if (!cos_ns_plot_clip_rect(&x, &y, &w, &h)) return NSERROR_OK;
    if (s->fill_type != PLOT_OP_TYPE_NONE) {
        vga_fill_rect(x, y, w, h, cos_ns_plot_colour(s->fill_colour));
    }
    if (s->stroke_type != PLOT_OP_TYPE_NONE) {
        /* stroke_width was ignored here, so every `border-width` drew as
         * one pixel no matter what CSS asked for. */
        int width = plot_style_fixed_to_int(s->stroke_width);
        cos_ns_stroke_rect(x, y, w, h, width, s->stroke_type,
                           cos_ns_plot_colour(s->stroke_colour));
    }
    return NSERROR_OK;
}

static nserror cos_ns_plot_polygon(const struct redraw_context *ctx,
                                   const plot_style_t *s, const int *p,
                                   unsigned int n)
{
    (void)ctx;
    if (s == NULL || p == NULL || n < 2) return NSERROR_OK;

    if (s->fill_type != PLOT_OP_TYPE_NONE && n >= 3) {
        uint64_t fill = cos_ns_plot_colour(s->fill_colour);

        int min_y = p[1], max_y = p[1];
        for (unsigned int i = 1; i < n; ++i) {
            int py = p[i * 2u + 1u];
            if (py < min_y) min_y = py;
            if (py > max_y) max_y = py;
        }
        if (cos_ns_clip_active) {
            if (min_y < cos_ns_clip_y0) min_y = cos_ns_clip_y0;
            if (max_y > cos_ns_clip_y1) max_y = cos_ns_clip_y1;
        }

        for (int y = min_y; y < max_y; ++y) {
            /* Crossings of the scanline through the pixel CENTRE
             * (y + 0.5), which avoids the classic double-count when a
             * vertex sits exactly on an integer scanline. */
            int xs[COS_NS_POLY_MAX_CROSSINGS];
            unsigned int count = 0;
            for (unsigned int i = 0; i < n && count < COS_NS_POLY_MAX_CROSSINGS; ++i) {
                unsigned int j = (i + 1u) % n;
                int y0 = p[i * 2u + 1u], y1 = p[j * 2u + 1u];
                int x0 = p[i * 2u],      x1 = p[j * 2u];
                if (y0 == y1) continue;
                int lo = (y0 < y1) ? y0 : y1;
                int hi = (y0 < y1) ? y1 : y0;
                if (y < lo || y >= hi) continue;
                /* Linear interpolation in integer arithmetic. */
                xs[count++] = x0 + (int)(((int64_t)(y - y0) * (x1 - x0)) / (y1 - y0));
            }
            if (count < 2) continue;

            for (unsigned int a = 1; a < count; ++a) {
                int key = xs[a];
                int b = (int)a - 1;
                while (b >= 0 && xs[b] > key) { xs[b + 1] = xs[b]; b--; }
                xs[b + 1] = key;
            }
            for (unsigned int a = 0; a + 1 < count; a += 2) {
                int left = xs[a], right = xs[a + 1];
                if (cos_ns_clip_active) {
                    if (left < cos_ns_clip_x0) left = cos_ns_clip_x0;
                    if (right > cos_ns_clip_x1) right = cos_ns_clip_x1;
                }
                if (right > left) vga_fill_rect(left, y, right - left, 1, fill);
            }
        }
    }

    if (s->stroke_type != PLOT_OP_TYPE_NONE) {
        uint64_t c = cos_ns_plot_colour(s->stroke_colour);
        for (unsigned int i = 0; i < n; i++) {
            unsigned int j = (i + 1u) % n;
            vga_draw_line(p[i * 2u], p[i * 2u + 1u],
                          p[j * 2u], p[j * 2u + 1u], c);
        }
    }
    return NSERROR_OK;
}

/* ---- path rasterisation helpers ------------------------------------------
 *
 * cos_ns_plot_path() decodes a PLOTTER_PATH_* opcode stream (move/line/
 * bezier/close) into a flattened polyline per subpath, then hands each
 * subpath to cos_ns_path_render_subpath() to actually draw. Split out
 * because the opcode decoder and the drawing decision (fill vs stroke,
 * closed vs open) are genuinely separate concerns: the decoder does not
 * need to know how a subpath ends up on screen, only where its points
 * are. */

/* Cap on vertices per flattened subpath. A bezier expands to
 * COS_NS_PATH_BEZIER_STEPS points; a pathological path with many curves
 * in one unclosed subpath must not overflow the caller's stack array, so
 * further points are silently dropped rather than written out of
 * bounds - a truncated curve is a visible glitch, not a crash. */
#define COS_NS_PATH_MAX_POINTS 256u

/* Subdivisions per cubic bezier segment. 16 keeps curves smooth at the
 * sizes CSS border-radius / SVG paths actually render at in this
 * renderer, without the point count above dominating the path budget. */
#define COS_NS_PATH_BEZIER_STEPS 16u

/* Applies the path's affine transform: NetSurf's plotter path opcodes
 * carry coordinates in the content's own space, and `transform` (the
 * standard 2x3 affine matrix [a b c d e f], same convention as SVG/
 * Cairo) maps that space to device pixels:
 *
 *     tx = a*x + c*y + e
 *     ty = b*x + d*y + f
 *
 * A NULL transform means identity - some callers pass one, some don't. */
static void cos_ns_path_transform_point(const float transform[6],
                                        float x, float y, int *out_x, int *out_y)
{
    if (transform == NULL) {
        *out_x = (int)(x + 0.5f);
        *out_y = (int)(y + 0.5f);
        return;
    }
    float tx = transform[0] * x + transform[2] * y + transform[4];
    float ty = transform[1] * x + transform[3] * y + transform[5];
    *out_x = (int)(tx + (tx >= 0.0f ? 0.5f : -0.5f));
    *out_y = (int)(ty + (ty >= 0.0f ? 0.5f : -0.5f));
}

/* Appends one already-transformed point, bounded by the cap. */
static void cos_ns_path_append(int points[][2], unsigned int *count, int x, int y)
{
    if (*count >= COS_NS_PATH_MAX_POINTS) return;
    points[*count][0] = x;
    points[*count][1] = y;
    (*count)++;
}

/* Draws one flattened subpath: filled (via the real scanline polygon
 * rasteriser above - `points` is laid out identically to the flat
 * (x,y)-interleaved array cos_ns_plot_polygon expects, since an
 * `int[N][2]` and a flat `int[2N]` share the same memory layout in C)
 * when the style asks for a fill and the subpath is closed, and/or
 * stroked as a connected polyline when it asks for a stroke.
 *
 * An open subpath is never filled: SVG and CSS both leave an open path's
 * fill undefined in exactly the way that "connect the endpoints and fill
 * that" is not, so drawing one would show something no author asked
 * for. */
static void cos_ns_path_render_subpath(const plot_style_t *s,
                                       int points[][2], unsigned int count,
                                       bool closed)
{
    if (s == NULL || count == 0u) return;

    if (closed && count >= 3u && s->fill_type != PLOT_OP_TYPE_NONE) {
        cos_ns_plot_polygon(NULL, s, &points[0][0], count);
    }

    if (s->stroke_type != PLOT_OP_TYPE_NONE && count >= 2u) {
        uint64_t c = cos_ns_plot_colour(s->stroke_colour);
        for (unsigned int i = 0; i + 1 < count; ++i) {
            vga_draw_line(points[i][0], points[i][1],
                          points[i + 1][0], points[i + 1][1], c);
        }
        if (closed && count >= 2u) {
            vga_draw_line(points[count - 1][0], points[count - 1][1],
                          points[0][0], points[0][1], c);
        }
    }
}

static nserror cos_ns_plot_path(const struct redraw_context *ctx,
                                const plot_style_t *s, const float *p,
                                unsigned int n, const float transform[6])
{
    (void)ctx;
    if (s == NULL || p == NULL || n == 0u) return NSERROR_OK;

    int points[COS_NS_PATH_MAX_POINTS][2];
    unsigned int point_count = 0;
    bool closed = false;
    float current_x = 0.0f, current_y = 0.0f;

    for (unsigned int i = 0; i < n;) {
        int command = (int)p[i];
        if (command == PLOTTER_PATH_MOVE || command == PLOTTER_PATH_LINE) {
            if (i + 2u >= n) break;
            if (command == PLOTTER_PATH_MOVE && point_count != 0u) {
                cos_ns_path_render_subpath(s, points, point_count, closed);
                point_count = 0;
                closed = false;
            }
            current_x = p[i + 1u];
            current_y = p[i + 2u];
            int x, y;
            cos_ns_path_transform_point(transform, current_x, current_y, &x, &y);
            cos_ns_path_append(points, &point_count, x, y);
            i += 3u;
        } else if (command == PLOTTER_PATH_CLOSE) {
            closed = true;
            cos_ns_path_render_subpath(s, points, point_count, true);
            point_count = 0;
            closed = false;
            ++i;
        } else if (command == PLOTTER_PATH_BEZIER) {
            if (i + 6u >= n || point_count == 0u) break;
            float start_x = current_x, start_y = current_y;
            float c1x = p[i + 1u], c1y = p[i + 2u];
            float c2x = p[i + 3u], c2y = p[i + 4u];
            current_x = p[i + 5u];
            current_y = p[i + 6u];
            for (unsigned int step = 1; step <= COS_NS_PATH_BEZIER_STEPS; ++step) {
                float t = (float)step / (float)COS_NS_PATH_BEZIER_STEPS;
                float mt = 1.0f - t;
                float x = mt * mt * mt * start_x + 3.0f * mt * mt * t * c1x +
                          3.0f * mt * t * t * c2x + t * t * t * current_x;
                float y = mt * mt * mt * start_y + 3.0f * mt * mt * t * c1y +
                          3.0f * mt * t * t * c2y + t * t * t * current_y;
                int tx, ty;
                cos_ns_path_transform_point(transform, x, y, &tx, &ty);
                cos_ns_path_append(points, &point_count, tx, ty);
            }
            i += 7u;
        } else {
            /* Invalid externally supplied opcode: stop this path only. */
            break;
        }
    }
    if (point_count != 0u) cos_ns_path_render_subpath(s, points, point_count, closed);
    return NSERROR_OK;
}

/* C-OS JPEG and PNG codecs write bytes in BGRA order. On little-endian
 * x86-64 this is the uint32_t value 0xAARRGGBB, already matching the channel
 * positions of the XRGB backbuffer after alpha is removed. The previous code
 * incorrectly treated it as 0xXXBBGGRR and swapped red/blue, producing the
 * visibly wrong skin and jacket colours on ordinary JPEG pages. */
static inline __m128i cos_ns_bgra_to_xrgb4_sse2(__m128i in)
{
    return _mm_and_si128(in, _mm_set1_epi32(0x00ffffff));
}

__attribute__((target("avx2")))
static void cos_ns_bgra_to_xrgb_avx2(uint32_t *dst, const uint32_t *src, int count)
{
    int i = 0;
    const __m256i mask = _mm256_set1_epi32(0x00ffffff);
    for (; i + 8 <= count; i += 8) {
        __m256i in = _mm256_loadu_si256((const __m256i *)(src + i));
        _mm256_storeu_si256((__m256i *)(dst + i), _mm256_and_si256(in, mask));
    }
    for (; i < count; ++i) dst[i] = src[i] & 0x00ffffffu;
    _mm256_zeroupper();
}

static void cos_ns_bgra_to_xrgb_sse2(uint32_t *dst, const uint32_t *src, int count)
{
    int i = 0;
    for (; i + 4 <= count; i += 4) {
        __m128i in = _mm_loadu_si128((const __m128i *)(src + i));
        _mm_storeu_si128((__m128i *)(dst + i), cos_ns_bgra_to_xrgb4_sse2(in));
    }
    for (; i < count; ++i) dst[i] = src[i] & 0x00ffffffu;
}

static inline uint32_t cos_ns_bgra_over_xrgb(uint32_t src, uint32_t dst)
{
    uint32_t alpha = src >> 24;
    if (alpha == 0u) return dst;
    if (alpha == 255u) return src & 0x00ffffffu;
    uint32_t inv = 255u - alpha;
    uint32_t r = ((((src >> 16) & 0xffu) * alpha) +
                  (((dst >> 16) & 0xffu) * inv) + 127u) / 255u;
    uint32_t g = ((((src >> 8) & 0xffu) * alpha) +
                  (((dst >> 8) & 0xffu) * inv) + 127u) / 255u;
    uint32_t b = (((src & 0xffu) * alpha) + ((dst & 0xffu) * inv) + 127u) / 255u;
    return (r << 16) | (g << 8) | b;
}

extern bool gfx_blit_avx2_available(void);

static nserror cos_ns_plot_bitmap(const struct redraw_context *ctx,
                                  struct bitmap *bitmap, int x, int y,
                                  int width, int height, colour bg,
                                  bitmap_flags_t flags)
{
    (void)ctx; (void)bg; (void)flags;
    ++cos_ns_plot_bitmap_count;
    struct cos_ns_bitmap *b = (struct cos_ns_bitmap *)bitmap;
    if (b == NULL || b->pixels == NULL || b->width <= 0 || b->height <= 0 ||
        width <= 0 || height <= 0) return NSERROR_OK;

    /* The framebuffer stores XRGB while codec bitmap bytes are BGRA. The
     * equal-size opaque path is the browser's hot case (JPEG/BMP plus opaque
     * PNG): clear alpha from cache-friendly rows via AVX2 or SSE2. */
    int original_x = x, original_y = y, visible_w = width, visible_h = height;
    if (b->opaque && width == b->width && height == b->height &&
        backbuffer != NULL && cos_ns_plot_clip_rect(&x, &y, &visible_w, &visible_h)) {
        int source_x = x - original_x;
        int source_y = y - original_y;
        bool use_avx2 = gfx_blit_avx2_available();
        for (int row = 0; row < visible_h; ++row) {
            uint32_t *dst = backbuffer + (size_t)(y + row) * (size_t)SCREEN_W + (size_t)x;
            const uint32_t *src = (const uint32_t *)b->pixels +
                (size_t)(source_y + row) * (size_t)b->width + (size_t)source_x;
            if (use_avx2) cos_ns_bgra_to_xrgb_avx2(dst, src, visible_w);
            else cos_ns_bgra_to_xrgb_sse2(dst, src, visible_w);
        }
        return NSERROR_OK;
    }

    /* Scaled and transparent resources use nearest-neighbour sampling.  Keep
     * the exact existing semantics, but clip once and write the backbuffer
     * directly: the former per-pixel vga_set_pixel() call re-checked buffer
     * state and bounds for every sample, which made ordinary scaled page art
     * disproportionately expensive. NetSurf transparency is binary here, so
     * an untouched destination is the correct transparent result. */
    int dx0 = 0, dy0 = 0, dx1 = width, dy1 = height;
    if (cos_ns_clip_active) {
        if (x < cos_ns_clip_x0) dx0 = cos_ns_clip_x0 - x;
        if (y < cos_ns_clip_y0) dy0 = cos_ns_clip_y0 - y;
        if (x + dx1 > cos_ns_clip_x1) dx1 = cos_ns_clip_x1 - x;
        if (y + dy1 > cos_ns_clip_y1) dy1 = cos_ns_clip_y1 - y;
    }
    if (dx1 <= dx0 || dy1 <= dy0) return NSERROR_OK;
    if (backbuffer != NULL) {
        const uint32_t *source = (const uint32_t *)b->pixels;
        for (int dy = dy0; dy < dy1; ++dy) {
            int sy = (dy * b->height) / height;
            uint32_t *dst = backbuffer + (size_t)(y + dy) * (size_t)SCREEN_W +
                            (size_t)(x + dx0);
            const uint32_t *src_row = source + (size_t)sy * (size_t)b->width;
            for (int dx = dx0; dx < dx1; ++dx) {
                uint32_t src = src_row[(dx * b->width) / width];
                if (src != NS_TRANSPARENT) {
                    *dst = cos_ns_bgra_over_xrgb(src, *dst);
                }
                ++dst;
            }
        }
        return NSERROR_OK;
    }
    for (int dy = dy0; dy < dy1; ++dy) {
        int sy = (dy * b->height) / height;
        for (int dx = dx0; dx < dx1; ++dx) {
            uint32_t src = ((uint32_t *)b->pixels)[sy * b->width +
                                                     (dx * b->width) / width];
            if (src != NS_TRANSPARENT && (src >> 24) != 0u) {
                vga_set_pixel(x + dx, y + dy, src & 0x00ffffffu);
            }
        }
    }
    return NSERROR_OK;
}

/* ---- text ---------------------------------------------------------------
 *
 * This used to call vga_draw_string_len(), which draws at the one fixed
 * bitmap size with no weight and no slant - so CSS font-size,
 * font-weight and font-style had no visual effect whatever, and the
 * drawn text did not match the width layout had measured for it.
 *
 * It now builds each glyph through cos_ns_font.c at the size the style
 * asks for, and advances by cos_ns_font_advance() - the SAME function
 * the layout callbacks above use. That shared advance is the invariant:
 * if drawing stepped differently from measuring, text would drift out of
 * its box a pixel at a time.
 *
 * Clipping is per-glyph rather than per-string. The old code rejected a
 * whole run whose baseline fell outside the clip box, which makes
 * `overflow: hidden` an all-or-nothing effect: a line half inside the
 * clip either drew entirely (spilling out) or vanished entirely. */
/* Draws a 1bpp glyph mask, trimmed to the active clip rectangle.
 *
 * Per-PIXEL clipping, not per-glyph: a glyph straddling the clip edge
 * must be drawn with only the inside part visible. Rejecting the whole
 * glyph instead leaves a ragged one-character gap at every clip
 * boundary, which is what `overflow: hidden` looks like when it is
 * implemented by whole-object rejection. */
static void cos_ns_blit_glyph_clipped(int x, int y, const uint8_t *bits,
                                      int w, int h, int stride,
                                      uint64_t fg, uint64_t bg)
{
    if (bits == NULL || w <= 0 || h <= 0) return;

    int sx = 0, sy = 0;
    int dw = w, dh = h;

    if (cos_ns_clip_active) {
        if (x < cos_ns_clip_x0) { sx = cos_ns_clip_x0 - x; dw -= sx; x = cos_ns_clip_x0; }
        if (y < cos_ns_clip_y0) { sy = cos_ns_clip_y0 - y; dh -= sy; y = cos_ns_clip_y0; }
        if (x + dw > cos_ns_clip_x1) dw = cos_ns_clip_x1 - x;
        if (y + dh > cos_ns_clip_y1) dh = cos_ns_clip_y1 - y;
        if (dw <= 0 || dh <= 0) return;
    }

    if (sx == 0 && sy == 0 && dw == w && dh == h) {
        vga_blit_mask(x, y, bits, w, h, stride, fg, bg);
        return;
    }

    /* Partially clipped: repack the visible sub-rectangle. Shifting bits
     * rather than drawing pixel by pixel keeps the common case (a line
     * of text meeting a scroll edge) cheap. */
    uint8_t sub[COS_NS_FONT_MASK_BYTES];
    int sub_stride = (dw + 7) / 8;
    if (sub_stride * dh > (int)sizeof(sub)) return;
    for (int r = 0; r < dh; ++r) {
        for (int c = 0; c < dw; ++c) {
            int srcc = sx + c;
            uint8_t byte = bits[(sy + r) * stride + (srcc >> 3)];
            if (byte & (uint8_t)(0x80u >> (srcc & 7))) {
                sub[r * sub_stride + (c >> 3)] |= (uint8_t)(0x80u >> (c & 7));
            } else {
                /* Cleared explicitly rather than memset up front: the
                 * buffer is reused across glyphs and stale ink from a
                 * previous one would show as speckle. */
                sub[r * sub_stride + (c >> 3)] &= (uint8_t)~(0x80u >> (c & 7));
            }
        }
    }
    vga_blit_mask(x, y, sub, dw, dh, sub_stride, fg, bg);
}

static nserror cos_ns_plot_text(const struct redraw_context *ctx,
                                const plot_font_style_t *s, int x, int y,
                                const char *text, size_t n)
{
    (void)ctx;
    ++cos_ns_plot_text_count;
    if (s == NULL || text == NULL || n == 0) return NSERROR_OK;

    cos_ns_font_t font;
    cos_ns_font_from_style(s, &font);

    uint64_t foreground = cos_ns_plot_colour(s->foreground);
    uint64_t background = cos_ns_plot_colour(s->background);

    /* NetSurf plots text by its BASELINE; the mask's top edge is the
     * ascent above that. */
    int top = y - cos_ns_font_ascent(&font);
    int bottom = top + cos_ns_font_height(&font);

    if (cos_ns_text_plot_diag_remaining != 0) {
        --cos_ns_text_plot_diag_remaining;
        serial_puts("[NSTEXT] x="); serial_putdec((uint64_t)(int64_t)x);
        serial_puts(" y="); serial_putdec((uint64_t)(int64_t)y);
        serial_puts(" px="); serial_putdec((uint64_t)cos_ns_font_height(&font));
        serial_puts(s->weight >= 600 ? " bold" : "");
        serial_puts((s->flags & (FONTF_ITALIC | FONTF_OBLIQUE)) ? " italic" : "");
        serial_puts(" fg=0x"); serial_puthex(foreground);
        serial_puts(" n="); serial_putdec((uint64_t)n);
        serial_puts("\n");
    }

    /* Whole-run vertical reject: cheap, and correct because every glyph
     * in the run shares this baseline. */
    if (cos_ns_clip_active && (bottom <= cos_ns_clip_y0 || top >= cos_ns_clip_y1)) {
        return NSERROR_OK;
    }

    uint8_t mask[COS_NS_FONT_MASK_BYTES];

    for (size_t off = 0; off < n; ) {
        size_t step = 0;
        uint32_t cp = cos_ns_font_decode(text + off, n - off, &step);
        if (step == 0) break;
        off += step;

        int advance = cos_ns_font_advance(&font, cp);
        if (advance == 0) continue;               /* combining mark / joiner */

        /* Stop once past the right clip edge - the rest of the run
         * cannot be visible, and a long run off-screen is the common
         * case for a wide page. */
        if (cos_ns_clip_active && x >= cos_ns_clip_x1) break;

        if (!cos_ns_clip_active || (x + advance > cos_ns_clip_x0)) {
            int stride = 0;
            int w = cos_ns_font_build_mask(&font, cp, mask, &stride);
            if (w > 0) {
                cos_ns_blit_glyph_clipped(x, top, mask, w,
                                          cos_ns_font_height(&font), stride,
                                          foreground, background);
            }
        }
        x += advance;
    }
    return NSERROR_OK;
}

static const struct plotter_table cos_ns_plotter_table = {
    .clip = cos_ns_plot_clip,
    .arc = cos_ns_plot_arc,
    .disc = cos_ns_plot_disc,
    .line = cos_ns_plot_line,
    .rectangle = cos_ns_plot_rect,
    .polygon = cos_ns_plot_polygon,
    .path = cos_ns_plot_path,
    .bitmap = cos_ns_plot_bitmap,
    .text = cos_ns_plot_text,
    .group_start = NULL,
    .group_end = NULL,
    .flush = NULL,
    .option_knockout = false
};

const struct plotter_table *cos_netsurf_plotter_table(void)
{
    return &cos_ns_plotter_table;
}
