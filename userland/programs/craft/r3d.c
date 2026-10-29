/* r3d.c - software renderer for the C-OS port of Craft. See r3d.h.
 *
 * Pipeline: vertices -> clip space (Craft's matrix) -> reject / back-face cull -> shade per vertex (the vertex shader's job:
 * light, ambient occlusion, fog) -> clip against the near plane -> screen-space triangles -> scan-line fill with
 * perspective-correct texture coordinates and a 1/w depth buffer (the fragment shader's job).
 *
 * The fill works on horizontal bands of the framebuffer so that several threads can share it: the vertex stage fills a list
 * of screen-space triangles, then every band walks the whole list (in order) but only touches its own rows.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define acosf(x) ((float)acos(x))
#define asinf(x) ((float)asin(x))
#include <xmmintrin.h>
#include <emmintrin.h>
#include "cos.h"
#include "r3d.h"
#include <stdio.h>
#ifdef R3D_DEBUG
static void dbg(const char *fmt, int a, int b, int c) { static int n; if (n++ > 40) return; char buf[120]; snprintf(buf, sizeof buf, fmt, a, b, c); cos_puts(buf); }
#else
#define dbg(...) ((void)0)
#endif

extern unsigned long long r3d_c_in, r3d_c_back, r3d_c_out, r3d_c_shaded, r3d_c_clipped;
#define NA 6                       /* attributes per screen vertex: invw, u/w, v/w, shade, fog, fog_height */
#define MAX_THREADS 8

/* ---------------------------------------------------------------- state */
static uint32_t *s_fb;             /* 0x00RRGGBB */
static float    *s_zb;             /* 1/w, larger = nearer; 0 = empty */
static int s_w, s_h;

static uint32_t *s_atlas; static int s_aw, s_ah;      /* 0xAARRGGBB, A = 0 for the magenta key */
static uint8_t   s_tile_cloud[256];
static uint32_t *s_font; static int s_fw, s_fh;
static uint32_t *s_sign; static int s_sw, s_sh;
static uint8_t  *s_sky;  static int s_skw, s_skh;     /* RGB */
static uint32_t  s_sky_lut[256];
static uint32_t  s_fog_lut[256];                      /* the fog colour, indexed by (sin of the view elevation + 1) / 2 */
static uint8_t   s_fogmap[256]; static int s_fogmap_ready;                      /* the sky colour for each height at the current time of day */

/* A screen triangle. Every attribute is linear in screen space (u/w, v/w and 1/w are, which is what makes the texturing perspective-correct),
 * so the vertex stage solves the plane once: a(x, y) = base + dx * x + dy * y. The fill then needs a few multiplies per row. */
typedef struct { float x[3], y[3]; float base[8], dx[8], dy[8]; int ys, ye; } STri;       /* ys, ye: the rows it can touch */
typedef struct { STri *t; int n, cap; int fixed; int *rw; int rwcap; } TriList;          /* fixed: a worker's list (preallocated, never grows) */
static TriList s_main;
enum { M_BLOCK, M_SIGN, M_SKY };
static int s_mode;
static int s_depth_test = 1, s_depth_write = 1;
static float s_depth_bias = 1.0f;

