/* cos_ui.c - see cos_ui.h.
 *
 * Anti-aliasing: every shape is described by a signed distance d from the
 * pixel centre to its edge (negative inside); coverage = clamp(0.5 - d, 0, 1).
 * One evaluation per pixel gives smooth 1-px edges without supersampling,
 * and interior pixels take a fast solid-fill path.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cos_ui.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb_truetype.h"

extern const unsigned char cos_ui_font_sans[];
extern const unsigned char cos_ui_font_sans_bold[];

/* ------------------------------------------------------------------ */
/* canvas / colour                                                     */
/* ------------------------------------------------------------------ */
void cui_canvas_init(cui_canvas *c, uint32_t *px, int w, int h, int stride) {
    c->px = px; c->w = w; c->h = h; c->stride = stride;
    cui_unclip(c);
}
void cui_unclip(cui_canvas *c) { c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h; }
void cui_clip(cui_canvas *c, int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    if (x > c->cx0) c->cx0 = x;
    if (y > c->cy0) c->cy0 = y;
    if (x1 < c->cx1) c->cx1 = x1;
    if (y1 < c->cy1) c->cy1 = y1;
    if (c->cx1 < c->cx0) c->cx1 = c->cx0;
    if (c->cy1 < c->cy0) c->cy1 = c->cy0;
}

uint32_t cui_mix(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a;
    if (t >= 255) return b;
    /* Per channel. Blending the packed R|B pair and then dividing the
     * whole word by 255 leaked the red remainder into blue (text got
     * yellow-green fringes, gradients banded). */
    uint32_t it = (uint32_t)(255 - t), tt = (uint32_t)t;
    uint32_t r = (((a >> 16) & 0xFFu) * it + ((b >> 16) & 0xFFu) * tt + 127u) / 255u;
    uint32_t g = (((a >> 8) & 0xFFu) * it + ((b >> 8) & 0xFFu) * tt + 127u) / 255u;
    uint32_t bl = ((a & 0xFFu) * it + (b & 0xFFu) * tt + 127u) / 255u;
    return (r << 16) | (g << 8) | bl;
}

static inline void plot(cui_canvas *c, int x, int y, uint32_t color, int a) {
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1 || a <= 0) return;
    uint32_t *p = &c->px[y * c->stride + x];
    *p = a >= 255 ? color : cui_mix(*p, color, a);
}

static bool clip_box(const cui_canvas *c, int *x, int *y, int *w, int *h) {
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < c->cx0) x0 = c->cx0;
    if (y0 < c->cy0) y0 = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}

