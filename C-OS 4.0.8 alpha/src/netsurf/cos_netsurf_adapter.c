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

static int cos_ns_layout_advance(const char *s, size_t remaining, size_t *step)
{
    uint32_t codepoint = cos_ns_utf8_decode(s, remaining, step);
    return vga_get_codepoint_width(codepoint);
}

static nserror cos_ns_layout_width(const struct plot_font_style *style,
        const char *string, size_t length, int *width)
{
    (void)style;
    if (width == NULL || (string == NULL && length != 0)) return NSERROR_BAD_PARAMETER;
    int px = 0;
    for (size_t off = 0; off < length; ) {
        size_t step = 0;
        int advance = cos_ns_layout_advance(string + off, length - off, &step);
        if (step == 0) break;
        px += advance;
        off += step;
    }
    *width = px;
    return NSERROR_OK;
}

static nserror cos_ns_layout_position(const struct plot_font_style *style,
        const char *string, size_t length, int x, size_t *char_offset, int *actual_x)
{
    (void)style;
    if (char_offset == NULL || actual_x == NULL) return NSERROR_BAD_PARAMETER;
    size_t off = 0;
    int px = 0;
    if (x < 0) x = 0;
    while (off < length) {
        size_t step = 0;
        int advance = cos_ns_layout_advance(string + off, length - off, &step);
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
    nserror err = cos_ns_layout_position(style, string, length, x, char_offset, actual_x);
    if (err != NSERROR_OK) return err;
    if (*char_offset == 0 && length != 0) {
        size_t step = 0;
        int advance = cos_ns_layout_advance(string, length, &step);
        *char_offset = step;
        *actual_x = advance;
    }
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

static nserror cos_ns_plot_line(const struct redraw_context *ctx,
                                const plot_style_t *s, const struct rect *r)
{
    (void)ctx;
    if (s == NULL || r == NULL || s->stroke_type == PLOT_OP_TYPE_NONE) {
        return NSERROR_OK;
    }
    int width = plot_style_fixed_to_int(s->stroke_width);
    if (width < 1) width = 1;
    vga_draw_line_thick(r->x0, r->y0, r->x1, r->y1, width,
                        cos_ns_plot_colour(s->stroke_colour));
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
        vga_draw_rect(x, y, w, h, cos_ns_plot_colour(s->stroke_colour));
    }
    return NSERROR_OK;
}

static nserror cos_ns_plot_polygon(const struct redraw_context *ctx,
                                   const plot_style_t *s, const int *p,
                                   unsigned int n)
{
    (void)ctx;
    if (s == NULL || p == NULL || n < 2) return NSERROR_OK;
    /* CSS arrows and small widgets use polygons. Drawing the edge preserves
     * their affordance even before an accelerated winding-rule rasteriser is
     * introduced. */
    if (s->stroke_type != PLOT_OP_TYPE_NONE || s->fill_type != PLOT_OP_TYPE_NONE) {
        uint64_t c = cos_ns_plot_colour((s->stroke_type != PLOT_OP_TYPE_NONE)
                                        ? s->stroke_colour : s->fill_colour);
        for (unsigned int i = 0; i < n; i++) {
            unsigned int j = (i + 1u) % n;
            vga_draw_line(p[i * 2u], p[i * 2u + 1u],
                          p[j * 2u], p[j * 2u + 1u], c);
        }
    }
    return NSERROR_OK;
}

/* Keep vector path work bounded: a malformed remote SVG must not allocate or
 * monopolise the GUI owner. Curves are flattened into short line segments and
 * each closed subpath is scanline-filled with the normal even-odd rule. */
#define COS_NS_PATH_MAX_POINTS 192u
#define COS_NS_PATH_BEZIER_STEPS 8u

static int cos_ns_path_round(float value)
{
    return (int)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static void cos_ns_path_transform_point(const float transform[6], float x,
                                        float y, int *out_x, int *out_y)
{
    if (transform == NULL) {
        *out_x = cos_ns_path_round(x);
        *out_y = cos_ns_path_round(y);
        return;
    }
    *out_x = cos_ns_path_round(transform[0] * x + transform[2] * y + transform[4]);
    *out_y = cos_ns_path_round(transform[1] * x + transform[3] * y + transform[5]);
}

static void cos_ns_path_append(int points[COS_NS_PATH_MAX_POINTS][2],
                               unsigned int *count, int x, int y)
{
    if (*count != 0u && points[*count - 1u][0] == x &&
        points[*count - 1u][1] == y) {
        return;
    }
    if (*count < COS_NS_PATH_MAX_POINTS) {
        points[*count][0] = x;
        points[*count][1] = y;
        ++(*count);
    }
}

static void cos_ns_path_render_subpath(const plot_style_t *s,
                                       int points[COS_NS_PATH_MAX_POINTS][2],
                                       unsigned int count, bool closed)
{
    if (s == NULL || count < 2u) return;

    if (closed && count >= 3u && s->fill_type != PLOT_OP_TYPE_NONE) {
        int min_y = points[0][1];
        int max_y = points[0][1];
        for (unsigned int i = 1; i < count; ++i) {
            if (points[i][1] < min_y) min_y = points[i][1];
            if (points[i][1] > max_y) max_y = points[i][1];
        }
        if (cos_ns_clip_active) {
            if (min_y < cos_ns_clip_y0) min_y = cos_ns_clip_y0;
            if (max_y >= cos_ns_clip_y1) max_y = cos_ns_clip_y1 - 1;
        }
        const uint64_t fill = cos_ns_plot_colour(s->fill_colour);
        for (int y = min_y; y <= max_y; ++y) {
            int intersections[COS_NS_PATH_MAX_POINTS];
            unsigned int intersections_count = 0;
            for (unsigned int i = 0; i < count; ++i) {
                unsigned int j = (i + 1u) % count;
                int y0 = points[i][1], y1 = points[j][1];
                if (((y0 <= y) && (y1 > y)) || ((y1 <= y) && (y0 > y))) {
                    if (intersections_count < COS_NS_PATH_MAX_POINTS) {
                        intersections[intersections_count++] = points[i][0] +
                            (int)(((int64_t)(y - y0) * (points[j][0] - points[i][0])) /
                                  (int64_t)(y1 - y0));
                    }
                }
            }
            for (unsigned int i = 1; i < intersections_count; ++i) {
                int value = intersections[i];
                unsigned int j = i;
                while (j > 0u && intersections[j - 1u] > value) {
                    intersections[j] = intersections[j - 1u];
                    --j;
                }
                intersections[j] = value;
            }
            for (unsigned int i = 0; i + 1u < intersections_count; i += 2u) {
                int x0 = intersections[i];
                int x1 = intersections[i + 1u];
                if (cos_ns_clip_active) {
                    if (x0 < cos_ns_clip_x0) x0 = cos_ns_clip_x0;
                    if (x1 >= cos_ns_clip_x1) x1 = cos_ns_clip_x1 - 1;
                }
                if (x1 >= x0) vga_fill_rect(x0, y, x1 - x0 + 1, 1, fill);
            }
        }
    }

    if (s->stroke_type != PLOT_OP_TYPE_NONE) {
        int width = plot_style_fixed_to_int(s->stroke_width);
        if (width < 1) width = 1;
        const uint64_t stroke = cos_ns_plot_colour(s->stroke_colour);
        for (unsigned int i = 1; i < count; ++i) {
            vga_draw_line_thick(points[i - 1u][0], points[i - 1u][1],
                                points[i][0], points[i][1], width, stroke);
        }
        if (closed) {
            vga_draw_line_thick(points[count - 1u][0], points[count - 1u][1],
                                points[0][0], points[0][1], width, stroke);
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

static nserror cos_ns_plot_text(const struct redraw_context *ctx,
                                const plot_font_style_t *s, int x, int y,
                                const char *text, size_t n)
{
    (void)ctx;
    ++cos_ns_plot_text_count;
    if (s == NULL || text == NULL || n == 0) return NSERROR_OK;
    uint64_t foreground = cos_ns_plot_colour(s->foreground);
    uint64_t background = cos_ns_plot_colour(s->background);
    if (cos_ns_text_plot_diag_remaining != 0) {
        --cos_ns_text_plot_diag_remaining;
        serial_puts("[NSTEXT] x="); serial_putdec((uint64_t)(int64_t)x);
        serial_puts(" y="); serial_putdec((uint64_t)(int64_t)y);
        serial_puts(" fg=0x"); serial_puthex(foreground);
        serial_puts(" bg=0x"); serial_puthex(background);
        serial_puts(" n="); serial_putdec((uint64_t)n);
        serial_puts("\n");
    }
    if (cos_ns_clip_active && (y < cos_ns_clip_y0 || y - FONT_H >= cos_ns_clip_y1 ||
        x >= cos_ns_clip_x1)) return NSERROR_OK;
    vga_draw_string_len(x, y - FONT_H, text, (int)n, foreground, background);
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
