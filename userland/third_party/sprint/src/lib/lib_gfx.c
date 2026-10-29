/* gfx: software-rendered canvas (SPEC 15.13).
 *
 * A canvas is a native object holding w*h pixels (0x00RRGGBB). All drawing is
 * integer-only and deterministic; there is no hidden state beyond the canvas'
 * own alpha. Output goes to files (PNG / PPM / BMP) through the fs capability
 * (--allow-write), so the module needs no windowing library. */
#define _POSIX_C_SOURCE 200809L
#include "sp.h"
#include "font_ja.h"
#include <errno.h>
#include <limits.h>
#include <unistd.h>

typedef struct Canvas {
    int w, h;
    int alpha; /* 0..255, applies to every drawing call */
    int cx0, cy0, cx1, cy1; /* clip box [cx0,cx1) x [cy0,cy1), initially the whole canvas */
    uint32_t px[];
} Canvas;

static const NativeClass canvas_class;

enum { MAX_PIXELS = 16 * 1024 * 1024, MAX_DIM = 16384 };

/* ---------------------------------------------------------------- */
/* argument helpers                                                  */
/* ---------------------------------------------------------------- */
static int a_int(VM *vm, SpValue v, const char *what, int64_t *out) {
    if (v.type != T_INT)
        return vm_set_error(vm, E_TYPE, "%s must be an int, not %s (use // for integer division)", what, type_name(v));
    *out = v.as.i;
    return 0;
}

static int a_coord(VM *vm, SpValue v, const char *what, int *out) {
    int64_t x;
    if (a_int(vm, v, what, &x)) return 1;
    if (x < -(1 << 28) || x > (1 << 28)) return vm_set_error(vm, E_VALUE, "%s out of range", what);
    *out = (int)x;
    return 0;
}

static int a_color(VM *vm, SpValue v, uint32_t *out) {
    int64_t x;
    if (a_int(vm, v, "color", &x)) return 1;
    if (x < 0 || x > 0xFFFFFF) return vm_set_error(vm, E_VALUE, "color must be in 0..0xFFFFFF (see gfx.rgb)");
    *out = (uint32_t)x;
    return 0;
}

static Canvas *self_canvas(VM *vm, SpValue v) {
    if (v.type != T_NATIVE || AS_NATIVE(v)->cls != &canvas_class) {
        (void)vm_set_error(vm, E_TYPE, "expected a canvas, not %s", type_name(v));
        return NULL;
    }
    return (Canvas *)AS_NATIVE(v)->data;
}

/* ---------------------------------------------------------------- */
/* pixel primitives                                                  */
/* ---------------------------------------------------------------- */
/* (v + 127) / 255 for v <= 255 * 255 + 127, as a multiply and a shift (checked exhaustively) */
static inline uint32_t div255r(uint32_t v) { return (uint32_t)(((uint64_t)(v + 127) * 0x80808081ull) >> 39); }

static inline uint32_t blend_px(uint32_t col, uint32_t d, uint32_t a) {
    uint32_t ia = 255 - a;
    uint32_t r = div255r(((col >> 16) & 255) * a + ((d >> 16) & 255) * ia) << 16;
    r |= div255r(((col >> 8) & 255) * a + ((d >> 8) & 255) * ia) << 8;
    r |= div255r((col & 255) * a + (d & 255) * ia);
    return r;
}

static inline void put_px(Canvas *c, int x, int y, uint32_t col) {
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1) return;
    uint32_t *p = &c->px[(size_t)y * (size_t)c->w + (size_t)x];
    if (c->alpha == 255) { *p = col; return; }
    *p = blend_px(col, *p, (uint32_t)c->alpha);
}

/* fill n pixels with col: 64-bit stores, four per iteration */
static void fill32(uint32_t *p, uint32_t col, size_t n) {
    if (n < 8) {
        for (size_t i = 0; i < n; i++) p[i] = col;
        return;
    }
    uint64_t v = ((uint64_t)col << 32) | col;
    if (((uintptr_t)p & 4) != 0) { *p++ = col; n--; }
    while (n >= 8) {
        memcpy(p, &v, 8);
        memcpy(p + 2, &v, 8);
        memcpy(p + 4, &v, 8);
        memcpy(p + 6, &v, 8);
        p += 8;
        n -= 8;
    }
    while (n > 0) { *p++ = col; n--; }
}

static void hline(Canvas *c, int x0, int x1, int y, uint32_t col) {
    if (y < c->cy0 || y >= c->cy1) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x1 < c->cx0 || x0 >= c->cx1) return;
    if (x0 < c->cx0) x0 = c->cx0;
    if (x1 >= c->cx1) x1 = c->cx1 - 1;
    if (c->alpha == 255) {
        fill32(&c->px[(size_t)y * (size_t)c->w + (size_t)x0], col, (size_t)(x1 - x0 + 1));
    } else {
        uint32_t *p = &c->px[(size_t)y * (size_t)c->w];
        uint32_t a = (uint32_t)c->alpha;
        for (int x = x0; x <= x1; x++) p[x] = blend_px(col, p[x], a);
    }
}

/* blend `col` with coverage cov (0..255) on top of the canvas alpha */
static void put_cov(Canvas *c, int x, int y, uint32_t col, int cov) {
    if (cov <= 0) return;
    int saved = c->alpha;
    c->alpha = (saved * cov + 127) / 255;
    if (c->alpha > 0) put_px(c, x, y, col);
    c->alpha = saved;
}