void cui_fill(cui_canvas *c, int x, int y, int w, int h, uint32_t color) {
    if (!clip_box(c, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; ++j) {
        uint32_t *row = &c->px[(y + j) * c->stride + x];
        for (int i = 0; i < w; ++i) row[i] = color;
    }
}
void cui_fill_alpha(cui_canvas *c, int x, int y, int w, int h, uint32_t color, int alpha) {
    if (alpha >= 255) { cui_fill(c, x, y, w, h, color); return; }
    if (alpha <= 0 || !clip_box(c, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; ++j) {
        uint32_t *row = &c->px[(y + j) * c->stride + x];
        for (int i = 0; i < w; ++i) row[i] = cui_mix(row[i], color, alpha);
    }
}
void cui_vgradient(cui_canvas *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    if (h <= 0) return;
    for (int j = 0; j < h; ++j) cui_fill(c, x, y + j, w, 1, cui_mix(top, bottom, h > 1 ? j * 255 / (h - 1) : 0));
}
void cui_blit(cui_canvas *c, int x, int y, const uint32_t *src, int w, int h, int src_stride) {
    int bx = x, by = y, bw = w, bh = h;
    if (!clip_box(c, &bx, &by, &bw, &bh)) return;
    for (int j = 0; j < bh; ++j)
        memcpy(&c->px[(by + j) * c->stride + bx], &src[(by - y + j) * src_stride + (bx - x)], (size_t)bw * 4);
}

/* ------------------------------------------------------------------ */
/* anti-aliased shapes                                                 */
/* ------------------------------------------------------------------ */
static inline int cov(float d) {           /* signed distance -> 0..255 coverage */
    float a = 0.5f - d;
    if (a <= 0.0f) return 0;
    if (a >= 1.0f) return 255;
    return (int)(a * 255.0f + 0.5f);
}

/* Signed distance from p to a rounded rectangle (x,y,w,h,r). */
static float sd_round_rect(float px, float py, float x, float y, float w, float h, float r) {
    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    float qx = fabsf(px - cx) - (w * 0.5f - r);
    float qy = fabsf(py - cy) - (h * 0.5f - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float outside = sqrtf(ox * ox + oy * oy);
    float inside = qx > qy ? qx : qy;
    if (inside > 0) inside = 0;
    return outside + inside - r;
}

static void round_rect_impl(cui_canvas *c, int x, int y, int w, int h, int r, uint32_t color, int alpha) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    int bx = x, by = y, bw = w, bh = h;
    if (!clip_box(c, &bx, &by, &bw, &bh)) return;
    for (int j = by; j < by + bh; ++j) {
        bool corner_row = (j < y + r) || (j >= y + h - r);
        for (int i = bx; i < bx + bw; ++i) {
            int a;
            if (!corner_row && i >= x && i < x + w) a = 255;
            else if (corner_row && i >= x + r && i < x + w - r) a = 255;
            else a = cov(sd_round_rect((float)i + 0.5f, (float)j + 0.5f, (float)x, (float)y, (float)w, (float)h, (float)r));
            if (a) {
                uint32_t *p = &c->px[j * c->stride + i];
                int aa = alpha >= 255 ? a : a * alpha / 255;
                *p = aa >= 255 ? color : cui_mix(*p, color, aa);
            }
        }
    }
}
void cui_round_rect(cui_canvas *c, int x, int y, int w, int h, int r, uint32_t color) { round_rect_impl(c, x, y, w, h, r, color, 255); }
void cui_round_rect_alpha(cui_canvas *c, int x, int y, int w, int h, int r, uint32_t color, int alpha) { round_rect_impl(c, x, y, w, h, r, color, alpha); }

void cui_round_rect_outline(cui_canvas *c, int x, int y, int w, int h, int r, float t, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int bx = x, by = y, bw = w, bh = h;
    if (!clip_box(c, &bx, &by, &bw, &bh)) return;
    for (int j = by; j < by + bh; ++j)
        for (int i = bx; i < bx + bw; ++i) {
            float d = sd_round_rect((float)i + 0.5f, (float)j + 0.5f, (float)x, (float)y, (float)w, (float)h, (float)r);
            float band = fabsf(d + t * 0.5f) - t * 0.5f;     /* ring of thickness t just inside the edge */
            plot(c, i, j, color, cov(band));
        }
}

void cui_circle(cui_canvas *c, float cx, float cy, float r, uint32_t color) {
    int x0 = (int)floorf(cx - r - 1), y0 = (int)floorf(cy - r - 1);
    int x1 = (int)ceilf(cx + r + 1), y1 = (int)ceilf(cy + r + 1);
    for (int j = y0; j < y1; ++j)
        for (int i = x0; i < x1; ++i) {
            float dx = (float)i + 0.5f - cx, dy = (float)j + 0.5f - cy;
            plot(c, i, j, color, cov(sqrtf(dx * dx + dy * dy) - r));
        }
}
void cui_ring(cui_canvas *c, float cx, float cy, float r, float t, uint32_t color) {
    int x0 = (int)floorf(cx - r - 1), y0 = (int)floorf(cy - r - 1);
    int x1 = (int)ceilf(cx + r + 1), y1 = (int)ceilf(cy + r + 1);
    for (int j = y0; j < y1; ++j)
        for (int i = x0; i < x1; ++i) {
            float dx = (float)i + 0.5f - cx, dy = (float)j + 0.5f - cy;
            float d = fabsf(sqrtf(dx * dx + dy * dy) - (r - t * 0.5f)) - t * 0.5f;
            plot(c, i, j, color, cov(d));
        }
}

void cui_line(cui_canvas *c, float x0, float y0, float x1, float y1, float width, uint32_t color) {
    float hw = width * 0.5f;
    int bx0 = (int)floorf(fminf(x0, x1) - hw - 1), by0 = (int)floorf(fminf(y0, y1) - hw - 1);
    int bx1 = (int)ceilf(fmaxf(x0, x1) + hw + 1), by1 = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    for (int j = by0; j < by1; ++j)
        for (int i = bx0; i < bx1; ++i) {
            float px = (float)i + 0.5f - x0, py = (float)j + 0.5f - y0;
            float t = len2 > 0 ? (px * vx + py * vy) / len2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            float dx = px - vx * t, dy = py - vy * t;
            plot(c, i, j, color, cov(sqrtf(dx * dx + dy * dy) - hw));
        }
}

static float edge_dist(float px, float py, float ax, float ay, float bx, float by) {
    /* signed distance to the line through a->b (positive on the right) */
    float ex = bx - ax, ey = by - ay, l = sqrtf(ex * ex + ey * ey);
    if (l == 0) return 0;
    return ((px - ax) * ey - (py - ay) * ex) / l;
}
void cui_triangle(cui_canvas *c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color) {
    /* Orient so "inside" is negative for all three edges; the distance to the triangle is the max of the
     * three and cov() takes a signed distance (negative inside). This used to call cov(-d), which made
     * deep-inside coverage 0 and far-outside 255: the bounding box was filled EXCEPT the triangle. */
    if ((x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0) < 0) { float tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; }
    int bx0 = (int)floorf(fminf(x0, fminf(x1, x2))) - 1, by0 = (int)floorf(fminf(y0, fminf(y1, y2))) - 1;
    int bx1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2))) + 1, by1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2))) + 1;
    for (int j = by0; j < by1; ++j)
        for (int i = bx0; i < bx1; ++i) {
            float px = (float)i + 0.5f, py = (float)j + 0.5f;
            float d = edge_dist(px, py, x0, y0, x1, y1);
            float d2 = edge_dist(px, py, x1, y1, x2, y2);
            float d3 = edge_dist(px, py, x2, y2, x0, y0);
            if (d2 > d) d = d2;
            if (d3 > d) d = d3;
            plot(c, i, j, color, cov(d));
        }
}