/* ---------------------------------------------------------------- textures */
static uint32_t *conv_rgba(const uint8_t *p, int w, int h, int key_magenta) {
    uint32_t *o = (uint32_t *)malloc((size_t)w * h * 4);
    if (!o) return NULL;
    for (int i = 0; i < w * h; ++i) {
        uint32_t r = p[i * 4], g = p[i * 4 + 1], b = p[i * 4 + 2], a = p[i * 4 + 3];
        if (key_magenta && r == 255 && g == 0 && b == 255) a = 0;
        o[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    return o;
}
void r3d_set_atlas(const uint8_t *rgba, int w, int h) {
    free(s_atlas); s_atlas = conv_rgba(rgba, w, h, 1); s_aw = w; s_ah = h;
    memset(s_tile_cloud, 0, sizeof s_tile_cloud);
    if (!s_atlas || w != 256 || h != 256) return;
    for (int t = 0; t < 256; ++t) {                          /* a tile that is pure white is a cloud (the shader tints those differently) */
        int tx = (t & 15) * 16, ty = (t >> 4) * 16; int white = 1;
        for (int y = 0; y < 16 && white; ++y) for (int x = 0; x < 16; ++x) if (s_atlas[(ty + y) * 256 + tx + x] != 0xFFFFFFFFu) { white = 0; break; }
        s_tile_cloud[t] = (uint8_t)white;
    }
}
void r3d_set_font(const uint8_t *rgba, int w, int h) { free(s_font); s_font = conv_rgba(rgba, w, h, 0); s_fw = w; s_fh = h; }
void r3d_set_sign(const uint8_t *rgba, int w, int h) { free(s_sign); s_sign = conv_rgba(rgba, w, h, 0); s_sw = w; s_sh = h; }
static int s_lut_gen = -1;
void r3d_set_sky(const uint8_t *rgba, int w, int h) {
    s_lut_gen = -1;
    /* the sky is only ever read along one column per frame: keep it at 256 x 256 (box-filtered) */
    free(s_sky); s_sky = (uint8_t *)malloc(256 * 256 * 3); s_skw = s_skh = 256;
    if (!s_sky) return;
    int bx = w / 256 > 0 ? w / 256 : 1, by = h / 256 > 0 ? h / 256 : 1;
    for (int y = 0; y < 256; ++y) for (int x = 0; x < 256; ++x) {
        int sy = y * h / 256, sx = x * w / 256; unsigned r = 0, g = 0, b = 0, n = 0;
        for (int j = 0; j < by; ++j) for (int i = 0; i < bx; ++i) {
            const uint8_t *q = rgba + (((size_t)(sy + j) * w) + (sx + i)) * 4; r += q[0]; g += q[1]; b += q[2]; ++n;
        }
        uint8_t *o = s_sky + ((size_t)y * 256 + x) * 3; o[0] = (uint8_t)(r / n); o[1] = (uint8_t)(g / n); o[2] = (uint8_t)(b / n);
    }
}

/* the sky texture sampled at (timer, t) with t = 0 at the bottom of the picture, linear filtering, clamped */
static uint32_t sky_sample(float timer, float t) {
    if (!s_sky) return 0x80C0FF;
    float fx = timer * s_skw - 0.5f, fy = (1.0f - t) * s_skh - 0.5f;
    if (fx < 0) fx += s_skw;                                         /* the x axis is time of day: it wraps */
    if (fy < 0) fy = 0; if (fy > s_skh - 1) fy = (float)(s_skh - 1);
    int x0 = (int)fx, y0 = (int)fy; float ax = fx - x0, ay = fy - y0;
    int x1 = (x0 + 1) % s_skw, y1 = y0 + 1 < s_skh ? y0 + 1 : y0;
    uint32_t c = 0;
    for (int k = 0; k < 3; ++k) {
        float p00 = s_sky[((size_t)y0 * s_skw + x0) * 3 + k], p10 = s_sky[((size_t)y0 * s_skw + x1) * 3 + k];
        float p01 = s_sky[((size_t)y1 * s_skw + x0) * 3 + k], p11 = s_sky[((size_t)y1 * s_skw + x1) * 3 + k];
        float v = (p00 * (1 - ax) + p10 * ax) * (1 - ay) + (p01 * (1 - ax) + p11 * ax) * ay;
        c |= (uint32_t)(v + 0.5f) << (16 - 8 * k);
    }
    return c;
}
static void build_sky_lut(float timer) {
    if (!s_fogmap_ready) {                                            /* elevation: index of sin(e) -> index of e in the sky picture */
        for (int i = 0; i < 256; ++i) {
            float sn = (float)i / 255.0f * 2.0f - 1.0f; if (sn > 1) sn = 1; if (sn < -1) sn = -1;
            int idx = (int)((asinf(sn) + 1.5707963f) * 0.31830989f * 255.0f + 0.5f);
            s_fogmap[i] = (uint8_t)(idx < 0 ? 0 : idx > 255 ? 255 : idx);
        }
        s_fogmap_ready = 1; s_lut_gen = -1;
    }
    int gen = (int)(timer * 4096.0f);                                  /* the time of day moves slowly: rebuild only when it has moved visibly */
    if (gen == s_lut_gen) return;
    for (int i = 0; i < 256; ++i) s_sky_lut[i] = sky_sample(timer, (float)i / 255.0f);
    for (int i = 0; i < 256; ++i) s_fog_lut[i] = s_sky_lut[s_fogmap[i]];
    s_lut_gen = gen;
}

/* ---------------------------------------------------------------- framebuffer */
int r3d_resize(int w, int h) {
    r3d_flush();
    if (w == s_w && h == s_h && s_fb) return 0;
    free(s_fb); free(s_zb);
    s_fb = (uint32_t *)malloc((size_t)w * h * 4); s_zb = (float *)malloc((size_t)w * h * 4);
    if (!s_fb || !s_zb) { free(s_fb); free(s_zb); s_fb = NULL; s_zb = NULL; s_w = s_h = 0; return -1; }
    s_w = w; s_h = h;
    return 0;
}
int r3d_width(void) { return s_w; }
int r3d_height(void) { return s_h; }
static uint32_t *s_ov_fb; static float *s_ov_zb; static int s_ov_w, s_ov_h, s_ov_on;
void r3d_overlay_begin(uint32_t *px, int w, int h) {
    r3d_flush();
    if (s_ov_on || !px) return;
    s_ov_fb = s_fb; s_ov_zb = s_zb; s_ov_w = s_w; s_ov_h = s_h; s_ov_on = 1;
    s_fb = px; s_zb = NULL; s_w = w; s_h = h;
}
void r3d_overlay_end(void) {
    if (!s_ov_on) return;
    s_fb = s_ov_fb; s_zb = s_ov_zb; s_w = s_ov_w; s_h = s_ov_h; s_ov_on = 0;
}
void r3d_clear(uint32_t rgb) {
    r3d_flush();
    if (rgb == 0) { memset(s_fb, 0, (size_t)s_w * s_h * 4); return; }
    for (int i = 0; i < s_w * s_h; ++i) s_fb[i] = rgb; }
void r3d_clear_depth(void) {
    r3d_flush(); memset(s_zb, 0, (size_t)s_w * s_h * 4); }

static inline float clampf_(float v) { return v < -1e6f ? -1e6f : v > 1e6f ? 1e6f : v; }       /* screen coordinates can be huge near the camera: keep the int conversion defined */
static inline int ceil_i(float v) { v = clampf_(v); int i = (int)v; return i + ((float)i < v); }
static inline int floor_i(float v) { v = clampf_(v); int i = (int)v; return i - ((float)i > v); }

/* ---------------------------------------------------------------- the fill (one band of rows) */
static void fill_tri(const STri *t, int by0, int by1) {
    /* sort the three vertices by y */
    int i0 = 0, i1 = 1, i2 = 2, tmp;
    if (t->y[i0] > t->y[i1]) { tmp = i0; i0 = i1; i1 = tmp; }
    if (t->y[i1] > t->y[i2]) { tmp = i1; i1 = i2; i2 = tmp; }
    if (t->y[i0] > t->y[i1]) { tmp = i0; i0 = i1; i1 = tmp; }
    float y0 = t->y[i0], y1 = t->y[i1], y2 = t->y[i2];
    if (y2 - y0 < 1e-6f) return;
    int ys = ceil_i(y0 - 0.5f), ye = ceil_i(y2 - 0.5f);
    if (ys < by0) ys = by0;
    if (ye > by1) ye = by1;
    if (ys >= ye) return;
    float x0 = t->x[i0], x1 = t->x[i1], x2 = t->x[i2];
    float s02 = (x2 - x0) / (y2 - y0);
    float s01 = y1 - y0 > 1e-6f ? (x1 - x0) / (y1 - y0) : 0.0f, s12 = y2 - y1 > 1e-6f ? (x2 - x1) / (y2 - y1) : 0.0f;
    /* the attribute planes: row values are base + dy * y_centre (+ half a dx, so a pixel adds dx * its integer x) */
    const __m128 DXL = _mm_loadu_ps(t->dx), DXH = _mm_loadu_ps(t->dx + 4);
    const __m128 DYL = _mm_loadu_ps(t->dy), DYH = _mm_loadu_ps(t->dy + 4);
    const __m128 half = _mm_set1_ps(0.5f);
    const __m128 BL = _mm_add_ps(_mm_loadu_ps(t->base), _mm_mul_ps(DXL, half)), BH = _mm_add_ps(_mm_loadu_ps(t->base + 4), _mm_mul_ps(DXH, half));
    const float dx0 = t->dx[0];
    const int mode = s_mode, test = s_depth_test, wr = s_depth_write; const float bias = s_depth_bias;
    const uint32_t *tex = mode == M_BLOCK ? s_atlas : s_sign;
    const int tw = mode == M_BLOCK ? s_aw : s_sw, th = mode == M_BLOCK ? s_ah : s_sh;
    const int block = mode == M_BLOCK;
    for (int y = ys; y < ye; ++y) {
        float yc = (float)y + 0.5f;
        float xl = x0 + (yc - y0) * s02, xr = yc < y1 ? x0 + (yc - y0) * s01 : x1 + (yc - y1) * s12;
        if (xl > xr) { float tx = xl; xl = xr; xr = tx; }
        int xs = ceil_i(xl - 0.5f), xe = ceil_i(xr - 0.5f);
        if (xs < 0) xs = 0;
        if (xe > s_w) xe = s_w;
        if (xs >= xe) continue;
        const __m128 yv = _mm_set1_ps(yc);
        const __m128 RL = _mm_add_ps(BL, _mm_mul_ps(DYL, yv)), RH = _mm_add_ps(BH, _mm_mul_ps(DYH, yv));
        float rc[8]; _mm_storeu_ps(rc, RL); _mm_storeu_ps(rc + 4, RH);
        uint32_t *row = s_fb + (size_t)y * s_w; float *zrow = s_zb + (size_t)y * s_w;
        if (mode != M_SKY) {
            const float r0 = rc[0];
            for (int x = xs; x < xe; ++x) {
                float xf = (float)x;
                float iw = r0 + dx0 * xf;
                if (!test || iw * bias > zrow[x]) {
                    const __m128 xv = _mm_set1_ps(xf);
                    float a[8];
                    _mm_storeu_ps(a, _mm_add_ps(RL, _mm_mul_ps(DXL, xv)));
                    float w = 1.0f / iw;
                    int tu = (int)(a[1] * w), tv = (int)(a[2] * w);
                    if (tu < 0) tu = 0; else if (tu >= tw) tu = tw - 1;
                    if (tv < 0) tv = 0; else if (tv >= th) tv = th - 1;
                    uint32_t px = tex[tv * tw + tu];
                    if (block ? (px >> 24) != 0 : px != 0xFFFFFFFFu) {
                        if (block) {
                            _mm_storeu_ps(a + 4, _mm_add_ps(RH, _mm_mul_ps(DXH, xv)));
                            int si = (int)(a[3] * 256.0f); if (si > 256) si = 256; if (si < 0) si = 0;
                            int r = (int)((px >> 16) & 255) * si >> 8, g = (int)((px >> 8) & 255) * si >> 8, b = (int)(px & 255) * si >> 8;
                            float f = a[4];
                            if (f > 0.004f) {
                                int fi = (int)(f * 256.0f); if (fi > 256) fi = 256;
                                int hi = (int)(a[5] * 255.0f); if (hi < 0) hi = 0; else if (hi > 255) hi = 255;
                                uint32_t fc = s_fog_lut[hi];
                                r += ((int)((fc >> 16) & 255) - r) * fi >> 8; g += ((int)((fc >> 8) & 255) - g) * fi >> 8; b += ((int)(fc & 255) - b) * fi >> 8;
                            }
                            row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                        } else row[x] = px & 0xFFFFFF;
                        if (wr) zrow[x] = iw;
                    }
                }
            }
        } else {                                              /* sky: only the height (v) matters */
            for (int x = xs; x < xe; ++x) {
                float xf = (float)x;
                float w = 1.0f / (rc[0] + dx0 * xf); int hi = (int)((rc[2] + t->dx[2] * xf) * w * 255.0f);
                if (hi < 0) hi = 0; else if (hi > 255) hi = 255;
                row[x] = s_sky_lut[hi];
            }
        }
    }
}

/* ---------------------------------------------------------------- triangle lists */
unsigned long long r3d_c_in, r3d_c_back, r3d_c_out, r3d_c_shaded, r3d_c_clipped, r3d_cyc_fill, r3d_cyc_blocks, r3d_cyc_present, r3d_ntri_total;
#ifdef R3D_PROF
static inline unsigned long long tsc(void) { return __builtin_ia32_rdtsc(); }
#else
#define tsc() 0ULL
#endif
static int s_overflow;
static STri *new_tri(TriList *L) {
    if (L->n == L->cap) {
        if (L->fixed) { s_overflow = 1; return NULL; }
        int nc = L->cap ? L->cap * 2 : 16384;
        STri *n = (STri *)realloc(L->t, (size_t)nc * sizeof(STri));
        if (!n) return NULL;
        L->t = n; L->cap = nc;
    }
    return &L->t[L->n++];
}
/* the serial fill of a list (the caller's own thread, all rows) - signs, sky and the unthreaded path */
static void flush_list(TriList *L) {
    if (!L->n) return;
    unsigned long long t0 = tsc(); r3d_ntri_total += L->n;
    for (int i = 0; i < L->n; ++i) fill_tri(&L->t[i], 0, s_h);
    L->n = 0;
    r3d_cyc_fill += tsc() - t0;
}
#define flush_tris() flush_list(&s_main)

/* ---------------------------------------------------------------- vertex stage */
typedef struct { float c[4]; float a[NA]; float pos[3]; } CV;     /* a clip-space vertex with its attributes */

static inline void xform(const float m[16], const float *p, float *o) {
    o[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    o[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    o[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
    o[3] = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
}
static inline int outcode(const float *c) {
    int o = 0;
    if (c[0] < -c[3]) o |= 1; if (c[0] > c[3]) o |= 2;
    if (c[1] < -c[3]) o |= 4; if (c[1] > c[3]) o |= 8;
    if (c[2] < -c[3]) o |= 16; if (c[2] > c[3]) o |= 32;
    return o;
}
/* The fill work per screen row, used to cut the rows into bands of equal work: every triangle adds its bounding width to each row it touches. */
static inline void add_weight(TriList *L, STri *t, float miny, float maxy, float minx, float maxx) {
    int r0 = ceil_i(miny - 0.5f), r1 = ceil_i(maxy - 0.5f);
    if (r0 < 0) r0 = 0; if (r1 > s_h) r1 = s_h;
    t->ys = r0; t->ye = r1;
    if (!L->rw) return;
    int wg = (int)(maxx - minx) + 3;
    for (int r = r0; r < r1; ++r) L->rw[r] += wg;
}

/* Screen vertices -> a triangle for the fill: back-face / empty / off-screen rejection, then the attribute planes. */
static void push_tri(TriList *L, const float *sx, const float *sy, const float *iw, CV *out, const int idx[3], int cull_back) {
    int j0 = idx[0], j1 = idx[1], j2 = idx[2];
    float x0 = sx[j0], y0 = sy[j0];
    float ex1 = sx[j1] - x0, ey1 = sy[j1] - y0, ex2 = sx[j2] - x0, ey2 = sy[j2] - y0;
    float area = ex1 * ey2 - ex2 * ey1;
    if (cull_back ? area >= 0 : area == 0) return;               /* counter-clockwise (in GL's y-up) = front */
    float minx = x0, maxx = x0, miny = y0, maxy = y0;
    if (sx[j1] < minx) minx = sx[j1]; if (sx[j1] > maxx) maxx = sx[j1]; if (sy[j1] < miny) miny = sy[j1]; if (sy[j1] > maxy) maxy = sy[j1];
    if (sx[j2] < minx) minx = sx[j2]; if (sx[j2] > maxx) maxx = sx[j2]; if (sy[j2] < miny) miny = sy[j2]; if (sy[j2] > maxy) maxy = sy[j2];
    if (maxy < 0 || miny > s_h || maxx < 0 || minx > s_w) return;
    STri *t = new_tri(L); if (!t) return;
    add_weight(L, t, miny, maxy, minx, maxx);
    t->x[0] = x0; t->x[1] = sx[j1]; t->x[2] = sx[j2]; t->y[0] = y0; t->y[1] = sy[j1]; t->y[2] = sy[j2];
    float inv = 1.0f / area;
    float a0[8], a1[8], a2[8];
    a0[0] = iw[j0]; a1[0] = iw[j1]; a2[0] = iw[j2];
    for (int q = 1; q < NA; ++q) { a0[q] = out[j0].a[q]; a1[q] = out[j1].a[q]; a2[q] = out[j2].a[q]; }
    a0[6] = a0[7] = a1[6] = a1[7] = a2[6] = a2[7] = 0;
    for (int h = 0; h < 8; h += 4) {
        __m128 A0 = _mm_loadu_ps(a0 + h), D1 = _mm_sub_ps(_mm_loadu_ps(a1 + h), A0), D2 = _mm_sub_ps(_mm_loadu_ps(a2 + h), A0);
        __m128 DX = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(D1, _mm_set1_ps(ey2)), _mm_mul_ps(D2, _mm_set1_ps(ey1))), _mm_set1_ps(inv));
        __m128 DY = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(D2, _mm_set1_ps(ex1)), _mm_mul_ps(D1, _mm_set1_ps(ex2))), _mm_set1_ps(inv));
        __m128 B = _mm_sub_ps(_mm_sub_ps(A0, _mm_mul_ps(DX, _mm_set1_ps(x0))), _mm_mul_ps(DY, _mm_set1_ps(y0)));
        _mm_storeu_ps(t->dx + h, DX); _mm_storeu_ps(t->dy + h, DY); _mm_storeu_ps(t->base + h, B);
    }
}

/* The common case, a triangle that needs no clipping: the three vertices stay in the lanes of vectors all the way into the attribute planes. */
static void push_tri_v(TriList *L, __m128 SX, __m128 SY, __m128 IW, __m128 UW, __m128 VW, __m128 SH, __m128 FOG, __m128 FH) {
    float xs[4], ys[4];
    _mm_storeu_ps(xs, SX); _mm_storeu_ps(ys, SY);
    float ex1 = xs[1] - xs[0], ey1 = ys[1] - ys[0], ex2 = xs[2] - xs[0], ey2 = ys[2] - ys[0];
    float area = ex1 * ey2 - ex2 * ey1;
    if (area >= 0) return;                                       /* counter-clockwise (in GL's y-up) = front */
    float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int k = 1; k < 3; ++k) { if (xs[k] < minx) minx = xs[k]; if (xs[k] > maxx) maxx = xs[k]; if (ys[k] < miny) miny = ys[k]; if (ys[k] > maxy) maxy = ys[k]; }
    if (maxy < 0 || miny > s_h || maxx < 0 || minx > s_w) return;
    STri *t = new_tri(L); if (!t) return;
    add_weight(L, t, miny, maxy, minx, maxx);
    t->x[0] = xs[0]; t->x[1] = xs[1]; t->x[2] = xs[2]; t->y[0] = ys[0]; t->y[1] = ys[1]; t->y[2] = ys[2];
    __m128 Z = _mm_setzero_ps();
    _MM_TRANSPOSE4_PS(IW, UW, VW, SH);                           /* now one vertex per vector: (1/w, u/w, v/w, shade) */
    _MM_TRANSPOSE4_PS(FOG, FH, Z, Z);                            /*                           (fog, fog height, 0, 0) */
    const __m128 INV = _mm_set1_ps(1.0f / area), EX1 = _mm_set1_ps(ex1), EX2 = _mm_set1_ps(ex2), EY1 = _mm_set1_ps(ey1), EY2 = _mm_set1_ps(ey2);
    const __m128 X0 = _mm_set1_ps(xs[0]), Y0 = _mm_set1_ps(ys[0]);
    __m128 A0[2] = { IW, FOG }, A1[2] = { UW, FH }, A2[2] = { VW, Z };
    for (int h = 0; h < 2; ++h) {
        __m128 D1 = _mm_sub_ps(A1[h], A0[h]), D2 = _mm_sub_ps(A2[h], A0[h]);
        __m128 DX = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(D1, EY2), _mm_mul_ps(D2, EY1)), INV);
        __m128 DY = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(D2, EX1), _mm_mul_ps(D1, EX2)), INV);
        __m128 B = _mm_sub_ps(_mm_sub_ps(A0[h], _mm_mul_ps(DX, X0)), _mm_mul_ps(DY, Y0));
        _mm_storeu_ps(t->dx + h * 4, DX); _mm_storeu_ps(t->dy + h * 4, DY); _mm_storeu_ps(t->base + h * 4, B);
    }
}