/* Wu's anti-aliased line, 16.16 fixed point, integer only */
static void line_aa_px(Canvas *c, int x0, int y0, int x1, int y1, uint32_t col) {
    int steep = (y1 > y0 ? y1 - y0 : y0 - y1) > (x1 > x0 ? x1 - x0 : x0 - x1);
    if (steep) { int t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    int64_t dx = x1 - x0, dy = y1 - y0;
    int64_t grad = dx == 0 ? 65536 : (dy * 65536) / dx;
    int64_t y = (int64_t)y0 * 65536;
    for (int x = x0; x <= x1; x++) {
        int iy = (int)(y >> 16);
        int f = (int)((y & 65535) >> 8); /* 0..255 coverage of the lower pixel */
        if (steep) { put_cov(c, iy, x, col, 255 - f); put_cov(c, iy + 1, x, col, f); }
        else { put_cov(c, x, iy, col, 255 - f); put_cov(c, x, iy + 1, col, f); }
        y += grad;
    }
}

static int isqrt_i(int64_t v) {
    if (v <= 0) return 0;
    int64_t r = 0, b = (int64_t)1 << 40;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (int)r;
}

/* horizontal inset of row i (0 = outermost) of a rounded corner with radius r */
static int rr_inset(int r, int i) {
    int64_t dy2 = 2 * (int64_t)r - 2 * i - 1; /* twice the distance from the row centre to the arc centre */
    int hw = (isqrt_i(4 * (int64_t)r * r - dy2 * dy2) + 1) / 2;
    return r - hw;
}

static void round_rect_px(Canvas *c, int x, int y, int w, int h, int r, uint32_t col, int fill) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    for (int i = 0; i < h; i++) {
        int k = i < r ? i : (i >= h - r ? h - 1 - i : r);
        int ins = k < r ? rr_inset(r, k) : 0;
        if (fill || i == 0 || i == h - 1) {
            hline(c, x + ins, x + w - 1 - ins, y + i, col);
        } else {
            int pin = (k < r && k > 0) ? rr_inset(r, k - 1) : ins; /* previous row's inset */
            int span = pin > ins ? pin - 1 : ins;
            hline(c, x + ins, x + span, y + i, col);
            hline(c, x + w - 1 - span, x + w - 1 - ins, y + i, col);
        }
    }
}

static void line_px(Canvas *c, int x0, int y0, int x1, int y1, uint32_t col) {
    int64_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int64_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int64_t err = dx + dy;
    for (;;) {
        put_px(c, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int64_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += (int)sx; }
        if (e2 <= dx) { err += dx; y0 += (int)sy; }
    }
}

static void circle_px(Canvas *c, int cx, int cy, int r, uint32_t col, int fill) {
    if (r < 0) return;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        if (fill) {
            hline(c, cx - x, cx + x, cy + y, col);
            if (y) hline(c, cx - x, cx + x, cy - y, col);
            if (x != y) {
                hline(c, cx - y, cx + y, cy + x, col);
                hline(c, cx - y, cx + y, cy - x, col);
            }
        } else {
            put_px(c, cx + x, cy + y, col); put_px(c, cx - x, cy + y, col);
            put_px(c, cx + x, cy - y, col); put_px(c, cx - x, cy - y, col);
            put_px(c, cx + y, cy + x, col); put_px(c, cx - y, cy + x, col);
            put_px(c, cx + y, cy - x, col); put_px(c, cx - y, cy - x, col);
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static void tri_px(Canvas *c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col) {
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= c->w) maxx = c->w - 1;
    if (maxy >= c->h) maxy = c->h - 1;
    int64_t area = (int64_t)(x1 - x0) * (y2 - y0) - (int64_t)(y1 - y0) * (x2 - x0);
    if (area == 0) return;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            int64_t w0 = (int64_t)(x1 - x0) * (y - y0) - (int64_t)(y1 - y0) * (x - x0);
            int64_t w1 = (int64_t)(x2 - x1) * (y - y1) - (int64_t)(y2 - y1) * (x - x1);
            int64_t w2 = (int64_t)(x0 - x2) * (y - y2) - (int64_t)(y0 - y2) * (x - x2);
            int in = area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (in) put_px(c, x, y, col);
        }
}

/* ---------------------------------------------------------------- */
/* 5x7 font (ASCII 0x20..0x7E); column bytes, bit0 = top row          */
/* ---------------------------------------------------------------- */
static const uint8_t font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x02,0x01,0x02,0x04,0x02}};

static const uint8_t *glyph_for(uint32_t cp) {
    return font5x7[(cp >= 32 && cp <= 126) ? cp - 32 : '?' - 32];
}

/* decode one UTF-8 code point (input is validated string data) */
static uint32_t next_cp(const unsigned char *s, size_t n, size_t *i) {
    unsigned char c = s[*i];
    if (c < 0x80 || *i + 1 >= n) { (*i)++; return c; }
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
    uint32_t cp = c & (0xFFu >> (len + 1));
    for (int k = 1; k < len && *i + (size_t)k < n; k++) cp = (cp << 6) | (s[*i + (size_t)k] & 0x3Fu);
    *i += (size_t)len;
    return cp;
}