/* ------------------------------------------------------------------ */
/* text                                                                */
/* ------------------------------------------------------------------ */
#define GLYPH_CACHE 256
typedef struct {
    uint32_t cp;
    bool used;
    int w, h, xoff, yoff, advance;
    unsigned char *bmp;
} glyph_t;

struct cui_font {
    stbtt_fontinfo info;
    const void *data;
    int px;
    float scale;
    int ascent, descent, line_gap, height;
    struct cui_font *fallback;     /* used for code points this font has no glyph for */
    glyph_t cache[GLYPH_CACHE];
};

#define FONT_SLOTS 16
static cui_font *s_fonts[FONT_SLOTS];

cui_font *cui_font_load(const void *ttf, int px) {
    if (!ttf || px < 4) return NULL;
    for (int i = 0; i < FONT_SLOTS; ++i)
        if (s_fonts[i] && s_fonts[i]->data == ttf && s_fonts[i]->px == px) return s_fonts[i];
    cui_font *f = (cui_font *)calloc(1, sizeof(cui_font));
    if (!f) return NULL;
    if (!stbtt_InitFont(&f->info, (const unsigned char *)ttf, stbtt_GetFontOffsetForIndex((const unsigned char *)ttf, 0))) {
        free(f);
        return NULL;
    }
    f->data = ttf;
    f->px = px;
    f->scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
    int a, d, g;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
    f->ascent = (int)ceilf(a * f->scale);
    f->descent = (int)floorf(d * f->scale);
    f->line_gap = (int)(g * f->scale);
    f->height = f->ascent - f->descent + f->line_gap;
    for (int i = 0; i < FONT_SLOTS; ++i) if (!s_fonts[i]) { s_fonts[i] = f; break; }
    return f;
}
cui_font *cui_font_default(int px) { return cui_font_load(cos_ui_font_sans, px); }
cui_font *cui_font_bold(int px) { return cui_font_load(cos_ui_font_sans_bold, px); }
int cui_font_height(const cui_font *f) { return f ? f->height : 0; }
int cui_font_ascent(const cui_font *f) { return f ? f->ascent : 0; }