/* clip a polygon against the near plane (z >= -w) and emit it as a fan of screen triangles; attributes are (invw-premultiplied by the caller) */
static void emit_poly(TriList *L, CV *in, int n, int cull_back) {
    CV tmp[8]; CV *out = tmp; int m = 0;
    if (n == 3 && in[0].c[2] + in[0].c[3] >= 0 && in[1].c[2] + in[1].c[3] >= 0 && in[2].c[2] + in[2].c[3] >= 0) { out = in; m = 3; }   /* nothing to clip */
    else for (int i = 0; i < n; ++i) {
        CV *a = &in[i], *b = &in[(i + 1) % n];
        float da = a->c[2] + a->c[3], db = b->c[2] + b->c[3];
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) {
            float t = da / (da - db); CV *o = &out[m++];
            for (int k = 0; k < 4; ++k) o->c[k] = a->c[k] + t * (b->c[k] - a->c[k]);
            for (int k = 0; k < NA; ++k) o->a[k] = a->a[k] + t * (b->a[k] - a->a[k]);
        }
    }
    if (m < 3) return;
    float sx[8], sy[8], iw[8];
    for (int i = 0; i < m; ++i) {
        float w = out[i].c[3]; if (w < 1e-6f) w = 1e-6f;
        iw[i] = 1.0f / w;
        out[i].a[1] *= iw[i]; out[i].a[2] *= iw[i];                  /* u/w, v/w: the clipping above interpolated plain u, v */
        sx[i] = (out[i].c[0] * iw[i] * 0.5f + 0.5f) * (float)s_w;
        sy[i] = (1.0f - (out[i].c[1] * iw[i] * 0.5f + 0.5f)) * (float)s_h;
    }
    for (int i = 1; i + 1 < m; ++i) {
        int idx[3] = { 0, i, i + 1 };
        push_tri(L, sx, sy, iw, out, idx, cull_back);
    }
}