/* 12x12 glyph of a Japanese code point (kana, punctuation, full-width forms, JIS X 0208 kanji), or NULL */
static const uint8_t *ja_glyph(uint32_t cp) {
    if (cp < 0x80 || cp > 0xFFFF) return NULL;
    int lo = 0, hi = font_ja_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t m = font_ja_code[mid];
        if (m == cp) return font_ja_bits[mid];
        if (m < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* advance of one code point in unscaled pixels: 6 for the 5x7 Latin font, 12 for a Japanese glyph */
static int cp_advance(uint32_t cp) { return ja_glyph(cp) ? 12 : 6; }

static void draw_text(Canvas *c, int x, int y, const SString *s, uint32_t col, int scale) {
    const unsigned char *d = (const unsigned char *)s->data;
    size_t i = 0, n = s->len;
    int cx = x;
    while (i < n) {
        uint32_t cp = next_cp(d, n, &i);
        if (cp == '\n') { cx = x; y += 8 * scale; continue; }
        const uint8_t *j = ja_glyph(cp);
        if (j) {
            /* the glyph cell is 12 rows; its ink is centred on the 8-row line of the Latin font */
            for (int gy = 0; gy < 12; gy++)
                for (int gx = 0; gx < 12; gx++) {
                    unsigned b = (unsigned)(gy * 12 + gx);
                    if (j[b >> 3] & (1u << (b & 7)))
                        for (int sy = 0; sy < scale; sy++)
                            for (int sx = 0; sx < scale; sx++)
                                put_px(c, cx + gx * scale + sx, y + (gy - 3) * scale + sy, col);
                }
            cx += 12 * scale;
            continue;
        }
        const uint8_t *g = glyph_for(cp);
        if (cx + 5 * scale <= c->cx0 || cx >= c->cx1 || y + 7 * scale <= c->cy0 || y >= c->cy1) { cx += 6 * scale; continue; }
        if (cx >= c->cx0 && cx + 5 * scale <= c->cx1 && y >= c->cy0 && y + 7 * scale <= c->cy1) {
            /* the whole cell is inside the clip: no per-pixel tests */
            for (int gx = 0; gx < 5; gx++)
                for (int gy = 0; gy < 7; gy++)
                    if (g[gx] & (1u << gy))
                        for (int sy = 0; sy < scale; sy++) {
                            uint32_t *row = &c->px[(size_t)(y + gy * scale + sy) * (size_t)c->w + (size_t)(cx + gx * scale)];
                            if (c->alpha == 255) {
                                for (int sx = 0; sx < scale; sx++) row[sx] = col;
                            } else {
                                for (int sx = 0; sx < scale; sx++) row[sx] = blend_px(col, row[sx], (uint32_t)c->alpha);
                            }
                        }
            cx += 6 * scale;
            continue;
        }
        for (int gx = 0; gx < 5; gx++)
            for (int gy = 0; gy < 7; gy++)
                if (g[gx] & (1u << gy))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            put_px(c, cx + gx * scale + sx, y + gy * scale + sy, col);
        cx += 6 * scale;
    }
}

/* total advance of the widest line, in pixels (a Japanese glyph is 12 wide, other characters 6) */
static int64_t text_advance(const SString *s, int scale) {
    const unsigned char *d = (const unsigned char *)s->data;
    size_t i = 0, n = s->len;
    int64_t cur = 0, best = 0;
    while (i < n) {
        uint32_t cp = next_cp(d, n, &i);
        if (cp == '\n') { cur = 0; continue; }
        cur += (int64_t)cp_advance(cp) * scale;
        if (cur > best) best = cur;
    }
    return best;
}

static int64_t text_width(const SString *s, int scale) {
    int64_t a = text_advance(s, scale);
    return a > 0 ? a - scale : 0;
}

/* ---------------------------------------------------------------- */
/* image encoders                                                    */
/* ---------------------------------------------------------------- */
typedef struct Buf { uint8_t *d; size_t n, cap; int oom; } Buf;

static void buf_put(Buf *b, const void *p, size_t n) {
    if (b->oom) return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->n + n) nc *= 2;
        uint8_t *nd = realloc(b->d, nc);
        if (!nd) { b->oom = 1; return; }
        b->d = nd;
        b->cap = nc;
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void buf_u8(Buf *b, unsigned v) { uint8_t c = (uint8_t)v; buf_put(b, &c, 1); }
static void buf_be32(Buf *b, uint32_t v) { buf_u8(b, v >> 24); buf_u8(b, v >> 16); buf_u8(b, v >> 8); buf_u8(b, v); }
static void buf_le16(Buf *b, uint32_t v) { buf_u8(b, v); buf_u8(b, v >> 8); }
static void buf_le32(Buf *b, uint32_t v) { buf_le16(b, v & 0xFFFF); buf_le16(b, v >> 16); }

static const uint32_t crc_nib[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};
static uint32_t crc32_of(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        crc = crc_nib[crc & 15] ^ (crc >> 4);
        crc = crc_nib[crc & 15] ^ (crc >> 4);
    }
    return ~crc;
}

/* --- deflate with the fixed Huffman code and a small LZ77 matcher --- */
typedef struct BitW { Buf *b; uint32_t acc; int nb; } BitW;
static void put_bits(BitW *w, uint32_t v, int n) {
    w->acc |= v << w->nb;
    w->nb += n;
    while (w->nb >= 8) { buf_u8(w->b, w->acc & 255); w->acc >>= 8; w->nb -= 8; }
}
static void put_code(BitW *w, uint32_t code, int len) { /* Huffman codes are MSB-first */
    uint32_t r = 0;
    for (int i = 0; i < len; i++) r |= ((code >> i) & 1u) << (len - 1 - i);
    put_bits(w, r, len);
}
static void put_lit(BitW *w, unsigned s) {
    if (s <= 143) put_code(w, 0x30 + s, 8);
    else if (s <= 255) put_code(w, 0x190 + (s - 144), 9);
    else if (s <= 279) put_code(w, s - 256, 7);
    else put_code(w, 0xC0 + (s - 280), 8);
}
static const uint16_t len_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t len_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t dist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t dist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static void put_match(BitW *w, unsigned len, unsigned dist) {
    int li = 28;
    while (len_base[li] > len) li--;
    put_lit(w, 257 + (unsigned)li);
    if (len_extra[li]) put_bits(w, len - len_base[li], len_extra[li]);
    int di = 29;
    while (dist_base[di] > dist) di--;
    put_code(w, (uint32_t)di, 5);
    if (dist_extra[di]) put_bits(w, dist - dist_base[di], dist_extra[di]);
}

enum { HBITS = 15, HSIZE = 1 << HBITS, WSIZE = 32768, CHAIN = 24 };

static int deflate_fixed(Buf *out, const uint8_t *in, size_t n) {
    int32_t *head = malloc(sizeof(int32_t) * HSIZE);
    int32_t *prev = malloc(sizeof(int32_t) * (n ? n : 1));
    if (!head || !prev) { free(head); free(prev); return 1; }
    for (int i = 0; i < HSIZE; i++) head[i] = -1;
    BitW w = {out, 0, 0};
    put_bits(&w, 1, 1); /* BFINAL */
    put_bits(&w, 1, 2); /* BTYPE = fixed Huffman */
    size_t i = 0;
#define HASH3(p) ((((uint32_t)(p)[0] << 10) ^ ((uint32_t)(p)[1] << 5) ^ (uint32_t)(p)[2]) & (HSIZE - 1))
    while (i < n) {
        unsigned best = 0, bestd = 0;
        if (i + 3 <= n) {
            uint32_t h = HASH3(in + i);
            int32_t cand = head[h];
            int chain = CHAIN;
            size_t maxlen = n - i < 258 ? n - i : 258;
            while (cand >= 0 && i - (size_t)cand <= WSIZE && chain-- > 0) {
                size_t l = 0;
                while (l < maxlen && in[(size_t)cand + l] == in[i + l]) l++;
                if (l > best) { best = (unsigned)l; bestd = (unsigned)(i - (size_t)cand); if (l == maxlen) break; }
                cand = prev[cand];
            }
        }
        size_t adv = 1;
        if (best >= 3) { put_match(&w, best, bestd); adv = best; }
        else put_lit(&w, in[i]);
        for (size_t k = 0; k < adv; k++) {
            if (i + k + 3 <= n) {
                uint32_t h = HASH3(in + i + k);
                prev[i + k] = head[h];
                head[h] = (int32_t)(i + k);
            }
        }
        i += adv;
    }
#undef HASH3
    put_code(&w, 0, 7); /* end of block */
    if (w.nb > 0) put_bits(&w, 0, 8 - w.nb);
    free(head);
    free(prev);
    return out->oom;
}

static void png_chunk(Buf *o, const char *type, const uint8_t *data, size_t n) {
    buf_be32(o, (uint32_t)n);
    size_t start = o->n;
    buf_put(o, type, 4);
    if (n) buf_put(o, data, n);
    if (!o->oom) buf_be32(o, crc32_of(0, o->d + start, n + 4));
}

static int encode_png(const Canvas *c, Buf *out) {
    size_t stride = (size_t)c->w * 3, rawn = (stride + 1) * (size_t)c->h;
    uint8_t *raw = malloc(rawn), *cur = malloc(stride), *prev = calloc(stride, 1);
    if (!raw || !cur || !prev) { free(raw); free(cur); free(prev); return 1; }
    for (int y = 0; y < c->h; y++) {
        for (int x = 0; x < c->w; x++) {
            uint32_t p = c->px[(size_t)y * (size_t)c->w + (size_t)x];
            cur[x * 3] = (uint8_t)(p >> 16); cur[x * 3 + 1] = (uint8_t)(p >> 8); cur[x * 3 + 2] = (uint8_t)p;
        }
        uint8_t *dst = raw + (size_t)y * (stride + 1);
        /* choose none/sub/up by the smallest sum of |signed bytes| */
        uint64_t score[3] = {0, 0, 0};
        for (size_t k = 0; k < stride; k++) {
            uint8_t a = k >= 3 ? cur[k - 3] : 0;
            int8_t v0 = (int8_t)cur[k], v1 = (int8_t)(uint8_t)(cur[k] - a), v2 = (int8_t)(uint8_t)(cur[k] - prev[k]);
            score[0] += (uint64_t)(v0 < 0 ? -v0 : v0);
            score[1] += (uint64_t)(v1 < 0 ? -v1 : v1);
            score[2] += (uint64_t)(v2 < 0 ? -v2 : v2);
        }
        int f = score[1] < score[0] ? 1 : 0;
        if (score[2] < score[f]) f = 2;
        dst[0] = (uint8_t)f;
        for (size_t k = 0; k < stride; k++) {
            uint8_t a = k >= 3 ? cur[k - 3] : 0;
            dst[1 + k] = f == 0 ? cur[k] : f == 1 ? (uint8_t)(cur[k] - a) : (uint8_t)(cur[k] - prev[k]);
        }
        uint8_t *t = prev; prev = cur; cur = t;
    }
    free(cur);
    free(prev);
    Buf z = {0, 0, 0, 0};
    buf_u8(&z, 0x78); buf_u8(&z, 0x01);
    int bad = deflate_fixed(&z, raw, rawn);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawn; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    buf_be32(&z, (b << 16) | a);
    free(raw);
    if (bad || z.oom) { free(z.d); return 1; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    buf_put(out, sig, 8);
    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)((unsigned)c->w >> 24); ihdr[1] = (uint8_t)((unsigned)c->w >> 16);
    ihdr[2] = (uint8_t)((unsigned)c->w >> 8); ihdr[3] = (uint8_t)c->w;
    ihdr[4] = (uint8_t)((unsigned)c->h >> 24); ihdr[5] = (uint8_t)((unsigned)c->h >> 16);
    ihdr[6] = (uint8_t)((unsigned)c->h >> 8); ihdr[7] = (uint8_t)c->h;
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(out, "IHDR", ihdr, 13);
    png_chunk(out, "IDAT", z.d, z.n);
    png_chunk(out, "IEND", NULL, 0);
    free(z.d);
    return out->oom;
}

static int encode_ppm(const Canvas *c, Buf *out) {
    char hdr[48];
    int n = snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", c->w, c->h);
    buf_put(out, hdr, (size_t)n);
    for (size_t i = 0; i < (size_t)c->w * (size_t)c->h; i++) {
        uint32_t p = c->px[i];
        buf_u8(out, p >> 16); buf_u8(out, p >> 8); buf_u8(out, p);
    }
    return out->oom;
}

static int encode_bmp(const Canvas *c, Buf *out) {
    size_t rowb = ((size_t)c->w * 3 + 3) & ~(size_t)3, datan = rowb * (size_t)c->h;
    buf_u8(out, 'B'); buf_u8(out, 'M');
    buf_le32(out, (uint32_t)(54 + datan)); buf_le32(out, 0); buf_le32(out, 54);
    buf_le32(out, 40); buf_le32(out, (uint32_t)c->w); buf_le32(out, (uint32_t)c->h);
    buf_le16(out, 1); buf_le16(out, 24); buf_le32(out, 0); buf_le32(out, (uint32_t)datan);
    buf_le32(out, 2835); buf_le32(out, 2835); buf_le32(out, 0); buf_le32(out, 0);
    for (int y = c->h - 1; y >= 0; y--) {
        size_t w = 0;
        for (int x = 0; x < c->w; x++) {
            uint32_t p = c->px[(size_t)y * (size_t)c->w + (size_t)x];
            buf_u8(out, p); buf_u8(out, p >> 8); buf_u8(out, p >> 16);
            w += 3;
        }
        while (w++ < rowb) buf_u8(out, 0);
    }
    return out->oom;
}

static int save_file(VM *vm, SpValue pathv, const Buf *b, const char *op) {
    char rp[PATH_MAX];
    if (lib_fs_check(vm, pathv, 1, op, rp)) return 1;
    const char *orig = AS_STR(pathv)->data;
    char tmp[PATH_MAX + 32];
    (void)snprintf(tmp, sizeof tmp, "%.*s.sprint-tmp-%ld", PATH_MAX, rp, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return lib_io_fail(vm, errno, op, orig);
    int ok = fwrite(b->d, 1, b->n, f) == b->n;
    int e = errno;
    if (fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (ok && rename(tmp, rp) != 0) { ok = 0; e = errno; }
    if (!ok) {
        (void)unlink(tmp);
        return lib_io_fail(vm, e ? e : EIO, op, orig);
    }
    return 0;
}

static int save_as(VM *vm, SpValue *argv, int fmt) {
    Canvas *c = self_canvas(vm, argv[0]);
    if (!c) return 1;
    if (argv[1].type != T_STRING) return vm_set_error(vm, E_TYPE, "path must be a string, not %s", type_name(argv[1]));
    Buf b = {0, 0, 0, 0};
    int bad = fmt == 0 ? encode_png(c, &b) : fmt == 1 ? encode_ppm(c, &b) : encode_bmp(c, &b);
    if (bad) { free(b.d); vm_fatal(vm, RS_MEMORY, "out of memory"); }
    int r = save_file(vm, argv[1], &b, fmt == 0 ? "save_png" : fmt == 1 ? "save_ppm" : "save_bmp");
    free(b.d);
    return r;
}

/* ---------------------------------------------------------------- */
/* canvas methods                                                    */
/* ---------------------------------------------------------------- */
#define SELF() Canvas *c = self_canvas(vm, argv[0]); if (!c) return 1
#define COORD(i, name, var) int var; if (a_coord(vm, argv[i], name, &var)) return 1
#define COLOR(i, var) uint32_t var; if (a_color(vm, argv[i], &var)) return 1

static int cm_width(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; SELF(); *ret = v_int(c->w); return 0; }
static int cm_height(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; SELF(); *ret = v_int(c->h); return 0; }

static int cm_clear(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    SELF();
    COLOR(1, col);
    int saved = c->alpha;
    c->alpha = 255;
    fill32(c->px, col, (size_t)c->w * (size_t)c->h);
    c->alpha = saved;
    return 0;
}

static int cm_set_alpha(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    SELF();
    int64_t a;
    if (a_int(vm, argv[1], "alpha", &a)) return 1;
    if (a < 0 || a > 255) return vm_set_error(vm, E_VALUE, "alpha must be in 0..255");
    c->alpha = (int)a;
    return 0;
}
static int cm_alpha(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; SELF(); *ret = v_int(c->alpha); return 0; }

static int cm_pixel(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    SELF(); COORD(1, "x", x); COORD(2, "y", y); COLOR(3, col);
    put_px(c, x, y, col);
    return 0;
}
static int cm_get(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    SELF(); COORD(1, "x", x); COORD(2, "y", y);
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return vm_set_error(vm, E_INDEX, "pixel (%d, %d) outside the %dx%d canvas", x, y, c->w, c->h);
    *ret = v_int(c->px[(size_t)y * (size_t)c->w + (size_t)x]);
    return 0;
}
static int cm_line(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    SELF(); COORD(1, "x0", x0); COORD(2, "y0", y0); COORD(3, "x1", x1); COORD(4, "y1", y1); COLOR(5, col);
    line_px(c, x0, y0, x1, y1, col);
    return 0;
}
static int rect_impl(VM *vm, SpValue *argv, int fill) {
    SELF(); COORD(1, "x", x); COORD(2, "y", y); COORD(3, "w", w); COORD(4, "h", h); COLOR(5, col);
    if (w <= 0 || h <= 0) return 0;
    if (fill) {
        for (int yy = y; yy < y + h; yy++) hline(c, x, x + w - 1, yy, col);
    } else {
        hline(c, x, x + w - 1, y, col);
        if (h > 1) hline(c, x, x + w - 1, y + h - 1, col);
        for (int yy = y + 1; yy < y + h - 1; yy++) { put_px(c, x, yy, col); if (w > 1) put_px(c, x + w - 1, yy, col); }
    }
    return 0;
}
static int cm_rect(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)ret; return rect_impl(vm, argv, 0); }
static int cm_fill_rect(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)ret; return rect_impl(vm, argv, 1); }
static int circle_impl(VM *vm, SpValue *argv, int fill) {
    SELF(); COORD(1, "cx", cx); COORD(2, "cy", cy); COORD(3, "r", r); COLOR(4, col);
    if (r < 0) return vm_set_error(vm, E_VALUE, "radius must be >= 0");
    circle_px(c, cx, cy, r, col, fill);
    return 0;
}
static int cm_circle(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)ret; return circle_impl(vm, argv, 0); }
static int cm_fill_circle(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)ret; return circle_impl(vm, argv, 1); }
static int cm_fill_triangle(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    SELF(); COORD(1, "x0", x0); COORD(2, "y0", y0); COORD(3, "x1", x1); COORD(4, "y1", y1);
    COORD(5, "x2", x2); COORD(6, "y2", y2); COLOR(7, col);
    tri_px(c, x0, y0, x1, y1, x2, y2, col);
    return 0;
}
static int text_scale(VM *vm, int argc, SpValue *argv, int idx, int *scale) {
    *scale = 1;
    if (argc > idx) {
        int64_t s;
        if (a_int(vm, argv[idx], "scale", &s)) return 1;
        if (s < 1 || s > 64) return vm_set_error(vm, E_VALUE, "scale must be in 1..64");
        *scale = (int)s;
    }
    return 0;
}
static int cm_text(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)ret;
    SELF(); COORD(1, "x", x); COORD(2, "y", y);
    if (argv[3].type != T_STRING) return vm_set_error(vm, E_TYPE, "text must be a string, not %s", type_name(argv[3]));
    COLOR(4, col);
    int scale;
    if (text_scale(vm, argc, argv, 5, &scale)) return 1;
    draw_text(c, x, y, AS_STR(argv[3]), col, scale);
    return 0;
}
static int cm_text_width(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    SELF();
    (void)c;
    if (argv[1].type != T_STRING) return vm_set_error(vm, E_TYPE, "text must be a string, not %s", type_name(argv[1]));
    int scale;
    if (text_scale(vm, argc, argv, 2, &scale)) return 1;
    *ret = v_int(text_width(AS_STR(argv[1]), scale));
    return 0;
}
static int cm_blit(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    SELF();
    Canvas *s = self_canvas(vm, argv[1]);
    if (!s) return 1;
    COORD(2, "x", x); COORD(3, "y", y);
    for (int sy = 0; sy < s->h; sy++)
        for (int sx = 0; sx < s->w; sx++) put_px(c, x + sx, y + sy, s->px[(size_t)sy * (size_t)s->w + (size_t)sx]);
    return 0;
}
static int cm_set_clip(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    SELF(); COORD(1, "x", x); COORD(2, "y", y); COORD(3, "w", w); COORD(4, "h", h);
    int x1 = x + (w > 0 ? w : 0), y1 = y + (h > 0 ? h : 0);
    c->cx0 = x < 0 ? 0 : (x > c->w ? c->w : x);
    c->cy0 = y < 0 ? 0 : (y > c->h ? c->h : y);
    c->cx1 = x1 < c->cx0 ? c->cx0 : (x1 > c->w ? c->w : x1);
    c->cy1 = y1 < c->cy0 ? c->cy0 : (y1 > c->h ? c->h : y1);
    return 0;
}
static int cm_clear_clip(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    SELF();
    c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h;
    return 0;
}
static int cm_line_aa(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    SELF(); COORD(1, "x0", x0); COORD(2, "y0", y0); COORD(3, "x1", x1); COORD(4, "y1", y1); COLOR(5, col);
    line_aa_px(c, x0, y0, x1, y1, col);
    return 0;
}
static int rr_impl(VM *vm, SpValue *argv, int fill) {
    SELF(); COORD(1, "x", x); COORD(2, "y", y); COORD(3, "w", w); COORD(4, "h", h); COORD(5, "r", r); COLOR(6, col);
    if (r < 0) return vm_set_error(vm, E_VALUE, "radius must be >= 0");
    round_rect_px(c, x, y, w, h, r, col, fill);
    return 0;
}
static int cm_round_rect(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)ret; return rr_impl(vm, argv, 0); }
static int cm_fill_round_rect(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)ret; return rr_impl(vm, argv, 1); }
static int cm_gradient_rect(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    SELF(); COORD(1, "x", x); COORD(2, "y", y); COORD(3, "w", w); COORD(4, "h", h); COLOR(5, c0); COLOR(6, c1);
    int vertical = 1;
    if (argc > 7) vertical = sp_truthy(argv[7]);
    if (w <= 0 || h <= 0) return 0;
    int n = vertical ? h : w;
    for (int i = 0; i < n; i++) {
        uint32_t col = 0;
        for (int sh = 0; sh <= 16; sh += 8) {
            int64_t a = (c0 >> sh) & 255, b = (c1 >> sh) & 255;
            int64_t v = n > 1 ? (a * (n - 1 - i) + b * i + (n - 1) / 2) / (n - 1) : a;
            col |= (uint32_t)v << sh;
        }
        if (vertical) hline(c, x, x + w - 1, y + i, col);
        else for (int yy = 0; yy < h; yy++) put_px(c, x + i, y + yy, col);
    }
    return 0;
}
static int cm_blit_part(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    SELF();
    Canvas *s = self_canvas(vm, argv[1]);
    if (!s) return 1;
    COORD(2, "sx", sx0); COORD(3, "sy", sy0); COORD(4, "w", w); COORD(5, "h", h); COORD(6, "x", x); COORD(7, "y", y);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sx = sx0 + i, sy = sy0 + j;
            if (sx < 0 || sy < 0 || sx >= s->w || sy >= s->h) continue;
            put_px(c, x + i, y + j, s->px[(size_t)sy * (size_t)s->w + (size_t)sx]);
        }
    return 0;
}
static int cm_blit_scaled(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    SELF();
    Canvas *s = self_canvas(vm, argv[1]);
    if (!s) return 1;
    COORD(2, "x", x); COORD(3, "y", y);
    int sc;
    if (text_scale(vm, argc, argv, 4, &sc)) return 1;
    for (int sy = 0; sy < s->h; sy++)
        for (int sx = 0; sx < s->w; sx++) {
            uint32_t col = s->px[(size_t)sy * (size_t)s->w + (size_t)sx];
            for (int j = 0; j < sc; j++) hline(c, x + sx * sc, x + sx * sc + sc - 1, y + sy * sc + j, col);
        }
    return 0;
}
static int cm_hash(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    SELF();
    const uint64_t prime = UINT64_C(1099511628211);
    uint64_t hsh = UINT64_C(1469598103934665603); /* FNV-1a over dimensions and pixels */
    hsh = (hsh ^ (uint64_t)c->w) * prime;
    hsh = (hsh ^ (uint64_t)c->h) * prime;
    for (size_t i = 0; i < (size_t)c->w * (size_t)c->h; i++) hsh = (hsh ^ c->px[i]) * prime;
    *ret = v_int((int64_t)(hsh >> 2)); /* 62 bits: always a non-negative int */
    return 0;
}
static int cm_diff(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    SELF();
    Canvas *o = self_canvas(vm, argv[1]);
    if (!o) return 1;
    if (o->w != c->w || o->h != c->h) return vm_set_error(vm, E_VALUE, "diff needs canvases of the same size (%dx%d vs %dx%d)", c->w, c->h, o->w, o->h);
    int64_t n = 0;
    for (size_t i = 0; i < (size_t)c->w * (size_t)c->h; i++) n += c->px[i] != o->px[i];
    *ret = v_int(n);
    return 0;
}
static int cm_save_png(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)ret; return save_as(vm, argv, 0); }
static int cm_save_ppm(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)ret; return save_as(vm, argv, 1); }
static int cm_save_bmp(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)ret; return save_as(vm, argv, 2); }