static glyph_t *glyph_get(cui_font *f, uint32_t cp) {
    glyph_t *g = &f->cache[cp % GLYPH_CACHE];
    if (g->used && g->cp == cp) return g;
    if (g->used && g->bmp) stbtt_FreeBitmap(g->bmp, NULL);
    memset(g, 0, sizeof(*g));
    g->cp = cp;
    g->used = true;
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&f->info, (int)cp, &adv, &lsb);
    g->advance = (int)lroundf(adv * f->scale);
    g->bmp = stbtt_GetCodepointBitmap(&f->info, 0, f->scale, (int)cp, &g->w, &g->h, &g->xoff, &g->yoff);
    return g;
}

void cui_font_set_fallback(cui_font *f, cui_font *fallback) { if (f && f != fallback) f->fallback = fallback; }

/* the glyph for cp, taken from the fallback chain when f itself has none (glyph index 0) */
static glyph_t *glyph_get_fb(cui_font *f, cui_font **owner, uint32_t cp) {
    for (cui_font *q = f; q; q = q->fallback) {
        if (stbtt_FindGlyphIndex(&q->info, (int)cp) != 0 || !q->fallback) { *owner = q; return glyph_get(q, cp); }
    }
    *owner = f;
    return glyph_get(f, cp);
}

static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u); p += 3; }
    else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63u) << 6) | (p[1] & 63u); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63u); p += 1; }
    *s = (const char *)p;
    return c;
}

static int text_impl(cui_canvas *c, cui_font *f, int x, int y, const char *s, uint32_t color, bool draw, int max_w) {
    if (!f || !s) return 0;
    int pen = 0;
    uint32_t prev = 0;
    while (*s) {
        const char *before = s;
        uint32_t cp = utf8_next(&s);
        cui_font *owner;
        glyph_t *g = glyph_get_fb(f, &owner, cp);
        if (prev && owner == f) pen += (int)lroundf(stbtt_GetCodepointKernAdvance(&f->info, (int)prev, (int)cp) * f->scale);
        if (max_w >= 0 && pen + g->advance > max_w) { s = before; break; }
        if (draw && g->bmp) {
            int gx = x + pen + g->xoff, gy = y + f->ascent + g->yoff;
            for (int j = 0; j < g->h; ++j)
                for (int i = 0; i < g->w; ++i) {
                    int a = g->bmp[j * g->w + i];
                    if (a) plot(c, gx + i, gy + j, color, a);
                }
        }
        pen += g->advance;
        prev = cp;
    }
    return pen;
}

int cui_text(cui_canvas *c, cui_font *f, int x, int y, const char *s, uint32_t color) { return text_impl(c, f, x, y, s, color, true, -1); }
int cui_text_width(cui_font *f, const char *s) { return text_impl(NULL, f, 0, 0, s, 0, false, -1); }

int cui_text_fit(cui_canvas *c, cui_font *f, int x, int y, int max_w, const char *s, uint32_t color) {
    if (cui_text_width(f, s) <= max_w) return cui_text(c, f, x, y, s, color);
    int ell = cui_text_width(f, "...");
    int w = text_impl(c, f, x, y, s, color, true, max_w - ell > 0 ? max_w - ell : 0);
    return w + cui_text(c, f, x + w, y, "...", color);
}

void cui_text_center(cui_canvas *c, cui_font *f, int x, int y, int w, int h, const char *s, uint32_t color) {
    int tw = cui_text_width(f, s);
    cui_text(c, f, x + (w - tw) / 2, y + (h - (f ? f->ascent - f->descent : 0)) / 2, s, color);
}