/* ---------------------------------------------------------------- block faces */
static inline float fast_atan2(float y, float x) {           /* |error| < 0.005 rad: it only picks a fog colour */
    float ax = fabsf(x), ay = fabsf(y);
    float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (mx < 1e-9f) return 0.0f;
    float a = mn / mx, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.5707963f - r;
    if (x < 0) r = 3.14159265f - r;
    return y < 0 ? -r : r;
}

/* A queued r3d_blocks call. The vertex stage and the fill run when the queue is flushed (r3d_flush), so that they can be spread over several cores. */
typedef struct { const float *v; int count; float m[16]; float camera[3]; float timer, daylight, fog_distance; int ortho; } Pend;
#define MAX_PEND 4096
static Pend s_pend[MAX_PEND]; static int s_npend;

/* the vertex stage for triangles [t0, t1) of one queued call */
static void blocks_range(const Pend *e, int t0, int t1, TriList *L) {
    const float *v = e->v; const float *m = e->m; const float *camera = e->camera;
    const float daylight = e->daylight, fog_distance = e->fog_distance; const int ortho = e->ortho;
    const float lx = -0.57735027f, ly = 0.57735027f, lz = -0.57735027f;
    const float inv_fog = fog_distance > 0 ? 1.0f / fog_distance : 0;
    const float fog_near = fog_distance * 0.25f, fog_near2 = fog_near * fog_near;     /* closer than this the fog term is below the 0.004 the shader ignores */
    const float aw = (float)s_aw, ah = (float)s_ah, sw = (float)s_w, sh = (float)s_h;
    const __m128 M0 = _mm_loadu_ps(m), M1 = _mm_loadu_ps(m + 4), M2 = _mm_loadu_ps(m + 8), M3 = _mm_loadu_ps(m + 12);
    const __m128 CAM = _mm_set_ps(0.0f, camera[2], camera[1], camera[0]);
    const __m128 LANE3 = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
    const int cull = !ortho && fog_distance < 1e8f;                  /* the held item has its own camera: no object-space shortcut for it */
    for (int ti = t0; ti < t1; ++ti) {
        const float *p0 = v + (size_t)ti * 30;
        __m128 P0 = _mm_loadu_ps(p0), P1 = _mm_loadu_ps(p0 + 10), P2 = _mm_loadu_ps(p0 + 20);
        if (cull) {                                                  /* object-space back-face test: skips the transform of the half that faces away */
            __m128 E1 = _mm_sub_ps(P1, P0), E2 = _mm_sub_ps(P2, P0);
            __m128 n = _mm_sub_ps(_mm_mul_ps(_mm_shuffle_ps(E1, E1, _MM_SHUFFLE(3, 0, 2, 1)), _mm_shuffle_ps(E2, E2, _MM_SHUFFLE(3, 1, 0, 2))),
                                  _mm_mul_ps(_mm_shuffle_ps(E1, E1, _MM_SHUFFLE(3, 1, 0, 2)), _mm_shuffle_ps(E2, E2, _MM_SHUFFLE(3, 0, 2, 1))));
            __m128 pr = _mm_and_ps(_mm_mul_ps(n, _mm_sub_ps(P0, CAM)), LANE3);
            pr = _mm_add_ps(pr, _mm_shuffle_ps(pr, pr, _MM_SHUFFLE(2, 3, 0, 1)));
            pr = _mm_add_ps(pr, _mm_shuffle_ps(pr, pr, _MM_SHUFFLE(1, 0, 3, 2)));
            if (_mm_cvtss_f32(pr) >= 0) continue;
        }
        CV cv[3];
        int oc[3];
        const __m128 PP[3] = { P0, P1, P2 };
        for (int k = 0; k < 3; ++k) {
            __m128 P = PP[k];
            __m128 C = _mm_add_ps(_mm_add_ps(_mm_mul_ps(M0, _mm_shuffle_ps(P, P, 0x00)), _mm_mul_ps(M1, _mm_shuffle_ps(P, P, 0x55))),
                                  _mm_add_ps(_mm_mul_ps(M2, _mm_shuffle_ps(P, P, 0xAA)), M3));
            _mm_storeu_ps(cv[k].c, C);
            __m128 W = _mm_shuffle_ps(C, C, 0xFF);
            oc[k] = (_mm_movemask_ps(_mm_cmplt_ps(C, _mm_sub_ps(_mm_setzero_ps(), W))) & 7) | ((_mm_movemask_ps(_mm_cmpgt_ps(C, W)) & 7) << 3);
        }
        if (oc[0] & oc[1] & oc[2]) continue;       /* wholly outside one clip plane */
        /* shading (the vertex shader): the three vertices ride in the three lanes of each vector */
        const float *n0 = p0 + 3;
        float diffuse = n0[0] * lx + n0[1] * ly + n0[2] * lz; if (diffuse < 0) diffuse = 0;
        int tile = (int)(p0[6] * 16.0f) + 16 * (int)((1.0f - p0[7]) * 16.0f);
        int cloud = tile >= 0 && tile < 256 && s_tile_cloud[tile];
#define G3(off) _mm_set_ps(0.0f, p0[20 + (off)], p0[10 + (off)], p0[off])
        const __m128 one = _mm_set1_ps(1.0f), LT = G3(9);
        __m128 ao = _mm_add_ps(_mm_set1_ps(0.3f), _mm_mul_ps(_mm_sub_ps(one, G3(8)), _mm_set1_ps(0.7f)));
        __m128 df = _mm_set1_ps(diffuse);
        if (cloud) { df = _mm_set1_ps(1.0f - diffuse * 0.2f); ao = _mm_sub_ps(one, _mm_mul_ps(_mm_sub_ps(one, ao), _mm_set1_ps(0.2f))); }
        ao = _mm_min_ps(_mm_add_ps(ao, LT), one); df = _mm_min_ps(_mm_add_ps(df, LT), one);
        __m128 lc = _mm_add_ps(_mm_mul_ps(_mm_min_ps(_mm_add_ps(_mm_set1_ps(daylight), LT), one), _mm_set1_ps(0.3f)), _mm_set1_ps(0.2f));
        __m128 shade = _mm_mul_ps(_mm_add_ps(lc, _mm_mul_ps(lc, df)), ao);
        __m128 fog = _mm_setzero_ps(), fh = _mm_setzero_ps();
        if (!ortho) {
            __m128 dx = _mm_sub_ps(G3(0), _mm_set1_ps(camera[0])), dy = _mm_sub_ps(G3(1), _mm_set1_ps(camera[1])), dz = _mm_sub_ps(G3(2), _mm_set1_ps(camera[2]));
            __m128 d2 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz));
            if (_mm_movemask_ps(_mm_cmpgt_ps(d2, _mm_set1_ps(fog_near2))) & 7) {   /* closer than that, the fog is below the 0.004 the shader ignores */
                __m128 dist = _mm_max_ps(_mm_sqrt_ps(d2), _mm_set1_ps(1e-6f));
                __m128 q = _mm_min_ps(_mm_mul_ps(dist, _mm_set1_ps(inv_fog)), one);
                q = _mm_mul_ps(q, q); fog = _mm_mul_ps(q, q);
                fh = _mm_mul_ps(_mm_add_ps(_mm_div_ps(dy, dist), one), _mm_set1_ps(0.5f));   /* the sky colour is looked up by the sine of the elevation */
            }
        }
        __m128 IW = _mm_div_ps(one, _mm_max_ps(_mm_set_ps(1.0f, cv[2].c[3], cv[1].c[3], cv[0].c[3]), _mm_set1_ps(1e-6f)));
        __m128 AU = _mm_mul_ps(G3(6), _mm_set1_ps(aw)), AV = _mm_mul_ps(_mm_sub_ps(one, G3(7)), _mm_set1_ps(ah));
        if (!((oc[0] | oc[1] | oc[2]) & 4)) {                      /* no vertex is beyond the near plane: nothing to clip */
            const __m128 CX = _mm_set_ps(0.0f, cv[2].c[0], cv[1].c[0], cv[0].c[0]), CY = _mm_set_ps(0.0f, cv[2].c[1], cv[1].c[1], cv[0].c[1]);
            const __m128 half = _mm_set1_ps(0.5f);
            __m128 SX = _mm_mul_ps(_mm_add_ps(_mm_mul_ps(_mm_mul_ps(CX, IW), half), half), _mm_set1_ps(sw));
            __m128 SY = _mm_mul_ps(_mm_sub_ps(half, _mm_mul_ps(_mm_mul_ps(CY, IW), half)), _mm_set1_ps(sh));
            push_tri_v(L, SX, SY, IW, _mm_mul_ps(AU, IW), _mm_mul_ps(AV, IW), shade, fog, fh);
            continue;
        }
        float t[6][4];
        _mm_storeu_ps(t[0], IW); _mm_storeu_ps(t[1], AU); _mm_storeu_ps(t[2], AV); _mm_storeu_ps(t[3], shade); _mm_storeu_ps(t[4], fog); _mm_storeu_ps(t[5], fh);
        for (int k = 0; k < 3; ++k) for (int j = 0; j < NA; ++j) cv[k].a[j] = t[j][k];