static const NativeDesc canvas_methods[] = {
    {"width", 1, 1, cm_width}, {"height", 1, 1, cm_height}, {"clear", 2, 2, cm_clear},
    {"set_alpha", 2, 2, cm_set_alpha}, {"alpha", 1, 1, cm_alpha},
    {"pixel", 4, 4, cm_pixel}, {"get", 3, 3, cm_get}, {"line", 6, 6, cm_line},
    {"rect", 6, 6, cm_rect}, {"fill_rect", 6, 6, cm_fill_rect},
    {"circle", 5, 5, cm_circle}, {"fill_circle", 5, 5, cm_fill_circle},
    {"fill_triangle", 8, 8, cm_fill_triangle},
    {"set_clip", 5, 5, cm_set_clip}, {"clear_clip", 1, 1, cm_clear_clip}, {"line_aa", 6, 6, cm_line_aa},
    {"round_rect", 7, 7, cm_round_rect}, {"fill_round_rect", 7, 7, cm_fill_round_rect},
    {"gradient_rect", 7, 8, cm_gradient_rect}, {"blit_part", 8, 8, cm_blit_part}, {"blit_scaled", 4, 5, cm_blit_scaled},
    {"hash", 1, 1, cm_hash}, {"diff", 2, 2, cm_diff},
    {"text", 5, 6, cm_text}, {"text_width", 2, 3, cm_text_width}, {"blit", 4, 4, cm_blit},
    {"save_png", 2, 2, cm_save_png}, {"save_ppm", 2, 2, cm_save_ppm}, {"save_bmp", 2, 2, cm_save_bmp},
    {NULL, 0, 0, NULL}};