#undef G3
        emit_poly(L, cv, 3, 1);
    }
}


/* ---------------------------------------------------------------- multi-core
 * The scheduler runs ring-3 threads on one core only, so extra cores are used through the compute-job calls (cos_par_*): a job is a
 * function that runs on an idle core with no system calls and no first touches of memory. Persistent workers spin on a flag; after
 * ~0.25 s without work a worker ends itself (its core goes back to the system) and is started again on demand.
 * One flush is two phases: every participant runs the vertex stage on its slice of the queued triangles into its own list, then (after a
 * barrier) fills its own band of rows from ALL the lists. Row bands never overlap, so no locking is needed. */
#define MAX_W 8
#define WSTACK (256 * 1024)
#define IDLE_CYCLES 600000000ULL
enum { W_DEAD = 0, W_IDLE = 1, W_BUSY = 2, W_EXIT = 3 };
static int s_nw = 1, s_tune_cur = 1, s_tune_cand, s_tune_state, s_tune_dir; static unsigned s_tune_n; static unsigned long long s_tune_t, s_tune_cur_avg;                                   /* participants wanted, the caller included */
static volatile int s_wst[MAX_W], s_wgo[MAX_W]; static int s_wid[MAX_W];
static TriList s_wl[MAX_W];
static char s_wstack[MAX_W][WSTACK] __attribute__((aligned(4096)));
static int s_par_off, s_warm, s_failed, s_claimed[MAX_W], s_capmul = 2;      /* a worker's list holds capmul/W of the queue (+ slack); it grows if a list ever fills up */
static int ensure_cap(TriList *L, int need);
static int s_use_now;                                  /* participants used by the current flush (chosen by the tuner below) */                          /* parallel disabled after a failure; serial flushes done so far */
static struct {
    volatile int go;                                   /* generation: bumped to release the claimed workers */
    volatile int arrive, done, abort;
    int W; int rank[MAX_W];
    int total; int pstart[MAX_PEND + 1];
    volatile int next; int nunits;                     /* the vertex stage is dealt out in units of UNIT triangles */
} s_job;
#define UNIT 384
#define MAX_UNITS 16384
static struct { int e, t0, t1; } s_units[MAX_UNITS];