static const NativeClass canvas_class = {"canvas", canvas_methods, NULL, NULL};

/* ---------------------------------------------------------------- */
/* module functions                                                  */
/* ---------------------------------------------------------------- */
static int g_canvas(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    int64_t w, h;
    if (a_int(vm, argv[0], "width", &w) || a_int(vm, argv[1], "height", &h)) return 1;
    if (w < 1 || h < 1 || w > MAX_DIM || h > MAX_DIM || w * h > MAX_PIXELS)
        return vm_set_error(vm, E_VALUE, "canvas size must be 1..%d per side and at most %d pixels", MAX_DIM, MAX_PIXELS);
    uint32_t bg = 0;
    if (argc > 2 && a_color(vm, argv[2], &bg)) return 1;
    SNativeObj *o = vm_new_native_obj(vm, &canvas_class, sizeof(Canvas) + (size_t)(w * h) * sizeof(uint32_t));
    Canvas *c = (Canvas *)o->data;
    c->w = (int)w;
    c->h = (int)h;
    c->alpha = 255;
    c->cx1 = c->w;
    c->cy1 = c->h;
    for (int64_t i = 0; i < w * h; i++) c->px[i] = bg;
    *ret = v_obj(T_NATIVE, o);
    return 0;
}

static int g_rgb(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    int64_t v[3];
    for (int i = 0; i < 3; i++) {
        if (a_int(vm, argv[i], i == 0 ? "r" : i == 1 ? "g" : "b", &v[i])) return 1;
        if (v[i] < 0 || v[i] > 255) return vm_set_error(vm, E_VALUE, "rgb components must be in 0..255");
    }
    *ret = v_int((v[0] << 16) | (v[1] << 8) | v[2]);
    return 0;
}