static void run_slice(int rank, int W) {
    TriList *L = &s_wl[rank]; L->n = 0;
    for (;;) {                                                     /* phase 1: whoever is free takes the next unit of triangles */
        int u = __atomic_fetch_add(&s_job.next, 1, __ATOMIC_RELAXED);
        if (u >= s_job.nunits) break;
        blocks_range(&s_pend[s_units[u].e], s_units[u].t0, s_units[u].t1, L);
    }
    __atomic_add_fetch(&s_job.arrive, 1, __ATOMIC_ACQ_REL);
    unsigned long spin = 0;
    while (__atomic_load_n(&s_job.arrive, __ATOMIC_ACQUIRE) < W) {
        if (s_job.abort) return;
        __builtin_ia32_pause();
        if (rank == 0 && (++spin & 0x3FFFF) == 0) {                /* the caller is waiting: are the workers alive? */
            for (int k = 1; k < W; ++k) {
                cos_par_fault_t f; int st = cos_par_poll(s_wid[s_claimed[k]], &f);
                if (st == COS_PAR_FAULTED || st == COS_PAR_KILLED || st < 0) { s_job.abort = 1; s_failed = 1; dbg("[R3D] worker fault vec=%d rip=%d\n", (int)f.vec, (int)(f.rip & 0xFFFFFFF), 0); return; }
            }
            if (spin > 0x3FFFF * 200) { s_job.abort = 1; s_failed = 1; return; }
        }
    }
    /* phase 2: cut the rows into W bands of equal fill work (every participant computes the same cut) and fill this band from all the lists */
    int rowsum[1100]; long long tot = 0; int hh = s_h < 1100 ? s_h : 1100;
    for (int r = 0; r < hh; ++r) { int a = 0; for (int w = 0; w < W; ++w) a += s_wl[w].rw ? s_wl[w].rw[r] : 0; rowsum[r] = a; tot += a; }
    int y0 = 0, y1 = s_h;
    if (tot > 0 && s_h <= 1100) {
        long long t0 = tot * rank / W, t1 = tot * (rank + 1) / W, cum = 0; int r = 0;
        if (rank > 0) { while (r < hh && cum < t0) cum += rowsum[r++]; y0 = r; }
        if (rank < W - 1) { while (r < hh && cum < t1) cum += rowsum[r++]; y1 = r; }
    } else { int rows = (s_h + W - 1) / W; y0 = rank * rows; y1 = (rank + 1) * rows > s_h ? s_h : (rank + 1) * rows; }
    for (int w = 0; w < W; ++w) { const TriList *M = &s_wl[w]; for (int i = 0; i < M->n; ++i) { const STri *t = &M->t[i]; if (t->ye <= y0 || t->ys >= y1) continue; fill_tri(t, y0, y1); } }
}
static void worker_main(void *arg) {
    int id = (int)(intptr_t)arg;
    unsigned long long idle0 = tsc();
    for (;;) {
        if (s_wst[id] == W_BUSY) {
            int seen = s_wgo[id];                                   /* the generation before the one that releases this job (set by the claimer) */
            while (s_job.go == seen) { if (s_job.abort) break; __builtin_ia32_pause(); }
            if (s_job.go != seen && !s_job.abort) run_slice(s_job.rank[id], s_job.W);
            __atomic_add_fetch(&s_job.done, 1, __ATOMIC_ACQ_REL);
            __atomic_store_n(&s_wst[id], W_IDLE, __ATOMIC_RELEASE);
            idle0 = tsc();
        } else if (tsc() - idle0 > IDLE_CYCLES) {
            int exp = W_IDLE;
            if (__atomic_compare_exchange_n(&s_wst[id], &exp, W_EXIT, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
        } else __builtin_ia32_pause();
        if (id == 0) return;                                        /* a dry run on the caller's core, only to bring the code in */
    }
}
static volatile unsigned long long s_selftest_ctr; static volatile int s_selftest_stop;
static char s_selftest_stack[64 * 1024] __attribute__((aligned(4096)));
static void selftest_job(void *a) { (void)a; while (!s_selftest_stop) { s_selftest_ctr++; __builtin_ia32_pause(); } }
static void par_selftest(void) {
    memset(s_selftest_stack, 1, sizeof s_selftest_stack);
    int id = cos_par_start(selftest_job, 0, s_selftest_stack, sizeof s_selftest_stack);
    dbg("[R3D] selftest start id=%d cpus=%d\n", id, cos_par_cpus(), 0);
    if (id < 0) return;
    unsigned long long a = s_selftest_ctr; cos_sleep_ms(300);
    dbg("[R3D] selftest after 300ms: ctr=%d state=%d\n", (int)(s_selftest_ctr - a), cos_par_poll(id, 0), 0);
    s_selftest_stop = 1; cos_sleep_ms(100);
    dbg("[R3D] selftest end state=%d ctr=%d\n", cos_par_poll(id, 0), (int)(s_selftest_ctr - a), 0);
    cos_par_free(id);
}
int r3d_cores(void) { return s_tune_cur; }
void r3d_set_threads(int n) {
    (void)par_selftest;                                  /* (kept for debugging the compute-job path) */
    if (n < 1) n = 1;
    if (n > MAX_W) n = MAX_W;
    s_nw = n; s_use_now = n; s_tune_cur = n;
}

/* a worker's list: sized for this flush on the caller's thread (the worker must not allocate or touch new pages) */
static int ensure_cap(TriList *L, int need) {
    if (L->t && L->cap >= need) return 1;
    int nc = need + need / 4 + 1024;
    free(L->t); L->t = (STri *)malloc((size_t)nc * sizeof(STri));
    if (!L->t) { L->cap = 0; return 0; }
    memset(L->t, 0, (size_t)nc * sizeof(STri));
    L->cap = nc; L->fixed = 1; L->n = 0;
    return 1;
}
static int ensure_rw(TriList *L) {
    if (!L->rw || L->rwcap < s_h + 8) {
        free(L->rw); L->rwcap = s_h + 8;
        L->rw = (int *)malloc((size_t)L->rwcap * sizeof(int));
        if (!L->rw) { L->rwcap = 0; return 0; }
    }
    memset(L->rw, 0, (size_t)L->rwcap * sizeof(int));
    return 1;
}

/* Runs the queue on several cores; returns 0 if it could not (nothing has been changed then). */
static int flush_parallel(int total) {
    int cpus = cos_par_cpus(); dbg("[R3D] par cpus=%d nw=%d total=%d\n", cpus, s_nw, total); if (cpus <= 0) { s_par_off = 1; return 0; }
    int want = s_use_now - 1; if (want > cpus) want = cpus;
    if (want > MAX_W - 1) want = MAX_W - 1;
    for (int w = 1; w <= want; ++w) {                              /* (re)start workers that have ended */
        if (s_wst[w] == W_EXIT) { if (s_wid[w] > 0) { cos_par_free(s_wid[w]); s_wid[w] = 0; } s_wst[w] = W_DEAD; }
        if (s_wst[w] != W_DEAD) continue;
        if (s_wid[w] > 0) { cos_par_free(s_wid[w]); s_wid[w] = 0; }
        if (!s_wstack[w][0]) memset(s_wstack[w], 1, WSTACK);       /* the stack pages must exist before the job runs */
        s_wst[w] = W_IDLE; s_wgo[w] = s_job.go;
        int id = cos_par_start(worker_main, (void *)(intptr_t)w, s_wstack[w], WSTACK);
        if (id < 0) { dbg("[R3D] start failed %d\n", id, 0, 0); s_wst[w] = W_DEAD; continue; }
        s_wid[w] = id; dbg("[R3D] worker %d started job %d\n", w, id, 0);
    }
    int W = 1; int *claimed = s_claimed; s_failed = 0;
    for (int w = 1; w <= want; ++w) {
        int exp = W_IDLE;
        s_wgo[w] = s_job.go;
        if (s_wid[w] > 0 && __atomic_compare_exchange_n(&s_wst[w], &exp, W_BUSY, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { claimed[W] = w; s_job.rank[w] = W; ++W; }
    }
    if (W < 2) return 0;
    int need = (int)((int64_t)total * s_capmul / W) + 8192, ok = 1;
    if (need > total + 4096) need = total + 4096;
    for (int k = 0; k < W; ++k) { if (!ensure_cap(&s_wl[k], need) || !ensure_rw(&s_wl[k])) ok = 0; }
    if (!ok) { for (int k = 1; k < W; ++k) __atomic_store_n(&s_wst[claimed[k]], W_IDLE, __ATOMIC_RELEASE); return 0; }
    s_job.W = W; s_job.total = total; s_job.arrive = 0; s_job.done = 0; s_job.abort = 0; s_overflow = 0;
    s_job.next = 0; s_job.nunits = 0;
    for (int e = 0; e < s_npend; ++e) for (int t0 = 0; t0 < s_pend[e].count / 3; t0 += UNIT) {
        if (s_job.nunits >= MAX_UNITS) break;
        int t1 = t0 + UNIT; if (t1 > s_pend[e].count / 3) t1 = s_pend[e].count / 3;
        s_units[s_job.nunits].e = e; s_units[s_job.nunits].t0 = t0; s_units[s_job.nunits].t1 = t1; s_job.nunits++;
    }
    __atomic_add_fetch(&s_job.go, 1, __ATOMIC_ACQ_REL);
    run_slice(0, W);
    __atomic_add_fetch(&s_job.done, 1, __ATOMIC_ACQ_REL);
    unsigned long long t0 = tsc(); unsigned spin = 0; int failed = s_failed;
    while (!failed && __atomic_load_n(&s_job.done, __ATOMIC_ACQUIRE) < W) {
        __builtin_ia32_pause();
        if ((++spin & 0xFFFFF) == 0) {                             /* look for a worker that stopped (fault) or a job that takes far too long */
            for (int k = 1; k < W; ++k) { cos_par_fault_t f; int st = cos_par_poll(s_wid[claimed[k]], &f); dbg("[R3D] wait: worker %d state %d done=%d\n", claimed[k], st, s_job.done); if (st == COS_PAR_FAULTED || st == COS_PAR_KILLED || st < 0) failed = 1; }
            if (tsc() - t0 > 20ULL * 1000 * 1000 * 1000) failed = 1;
            if (failed) { s_job.abort = 1; break; }
        }
    }
    int good = !failed && !s_overflow;
    if (s_overflow && s_capmul < W) s_capmul++;
    if (good) for (int w = 0; w < W; ++w) r3d_ntri_total += s_wl[w].n;
    for (int w = 0; w < MAX_W; ++w) s_wl[w].n = 0;
    if (failed) {
        s_par_off = 1;
        for (int k = 1; k < W; ++k) { cos_par_stop(s_wid[claimed[k]]); cos_par_free(s_wid[claimed[k]]); s_wid[claimed[k]] = 0; s_wst[claimed[k]] = W_DEAD; }
    }
    return good;
}

void r3d_flush(void) {
    if (!s_npend) return;
    if (s_fb && s_atlas) {
        build_sky_lut(s_pend[0].timer);
        s_mode = M_BLOCK; s_depth_test = 1; s_depth_write = 1; s_depth_bias = 1.0f;
        int total = 0;
        for (int e = 0; e < s_npend; ++e) { s_job.pstart[e] = total; total += s_pend[e].count / 3; }
        s_job.pstart[s_npend] = total;
        unsigned long long t0 = tsc();
        int done = 0;
        if (s_nw > 1 && !s_par_off && s_warm >= 3 && total >= 2000) {
            /* How many cores pay off depends on the machine (a host with fewer real cores than virtual CPUs gains nothing from spinning on more of them),
             * so the game tunes it: every few hundred frames it tries half or twice the current number for a short while and keeps the faster one. */
            s_use_now = s_tune_state ? s_tune_cand : s_tune_cur;
            unsigned long long tt0 = tsc();
            done = flush_parallel(total);
            unsigned long long dt = tsc() - tt0;
            if (done) {
                ++s_tune_n;
                if (!s_tune_state) {
                    if (s_tune_n > 240) s_tune_t += dt;                           /* the last 60 of 300 flushes are the reference */
                    if (s_tune_n >= 300) {
                        s_tune_cur_avg = s_tune_t / 60; s_tune_n = 0; s_tune_t = 0;
                        int up = s_tune_cur * 2 <= s_nw ? s_tune_cur * 2 : (s_tune_cur < s_nw ? s_nw : 0), down = s_tune_cur > 2 ? (s_tune_cur + 1) / 2 : 0;
                        int c = s_tune_dir ? (down ? down : up) : (up ? up : down);
                        s_tune_dir ^= 1;
                        if (c && c != s_tune_cur) { s_tune_cand = c; s_tune_state = 1; }
                    }
                } else {
                    s_tune_t += dt;
                    if (s_tune_n >= 60) {
                        unsigned long long cand_avg = s_tune_t / 60;
                        if (cand_avg * 100 < s_tune_cur_avg * 93) { s_tune_cur = s_tune_cand; dbg("[R3D] using %d cores\n", s_tune_cur, 0, 0); }
                        s_tune_state = 0; s_tune_n = 0; s_tune_t = 0;
                    }
                }
            }
        }
        if (!done && s_nw > 1 && !s_par_off && s_warm < 3 && ensure_cap(&s_wl[0], total + 4096)) {
            /* warm-up: the very code the workers will run, on this core, so its pages are mapped before another core needs them */
            s_job.W = 1; s_job.total = total; s_job.arrive = 0; s_job.abort = 0; s_overflow = 0; s_job.next = 0; s_job.nunits = 0;
            ensure_rw(&s_wl[0]);
            for (int e = 0; e < s_npend; ++e) for (int t0 = 0; t0 < s_pend[e].count / 3; t0 += UNIT) {
                if (s_job.nunits >= MAX_UNITS) break;
                int t1 = t0 + UNIT; if (t1 > s_pend[e].count / 3) t1 = s_pend[e].count / 3;
                s_units[s_job.nunits].e = e; s_units[s_job.nunits].t0 = t0; s_units[s_job.nunits].t1 = t1; s_job.nunits++;
            }
            run_slice(0, 1); s_wl[0].n = 0;
            s_wst[0] = W_IDLE; worker_main((void *)(intptr_t)0);
            if (s_overflow) s_par_off = 1;
            ++s_warm; done = 1;
        }
        if (!done) {
            for (int e = 0; e < s_npend; ++e) blocks_range(&s_pend[e], 0, s_pend[e].count / 3, &s_main);
            flush_tris();
        }
        r3d_cyc_fill += tsc() - t0;
    }
    s_npend = 0;
}

void r3d_blocks(const float *v, int count, const float m[16], const float camera[3],
                float timer, float daylight, float fog_distance, int ortho) {
    unsigned long long t0 = tsc();
    if (count < 3) return;
    if (s_npend == MAX_PEND) r3d_flush();
    Pend *e = &s_pend[s_npend++];
    e->v = v; e->count = count; memcpy(e->m, m, sizeof e->m); memcpy(e->camera, camera, sizeof e->camera);
    e->timer = timer; e->daylight = daylight; e->fog_distance = fog_distance; e->ortho = ortho;
    r3d_cyc_blocks += tsc() - t0;
}

/* ---------------------------------------------------------------- 3-D signs */
void r3d_signs(const float *v, int count, const float m[16]) {
    r3d_flush();
    if (!s_fb || !s_sign) return;
    s_mode = M_SIGN; s_depth_test = 1; s_depth_write = 0; s_depth_bias = 1.0004f;      /* polygon offset: in front of the block face it is printed on */
    for (int i = 0; i + 2 < count; i += 3) {
        CV cv[3];
        for (int k = 0; k < 3; ++k) {
            const float *p = v + (size_t)(i + k) * 5;
            xform(m, p, cv[k].c);
            float iw = 1.0f / (cv[k].c[3] > 1e-6f ? cv[k].c[3] : 1e-6f);
            cv[k].a[0] = iw;
            cv[k].a[1] = p[3] * (float)s_sw; cv[k].a[2] = (1.0f - p[4]) * (float)s_sh;
            cv[k].a[3] = cv[k].a[4] = cv[k].a[5] = 0;
        }
        if (outcode(cv[0].c) & outcode(cv[1].c) & outcode(cv[2].c)) continue;
        emit_poly(&s_main, cv, 3, 1);
    }
    flush_tris();
    s_depth_write = 1;
}

/* ---------------------------------------------------------------- sky */
void r3d_sky(const float *v, int count, const float m[16], float timer) {
    r3d_flush();
    if (!s_fb) return;
    build_sky_lut(timer);
    s_mode = M_SKY; s_depth_test = 0; s_depth_write = 0; s_depth_bias = 1.0f;
    for (int i = 0; i + 2 < count; i += 3) {
        CV cv[3];
        for (int k = 0; k < 3; ++k) {
            const float *p = v + (size_t)(i + k) * 8;
            xform(m, p, cv[k].c);
            float iw = 1.0f / (cv[k].c[3] > 1e-6f ? cv[k].c[3] : 1e-6f);
            cv[k].a[0] = iw; cv[k].a[1] = 0; cv[k].a[2] = p[7]; cv[k].a[3] = cv[k].a[4] = cv[k].a[5] = 0;
        }
        if (outcode(cv[0].c) & outcode(cv[1].c) & outcode(cv[2].c)) continue;
        emit_poly(&s_main, cv, 3, 0);
    }
    flush_tris();
    s_depth_test = 1; s_depth_write = 1;
}

/* ---------------------------------------------------------------- 2-D text (bilinear font, alpha blended) */
static inline uint32_t font_sample(float u, float v, int *alpha) {     /* v up; wraps like GL_REPEAT; bilinear in 8.8 fixed point */
    float fx = u * s_fw - 0.5f, fy = (1.0f - v) * s_fh - 0.5f;
    int x0 = floor_i(fx), y0 = floor_i(fy);
    int ax = (int)((fx - (float)x0) * 256.0f), ay = (int)((fy - (float)y0) * 256.0f);
    int x1 = (x0 + 1) & (s_fw - 1), y1 = (y0 + 1) & (s_fh - 1); x0 &= s_fw - 1; y0 &= s_fh - 1;
    uint32_t p00 = s_font[y0 * s_fw + x0], p10 = s_font[y0 * s_fw + x1], p01 = s_font[y1 * s_fw + x0], p11 = s_font[y1 * s_fw + x1];
    int w00 = (256 - ax) * (256 - ay), w10 = ax * (256 - ay), w01 = (256 - ax) * ay, w11 = ax * ay;
    int a = (int)(((p00 >> 24) * w00 + (p10 >> 24) * w10 + (p01 >> 24) * w01 + (p11 >> 24) * w11) >> 16);
    int r = (int)((((p00 >> 16) & 255) * w00 + ((p10 >> 16) & 255) * w10 + ((p01 >> 16) & 255) * w01 + ((p11 >> 16) & 255) * w11) >> 16);
    int g = (int)((((p00 >> 8) & 255) * w00 + ((p10 >> 8) & 255) * w10 + ((p01 >> 8) & 255) * w01 + ((p11 >> 8) & 255) * w11) >> 16);
    int b = (int)(((p00 & 255) * w00 + (p10 & 255) * w10 + (p01 & 255) * w01 + (p11 & 255) * w11) >> 16);
    *alpha = a;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
/* Text quads: Craft builds each glyph as an axis-aligned rectangle (two triangles, 6 vertices). Each one is drawn as a rectangle: the
 * font is sampled bilinearly in 8.8 fixed point with the texture coordinates stepped incrementally, and blended over the picture. */
void r3d_text2d(const float *v, int count, const float m[16]) {
    r3d_flush();
    if (!s_fb || !s_font) return;
    for (int i = 0; i + 5 < count; i += 6) {
        float sxl = 1e30f, sxr = -1e30f, syt = 1e30f, syb = -1e30f, ul = 0, ur = 0, vt = 0, vb = 0;
        float sx[6], sy[6];
        for (int k = 0; k < 6; ++k) {
            const float *p = v + (size_t)(i + k) * 4; float pp[3] = { p[0], p[1], 0 }, c[4];
            xform(m, pp, c);
            float iw = 1.0f / c[3];
            sx[k] = (c[0] * iw * 0.5f + 0.5f) * (float)s_w; sy[k] = (1.0f - (c[1] * iw * 0.5f + 0.5f)) * (float)s_h;
            if (sx[k] < sxl) { sxl = sx[k]; ul = p[2]; } if (sx[k] > sxr) { sxr = sx[k]; ur = p[2]; }
            if (sy[k] < syt) { syt = sy[k]; vt = p[3]; } if (sy[k] > syb) { syb = sy[k]; vb = p[3]; }
        }
        float w = sxr - sxl, h = syb - syt;
        if (w < 1e-6f || h < 1e-6f) continue;
        int x0 = ceil_i(sxl - 0.5f), x1 = ceil_i(sxr - 0.5f), y0 = ceil_i(syt - 0.5f), y1 = ceil_i(syb - 0.5f);
        int cx0 = x0 < 0 ? 0 : x0, cy0 = y0 < 0 ? 0 : y0; if (x1 > s_w) x1 = s_w; if (y1 > s_h) y1 = s_h;
        if (cx0 >= x1 || cy0 >= y1) continue;
        float dudx = (ur - ul) / w, dvdy = (vb - vt) / h;                 /* texture step per destination pixel */
        int fu0 = (int)(((ul + ((float)cx0 + 0.5f - sxl) * dudx) * (float)s_fw - 0.5f) * 256.0f + 65536.0f * 64.0f) - 256 * 64 * 256;
        int fv0 = (int)((((1.0f - (vt + ((float)cy0 + 0.5f - syt) * dvdy)) * (float)s_fh) - 0.5f) * 256.0f + 65536.0f * 64.0f) - 256 * 64 * 256;
        int dfu = (int)(dudx * (float)s_fw * 256.0f), dfv = (int)(-dvdy * (float)s_fh * 256.0f);
        const int mw = s_fw - 1, mh = s_fh - 1;
        for (int y = cy0; y < y1; ++y) {
            int fv = fv0 + (y - cy0) * dfv, fu = fu0;
            int ty0 = (fv >> 8) & mh, ty1 = ((fv >> 8) + 1) & mh, ay = fv & 255;
            const uint32_t *r0 = s_font + (size_t)ty0 * s_fw, *r1 = s_font + (size_t)ty1 * s_fw;
            uint32_t *row = s_fb + (size_t)y * s_w;
            for (int x = cx0; x < x1; ++x, fu += dfu) {
                int tx0 = (fu >> 8) & mw, tx1 = ((fu >> 8) + 1) & mw, ax = fu & 255;
                uint32_t p00 = r0[tx0], p10 = r0[tx1], p01 = r1[tx0], p11 = r1[tx1];
                int w00 = (256 - ax) * (256 - ay), w10 = ax * (256 - ay), w01 = (256 - ax) * ay, w11 = ax * ay;
                int a = (int)(((p00 >> 24) * w00 + (p10 >> 24) * w10 + (p01 >> 24) * w01 + (p11 >> 24) * w11) >> 16);
                if (a < 102) a = 102;                                      /* the shader keeps at least 0.4 alpha */
                int cr = (int)((((p00 >> 16) & 255) * w00 + ((p10 >> 16) & 255) * w10 + ((p01 >> 16) & 255) * w01 + ((p11 >> 16) & 255) * w11) >> 16);
                int cg = (int)((((p00 >> 8) & 255) * w00 + ((p10 >> 8) & 255) * w10 + ((p01 >> 8) & 255) * w01 + ((p11 >> 8) & 255) * w11) >> 16);
                int cb = (int)(((p00 & 255) * w00 + (p10 & 255) * w10 + (p01 & 255) * w01 + (p11 & 255) * w11) >> 16);
                uint32_t d = row[x];
                int r = (int)((d >> 16) & 255), g = (int)((d >> 8) & 255), b = (int)(d & 255);
                r += ((cr - r) * a * 257 + 32768) >> 16; g += ((cg - g) * a * 257 + 32768) >> 16; b += ((cb - b) * a * 257 + 32768) >> 16;
                row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }
}

/* ---------------------------------------------------------------- lines (inverted) */
static void inv_pixel(int x, int y, float iw, int dt) {
    if (x < 0 || y < 0 || x >= s_w || y >= s_h) return;
    size_t i = (size_t)y * s_w + x;
    if (dt && iw * 1.001f < s_zb[i]) return;
    s_fb[i] ^= 0x00FFFFFFu;
}
static void line2(float x0, float y0, float iw0, float x1, float y1, float iw1, int width, int dt) {
    float dx = x1 - x0, dy = y1 - y0; int n = (int)(fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy)) + 1;
    if (n > 4096) n = 4096;
    int half = width / 2;
    for (int i = 0; i <= n; ++i) {
        float t = (float)i / (float)n, x = x0 + dx * t, y = y0 + dy * t, iw = iw0 + (iw1 - iw0) * t;
        for (int j = 0; j < (width > 0 ? width : 1); ++j) {
            if (fabsf(dx) >= fabsf(dy)) inv_pixel((int)x, (int)y + j - half, iw, dt); else inv_pixel((int)x + j - half, (int)y, iw, dt);
        }
    }
}
void r3d_lines(const float *v, int comps, int count, const float m[16], int width, int depth_test) {
    r3d_flush();
    if (!s_fb) return;
    for (int i = 0; i + 1 < count; i += 2) {
        float c[2][4];
        for (int k = 0; k < 2; ++k) { float p[3] = { v[(size_t)(i + k) * comps], v[(size_t)(i + k) * comps + 1], comps > 2 ? v[(size_t)(i + k) * comps + 2] : 0 }; xform(m, p, c[k]); }
        /* clip to the near plane */
        float d0 = c[0][2] + c[0][3], d1 = c[1][2] + c[1][3];
        if (d0 < 0 && d1 < 0) continue;
        if (d0 < 0 || d1 < 0) {
            float t = d0 / (d0 - d1), nc[4];
            for (int k = 0; k < 4; ++k) nc[k] = c[0][k] + t * (c[1][k] - c[0][k]);
            memcpy(d0 < 0 ? c[0] : c[1], nc, sizeof nc);
        }
        float sx[2], sy[2], iw[2];
        for (int k = 0; k < 2; ++k) { float w = c[k][3] > 1e-6f ? c[k][3] : 1e-6f; iw[k] = 1.0f / w; sx[k] = (c[k][0] * iw[k] * 0.5f + 0.5f) * s_w; sy[k] = (1.0f - (c[k][1] * iw[k] * 0.5f + 0.5f)) * s_h; }
        line2(sx[0], sy[0], iw[0], sx[1], sy[1], iw[1], width, depth_test);
    }
}

void r3d_fill_rect(int x, int y, int w, int h, uint32_t rgb, int alpha) {
    r3d_flush();
    if (!s_fb) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > s_w ? s_w : x + w, y1 = y + h > s_h ? s_h : y + h;
    int cr = (rgb >> 16) & 255, cg = (rgb >> 8) & 255, cb = rgb & 255;
    for (int j = y0; j < y1; ++j) {
        uint32_t *row = s_fb + (size_t)j * s_w;
        for (int i = x0; i < x1; ++i) {
            uint32_t d = row[i];
            int r = (int)((d >> 16) & 255) + (((cr - (int)((d >> 16) & 255)) * alpha * 257 + 32768) >> 16);
            int g = (int)((d >> 8) & 255) + (((cg - (int)((d >> 8) & 255)) * alpha * 257 + 32768) >> 16);
            int b = (int)(d & 255) + (((cb - (int)(d & 255)) * alpha * 257 + 32768) >> 16);
            row[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

/* ---------------------------------------------------------------- present */
static void r3d_present_inner(uint32_t *dst, int stride, int dw, int dh);
void r3d_present(uint32_t *dst, int stride, int dw, int dh) {
    r3d_flush();
    unsigned long long t0 = tsc();
    r3d_present_inner(dst, stride, dw, dh);
    r3d_cyc_present += tsc() - t0;
}
static void r3d_present_inner(uint32_t *dst, int stride, int dw, int dh) {
    if (!s_fb) return;
    if (dw == s_w && dh == s_h) { for (int y = 0; y < dh; ++y) memcpy(dst + (size_t)y * stride, s_fb + (size_t)y * s_w, (size_t)dw * 4); return; }
    static int *xmap; static int xmap_dw, xmap_sw;                       /* the column map only changes with the window or picture size */
    if (xmap_dw != dw || xmap_sw != s_w || !xmap) {
        free(xmap); xmap = (int *)malloc((size_t)dw * sizeof(int)); xmap_dw = xmap_sw = 0;
        if (!xmap) return;
        for (int x = 0; x < dw; ++x) xmap[x] = (int)((int64_t)x * s_w / dw);
        xmap_dw = dw; xmap_sw = s_w;
    }
    int last_sy = -1;
    for (int y = 0; y < dh; ++y) {
        int sy = (int)((int64_t)y * s_h / dh);
        uint32_t *o = dst + (size_t)y * stride;
        if (sy == last_sy) { memcpy(o, o - stride, (size_t)dw * 4); continue; }
        const uint32_t *s = s_fb + (size_t)sy * s_w;
        if (dw == 2 * s_w) {                                            /* exactly doubled: interleave each pixel with itself */
            int x = 0;
            for (; x + 4 <= s_w; x += 4) {
                __m128i p = _mm_loadu_si128((const __m128i *)(s + x));
                _mm_storeu_si128((__m128i *)(o + 2 * x), _mm_unpacklo_epi32(p, p));
                _mm_storeu_si128((__m128i *)(o + 2 * x + 4), _mm_unpackhi_epi32(p, p));
            }
            for (; x < s_w; ++x) o[2 * x] = o[2 * x + 1] = s[x];
        } else {
            int x = 0;
            for (; x + 4 <= dw; x += 4) { o[x] = s[xmap[x]]; o[x + 1] = s[xmap[x + 1]]; o[x + 2] = s[xmap[x + 2]]; o[x + 3] = s[xmap[x + 3]]; }
            for (; x < dw; ++x) o[x] = s[xmap[x]];
        }
        last_sy = sy;
    }
}