/* text_advance(s, scale=1): the horizontal advance of the text, as the layout code needs it (no canvas required) */
static int g_text_advance(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    if (argv[0].type != T_STRING) return vm_set_error(vm, E_TYPE, "text must be a string, not %s", type_name(argv[0]));
    int scale;
    if (text_scale(vm, argc, argv, 1, &scale)) return 1;
    *ret = v_int(text_advance(AS_STR(argv[0]), scale));
    return 0;
}

/* hsv(h, s, v): h in degrees (any int, wrapped to 0..359), s and v in 0..100 (ints) */
static int g_hsv(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    int64_t hh, s, v;
    if (a_int(vm, argv[0], "h", &hh) || a_int(vm, argv[1], "s", &s) || a_int(vm, argv[2], "v", &v)) return 1;
    if (s < 0 || s > 100 || v < 0 || v > 100) return vm_set_error(vm, E_VALUE, "s and v must be in 0..100");
    hh %= 360;
    if (hh < 0) hh += 360;
    int64_t V = v * 255 / 100, region = hh / 60, rem = (hh % 60) * 255 / 60;
    int64_t p = V * (100 - s) / 100, q = V * (100 - s * rem / 255) / 100, t = V * (100 - s * (255 - rem) / 255) / 100;
    int64_t r, g, b;
    switch (region) {
    case 0: r = V; g = t; b = p; break;
    case 1: r = q; g = V; b = p; break;
    case 2: r = p; g = V; b = t; break;
    case 3: r = p; g = q; b = V; break;
    case 4: r = t; g = p; b = V; break;
    default: r = V; g = p; b = q; break;
    }
    *ret = v_int((r << 16) | (g << 8) | b);
    return 0;
}

const NativeDesc lib_gfx_fns[] = {
    {"canvas", 2, 3, g_canvas}, {"rgb", 3, 3, g_rgb}, {"hsv", 3, 3, g_hsv}, {"text_advance", 1, 2, g_text_advance}, {NULL, 0, 0, NULL}};

/* pixels of a canvas value for host code (the OS kernel blits them to the framebuffer); NULL if v is not a canvas */
const uint32_t *gfx_canvas_pixels(SpValue v, int *w, int *h) {
    if (v.type != T_NATIVE || AS_NATIVE(v)->cls != &canvas_class) return NULL;
    const Canvas *c = (const Canvas *)AS_NATIVE(v)->data;
    *w = c->w;
    *h = c->h;
    return c->px;
}
