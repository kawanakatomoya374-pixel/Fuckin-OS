/* mcgl.c - a small OpenGL 1.x (fixed function) software renderer: the subset mc-classic-c needs (immediate mode, vertex arrays, display
 * lists, matrix stacks, 2-D textures, alpha test, blending, fog, one light, depth test, culling, GL_SELECT picking) plus the few GLU/GLEW
 * calls it uses. Everything runs on the CPU into a 0x00RRGGBB framebuffer that glfw_cos.c copies into the window. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <GL/gl.h>
#include <GL/glew.h>
#include <GL/glu.h>
#include "mcgl.h"

/* ------------------------------------------------------------------ state */
typedef struct { float x, y, z, u, v; uint8_t r, g, b, a; int8_t nx, ny, nz, pad; } V;     /* a vertex as the application gave it */
typedef struct { int w, h, wrap_s, wrap_t; uint32_t *px; } Tex;                             /* px: 0xAARRGGBB */

enum { F_TEX = 1, F_DEPTH = 2, F_BLEND = 4, F_ALPHA = 8, F_CULL = 16, F_FOG = 32, F_LIGHT = 64, F_COLMAT = 128, F_LIGHT0 = 256, F_NORM = 512 };
enum { A_VERT = 1, A_COL = 2, A_TEX = 4, A_NORM = 8 };

enum { OP_ENABLE = 1, OP_DISABLE, OP_MATMODE, OP_IDENT, OP_PUSH, OP_POP, OP_TRANS, OP_ROT, OP_SCALE, OP_ORTHO, OP_FRUSTUM, OP_MULT, OP_LOAD,
       OP_COLOR, OP_NORMAL, OP_BIND, OP_BLEND, OP_ALPHAF, OP_DEPTHF, OP_DEPTHM, OP_COLORM, OP_CULLF, OP_FRONT, OP_FOGF, OP_FOGC, OP_FOGI,
       OP_LIGHT, OP_LMODEL, OP_CMAT, OP_VIEWPORT, OP_VERTS, OP_CALL, OP_NAME_INIT, OP_NAME_PUSH, OP_NAME_POP, OP_NAME_LOAD, OP_SHADE };

typedef struct { int op, a, b; float f[16]; V *v; int n; } Cmd;
/* A display list that holds only geometry (a chunk of the world) remembers the box around its vertices: when it is called, the box is tested against the view
 * volume and the view distance cap first, and a list that cannot be seen is skipped without one of its vertices being transformed. */
typedef struct { Cmd *c; int n, cap, used; float bmin[3], bmax[3]; int has_box, impure; } List;

static struct {
    int fw, fh; uint32_t *fb; uint32_t *zb;                  /* zb: depth as 0..0xFFFFFFFF (an integer compare is all a pixel needs) */
    float mv[64][16], pr[16][16]; int mvd, prd, mode;
    float cr, cg, cb, ca, cu, cv, nx, ny, nz;
    unsigned flags;
    int sfac, dfac, afunc, dfunc, dmask, cmask, cull, front;
    float aref;
    float fog_density, fog_start, fog_end, fog_col[4]; int fog_mode;
    float l_amb[4], l_dif[4], l_dir[3], m_amb[4]; int cmat_target;
    int vx, vy, vw, vh;
    float clr[4]; double clrd;
    unsigned arrays; const float *vp; int vp_size, vp_stride, vp_type; const void *cp; int cp_size, cp_type, cp_stride; const float *tp; int tp_stride; const void *np; int np_stride;
    Tex *tex; int ntex, captex, bound;
    List *lists; int nlists, caplists; int compiling; int exec_too; int call_depth;
    /* immediate mode */
    int in_begin, prim; V *im; int nim, capim;
    /* selection */
    int rmode; GLuint *sel_buf; int sel_cap, sel_n, sel_hits; GLuint names[64]; int nname; int hit; float hit_zmin, hit_zmax;
    int err;
} g;

static void mat_ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void mat_mul(float *r, const float *a, const float *b) {      /* r = a * b (column-major) */
    float t[16];
    for (int c = 0; c < 4; ++c) for (int rr = 0; rr < 4; ++rr)
        t[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] + a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
    memcpy(r, t, 64);
}
static float *cur(void) { return g.mode == GL_PROJECTION ? g.pr[g.prd] : g.mv[g.mvd]; }

static void ensure_init(void) {
    static int done; if (done) return; done = 1;
    mat_ident(g.mv[0]); mat_ident(g.pr[0]);
    g.mode = GL_MODELVIEW; g.cr = g.cg = g.cb = g.ca = 1; g.nz = 1;
    g.sfac = GL_ONE; g.dfac = GL_ZERO; g.afunc = GL_ALWAYS; g.dfunc = GL_LESS; g.dmask = 1; g.cmask = 1; g.cull = GL_BACK; g.front = GL_CCW;
    g.fog_density = 1; g.fog_end = 1; g.fog_mode = GL_EXP;
    g.l_amb[3] = g.l_dif[3] = g.m_amb[3] = 1; g.m_amb[0] = g.m_amb[1] = g.m_amb[2] = 0.2f; g.l_dif[0] = g.l_dif[1] = g.l_dif[2] = 1; g.l_dir[2] = 1;
    g.cmat_target = GL_AMBIENT_AND_DIFFUSE; g.clrd = 1; g.rmode = GL_RENDER;
    g.tex = calloc(64, sizeof(Tex)); g.captex = 64; g.ntex = 1;
    g.lists = calloc(64, sizeof(List)); g.caplists = 64; g.nlists = 1;
    g.vw = 640; g.vh = 480;
}

#ifdef MCGL_PROF
static uint64_t pf_verts, pf_raster, pf_clear, pf_present, pf_tris, pf_pixels, pf_vcount, pf_micro;
static uint64_t pf_occl_get(void);
static inline uint64_t pf_now(void) { return __builtin_ia32_rdtsc(); }
void mcgl_prof_report(void) {
    static uint64_t last; uint64_t t = pf_now(); if (!last) { last = t; return; }
    if (t - last < 2000000000ull) return;
    fprintf(stderr, "PROF Mcycles/2s: verts %llu raster %llu clear %llu present %llu | tris %llu verts %llu pixels %llu micro %llu occl %llu\n", (unsigned long long)(pf_verts >> 20), (unsigned long long)(pf_raster >> 20),
            (unsigned long long)(pf_clear >> 20), (unsigned long long)(pf_present >> 20), (unsigned long long)pf_tris, (unsigned long long)pf_vcount, (unsigned long long)pf_pixels, (unsigned long long)pf_micro, (unsigned long long)pf_occl_get());
    pf_verts = pf_raster = pf_clear = pf_present = pf_tris = pf_vcount = pf_pixels = pf_micro = 0; last = t;
}
#define PF_BEGIN(v) uint64_t v = pf_now()
#define PF_END(acc, v) (acc += pf_now() - v)
#else
void mcgl_prof_report(void) {}
#define PF_BEGIN(v)
#define PF_END(acc, v)
#endif
/* A cap on the view distance (eye distance, the clip w of a perspective projection) that the frame pacing in glfw_cos.c moves up and down: geometry
 * wholly beyond it is not drawn, and a linear fog is pulled in to end there, so the world fades out instead of ending in a cut. */
static float s_far_cap = 1e9f;
/* Foveated drawing: around the crosshair the picture is drawn pixel by pixel; outside that window (what the player is not looking at) in 2x1 and 2x2 blocks, which
 * costs a quarter to a half of the shading. Level 0 off, 1 the window is 56% of the width and height, 2 36%. 3-D only: the 2-D interface is always drawn in full. */
static int s_fovea_level, s_fx0, s_fx1, s_fy0, s_fy1;
static void fovea_rect(void) {
    int pct = s_fovea_level == 1 ? 56 : s_fovea_level == 2 ? 36 : 100;
    int hw = g.fw * pct / 200, hh = g.fh * pct / 200;
    s_fx0 = (g.fw / 2 - hw) & ~1; s_fx1 = (g.fw / 2 + hw) | 1; s_fy0 = (g.fh / 2 - hh) & ~1; s_fy1 = (g.fh / 2 + hh) | 1;       /* whole 2x2 blocks */
}
void mcgl_set_fovea(int level) { s_fovea_level = level < 0 ? 0 : level > 2 ? 2 : level; fovea_rect(); }
int mcgl_get_fovea(void) { return s_fovea_level; }
void mcgl_set_far_cap(float d) { s_far_cap = d; }
float mcgl_get_far_cap(void) { return s_far_cap; }

int mcgl_resize(int w, int h) {
    ensure_init();
    if (w < 16) w = 16; if (h < 16) h = 16;
    free(g.fb); free(g.zb);
    g.fb = malloc((size_t)w * h * 4); g.zb = malloc((size_t)w * h * 4);
    if (!g.fb || !g.zb) { g.fw = g.fh = 0; return -1; }
    g.fw = w; g.fh = h; memset(g.fb, 0, (size_t)w * h * 4); fovea_rect();
    g.vx = g.vy = 0; g.vw = w; g.vh = h;
    return 0;
}
uint32_t *mcgl_fb(void) { return g.fb; }
int mcgl_width(void) { return g.fw; }
int mcgl_height(void) { return g.fh; }
void mcgl_flush_queue(void);
void mcgl_present(uint32_t *dst, int stride, int w, int h) {
    mcgl_flush_queue();
    if (!g.fb) return;
    PF_BEGIN(t0_);
    if (g.fw == w && g.fh == h) { for (int y = 0; y < h; ++y) memcpy(dst + (size_t)y * stride, g.fb + (size_t)y * w, (size_t)w * 4); PF_END(pf_present, t0_); return; }
    static int *xmap; static int xmap_w, xmap_fw;
    if (xmap_w != w || xmap_fw != g.fw) { free(xmap); xmap = malloc((size_t)w * sizeof(int)); if (!xmap) return; for (int x = 0; x < w; ++x) xmap[x] = (int)((int64_t)x * g.fw / w); xmap_w = w; xmap_fw = g.fw; }
    int last_sy = -1; uint32_t *last_d = NULL;
    for (int y = 0; y < h; ++y) {
        int sy = (int)((int64_t)y * g.fh / h); uint32_t *d = dst + (size_t)y * stride;
        if (sy == last_sy) { memcpy(d, last_d, (size_t)w * 4); continue; }          /* the same source row again: copy the row just made */
        const uint32_t *s = g.fb + (size_t)sy * g.fw;
        for (int x = 0; x < w; ++x) d[x] = s[xmap[x]];
        last_sy = sy; last_d = d;
    }
    PF_END(pf_present, t0_);
}

static int dbg_poly, dbg_cull, dbg_ras, dbg_calls, dbg_verts;
/* ------------------------------------------------------------------ rasteriser */
typedef struct { float x, y, z, w, u, v, r, g, b, a, fog; } CV;       /* clip space vertex with its attributes */
typedef struct { float x, y, z, iw, uw, vw, r, g, b, a, fog, u, v; } SV;    /* screen space (y down) */

static inline float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static inline uint32_t zfix(float z) { return z <= 0.0f ? 0u : z >= 1.0f ? 0xFFFFFFFFu : (uint32_t)(z * 4294967040.0f); }
static int depth_pass(uint32_t z, uint32_t old) {
    switch (g.dfunc) {
        case GL_NEVER: return 0; case GL_LESS: return z < old; case GL_EQUAL: return z == old; case GL_LEQUAL: return z <= old;
        case GL_GREATER: return z > old; case GL_NOTEQUAL: return z != old; case GL_GEQUAL: return z >= old; default: return 1;
    }
}
static int alpha_pass(float a) {
    switch (g.afunc) {
        case GL_NEVER: return 0; case GL_LESS: return a < g.aref; case GL_EQUAL: return a == g.aref; case GL_LEQUAL: return a <= g.aref;
        case GL_GREATER: return a > g.aref; case GL_NOTEQUAL: return a != g.aref; case GL_GEQUAL: return a >= g.aref; default: return 1;
    }
}
static float blend_f(int f, const float *s, const float *d, int is_alpha) {
    switch (f) {
        case GL_ZERO: return 0; case GL_ONE: return 1;
        case GL_SRC_COLOR: return s[is_alpha ? 3 : 0]; case GL_ONE_MINUS_SRC_COLOR: return 1 - s[is_alpha ? 3 : 0];
        case GL_SRC_ALPHA: return s[3]; case GL_ONE_MINUS_SRC_ALPHA: return 1 - s[3];
        case GL_DST_ALPHA: return 1; case GL_ONE_MINUS_DST_ALPHA: return 0;
        case GL_DST_COLOR: return d[is_alpha ? 3 : 0]; case GL_ONE_MINUS_DST_COLOR: return 1 - d[is_alpha ? 3 : 0];
    }
    return 1;
}

static const Tex *cur_tex(void) {
    if (!(g.flags & F_TEX) || g.bound <= 0 || g.bound >= g.ntex) return NULL;
    const Tex *t = &g.tex[g.bound]; return t->px ? t : NULL;
}

/* shade and store one pixel; r,g,b,a in 0..1 */
static inline void put_pixel(int x, int y, float z, float r, float gg, float b, float a, float fog, const Tex *t, float u, float v) {
    size_t i = (size_t)y * g.fw + x;
    uint32_t zi = zfix(z);
    if ((g.flags & F_DEPTH) && !depth_pass(zi, g.zb[i])) return;
    if (t) {
        int tx = (int)floorf(u * t->w), ty = (int)floorf(v * t->h);
        if (t->wrap_s == GL_REPEAT) { tx %= t->w; if (tx < 0) tx += t->w; } else tx = tx < 0 ? 0 : tx >= t->w ? t->w - 1 : tx;
        if (t->wrap_t == GL_REPEAT) { ty %= t->h; if (ty < 0) ty += t->h; } else ty = ty < 0 ? 0 : ty >= t->h ? t->h - 1 : ty;
        uint32_t c = t->px[(size_t)ty * t->w + tx];
        const float k = 1.0f / 255.0f;
        r *= ((c >> 16) & 255) * k; gg *= ((c >> 8) & 255) * k; b *= (c & 255) * k; a *= (c >> 24) * k;
    }
    if ((g.flags & F_ALPHA) && !alpha_pass(a)) return;
    if (g.dmask) g.zb[i] = zi;
    if (!g.cmask) return;
    if (g.flags & F_FOG) { float f = fog; r = g.fog_col[0] + (r - g.fog_col[0]) * f; gg = g.fog_col[1] + (gg - g.fog_col[1]) * f; b = g.fog_col[2] + (b - g.fog_col[2]) * f; }
    if (g.flags & F_BLEND) {
        uint32_t d = g.fb[i]; float dc[4] = { ((d >> 16) & 255) / 255.0f, ((d >> 8) & 255) / 255.0f, (d & 255) / 255.0f, 1.0f };
        float sc[4] = { r, gg, b, a };
        float sf = blend_f(g.sfac, sc, dc, 0), df = blend_f(g.dfac, sc, dc, 0);
        float sfc[3], dfc[3];
        for (int k = 0; k < 3; ++k) { sfc[k] = g.sfac == GL_SRC_COLOR || g.sfac == GL_ONE_MINUS_SRC_COLOR || g.sfac == GL_DST_COLOR || g.sfac == GL_ONE_MINUS_DST_COLOR ? blend_f(g.sfac, sc + k, dc + k, 0) : sf;
                                      dfc[k] = g.dfac == GL_SRC_COLOR || g.dfac == GL_ONE_MINUS_SRC_COLOR || g.dfac == GL_DST_COLOR || g.dfac == GL_ONE_MINUS_DST_COLOR ? blend_f(g.dfac, sc + k, dc + k, 0) : df; }
        r = r * sfc[0] + dc[0] * dfc[0]; gg = gg * sfc[1] + dc[1] * dfc[1]; b = b * sfc[2] + dc[2] * dfc[2];
    }
    int ir = (int)(clamp01(r) * 255.0f + 0.5f), ig = (int)(clamp01(gg) * 255.0f + 0.5f), ib = (int)(clamp01(b) * 255.0f + 0.5f);
    g.fb[i] = (uint32_t)(ir << 16 | ig << 8 | ib);
}

/* The common state of the game's polygons is drawn by raster_tri_int(): everything per scanline and per pixel is integer arithmetic - edges and every
 * attribute (depth, 1/w, u/w, v/w, colour, fog) are planes in fixed point, stepped by additions - because floating point is what an emulated CPU (QEMU
 * without KVM: each float operation a helper call) and a slow real one are worst at; the floating point work is the set-up of a triangle and one reciprocal per
 * 8 pixels. Anything else (other blend or depth functions, a texture that is not a power of two, a colour mask, huge coordinates) takes put_pixel(), the
 * general path in raster_tri_float(). */
typedef struct { const Tex *t; int tw_mask, th_mask, wrap_s, wrap_t, tw_shift; float tw, th; int thr; int dless; int cmask; int fogc[3]; int cr, cg, cb, ca; } Fast;

#define FASTI static inline __attribute__((always_inline))
#define K32 2.3283064365386963e-10f                      /* 2^-32 */
FASTI void span_fast(const Fast *F, int y, int xs, int xe, int64_t zacc, int64_t zstep, int64_t iwa, int64_t iws, int64_t uwa, int64_t uws, int64_t vwa, int64_t vws,
                     int64_t ra, int64_t rs, int64_t ga, int64_t gs, int64_t ba, int64_t bs, int64_t aa, int64_t as, int64_t fa, int64_t fs,
                     const int TEX, const int FOG, const int BLEND, const int ATEST, const int FLAT, const int DEPTH, const int DW, const int SX, const int AFF) {
    size_t base = (size_t)y * g.fw;
    uint32_t *fb = g.fb + base; uint32_t *zb = g.zb + base;
    const Tex *t = F->t;
    int x = xs;
    float u0 = 0, v0 = 0;
    /* AFF: the planes hold the texel coordinates themselves (a triangle whose depth hardly changes needs no perspective correction): plain stepping, no division */
    int64_t iu = 0, iv = 0, du = 0, dv = 0;
    if (TEX && AFF) { iu = uwa >> 16; iv = vwa >> 16; du = uws >> 16; dv = vws >> 16; }
    if (TEX && !AFF) { float w = 1.0f / ((float)iwa * K32); u0 = (float)uwa * K32 * w * F->tw; v0 = (float)vwa * K32 * w * F->th; }
    while (x <= xe) {
        int n = xe - x + 1; if (n > 8) n = 8;
        if (TEX && !AFF) {
            int64_t iw1 = iwa + iws * n;
            float w1 = 1.0f / ((float)iw1 * K32);
            float u1 = (float)(uwa + uws * n) * K32 * w1 * F->tw, v1 = (float)(vwa + vws * n) * K32 * w1 * F->th;
            iu = (int64_t)(u0 * 65536.0f); iv = (int64_t)(v0 * 65536.0f);
            du = ((int64_t)(u1 * 65536.0f) - iu) / n; dv = ((int64_t)(v1 * 65536.0f) - iv) / n;
            u0 = u1; v0 = v1; iwa = iw1; uwa += uws * n; vwa += vws * n;
        }
        int fseg = 0;
        if (FOG) { fseg = (int)((fa + fs * (n / 2)) >> 24); fseg = fseg < 0 ? 0 : fseg > 256 ? 256 : fseg; fa += fs * n; }       /* the fog factor is taken once for the 8 pixels */
        for (int k = 0; k < n; k += SX, x += SX) {
            uint64_t zu = (uint64_t)zacc; zacc += zstep * SX;
            const int pair = SX == 2 && x + 1 < g.fw;                          /* SX 2: this pixel and the one to its right get the same value */
            if (zu >> 32) goto next;                                        /* outside the depth range */
            uint32_t zi = (uint32_t)zu;
            if (DEPTH) { uint32_t old = zb[x]; if (F->dless ? zi >= old : zi > old) goto next; }
            if (!F->cmask && !ATEST) { if (DW) { zb[x] = zi; if (pair) zb[x + 1] = zi; } goto next; }                       /* glColorMask(false): the depth only */
            {
            int cr, cg, cb, ca;
            if (FLAT) { cr = F->cr; cg = F->cg; cb = F->cb; ca = F->ca; }
            else {
                cr = (int)(ra >> 24); cg = (int)(ga >> 24); cb = (int)(ba >> 24); ca = (int)(aa >> 24);      /* 0..256 */
                cr = cr < 0 ? 0 : cr > 256 ? 256 : cr; cg = cg < 0 ? 0 : cg > 256 ? 256 : cg; cb = cb < 0 ? 0 : cb > 256 ? 256 : cb; ca = ca < 0 ? 0 : ca > 256 ? 256 : ca;
            }
            if (TEX) {
                int tx = (int)(iu >> 16), ty = (int)(iv >> 16);
                if (F->wrap_s) tx &= F->tw_mask; else tx = tx < 0 ? 0 : tx > F->tw_mask ? F->tw_mask : tx;
                if (F->wrap_t) ty &= F->th_mask; else ty = ty < 0 ? 0 : ty > F->th_mask ? F->th_mask : ty;
                uint32_t c = t->px[((size_t)ty << F->tw_shift) | (size_t)tx];
                cr = (cr * (int)((c >> 16) & 255)) >> 8; cg = (cg * (int)((c >> 8) & 255)) >> 8; cb = (cb * (int)(c & 255)) >> 8; ca = (ca * (int)(c >> 24)) >> 8;
            } else { cr = cr > 255 ? 255 : cr; cg = cg > 255 ? 255 : cg; cb = cb > 255 ? 255 : cb; ca = ca > 255 ? 255 : ca; }
            if (ATEST && ca <= F->thr) goto next;
            if (DW) { zb[x] = zi; if (pair) zb[x + 1] = zi; }
            if (!F->cmask) goto next;
            if (FOG) {
                cr = F->fogc[0] + (((cr - F->fogc[0]) * fseg) >> 8); cg = F->fogc[1] + (((cg - F->fogc[1]) * fseg) >> 8); cb = F->fogc[2] + (((cb - F->fogc[2]) * fseg) >> 8);
            }
            if (BLEND) {
                int a = ca > 255 ? 255 : ca, ia = 255 - a;
                for (int q = 0; q <= pair; ++q) {
                    uint32_t d = fb[x + q];
                    int r_ = (cr * a + (int)((d >> 16) & 255) * ia + 128) * 257 >> 16, g_ = (cg * a + (int)((d >> 8) & 255) * ia + 128) * 257 >> 16, b_ = (cb * a + (int)(d & 255) * ia + 128) * 257 >> 16;
                    fb[x + q] = (uint32_t)(r_ << 16 | g_ << 8 | b_);
                }
            } else {
                uint32_t px;
                if (TEX) px = (uint32_t)(cr << 16 | cg << 8 | cb);                    /* a texel (at most 255) times at most 256, shifted: no clamp needed */
                else px = (uint32_t)((cr > 255 ? 255 : cr) << 16 | (cg > 255 ? 255 : cg) << 8 | (cb > 255 ? 255 : cb));
                fb[x] = px; if (pair) fb[x + 1] = px;
            }
            }
        next:
            if (TEX) { iu += du * SX; iv += dv * SX; }
            if (!FLAT) { ra += rs * SX; ga += gs * SX; ba += bs * SX; aa += as * SX; }
        }
    }
}

/* floor/ceil/min/max without libm calls (each was a function call, and software floating point on top, in the emulated case) */
static inline int ifloor(float x) { int i = (int)x; return i - (x < (float)i); }
static inline int iceil(float x) { int i = (int)x; return i + (x > (float)i); }
#define FMIN(a, b) ((a) < (b) ? (a) : (b))
#define FMAX(a, b) ((a) > (b) ? (a) : (b))
static inline int is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }
static inline int64_t fx32(float v) { return (int64_t)(v * 4294967296.0f); }         /* the planes are 32.32 fixed point; values stay below 2^20 */
static inline int64_t fx24(float v) { return (int64_t)(v * 16777216.0f); }

static void raster_tri_float(const SV *a, const SV *b, const SV *c, float inv, int y0, int y1, int vminx, int vmaxx, const Tex *t) {
    float dx1 = b->x - a->x, dy1 = b->y - a->y, dx2 = c->x - a->x, dy2 = c->y - a->y;
#define GRAD(F, gx, gy) float gx = ((b->F - a->F) * dy2 - (c->F - a->F) * dy1) * inv, gy = ((c->F - a->F) * dx1 - (b->F - a->F) * dx2) * inv;
    GRAD(z, zx, zy) GRAD(iw, iwx, iwy) GRAD(uw, uwx, uwy) GRAD(vw, vwx, vwy) GRAD(r, rx, ry) GRAD(g, gx_, gy_) GRAD(b, bx, by) GRAD(a, ax, ay) GRAD(fog, fx, fy)
#undef GRAD
    const SV *vs[3] = { a, b, c };
    float slope[3]; int hz[3];
    for (int i = 0; i < 3; ++i) { const SV *p = vs[i], *q = vs[(i + 1) % 3]; float dy = q->y - p->y; hz[i] = dy == 0; slope[i] = hz[i] ? 0 : (q->x - p->x) / dy; }
    for (int y = y0; y <= y1; ++y) {
        float yc = y + 0.5f;
        int xs = vminx, xe = vmaxx, skip = 0;
        for (int i = 0; i < 3; ++i) {
            const SV *p = vs[i], *q = vs[(i + 1) % 3];
            if (!hz[i]) {
                float xe_ = p->x + slope[i] * (yc - p->y);
                if (xe_ > 100000.0f) xe_ = 100000.0f; else if (xe_ < -100000.0f) xe_ = -100000.0f;
                if (q->y < p->y) { int x = iceil(xe_ - 0.5f); if (x > xs) xs = x; }
                else { int x = ifloor(xe_ - 0.5f); if (x < xe) xe = x; }
            } else if ((q->x - p->x) * (yc - p->y) < 0) { skip = 1; break; }
        }
        if (skip || xs > xe) continue;
        float dx = (xs + 0.5f) - a->x, dy = yc - a->y;
        float z = a->z + zx * dx + zy * dy, iw = a->iw + iwx * dx + iwy * dy, uw = a->uw + uwx * dx + uwy * dy, vw = a->vw + vwx * dx + vwy * dy;
        float r = a->r + rx * dx + ry * dy, gg = a->g + gx_ * dx + gy_ * dy, bb = a->b + bx * dx + by * dy, aa = a->a + ax * dx + ay * dy, fg = a->fog + fx * dx + fy * dy;
        for (int x = xs; x <= xe; ++x) {
            if (z >= 0.0f && z <= 1.0f) {
                float u = 0, v = 0;
                if (t) { float w = 1.0f / iw; u = uw * w; v = vw * w; }
                put_pixel(x, y, z, r, gg, bb, aa, fg < 0 ? 0 : fg > 1 ? 1 : fg, t, u, v);
            }
            z += zx; iw += iwx; uw += uwx; vw += vwx; r += rx; gg += gx_; bb += bx; aa += ax; fg += fx;
        }
    }
}

static void raster_tri_impl(const SV *p0, const SV *p1, const SV *p2) {
    const SV *a = p0, *b = p1, *c = p2;
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0) return;
    if (area < 0) { const SV *t = b; b = c; c = t; area = -area; }
    float minx = FMIN(a->x, FMIN(b->x, c->x)), maxx = FMAX(a->x, FMAX(b->x, c->x));
    float miny = FMIN(a->y, FMIN(b->y, c->y)), maxy = FMAX(a->y, FMAX(b->y, c->y));
    int vminx = g.vx, vmaxx = g.vx + g.vw - 1;
    if (vminx < 0) vminx = 0; if (vmaxx >= g.fw) vmaxx = g.fw - 1;
    if (maxx < (float)vminx - 0.5f || minx > (float)vmaxx + 0.5f) return;                 /* off the picture */
    if (maxy < -0.5f || miny > (float)g.fh) return;
    int y0 = iceil(miny - 0.5f), y1 = ifloor(maxy - 0.5f);
    if (y0 < 0) y0 = 0; if (y1 >= g.fh) y1 = g.fh - 1;
    if (y0 > y1) return;
    const Tex *t = cur_tex();
    int depth = (g.flags & F_DEPTH) != 0;
    /* A triangle of a few pixels (most of the far terrain): no planes, no interpolation - the pixel centres it covers are found with integer edge functions
     * and it is shaded as one flat colour, the texel at its middle, at one depth. */
    {
        int x0m = iceil(minx - 0.5f), x1m = ifloor(maxx - 0.5f);
        if (x0m < vminx) x0m = vminx; if (x1m > vmaxx) x1m = vmaxx;
        if (x0m > x1m) return;
        if (maxx - minx < 6.0f && maxy - miny < 6.0f && !(g.flags & F_BLEND) && g.cmask && (!depth || g.dfunc == GL_LEQUAL || g.dfunc == GL_LESS || g.dfunc == GL_ALWAYS)
            && (!(g.flags & F_ALPHA) || g.afunc == GL_GREATER) && (!t || (is_pow2(t->w) && is_pow2(t->h)))) {
#ifdef MCGL_PROF
            pf_micro++;
#endif
            /* edge functions in 1/16 pixel units, relative to the first corner of the box (small numbers) */
            int ax = (int)((a->x - (float)x0m) * 16.0f), ay = (int)((a->y - (float)y0) * 16.0f);
            int bx = (int)((b->x - (float)x0m) * 16.0f), by = (int)((b->y - (float)y0) * 16.0f);
            int cx = (int)((c->x - (float)x0m) * 16.0f), cy = (int)((c->y - (float)y0) * 16.0f);
            uint32_t colour = 0; int have_colour = 0;
            uint32_t zi = 0; int thr = (int)(g.aref * 255.0f + 0.0001f);
            for (int py = y0; py <= y1; ++py) {
                int qy = (py - y0) * 16 + 8;
                for (int px = x0m; px <= x1m; ++px) {
                    int qx = (px - x0m) * 16 + 8;
                    int e0 = (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
                    int e1 = (cx - bx) * (qy - by) - (cy - by) * (qx - bx);
                    int e2 = (ax - cx) * (qy - cy) - (ay - cy) * (qx - cx);
                    if ((e0 | e1 | e2) < 0) continue;                                 /* the centre is outside */
                    if (!have_colour) {
                        have_colour = 1;
                        float zf = (a->z + b->z + c->z) * (1.0f / 3.0f); zi = zfix(zf);
                        float r = (a->r + b->r + c->r) * (1.0f / 3.0f), gg = (a->g + b->g + c->g) * (1.0f / 3.0f), bb = (a->b + b->b + c->b) * (1.0f / 3.0f), aa = (a->a + b->a + c->a) * (1.0f / 3.0f);
                        int cr = (int)(clamp01(r) * 256.0f), cg = (int)(clamp01(gg) * 256.0f), cb = (int)(clamp01(bb) * 256.0f), ca = (int)(clamp01(aa) * 256.0f);
                        if (t) {
                            float u = (a->u + b->u + c->u) * (1.0f / 3.0f), v = (a->v + b->v + c->v) * (1.0f / 3.0f);
                            int tx0 = (int)(u * (float)t->w), ty0 = (int)(v * (float)t->h);
                            if (u < 0) tx0--; if (v < 0) ty0--;
                            /* a triangle this small shows a few texels at most: the middle of four samples around the centre */
                            int sr = 0, sg = 0, sb = 0, sa = 0, step = (maxx - minx > 3.0f || maxy - miny > 3.0f) ? 2 : 0;
                            for (int q = 0; q < (step ? 4 : 1); ++q) {
                                int tx = tx0 + (step ? ((q & 1) ? step : -step) : 0), ty = ty0 + (step ? ((q & 2) ? step : -step) : 0);
                                if (t->wrap_s == GL_REPEAT) tx &= t->w - 1; else tx = tx < 0 ? 0 : tx >= t->w ? t->w - 1 : tx;
                                if (t->wrap_t == GL_REPEAT) ty &= t->h - 1; else ty = ty < 0 ? 0 : ty >= t->h ? t->h - 1 : ty;
                                uint32_t tc = t->px[(size_t)ty * (size_t)t->w + (size_t)tx];
                                sr += (int)((tc >> 16) & 255); sg += (int)((tc >> 8) & 255); sb += (int)(tc & 255); sa += (int)(tc >> 24);
                            }
                            int nsamp = step ? 4 : 1; sr /= nsamp; sg /= nsamp; sb /= nsamp; sa /= nsamp;
                            cr = (cr * sr) >> 8; cg = (cg * sg) >> 8; cb = (cb * sb) >> 8; ca = (ca * sa) >> 8;
                        } else { cr = cr > 255 ? 255 : cr; cg = cg > 255 ? 255 : cg; cb = cb > 255 ? 255 : cb; ca = ca > 255 ? 255 : ca; }
                        if (g.flags & F_FOG) {
                            float fg = (a->fog + b->fog + c->fog) * (1.0f / 3.0f); int f = (int)(clamp01(fg) * 256.0f);
                            int fr = (int)(clamp01(g.fog_col[0]) * 255.0f + 0.5f), fgc = (int)(clamp01(g.fog_col[1]) * 255.0f + 0.5f), fb_ = (int)(clamp01(g.fog_col[2]) * 255.0f + 0.5f);
                            cr = fr + (((cr - fr) * f) >> 8); cg = fgc + (((cg - fgc) * f) >> 8); cb = fb_ + (((cb - fb_) * f) >> 8);
                        }
                        colour = (uint32_t)(cr << 16 | cg << 8 | cb) | ((uint32_t)(ca > 255 ? 255 : ca) << 24);          /* alpha kept in the top byte for the test */
                        if ((g.flags & F_ALPHA) && (int)(colour >> 24) <= thr) return;                                   /* wholly transparent texel: nothing to draw */
                    }
                    size_t idx = (size_t)py * (size_t)g.fw + (size_t)px;
                    if (depth && (g.dfunc == GL_LESS ? zi >= g.zb[idx] : zi > g.zb[idx])) continue;
                    if (g.dmask) g.zb[idx] = zi;
                    g.fb[idx] = colour & 0x00FFFFFFu;
                }
            }
            return;
        }
    }
    float inv = 1.0f / area;
    int fast = (!depth || g.dfunc == GL_LEQUAL || g.dfunc == GL_LESS || g.dfunc == GL_ALWAYS)
        && (!(g.flags & F_ALPHA) || g.afunc == GL_GREATER) && (!(g.flags & F_BLEND) || (g.sfac == GL_SRC_ALPHA && g.dfac == GL_ONE_MINUS_SRC_ALPHA))
        && (!t || (is_pow2(t->w) && is_pow2(t->h)))
        && minx > -100000.0f && maxx < 100000.0f && miny > -100000.0f && maxy < 100000.0f;       /* the fixed point has room for these */
    const SV *vs[3] = { a, b, c };
    float slope[3]; int hz[3];
    if (fast) {
        for (int i = 0; i < 3; ++i) {
            const SV *p = vs[i], *q = vs[(i + 1) % 3]; float dy = q->y - p->y; hz[i] = dy == 0;
            if (hz[i]) slope[i] = 0; else { slope[i] = (q->x - p->x) / dy; if (slope[i] > 1000000.0f || slope[i] < -1000000.0f) fast = 0; }
        }
    }
    if (!fast) { raster_tri_float(a, b, c, inv, y0, y1, vminx, vmaxx, t); return; }

    int flat = a->r == b->r && b->r == c->r && a->g == b->g && b->g == c->g && a->b == b->b && b->b == c->b && a->a == b->a && b->a == c->a;
    int fog = (g.flags & F_FOG) != 0;
    float dx1 = b->x - a->x, dy1 = b->y - a->y, dx2 = c->x - a->x, dy2 = c->y - a->y;
    /* attribute planes: value(x, y) = C + Bx * x + By * y at pixel (x, y) (its centre), in 32.32 fixed point */
#define PLANE(F, C, BX, BY, SC) float gx_##F = ((b->F - a->F) * dy2 - (c->F - a->F) * dy1) * inv, gy_##F = ((c->F - a->F) * dx1 - (b->F - a->F) * dx2) * inv; \
    int64_t BX = fx32(gx_##F * (SC)), BY = fx32(gy_##F * (SC)), C = fx32((a->F + gx_##F * (0.5f - a->x) + gy_##F * (0.5f - a->y)) * (SC));
    PLANE(z, Cz, Bzx, Bzy, 1.0f)
    /* a triangle whose depth hardly changes (1/w within 20%) is textured without perspective correction: planes of the texel coordinates, no division in the span */
    float iwmin = FMIN(a->iw, FMIN(b->iw, c->iw)), iwmax = FMAX(a->iw, FMAX(b->iw, c->iw));
    const int aff = t != NULL && iwmax < iwmin * 1.2f;
    int64_t Ciw = 0, Biwx = 0, Biwy = 0, Cuw = 0, Buwx = 0, Buwy = 0, Cvw = 0, Bvwx = 0, Bvwy = 0, Cr = 0, Brx = 0, Bry = 0, Cg = 0, Bgx = 0, Bgy = 0, Cb = 0, Bbx = 0, Bby = 0, Ca = 0, Bax = 0, Bay = 0, Cf = 0, Bfx = 0, Bfy = 0;
    if (t && aff) { PLANE(u, c2, b3, b4, (float)t->w) Cuw = c2; Buwx = b3; Buwy = b4; PLANE(v, c3, b5, b6, (float)t->h) Cvw = c3; Bvwx = b5; Bvwy = b6; }
    else if (t) { PLANE(iw, c1, b1, b2, 1.0f) Ciw = c1; Biwx = b1; Biwy = b2; PLANE(uw, c2, b3, b4, 1.0f) Cuw = c2; Buwx = b3; Buwy = b4; PLANE(vw, c3, b5, b6, 1.0f) Cvw = c3; Bvwx = b5; Bvwy = b6; }
    if (!flat) { PLANE(r, c4, b7, b8, 1.0f) Cr = c4 << 0; Brx = b7; Bry = b8; PLANE(g, c5, b9, b10, 1.0f) Cg = c5; Bgx = b9; Bgy = b10; PLANE(b, c6, b11, b12, 1.0f) Cb = c6; Bbx = b11; Bby = b12; PLANE(a, c7, b13, b14, 1.0f) Ca = c7; Bax = b13; Bay = b14; }
    if (fog) { PLANE(fog, c8, b15, b16, 1.0f) Cf = c8; Bfx = b15; Bfy = b16; }
#undef PLANE
    Fast F; memset(&F, 0, sizeof F);
    F.cmask = g.cmask != 0; F.t = t; if (t) { F.tw_shift = __builtin_ctz((unsigned)t->w); F.tw_mask = t->w - 1; F.th_mask = t->h - 1; F.wrap_s = t->wrap_s == GL_REPEAT; F.wrap_t = t->wrap_t == GL_REPEAT; F.tw = (float)t->w; F.th = (float)t->h; }
    F.thr = (int)(g.aref * 255.0f + 0.0001f); F.dless = g.dfunc == GL_LESS;
    for (int k = 0; k < 3; ++k) F.fogc[k] = (int)(clamp01(g.fog_col[k]) * 255.0f + 0.5f);
    if (flat) { F.cr = (int)(clamp01(a->r) * 256.0f); F.cg = (int)(clamp01(a->g) * 256.0f); F.cb = (int)(clamp01(a->b) * 256.0f); F.ca = (int)(clamp01(a->a) * 256.0f); }
    if (g.dfunc == GL_ALWAYS) depth = 0;
    int tex = t != NULL, fgo = fog, bl = (g.flags & F_BLEND) != 0, at = (g.flags & F_ALPHA) != 0, dw = g.dmask != 0;
    /* the edges, x in 24.8-ish fixed point (int64, 24 fractional bits), stepped by one slope per scanline */
    int64_t ex[3], es[3]; float yc0 = y0 + 0.5f;
    for (int i = 0; i < 3; ++i) { const SV *p = vs[i]; if (hz[i]) { ex[i] = es[i] = 0; continue; } es[i] = fx24(slope[i]); ex[i] = fx24(p->x + slope[i] * (yc0 - p->y)); }
    const int64_t HALF = 1ll << 23;
    const int foveate = s_fovea_level && g.pr[g.prd][15] == 0.0f && g.vw == g.fw && g.vh == g.fh;
    for (int y = y0; y <= y1; ++y) {
        int xs = vminx, xe = vmaxx, skip = 0;
        int outside_band = foveate && (y < s_fy0 || y > s_fy1);
        for (int i = 0; i < 3; ++i) {
            if (!hz[i]) {
                if (vs[(i + 1) % 3]->y < vs[i]->y) { int x = (int)((ex[i] + HALF - 1) >> 24); if (x > xs) xs = x; }
                else { int x = (int)((ex[i] - HALF) >> 24); if (x < xe) xe = x; }
                ex[i] += es[i];
            } else { const SV *p = vs[i], *q = vs[(i + 1) % 3]; if ((q->x - p->x) * ((float)y + 0.5f - p->y) < 0) skip = 1; }
        }
        if (outside_band && (y & 1)) continue;                              /* the odd row of a 2x2 block is copied from the even one below */
        if (skip || xs > xe) continue;
        /* the pieces of this row and how they are drawn: [xa, xb] with a pixel step of 1 (every pixel) or 2 (blocks) */
        int pa[3], pb[3], pst[3], np = 0;
        if (!foveate) { pa[0] = xs; pb[0] = xe; pst[0] = 1; np = 1; }
        else if (outside_band) { pa[0] = (xs + 1) & ~1; pb[0] = xe; pst[0] = 2; np = 1; }
        else {
            if (xs < s_fx0) { pa[np] = (xs + 1) & ~1; pb[np] = xe < s_fx0 - 1 ? xe : s_fx0 - 1; pst[np] = 2; if (pa[np] <= pb[np]) np++; }
            { int a_ = xs > s_fx0 ? xs : s_fx0, b_ = xe < s_fx1 ? xe : s_fx1; if (a_ <= b_) { pa[np] = a_; pb[np] = b_; pst[np] = 1; np++; } }
            if (xe > s_fx1) { pa[np] = xs > s_fx1 + 1 ? ((xs + 1) & ~1) : s_fx1 + 1; pb[np] = xe; pst[np] = 2; if (pa[np] <= pb[np]) np++; }
        }
        for (int pi = 0; pi < np; ++pi) {
            int xa = pa[pi], xb = pb[pi], st = pst[pi];
#ifdef MCGL_PROF
            pf_pixels += (uint64_t)(xb - xa + 1) / (uint64_t)st;
#endif
            int64_t zacc = Cz + Bzx * xa + Bzy * y;
            int64_t iwa = Ciw + Biwx * xa + Biwy * y, uwa = Cuw + Buwx * xa + Buwy * y, vwa = Cvw + Bvwx * xa + Bvwy * y;
            int64_t ra = 0, ga = 0, ba = 0, aa = 0, fa = 0;
            if (!flat) { ra = Cr + Brx * xa + Bry * y; ga = Cg + Bgx * xa + Bgy * y; ba = Cb + Bbx * xa + Bby * y; aa = Ca + Bax * xa + Bay * y; }   /* 0..1 in 32 fractional bits: ">> 24" is 0..256 */
            if (fog) fa = Cf + Bfx * xa + Bfy * y;
            int64_t rs = Brx, gs = Bgx, bs = Bbx, as = Bax, fs = Bfx;
#define SPAN(TEX, FOG, BL, AT, FL, DE, DW, SXV, AF) span_fast(&F, y, xa, xb, zacc, Bzx, iwa, Biwx, uwa, Buwx, vwa, Bvwx, ra, rs, ga, gs, ba, bs, aa, as, fa, fs, TEX, FOG, BL, AT, FL, DE, DW, SXV, AF)
#define DISPATCH(SXV) \
            if (flat && depth && dw && tex && aff && !fgo && !bl && !at) SPAN(1, 0, 0, 0, 1, 1, 1, SXV, 1); \
            else if (flat && depth && dw && tex && aff && !fgo && !bl && at) SPAN(1, 0, 0, 1, 1, 1, 1, SXV, 1); \
            else if (flat && depth && dw && tex && aff && fgo && !bl && at) SPAN(1, 1, 0, 1, 1, 1, 1, SXV, 1); \
            else if (flat && depth && dw && tex && aff && fgo && !bl && !at) SPAN(1, 1, 0, 0, 1, 1, 1, SXV, 1); \
            else if (flat && depth && dw && tex && !aff && !fgo && !bl && !at) SPAN(1, 0, 0, 0, 1, 1, 1, SXV, 0); \
            else if (flat && depth && dw && tex && !aff && !fgo && !bl && at) SPAN(1, 0, 0, 1, 1, 1, 1, SXV, 0); \
            else if (flat && depth && dw && tex && !aff && fgo && !bl && at) SPAN(1, 1, 0, 1, 1, 1, 1, SXV, 0); \
            else if (flat && depth && dw && tex && !aff && fgo && !bl && !at) SPAN(1, 1, 0, 0, 1, 1, 1, SXV, 0); \
            else if (flat && depth && dw && tex && !aff && !fgo && bl && at) SPAN(1, 0, 1, 1, 1, 1, 1, SXV, 0); \
            else if (flat && depth && dw && tex && !aff && fgo && bl && at) SPAN(1, 1, 1, 1, 1, 1, 1, SXV, 0); \
            else if (flat && depth && dw && !tex && !fgo && !bl && !at) SPAN(0, 0, 0, 0, 1, 1, 1, SXV, 0); \
            else if (flat && depth && dw && !tex && !fgo && bl && !at) SPAN(0, 0, 1, 0, 1, 1, 1, SXV, 0); \
            else SPAN(tex, fgo, bl, at, flat, depth, dw, SXV, aff);
            if (st == 2) { DISPATCH(2) } else { DISPATCH(1) }
#undef DISPATCH
#undef SPAN
            if (outside_band && y + 1 < g.fh) {                              /* the odd row repeats the even one (colour and depth) */
                int c0 = xa, c1 = xb + 1 < g.fw ? xb + 1 : g.fw - 1;
                memcpy(g.fb + (size_t)(y + 1) * g.fw + c0, g.fb + (size_t)y * g.fw + c0, (size_t)(c1 - c0 + 1) * 4);
                memcpy(g.zb + (size_t)(y + 1) * g.fw + c0, g.zb + (size_t)y * g.fw + c0, (size_t)(c1 - c0 + 1) * 4);
            }
        }
    }
}

static void raster_tri(const SV *p0, const SV *p1, const SV *p2) {
#ifdef MCGL_PROF
    uint64_t t0 = pf_now(); raster_tri_impl(p0, p1, p2); pf_raster += pf_now() - t0; pf_tris++;
#else
    raster_tri_impl(p0, p1, p2);
#endif
}

static void raster_line(const SV *p, const SV *q) {
    float dx = q->x - p->x, dy = q->y - p->y;
    int n = (int)fmaxf(fabsf(dx), fabsf(dy)) + 1; if (n > 4096) n = 4096;
    for (int i = 0; i <= n; ++i) {
        float t = (float)i / n; int x = (int)(p->x + dx * t), y = (int)(p->y + dy * t);
        if (x < 0 || y < 0 || x >= g.fw || y >= g.fh) continue;
        float z = p->z + (q->z - p->z) * t - 0.0005f;
        if (z < 0 || z > 1) continue;
        put_pixel(x, y, z, p->r + (q->r - p->r) * t, p->g + (q->g - p->g) * t, p->b + (q->b - p->b) * t, p->a + (q->a - p->a) * t, p->fog + (q->fog - p->fog) * t, NULL, 0, 0);
    }
}

static void to_screen(const CV *c, SV *s) {
    float iw = 1.0f / c->w;
    s->x = g.vx + (c->x * iw * 0.5f + 0.5f) * g.vw;
    s->y = g.fh - (g.vy + (c->y * iw * 0.5f + 0.5f) * g.vh);
    s->z = c->z * iw * 0.5f + 0.5f; s->iw = iw; s->uw = c->u * iw; s->vw = c->v * iw;
    s->r = c->r; s->g = c->g; s->b = c->b; s->a = c->a; s->fog = c->fog; s->u = c->u; s->v = c->v;
}

/* GL_SELECT: does the polygon (clip space) touch the clip volume's x/y square? */
static int poly_hits_view(const CV *p, int n, float *zmin, float *zmax) {
    float px[8], py[8], pz[8];
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f; *zmin = 1e30f; *zmax = -1e30f;
    for (int i = 0; i < n; ++i) {
        float iw = 1.0f / p[i].w; px[i] = p[i].x * iw; py[i] = p[i].y * iw; pz[i] = p[i].z * iw * 0.5f + 0.5f;
        if (px[i] < minx) minx = px[i]; if (px[i] > maxx) maxx = px[i]; if (py[i] < miny) miny = py[i]; if (py[i] > maxy) maxy = py[i];
        if (pz[i] < *zmin) *zmin = pz[i]; if (pz[i] > *zmax) *zmax = pz[i];
    }
    if (maxx < -1 || minx > 1 || maxy < -1 || miny > 1) return 0;
    if (n < 3) return 1;
    /* separating axis: polygon edge normals against the square's corners */
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n; float ex = px[j] - px[i], ey = py[j] - py[i];
        float nxv = -ey, nyv = ex; int outside = 1; float side = 0;
        for (int k = 0; k < n; ++k) { float s = nxv * (px[k] - px[i]) + nyv * (py[k] - py[i]); if (fabsf(s) > fabsf(side)) side = s; }
        for (int cx = -1; cx <= 1 && outside; cx += 2) for (int cy = -1; cy <= 1; cy += 2) {
            float s = nxv * (cx - px[i]) + nyv * (cy - py[i]);
            if (side * s >= 0) { outside = 0; break; }
        }
        if (outside) return 0;
    }
    return 1;
}

static void flush_hit(void) {
    if (g.rmode != GL_SELECT || !g.hit) return;
    g.hit = 0;
    if (g.nname == 0) return;
    if (g.sel_n + 3 + g.nname > g.sel_cap) return;
    GLuint *o = g.sel_buf + g.sel_n;
    o[0] = (GLuint)g.nname;
    o[1] = (GLuint)(clamp01(g.hit_zmin) * 4294967295.0f); o[2] = (GLuint)(clamp01(g.hit_zmax) * 4294967295.0f);
    for (int i = 0; i < g.nname; ++i) o[3 + i] = g.names[i];
    g.sel_n += 3 + g.nname; g.sel_hits++;
}

static void emit_poly(const CV *p, int n) {
    dbg_poly++;            /* a clipped convex polygon in clip space */
    if (g.rmode == GL_SELECT) {
        float zmin, zmax;
        if (poly_hits_view(p, n, &zmin, &zmax)) {
            if (!g.hit) { g.hit = 1; g.hit_zmin = zmin; g.hit_zmax = zmax; } else { if (zmin < g.hit_zmin) g.hit_zmin = zmin; if (zmax > g.hit_zmax) g.hit_zmax = zmax; }
        }
        return;
    }
    SV s[8];
    for (int i = 0; i < n; ++i) to_screen(&p[i], &s[i]);
    for (int i = 1; i + 1 < n; ++i) {
        if (g.flags & F_CULL) {
            float area = (s[i].x - s[0].x) * (s[i + 1].y - s[0].y) - (s[i + 1].x - s[0].x) * (s[i].y - s[0].y);   /* y down: positive = clockwise on screen = counter-clockwise in GL */
            int ccw = area < 0;                                   /* in y-down screen coordinates a GL counter-clockwise triangle has negative area */
            if (g.front == GL_CW) ccw = !ccw;
            int is_front = ccw;
            if (g.cull == GL_FRONT_AND_BACK) continue;
            if (g.cull == GL_BACK && !is_front) { dbg_cull++; continue; }
            if (g.cull == GL_FRONT && is_front) continue;
        }
        dbg_ras++; raster_tri(&s[0], &s[i], &s[i + 1]);
    }
}
void mcgl_stats(void) { fprintf(stderr, "poly=%d cull=%d raster=%d calls=%d verts=%d\n", dbg_poly, dbg_cull, dbg_ras, dbg_calls, dbg_verts); dbg_poly = dbg_cull = dbg_ras = 0; }

static CV lerp_cv(const CV *a, const CV *b, float t) {
    CV r; const float *pa = &a->x, *pb = &b->x; float *pr_ = &r.x;
    for (int i = 0; i < 11; ++i) pr_[i] = pa[i] + (pb[i] - pa[i]) * t;
    return r;
}
static int clip_near(const CV *in, int n, CV *out) {      /* z >= -w, and w > small */
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const CV *a = &in[i], *b = &in[(i + 1) % n];
        float da = a->z + a->w - 0.0001f, db = b->z + b->w - 0.0001f;
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) out[m++] = lerp_cv(a, b, da / (da - db));
    }
    return m;
}
static inline int outcode(const CV *v) {
    return (v->x < -v->w) | (v->x > v->w) << 1 | (v->y < -v->w) << 2 | (v->y > v->w) << 3 | (v->z > v->w) << 4;
}
static void tri_in(const CV *a, const CV *b, const CV *c) {
    if (g.rmode != GL_SELECT) {                                  /* wholly outside one side of the view volume: nothing to draw */
        int oa = outcode(a), ob = outcode(b), oc = outcode(c);
        if ((oa & ob & oc) && a->w > 0.0001f && b->w > 0.0001f && c->w > 0.0001f) return;
        if (a->w <= 0.0001f && b->w <= 0.0001f && c->w <= 0.0001f) return;
    }
    CV in[3] = { *a, *b, *c }, out[8];
    if (a->w > 0.0001f && b->w > 0.0001f && c->w > 0.0001f && a->z >= -a->w && b->z >= -b->w && c->z >= -c->w) { emit_poly(in, 3); return; }
    int n = clip_near(in, 3, out);
    if (n >= 3) emit_poly(out, n);
}
static void line_in(const CV *a, const CV *b) {
    CV p = *a, q = *b;
    float da = p.z + p.w - 0.0001f, db = q.z + q.w - 0.0001f;
    if (da < 0 && db < 0) return;
    if (da < 0) p = lerp_cv(&p, &q, da / (da - db)); else if (db < 0) q = lerp_cv(&p, &q, da / (da - db));
    if (g.rmode == GL_SELECT) return;
    SV s, t; to_screen(&p, &s); to_screen(&q, &t); raster_line(&s, &t);
}

static void light_color(const V *v, const float *mv, float *r, float *gg, float *b) {
    float nx = v->nx / 127.0f, ny = v->ny / 127.0f, nz = v->nz / 127.0f;
    float ex = mv[0] * nx + mv[4] * ny + mv[8] * nz, ey = mv[1] * nx + mv[5] * ny + mv[9] * nz, ez = mv[2] * nx + mv[6] * ny + mv[10] * nz;
    float len = sqrtf(ex * ex + ey * ey + ez * ez); if (len > 0) { ex /= len; ey /= len; ez /= len; }
    float d = ex * g.l_dir[0] + ey * g.l_dir[1] + ez * g.l_dir[2]; if (d < 0) d = 0;
    if (!(g.flags & F_LIGHT0)) d = 0;
    float mr = 0.8f, mg = 0.8f, mb = 0.8f;
    if (g.flags & F_COLMAT) { mr = *r; mg = *gg; mb = *b; }
    float lr = g.m_amb[0] + g.l_amb[0] + d * g.l_dif[0], lg = g.m_amb[1] + g.l_amb[1] + d * g.l_dif[1], lb = g.m_amb[2] + g.l_amb[2] + d * g.l_dif[2];
    *r = clamp01(mr * lr); *gg = clamp01(mg * lg); *b = clamp01(mb * lb);
}

/* e^-x for the fog: a table instead of expf() per vertex (x = density * distance, 0..8) */
static float s_exp_tab[513]; static int s_exp_init;
static inline float fast_exp_neg(float x) {
    if (!s_exp_init) { for (int i = 0; i <= 512; ++i) s_exp_tab[i] = expf(-(float)i / 64.0f); s_exp_init = 1; }
    if (x >= 8.0f) return 0.0f;
    float f = x * 64.0f; int k = (int)f; float fr = f - (float)k;
    return s_exp_tab[k] + (s_exp_tab[k + 1] - s_exp_tab[k]) * fr;
}

/* A triangle wholly in front of the near plane, in screen space: back-face culling first (it needs the x/y only), then the rasteriser. */
static inline int tri_front(const SV *a, const SV *b, const SV *c) {
    if (!(g.flags & F_CULL)) return 1;
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    int ccw = area < 0;                                       /* in y-down screen coordinates a GL counter-clockwise triangle has negative area */
    if (g.front == GL_CW) ccw = !ccw;
    if (g.cull == GL_FRONT_AND_BACK) return 0;
    if (g.cull == GL_BACK) return ccw;
    if (g.cull == GL_FRONT) return !ccw;
    return 1;
}

/* Work is done only for what is used: every vertex gets its clip position and an outcode; the screen position when a triangle that is not
 * rejected needs it; the colour, texture coordinates and fog only for a triangle that survives culling (about half of them are back faces). */
static void draw_verts_impl(int prim, const V *v, int n);
static void draw_verts(int prim, const V *v, int n) {
#ifdef MCGL_PROF
    uint64_t t0 = pf_now(); uint64_t r0 = pf_raster; draw_verts_impl(prim, v, n); pf_verts += pf_now() - t0 - (pf_raster - r0); pf_vcount += (uint64_t)n;
#else
    draw_verts_impl(prim, v, n);
#endif
}
static void draw_verts_impl(int prim, const V *v, int n) {
    if (!g.fb || n <= 0) return;
    static int dbg_env = -1; if (dbg_env < 0) dbg_env = getenv("MCGL_DEBUG") != NULL;
    if (dbg_env) { dbg_calls++; dbg_verts += n; }
    static CV *cvbuf; static SV *svbuf; static uint8_t *ocb, *sok; static int cvcap;
    if (n > cvcap) {
        free(cvbuf); free(svbuf); free(ocb); free(sok); cvcap = n + 256;
        cvbuf = malloc((size_t)cvcap * sizeof(CV)); svbuf = malloc((size_t)cvcap * sizeof(SV)); ocb = malloc((size_t)cvcap); sok = malloc((size_t)cvcap);
        if (!cvbuf || !svbuf || !ocb || !sok) { cvcap = 0; return; }
    }
    CV *cv = cvbuf; SV *sv = svbuf;
    const float *mv = g.mv[g.mvd], *pr = g.pr[g.prd];
    float mvp[16]; mat_mul(mvp, pr, mv);                          /* clip = P * MV * v in one matrix */
    const int lighting = (g.flags & F_LIGHT) != 0, fogon = (g.flags & F_FOG) != 0;
    const float farcap = (g.rmode != GL_SELECT && pr[15] == 0.0f) ? s_far_cap : 1e30f;           /* perspective only: the GUI is not a distance */
    const float fog_end = (g.fog_mode == GL_LINEAR && s_far_cap < g.fog_end && pr[15] == 0.0f) ? s_far_cap : g.fog_end;
    const float k255 = 1.0f / 255;
    const float vx0 = (float)g.vx, vw_ = (float)g.vw, vy0 = (float)g.vy, vh_ = (float)g.vh, fhf = (float)g.fh;
    /* With a perspective projection a polygon faces the viewer when the eye is on the front side of its plane: that is decided in object space, before
     * any of its four vertices is transformed (about half of the faces of a chunk look away). */
    int objcull = 0; float eye[3] = { 0, 0, 0 }; int detpos = 1;
    if (g.rmode != GL_SELECT && (g.flags & F_CULL) && g.cull != GL_FRONT_AND_BACK && (prim == GL_QUADS || prim == GL_TRIANGLES) && pr[15] == 0.0f && pr[11] != 0.0f) {
        float a0 = mv[0], a1 = mv[4], a2 = mv[8], b0 = mv[1], b1 = mv[5], b2 = mv[9], c0 = mv[2], c1 = mv[6], c2 = mv[10];
        float det = a0 * (b1 * c2 - b2 * c1) - a1 * (b0 * c2 - b2 * c0) + a2 * (b0 * c1 - b1 * c0);
        if (det > 1e-9f || det < -1e-9f) {
            float id = 1.0f / det;
            float i00 = (b1 * c2 - b2 * c1) * id, i01 = (a2 * c1 - a1 * c2) * id, i02 = (a1 * b2 - a2 * b1) * id;
            float i10 = (b2 * c0 - b0 * c2) * id, i11 = (a0 * c2 - a2 * c0) * id, i12 = (a2 * b0 - a0 * b2) * id;
            float i20 = (b0 * c1 - b1 * c0) * id, i21 = (a1 * c0 - a0 * c1) * id, i22 = (a0 * b1 - a1 * b0) * id;
            float tx = mv[12], ty = mv[13], tz = mv[14];
            eye[0] = -(i00 * tx + i01 * ty + i02 * tz); eye[1] = -(i10 * tx + i11 * ty + i12 * tz); eye[2] = -(i20 * tx + i21 * ty + i22 * tz);
            detpos = det > 0; objcull = 1;
        }
    }
    const int step = prim == GL_QUADS ? 4 : prim == GL_TRIANGLES ? 3 : 1;
    int wantfront = (g.cull == GL_BACK);
    for (int i = 0; i < n; ++i) {
        if (objcull && i % step == 0 && i + step <= n) {
            const V *p0 = &v[i], *p1 = &v[i + 1], *p2 = &v[i + 2];
            float e1x = p1->x - p0->x, e1y = p1->y - p0->y, e1z = p1->z - p0->z, e2x = p2->x - p0->x, e2y = p2->y - p0->y, e2z = p2->z - p0->z;
            float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
            float dd = nx * (eye[0] - p0->x) + ny * (eye[1] - p0->y) + nz * (eye[2] - p0->z);
            int is_front = ((dd > 0) == (detpos != 0)) == (g.front == GL_CCW);
            if (is_front != wantfront) { for (int k = 0; k < step; ++k) { ocb[i + k] = 0x20; sok[i + k] = 0; } i += step - 1; continue; }
        }
        const V *s = &v[i]; CV *d = &cv[i];
        float x = s->x, y = s->y, z = s->z;
        d->x = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12]; d->y = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
        d->z = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14]; d->w = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
        ocb[i] = (uint8_t)(outcode(d) | (d->w > farcap ? 0x10 : 0) | (d->w <= 0.0001f ? 0x80 : 0) | (d->z < -d->w ? 0x40 : 0));     /* 0x80: behind the eye, 0x40: in front of the near plane's side; 0x10 is also "beyond the view distance cap" */
        sok[i] = 0;
    }
    /* attributes of vertex i into the clip-space copy (also what clipping works from) */
#define CV_ATTR(i) do { if (!(sok[i] & 4)) { const V *s_ = &v[i]; CV *d_ = &cv[i]; \
        d_->u = s_->u; d_->v = s_->v; d_->r = s_->r * k255; d_->g = s_->g * k255; d_->b = s_->b * k255; d_->a = s_->a * k255; \
        if (lighting) light_color(s_, mv, &d_->r, &d_->g, &d_->b); \
        d_->fog = 1; \
        if (fogon) { \
            float ez = mv[2] * s_->x + mv[6] * s_->y + mv[10] * s_->z + mv[14]; \
            float dist = ez < 0 ? -ez : ez, f; \
            if (g.fog_mode == GL_LINEAR) f = (fog_end - dist) / (fog_end - g.fog_start); \
            else if (g.fog_mode == GL_EXP2) f = fast_exp_neg((g.fog_density * dist) * (g.fog_density * dist)); \
            else f = fast_exp_neg(g.fog_density * dist); \
            d_->fog = clamp01(f); } \
        sok[i] |= 4; } } while (0)
    int select = g.rmode == GL_SELECT;
#define POS(i) do { if (!(sok[i] & 1)) { float iw_ = 1.0f / cv[i].w; \
        sv[i].x = vx0 + (cv[i].x * iw_ * 0.5f + 0.5f) * vw_; sv[i].y = fhf - (vy0 + (cv[i].y * iw_ * 0.5f + 0.5f) * vh_); \
        sv[i].z = cv[i].z * iw_ * 0.5f + 0.5f; sv[i].iw = iw_; sok[i] |= 1; } } while (0)
#define ATTR(i) do { if (!(sok[i] & 2)) { CV_ATTR(i); float iw_ = sv[i].iw; \
        sv[i].uw = cv[i].u * iw_; sv[i].vw = cv[i].v * iw_; sv[i].r = cv[i].r; sv[i].g = cv[i].g; sv[i].b = cv[i].b; sv[i].a = cv[i].a; sv[i].fog = cv[i].fog; sv[i].u = cv[i].u; sv[i].v = cv[i].v; sok[i] |= 2; } } while (0)
#define TRI(i0, i1, i2) do { \
        int o0 = ocb[i0], o1 = ocb[i1], o2 = ocb[i2]; \
        if ((o0 | o1 | o2) & 0x20) break;                                  /* a face that looks away */ \
        if (!select && !((o0 | o1 | o2) & 0xC0)) { \
            if (o0 & o1 & o2 & 0x1F) break;                                /* wholly outside one side of the view volume */ \
            POS(i0); POS(i1); POS(i2); \
            if (!tri_front(&sv[i0], &sv[i1], &sv[i2])) break; \
            ATTR(i0); ATTR(i1); ATTR(i2); \
            raster_tri(&sv[i0], &sv[i1], &sv[i2]); \
        } else { CV_ATTR(i0); CV_ATTR(i1); CV_ATTR(i2); tri_in(&cv[i0], &cv[i1], &cv[i2]); } \
    } while (0)
    switch (prim) {
        case GL_TRIANGLES: for (int i = 0; i + 2 < n; i += 3) TRI(i, i + 1, i + 2); break;
        case GL_QUADS: for (int i = 0; i + 3 < n; i += 4) { TRI(i, i + 1, i + 2); TRI(i, i + 2, i + 3); } break;
        case GL_TRIANGLE_STRIP: for (int i = 0; i + 2 < n; ++i) { if (i & 1) TRI(i + 1, i, i + 2); else TRI(i, i + 1, i + 2); } break;
        case GL_TRIANGLE_FAN: for (int i = 1; i + 1 < n; ++i) TRI(0, i, i + 1); break;
        case GL_LINES: for (int i = 0; i + 1 < n; i += 2) { CV_ATTR(i); CV_ATTR(i + 1); line_in(&cv[i], &cv[i + 1]); } break;
        case GL_LINE_STRIP: for (int i = 0; i + 1 < n; ++i) { CV_ATTR(i); CV_ATTR(i + 1); line_in(&cv[i], &cv[i + 1]); } break;
        case GL_LINE_LOOP: for (int i = 0; i < n; ++i) { CV_ATTR(i); CV_ATTR((i + 1) % n); line_in(&cv[i], &cv[(i + 1) % n]); } break;
        default: break;
    }
#undef TRI
#undef ATTR
#undef POS
#undef CV_ATTR
}

/* ------------------------------------------------------------------ command execution (also what display lists replay) */
static void run_list(int id);

/* ---- front-to-back drawing of display lists with occlusion culling ----------------------------------------------------------------------------
 * The game calls the display lists of its chunks one after another, in no useful order. While the state is the plain opaque one (depth test, no blending) such
 * calls are only queued; any other command first draws the queue: nearest lists first, and once the nearest few are down a coarse map of the farthest depth in
 * every 16x16 pixel tile is made from the depth buffer - a list whose box lies wholly behind what is already there is skipped without a vertex being transformed. */
typedef struct { int id; float key; float mv[16]; } QCall;
static QCall *q_calls; static int q_n, q_cap;
static int q_busy;
#ifdef MCGL_PROF
static uint64_t pf_occl;
static uint64_t pf_occl_get(void) { uint64_t v = pf_occl; pf_occl = 0; return v; }
#endif
static uint32_t *q_tile; static int q_tw, q_th, q_tile_cap;
static void run_list_now(int id);
static void q_build_tiles(void) {
    const int T = 16; q_tw = (g.fw + T - 1) / T; q_th = (g.fh + T - 1) / T;
    if (q_tw * q_th > q_tile_cap) { free(q_tile); q_tile_cap = q_tw * q_th; q_tile = malloc((size_t)q_tile_cap * 4); if (!q_tile) { q_tile_cap = 0; return; } }
    for (int ty = 0; ty < q_th; ++ty) for (int tx = 0; tx < q_tw; ++tx) {
        int x1 = (tx + 1) * T < g.fw ? (tx + 1) * T : g.fw, y1 = (ty + 1) * T < g.fh ? (ty + 1) * T : g.fh;
        uint32_t mx = 0;
        for (int y = ty * T; y < y1; ++y) { const uint32_t *row = g.zb + (size_t)y * g.fw; for (int x = tx * T; x < x1; ++x) if (row[x] > mx) mx = row[x]; }
        q_tile[ty * q_tw + tx] = mx;
    }
}
/* is the box of this list (under the matrix the call was made with) wholly behind what the tiles hold? */
static int q_occluded(const List *l, const float *mv) {
    float m[16]; mat_mul(m, g.pr[g.prd], mv);
    float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f, zmin = 1e30f;
    for (int k = 0; k < 8; ++k) {
        float x = (k & 1) ? l->bmax[0] : l->bmin[0], y = (k & 2) ? l->bmax[1] : l->bmin[1], z = (k & 4) ? l->bmax[2] : l->bmin[2];
        float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
        if (cw <= 0.01f) return 0;                                                   /* reaches the eye plane: keep it */
        float cx = m[0] * x + m[4] * y + m[8] * z + m[12], cy = m[1] * x + m[5] * y + m[9] * z + m[13], cz = m[2] * x + m[6] * y + m[10] * z + m[14];
        float iw = 1.0f / cw, nx = cx * iw, ny = cy * iw, nz = cz * iw * 0.5f + 0.5f;
        if (nx < xmin) xmin = nx; if (nx > xmax) xmax = nx; if (ny < ymin) ymin = ny; if (ny > ymax) ymax = ny; if (nz < zmin) zmin = nz;
    }
    if (zmin <= 0.0f) return 0;
    float fw = (float)g.fw, fh = (float)g.fh;
    int px0 = (int)((xmin * 0.5f + 0.5f) * (float)g.vw + (float)g.vx), px1 = (int)((xmax * 0.5f + 0.5f) * (float)g.vw + (float)g.vx);
    int py0 = (int)(fh - ((ymax * 0.5f + 0.5f) * (float)g.vh + (float)g.vy)), py1 = (int)(fh - ((ymin * 0.5f + 0.5f) * (float)g.vh + (float)g.vy));
    (void)fw;
    if (px0 < 0) px0 = 0; if (py0 < 0) py0 = 0; if (px1 >= g.fw) px1 = g.fw - 1; if (py1 >= g.fh) py1 = g.fh - 1;
    if (px0 > px1 || py0 > py1) return 0;
    uint32_t zn = zfix(zmin - 0.00002f);                                              /* a little margin */
    for (int ty = py0 / 16; ty <= py1 / 16; ++ty) for (int tx = px0 / 16; tx <= px1 / 16; ++tx) if (q_tile[ty * q_tw + tx] >= zn) return 0;
    return 1;
}
static int q_cmp(const void *a, const void *b) { float x = ((const QCall *)a)->key, y = ((const QCall *)b)->key; return x < y ? -1 : x > y; }
static void q_flush(void) {
    int n = q_n; q_n = 0;
    if (n <= 0 || q_busy) return;
    q_busy = 1;
    if (n > 1) qsort(q_calls, (size_t)n, sizeof(QCall), q_cmp);
    float saved[16]; memcpy(saved, g.mv[g.mvd], sizeof saved);
    const int use_occ = n >= 12 && g.pr[g.prd][15] == 0.0f;
    int warm1 = n / 5 > 6 ? n / 5 : 6, warm2 = n / 2, have_tiles = 0;
    for (int i = 0; i < n; ++i) {
        const QCall *qc = &q_calls[i];
        if (use_occ && (i == warm1 || i == warm2)) { q_build_tiles(); have_tiles = q_tile_cap > 0; }
        if (have_tiles && i >= warm1 && q_occluded(&g.lists[qc->id], qc->mv)) {
#ifdef MCGL_PROF
            pf_occl++;
#endif
            continue;
        }
        memcpy(g.mv[g.mvd], qc->mv, sizeof saved);
        run_list_now(qc->id);
    }
    memcpy(g.mv[g.mvd], saved, sizeof saved);
    q_busy = 0;
}
void mcgl_flush_queue(void) { if (q_n) q_flush(); }
/* may this call wait in the queue? a list of plain geometry, in the opaque depth-tested state, with a 3-D projection, at the top level */
static int q_eligible(const List *l) {
    return l->has_box && !l->impure && g.call_depth == 0 && !q_busy && g.rmode != GL_SELECT && (g.flags & F_DEPTH) && !(g.flags & F_BLEND) && g.dmask
        && (g.dfunc == GL_LEQUAL || g.dfunc == GL_LESS) && g.pr[g.prd][15] == 0.0f && g.pr[g.prd][11] != 0.0f;
}

static void exec(const Cmd *c) {
    float *m;
    if (q_n && c->op != OP_CALL) q_flush();
    switch (c->op) {
        case OP_ENABLE: case OP_DISABLE: {
            unsigned f = 0;
            switch (c->a) {
                case GL_TEXTURE_2D: f = F_TEX; break; case GL_DEPTH_TEST: f = F_DEPTH; break; case GL_BLEND: f = F_BLEND; break; case GL_ALPHA_TEST: f = F_ALPHA; break;
                case GL_CULL_FACE: f = F_CULL; break; case GL_FOG: f = F_FOG; break; case GL_LIGHTING: f = F_LIGHT; break; case GL_COLOR_MATERIAL: f = F_COLMAT; break;
                case GL_LIGHT0: f = F_LIGHT0; break; case GL_NORMALIZE: case GL_RESCALE_NORMAL: f = F_NORM; break;
                default: return;
            }
            if (c->op == OP_ENABLE) g.flags |= f; else g.flags &= ~f;
            return; }
        case OP_MATMODE: g.mode = c->a; return;
        case OP_IDENT: mat_ident(cur()); return;
        case OP_PUSH: if (g.mode == GL_PROJECTION) { if (g.prd < 15) { memcpy(g.pr[g.prd + 1], g.pr[g.prd], 64); g.prd++; } } else if (g.mvd < 63) { memcpy(g.mv[g.mvd + 1], g.mv[g.mvd], 64); g.mvd++; } return;
        case OP_POP: if (g.mode == GL_PROJECTION) { if (g.prd > 0) g.prd--; } else if (g.mvd > 0) g.mvd--; return;
        case OP_TRANS: { float t[16]; mat_ident(t); t[12] = c->f[0]; t[13] = c->f[1]; t[14] = c->f[2]; m = cur(); mat_mul(m, m, t); return; }
        case OP_SCALE: { float t[16]; mat_ident(t); t[0] = c->f[0]; t[5] = c->f[1]; t[10] = c->f[2]; m = cur(); mat_mul(m, m, t); return; }
        case OP_ROT: {
            float ang = c->f[0] * 3.14159265358979f / 180.0f, x = c->f[1], y = c->f[2], z = c->f[3];
            float len = sqrtf(x * x + y * y + z * z); if (len == 0) return; x /= len; y /= len; z /= len;
            float cs = cosf(ang), sn = sinf(ang), t = 1 - cs, r[16]; mat_ident(r);
            r[0] = t * x * x + cs; r[4] = t * x * y - sn * z; r[8] = t * x * z + sn * y;
            r[1] = t * x * y + sn * z; r[5] = t * y * y + cs; r[9] = t * y * z - sn * x;
            r[2] = t * x * z - sn * y; r[6] = t * y * z + sn * x; r[10] = t * z * z + cs;
            m = cur(); mat_mul(m, m, r); return; }
        case OP_ORTHO: { float l = c->f[0], r_ = c->f[1], b = c->f[2], t_ = c->f[3], n = c->f[4], f = c->f[5], o[16]; mat_ident(o);
            o[0] = 2 / (r_ - l); o[5] = 2 / (t_ - b); o[10] = -2 / (f - n); o[12] = -(r_ + l) / (r_ - l); o[13] = -(t_ + b) / (t_ - b); o[14] = -(f + n) / (f - n);
            m = cur(); mat_mul(m, m, o); return; }
        case OP_FRUSTUM: { float l = c->f[0], r_ = c->f[1], b = c->f[2], t_ = c->f[3], n = c->f[4], f = c->f[5], o[16]; memset(o, 0, 64);
            o[0] = 2 * n / (r_ - l); o[5] = 2 * n / (t_ - b); o[8] = (r_ + l) / (r_ - l); o[9] = (t_ + b) / (t_ - b); o[10] = -(f + n) / (f - n); o[11] = -1; o[14] = -2 * f * n / (f - n);
            m = cur(); mat_mul(m, m, o); return; }
        case OP_MULT: m = cur(); mat_mul(m, m, c->f); return;
        case OP_LOAD: memcpy(cur(), c->f, 64); return;
        case OP_COLOR: g.cr = c->f[0]; g.cg = c->f[1]; g.cb = c->f[2]; g.ca = c->f[3]; return;
        case OP_NORMAL: g.nx = c->f[0]; g.ny = c->f[1]; g.nz = c->f[2]; return;
        case OP_BIND: g.bound = c->a; return;
        case OP_BLEND: g.sfac = c->a; g.dfac = c->b; return;
        case OP_ALPHAF: g.afunc = c->a; g.aref = c->f[0]; return;
        case OP_DEPTHF: g.dfunc = c->a; return;
        case OP_DEPTHM: g.dmask = c->a; return;
        case OP_COLORM: g.cmask = c->a; return;
        case OP_CULLF: g.cull = c->a; return;
        case OP_FRONT: g.front = c->a; return;
        case OP_FOGF: if (c->a == GL_FOG_DENSITY) g.fog_density = c->f[0]; else if (c->a == GL_FOG_START) g.fog_start = c->f[0]; else if (c->a == GL_FOG_END) g.fog_end = c->f[0]; else if (c->a == GL_FOG_MODE) g.fog_mode = (int)c->f[0]; return;
        case OP_FOGC: memcpy(g.fog_col, c->f, 16); return;
        case OP_LIGHT:
            if (c->a == GL_AMBIENT) memcpy(g.l_amb, c->f, 16);
            else if (c->a == GL_DIFFUSE) memcpy(g.l_dif, c->f, 16);
            else if (c->a == GL_POSITION) {                     /* kept as a direction in eye space */
                const float *mv = g.mv[g.mvd]; float x = c->f[0], y = c->f[1], z = c->f[2];
                float ex = mv[0] * x + mv[4] * y + mv[8] * z, ey = mv[1] * x + mv[5] * y + mv[9] * z, ez = mv[2] * x + mv[6] * y + mv[10] * z;
                float len = sqrtf(ex * ex + ey * ey + ez * ez); if (len == 0) len = 1;
                g.l_dir[0] = ex / len; g.l_dir[1] = ey / len; g.l_dir[2] = ez / len;
            }
            return;
        case OP_LMODEL: memcpy(g.m_amb, c->f, 16); return;
        case OP_CMAT: g.cmat_target = c->b; return;
        case OP_VIEWPORT: g.vx = c->a; g.vy = c->b; g.vw = (int)c->f[0]; g.vh = (int)c->f[1]; return;
        case OP_VERTS: draw_verts(c->a, c->v, c->n); return;
        case OP_CALL: {
            int id = c->a;
            if (id > 0 && id < g.nlists && q_eligible(&g.lists[id])) {
                if (q_n == q_cap) { q_cap = q_cap ? q_cap * 2 : 256; q_calls = realloc(q_calls, (size_t)q_cap * sizeof(QCall)); }
                QCall *qc = &q_calls[q_n++]; qc->id = id; memcpy(qc->mv, g.mv[g.mvd], sizeof qc->mv);
                /* the key: the depth (clip w) of the middle of the list's box */
                const float *mvp_ = qc->mv; const List *l = &g.lists[id];
                float cx = (l->bmin[0] + l->bmax[0]) * 0.5f, cy = (l->bmin[1] + l->bmax[1]) * 0.5f, cz = (l->bmin[2] + l->bmax[2]) * 0.5f;
                qc->key = -(mvp_[2] * cx + mvp_[6] * cy + mvp_[10] * cz + mvp_[14]);                 /* the distance in front of the eye */
                return;
            }
            if (q_n) q_flush();
            run_list(id); return;
        }
        case OP_NAME_INIT: flush_hit(); g.nname = 0; return;
        case OP_NAME_PUSH: flush_hit(); if (g.nname < 64) g.names[g.nname++] = (GLuint)c->a; return;
        case OP_NAME_POP: flush_hit(); if (g.nname > 0) g.nname--; return;
        case OP_NAME_LOAD: flush_hit(); if (g.nname > 0) g.names[g.nname - 1] = (GLuint)c->a; return;
    }
}

static void run_list(int id) { run_list_now(id); }
static void run_list_now(int id) {
    if (id <= 0 || id >= g.nlists || g.call_depth > 8) return;
    List *l = &g.lists[id];
    if (l->has_box && !l->impure && g.rmode != GL_SELECT && g.pr[g.prd][15] == 0.0f && g.pr[g.prd][11] != 0.0f) {
        const float *mv = g.mv[g.mvd], *pr = g.pr[g.prd];
        float m[16]; mat_mul(m, pr, mv);
        int all = 0x1F, behind = 0;
        for (int k = 0; k < 8; ++k) {
            float x = (k & 1) ? l->bmax[0] : l->bmin[0], y = (k & 2) ? l->bmax[1] : l->bmin[1], z = (k & 4) ? l->bmax[2] : l->bmin[2];
            float cx = m[0] * x + m[4] * y + m[8] * z + m[12], cy = m[1] * x + m[5] * y + m[9] * z + m[13];
            float cz = m[2] * x + m[6] * y + m[10] * z + m[14], cw = m[3] * x + m[7] * y + m[11] * z + m[15];
            if (cw <= 0.0001f) { behind = 1; break; }                       /* the box reaches the eye plane: leave it to the per-triangle clipping */
            int oc = (cx < -cw) | (cx > cw) << 1 | (cy < -cw) << 2 | (cy > cw) << 3 | ((cz > cw) | (cw > s_far_cap)) << 4;
            all &= oc;
        }
        if (!behind && all) return;
    }
    g.call_depth++;
    for (int i = 0; i < l->n; ++i) exec(&l->c[i]);
    g.call_depth--;
}
static void list_clear(List *l) { for (int i = 0; i < l->n; ++i) free(l->c[i].v); free(l->c); memset(l, 0, sizeof *l); }

static int cur_list;
/* record into the open display list, and run now unless it is GL_COMPILE only */
static void put(const Cmd *c) {
    ensure_init();
    if (g.compiling) {
        List *l = &g.lists[cur_list];
        if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 32; l->c = realloc(l->c, (size_t)l->cap * sizeof(Cmd)); }
        l->c[l->n++] = *c;
        if (c->op == OP_VERTS) {
            for (int i = 0; i < c->n; ++i) {
                const V *v = &c->v[i];
                if (!l->has_box) { l->bmin[0] = l->bmax[0] = v->x; l->bmin[1] = l->bmax[1] = v->y; l->bmin[2] = l->bmax[2] = v->z; l->has_box = 1; continue; }
                if (v->x < l->bmin[0]) l->bmin[0] = v->x; if (v->x > l->bmax[0]) l->bmax[0] = v->x;
                if (v->y < l->bmin[1]) l->bmin[1] = v->y; if (v->y > l->bmax[1]) l->bmax[1] = v->y;
                if (v->z < l->bmin[2]) l->bmin[2] = v->z; if (v->z > l->bmax[2]) l->bmax[2] = v->z;
            }
        } else if (c->op != OP_BIND && c->op != OP_COLOR && c->op != OP_NORMAL) l->impure = 1;       /* state changes: the list is run whatever the box says */
        if (!g.exec_too) return;
    }
    exec(c);
}
static void cmd_f(int op, int a, int b, float f0, float f1, float f2, float f3) {
    Cmd c; memset(&c, 0, sizeof c); c.op = op; c.a = a; c.b = b; c.f[0] = f0; c.f[1] = f1; c.f[2] = f2; c.f[3] = f3; put(&c);
}
static void cmd_v4(int op, int a, int b, const float *f) { Cmd c; memset(&c, 0, sizeof c); c.op = op; c.a = a; c.b = b; memcpy(c.f, f, 16); put(&c); }

/* ------------------------------------------------------------------ GL API */
void glEnable(GLenum c) { cmd_f(OP_ENABLE, (int)c, 0, 0, 0, 0, 0); }
void glDisable(GLenum c) { cmd_f(OP_DISABLE, (int)c, 0, 0, 0, 0, 0); }
void glMatrixMode(GLenum m) { cmd_f(OP_MATMODE, (int)m, 0, 0, 0, 0, 0); }
void glLoadIdentity(void) { cmd_f(OP_IDENT, 0, 0, 0, 0, 0, 0); }
void glPushMatrix(void) { cmd_f(OP_PUSH, 0, 0, 0, 0, 0, 0); }
void glPopMatrix(void) { cmd_f(OP_POP, 0, 0, 0, 0, 0, 0); }
void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { cmd_f(OP_TRANS, 0, 0, x, y, z, 0); }
void glTranslated(GLdouble x, GLdouble y, GLdouble z) { glTranslatef((float)x, (float)y, (float)z); }
void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) { cmd_f(OP_ROT, 0, 0, a, x, y, z); }
void glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) { glRotatef((float)a, (float)x, (float)y, (float)z); }
void glScalef(GLfloat x, GLfloat y, GLfloat z) { cmd_f(OP_SCALE, 0, 0, x, y, z, 0); }
void glScaled(GLdouble x, GLdouble y, GLdouble z) { glScalef((float)x, (float)y, (float)z); }
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    Cmd c; memset(&c, 0, sizeof c); c.op = OP_ORTHO; c.f[0] = (float)l; c.f[1] = (float)r; c.f[2] = (float)b; c.f[3] = (float)t; c.f[4] = (float)n; c.f[5] = (float)f; put(&c);
}
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    Cmd c; memset(&c, 0, sizeof c); c.op = OP_FRUSTUM; c.f[0] = (float)l; c.f[1] = (float)r; c.f[2] = (float)b; c.f[3] = (float)t; c.f[4] = (float)n; c.f[5] = (float)f; put(&c);
}
void glLoadMatrixf(const GLfloat *m) { Cmd c; memset(&c, 0, sizeof c); c.op = OP_LOAD; memcpy(c.f, m, 64); put(&c); }
void glMultMatrixf(const GLfloat *m) { Cmd c; memset(&c, 0, sizeof c); c.op = OP_MULT; memcpy(c.f, m, 64); put(&c); }

static void im_push(float x, float y, float z) {
    if (!g.in_begin) return;
    if (g.nim == g.capim) { g.capim = g.capim ? g.capim * 2 : 256; g.im = realloc(g.im, (size_t)g.capim * sizeof(V)); }
    V *v = &g.im[g.nim++];
    v->x = x; v->y = y; v->z = z; v->u = g.cu; v->v = g.cv;
    v->r = (uint8_t)(clamp01(g.cr) * 255 + 0.5f); v->g = (uint8_t)(clamp01(g.cg) * 255 + 0.5f); v->b = (uint8_t)(clamp01(g.cb) * 255 + 0.5f); v->a = (uint8_t)(clamp01(g.ca) * 255 + 0.5f);
    v->nx = (int8_t)(g.nx * 127); v->ny = (int8_t)(g.ny * 127); v->nz = (int8_t)(g.nz * 127);
}
static void submit(int prim, V *copy, int n) {                 /* takes ownership of copy */
    Cmd c; memset(&c, 0, sizeof c); c.op = OP_VERTS; c.a = prim; c.v = copy; c.n = n;
    if (g.compiling) { put(&c); return; }
    exec(&c); free(copy);
}
void glBegin(GLenum prim) { ensure_init(); g.in_begin = 1; g.prim = (int)prim; g.nim = 0; }
void glEnd(void) {
    if (!g.in_begin) return; g.in_begin = 0;
    if (g.nim <= 0) return;
    V *copy = malloc((size_t)g.nim * sizeof(V)); if (!copy) return;
    memcpy(copy, g.im, (size_t)g.nim * sizeof(V)); submit(g.prim, copy, g.nim);
}
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { im_push(x, y, z); }
void glVertex2f(GLfloat x, GLfloat y) { im_push(x, y, 0); }
void glTexCoord2f(GLfloat u, GLfloat v) { g.cu = u; g.cv = v; }
void glColor4f(GLfloat r, GLfloat gg, GLfloat b, GLfloat a) {
    ensure_init();
    if (g.in_begin) { g.cr = r; g.cg = gg; g.cb = b; g.ca = a; return; }
    cmd_f(OP_COLOR, 0, 0, r, gg, b, a);
}
void glColor3f(GLfloat r, GLfloat gg, GLfloat b) { glColor4f(r, gg, b, 1); }
void glColor3ub(GLubyte r, GLubyte gg, GLubyte b) { glColor4f(r / 255.0f, gg / 255.0f, b / 255.0f, 1); }
void glColor4ub(GLubyte r, GLubyte gg, GLubyte b, GLubyte a) { glColor4f(r / 255.0f, gg / 255.0f, b / 255.0f, a / 255.0f); }
void glNormal3f(GLfloat x, GLfloat y, GLfloat z) { ensure_init(); if (g.in_begin) { g.nx = x; g.ny = y; g.nz = z; } else cmd_f(OP_NORMAL, 0, 0, x, y, z, 0); }

void glBindTexture(GLenum t, GLuint id) { (void)t; cmd_f(OP_BIND, (int)id, 0, 0, 0, 0, 0); }
void glGenTextures(GLsizei n, GLuint *out) {
    ensure_init();
    for (int i = 0; i < n; ++i) {
        if (g.ntex == g.captex) { g.captex *= 2; g.tex = realloc(g.tex, (size_t)g.captex * sizeof(Tex)); memset(g.tex + g.ntex, 0, (size_t)(g.captex - g.ntex) * sizeof(Tex)); }
        g.tex[g.ntex].wrap_s = g.tex[g.ntex].wrap_t = GL_REPEAT; out[i] = (GLuint)g.ntex++;
    }
}
void glDeleteTextures(GLsizei n, const GLuint *ids) { for (int i = 0; i < n; ++i) if (ids[i] > 0 && (int)ids[i] < g.ntex) { free(g.tex[ids[i]].px); g.tex[ids[i]].px = NULL; } }
void glTexParameteri(GLenum t, GLenum p, GLint v) {
    (void)t; if (g.bound <= 0 || g.bound >= g.ntex) return;
    if (p == GL_TEXTURE_WRAP_S) g.tex[g.bound].wrap_s = v == GL_REPEAT ? GL_REPEAT : GL_CLAMP;
    else if (p == GL_TEXTURE_WRAP_T) g.tex[g.bound].wrap_t = v == GL_REPEAT ? GL_REPEAT : GL_CLAMP;
}
static void tex_store(Tex *t, int x0, int y0, int w, int h, GLenum fmt, const uint8_t *src) {
    int bpp = fmt == GL_RGB ? 3 : 4;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const uint8_t *p = src + ((size_t)y * w + x) * bpp;
        t->px[(size_t)(y0 + y) * t->w + x0 + x] = (uint32_t)(bpp == 4 ? p[3] : 255) << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
}
void glTexImage2D(GLenum tg, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *data) {
    (void)tg; (void)ifmt; (void)border; (void)type;
    if (level != 0 || g.bound <= 0 || g.bound >= g.ntex || w <= 0 || h <= 0) return;
    Tex *t = &g.tex[g.bound]; free(t->px);
    t->w = w; t->h = h; t->px = calloc((size_t)w * h, 4);
    if (t->px && data) tex_store(t, 0, 0, w, h, fmt, data);
}
void glTexSubImage2D(GLenum tg, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *data) {
    (void)tg; (void)type;
    if (level != 0 || g.bound <= 0 || g.bound >= g.ntex || !data) return;
    Tex *t = &g.tex[g.bound];
    if (!t->px || x < 0 || y < 0 || x + w > t->w || y + h > t->h) return;
    tex_store(t, x, y, w, h, fmt, data);
}
int gluBuild2DMipmaps(GLenum tg, GLint comps, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *data) { glTexImage2D(tg, 0, comps, w, h, 0, fmt, type, data); return 0; }
void glPixelStorei(GLenum p, GLint v) { (void)p; (void)v; }

void glBlendFunc(GLenum s, GLenum d) { cmd_f(OP_BLEND, (int)s, (int)d, 0, 0, 0, 0); }
void glAlphaFunc(GLenum f, GLclampf r) { cmd_f(OP_ALPHAF, (int)f, 0, r, 0, 0, 0); }
void glDepthFunc(GLenum f) { cmd_f(OP_DEPTHF, (int)f, 0, 0, 0, 0, 0); }
void glDepthMask(GLboolean m) { cmd_f(OP_DEPTHM, m ? 1 : 0, 0, 0, 0, 0, 0); }
void glColorMask(GLboolean r, GLboolean gg, GLboolean b, GLboolean a) { cmd_f(OP_COLORM, (r || gg || b || a) ? 1 : 0, 0, 0, 0, 0, 0); }
void glCullFace(GLenum m) { cmd_f(OP_CULLF, (int)m, 0, 0, 0, 0, 0); }
void glFrontFace(GLenum m) { cmd_f(OP_FRONT, (int)m, 0, 0, 0, 0, 0); }
void glShadeModel(GLenum m) { (void)m; }
void glHint(GLenum t, GLenum m) { (void)t; (void)m; }
void glLineWidth(GLfloat w) { (void)w; }
void glFogf(GLenum p, GLfloat v) { cmd_f(OP_FOGF, (int)p, 0, v, 0, 0, 0); }
void glFogi(GLenum p, GLint v) { cmd_f(OP_FOGF, (int)p, 0, (float)v, 0, 0, 0); }
void glFogfv(GLenum p, const GLfloat *v) { if (p == GL_FOG_COLOR) cmd_v4(OP_FOGC, 0, 0, v); else cmd_f(OP_FOGF, (int)p, 0, v[0], 0, 0, 0); }
void glLightfv(GLenum l, GLenum p, const GLfloat *v) { (void)l; cmd_v4(OP_LIGHT, (int)p, 0, v); }
void glLightModelfv(GLenum p, const GLfloat *v) { if (p == GL_LIGHT_MODEL_AMBIENT) cmd_v4(OP_LMODEL, 0, 0, v); }
void glColorMaterial(GLenum face, GLenum mode) { cmd_f(OP_CMAT, (int)face, (int)mode, 0, 0, 0, 0); }
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { cmd_f(OP_VIEWPORT, x, y, (float)w, (float)h, 0, 0); }

void glClearColor(GLclampf r, GLclampf gg, GLclampf b, GLclampf a) { ensure_init(); g.clr[0] = r; g.clr[1] = gg; g.clr[2] = b; g.clr[3] = a; }
void glClearDepth(GLclampd d) { ensure_init(); g.clrd = d; }
void glClear(GLbitfield m) {
    if (q_n) q_flush();
    PF_BEGIN(t0_);
    ensure_init(); if (!g.fb) return;
    size_t n = (size_t)g.fw * g.fh;
    if (m & GL_COLOR_BUFFER_BIT) {
        uint32_t c = (uint32_t)(clamp01(g.clr[0]) * 255 + 0.5f) << 16 | (uint32_t)(clamp01(g.clr[1]) * 255 + 0.5f) << 8 | (uint32_t)(clamp01(g.clr[2]) * 255 + 0.5f);
        for (size_t i = 0; i < n; ++i) g.fb[i] = c;
    }
    if (m & GL_DEPTH_BUFFER_BIT) { uint32_t z = zfix((float)g.clrd); if (z == 0 || z == 0xFFFFFFFFu) memset(g.zb, z ? 0xFF : 0, n * 4); else for (size_t i = 0; i < n; ++i) g.zb[i] = z; }
    PF_END(pf_clear, t0_);
}

void glEnableClientState(GLenum a) { ensure_init(); g.arrays |= a == GL_VERTEX_ARRAY ? A_VERT : a == GL_COLOR_ARRAY ? A_COL : a == GL_TEXTURE_COORD_ARRAY ? A_TEX : a == GL_NORMAL_ARRAY ? A_NORM : 0; }
void glDisableClientState(GLenum a) { ensure_init(); g.arrays &= ~(a == GL_VERTEX_ARRAY ? A_VERT : a == GL_COLOR_ARRAY ? A_COL : a == GL_TEXTURE_COORD_ARRAY ? A_TEX : a == GL_NORMAL_ARRAY ? A_NORM : 0); }
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *p) { g.vp = p; g.vp_size = size; g.vp_type = (int)type; g.vp_stride = stride; }
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *p) { (void)size; (void)type; g.tp = p; g.tp_stride = stride; }
void glColorPointer(GLint size, GLenum type, GLsizei stride, const void *p) { g.cp = p; g.cp_size = size; g.cp_type = (int)type; g.cp_stride = stride; }
void glNormalPointer(GLenum type, GLsizei stride, const void *p) { (void)type; g.np = p; g.np_stride = stride; }
void glDrawArrays(GLenum prim, GLint first, GLsizei count) {
    ensure_init();
    if (!(g.arrays & A_VERT) || !g.vp || count <= 0) return;
    V *v = malloc((size_t)count * sizeof(V)); if (!v) return;
    for (int i = 0; i < count; ++i) {
        int k = first + i; V *o = &v[i];
        const float *p = (const float *)((const char *)g.vp + (size_t)k * (g.vp_stride ? g.vp_stride : g.vp_size * 4));
        o->x = p[0]; o->y = p[1]; o->z = g.vp_size > 2 ? p[2] : 0; o->u = o->v = 0;
        if ((g.arrays & A_TEX) && g.tp) { const float *t = (const float *)((const char *)g.tp + (size_t)k * (g.tp_stride ? g.tp_stride : 8)); o->u = t[0]; o->v = t[1]; }
        float r = g.cr, gg = g.cg, b = g.cb, a = g.ca;
        if ((g.arrays & A_COL) && g.cp) {
            if (g.cp_type == GL_UNSIGNED_BYTE) { const uint8_t *c = (const uint8_t *)g.cp + (size_t)k * (g.cp_stride ? g.cp_stride : g.cp_size); r = c[0] / 255.0f; gg = c[1] / 255.0f; b = c[2] / 255.0f; a = g.cp_size > 3 ? c[3] / 255.0f : 1; }
            else { const float *c = (const float *)((const char *)g.cp + (size_t)k * (g.cp_stride ? g.cp_stride : g.cp_size * 4)); r = c[0]; gg = c[1]; b = c[2]; a = g.cp_size > 3 ? c[3] : 1; }
        }
        o->r = (uint8_t)(clamp01(r) * 255 + 0.5f); o->g = (uint8_t)(clamp01(gg) * 255 + 0.5f); o->b = (uint8_t)(clamp01(b) * 255 + 0.5f); o->a = (uint8_t)(clamp01(a) * 255 + 0.5f);
        float nx = g.nx, ny = g.ny, nz = g.nz;
        if ((g.arrays & A_NORM) && g.np) { const float *n = (const float *)((const char *)g.np + (size_t)k * (g.np_stride ? g.np_stride : 12)); nx = n[0]; ny = n[1]; nz = n[2]; }
        o->nx = (int8_t)(nx * 127); o->ny = (int8_t)(ny * 127); o->nz = (int8_t)(nz * 127);
    }
    submit((int)prim, v, count);
}

GLuint glGenLists(GLsizei n) {
    ensure_init();
    while (g.nlists + n > g.caplists) { g.lists = realloc(g.lists, (size_t)g.caplists * 2 * sizeof(List)); memset(g.lists + g.caplists, 0, (size_t)g.caplists * sizeof(List)); g.caplists *= 2; }
    GLuint base = (GLuint)g.nlists; g.nlists += n;
    for (int i = 0; i < n; ++i) g.lists[base + i].used = 1;
    return base;
}
void glNewList(GLuint id, GLenum mode) {
    ensure_init(); if (id == 0 || (int)id >= g.nlists) return;
    list_clear(&g.lists[id]); g.lists[id].used = 1; cur_list = (int)id; g.compiling = 1; g.exec_too = mode == GL_COMPILE_AND_EXECUTE;
}
void glEndList(void) { g.compiling = 0; g.exec_too = 0; }
void glCallList(GLuint id) { cmd_f(OP_CALL, (int)id, 0, 0, 0, 0, 0); }
void glDeleteLists(GLuint id, GLsizei n) { for (int i = 0; i < n; ++i) if (id + i > 0 && (int)(id + i) < g.nlists) list_clear(&g.lists[id + i]); }
void glCallLists(GLsizei n, GLenum type, const void *lists) {
    for (int i = 0; i < n; ++i) {
        unsigned id = type == GL_UNSIGNED_BYTE ? ((const uint8_t *)lists)[i] : type == GL_UNSIGNED_INT ? ((const uint32_t *)lists)[i] : type == GL_INT ? (unsigned)((const int *)lists)[i] : 0;
        glCallList(id);
    }
}

void glSelectBuffer(GLsizei n, GLuint *buf) { ensure_init(); g.sel_buf = buf; g.sel_cap = n; }
GLint glRenderMode(GLenum mode) {
    if (q_n) q_flush();
    ensure_init(); GLint r = 0;
    if (g.rmode == GL_SELECT) { flush_hit(); r = g.sel_hits; }
    g.rmode = (int)mode;
    if (mode == GL_SELECT) { g.sel_n = 0; g.sel_hits = 0; g.hit = 0; g.nname = 0; }
    return r;
}
void glInitNames(void) { cmd_f(OP_NAME_INIT, 0, 0, 0, 0, 0, 0); }
void glPushName(GLuint n) { cmd_f(OP_NAME_PUSH, (int)n, 0, 0, 0, 0, 0); }
void glPopName(void) { cmd_f(OP_NAME_POP, 0, 0, 0, 0, 0, 0); }
void glLoadName(GLuint n) { cmd_f(OP_NAME_LOAD, (int)n, 0, 0, 0, 0, 0); }

void glGetFloatv(GLenum p, GLfloat *out) {
    if (q_n) q_flush();
    ensure_init();
    if (p == GL_MODELVIEW_MATRIX) memcpy(out, g.mv[g.mvd], 64);
    else if (p == GL_PROJECTION_MATRIX) memcpy(out, g.pr[g.prd], 64);
}
void glGetIntegerv(GLenum p, GLint *out) {
    if (q_n) q_flush();
    ensure_init();
    if (p == GL_VIEWPORT) { out[0] = g.vx; out[1] = g.vy; out[2] = g.vw; out[3] = g.vh; }
}
GLenum glGetError(void) { return GL_NO_ERROR; }

/* GLU / GLEW */
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zn, GLdouble zf) {
    double ymax = zn * tan(fovy * 3.14159265358979 / 360.0), xmax = ymax * aspect;
    glFrustum(-xmax, xmax, -ymax, ymax, zn, zf);
}
void gluPickMatrix(GLdouble x, GLdouble y, GLdouble w, GLdouble h, const GLint vp[4]) {
    if (w <= 0 || h <= 0) return;
    glTranslatef((float)((vp[2] - 2 * (x - vp[0])) / w), (float)((vp[3] - 2 * (y - vp[1])) / h), 0);
    glScalef((float)(vp[2] / w), (float)(vp[3] / h), 1);
}
const GLubyte *gluErrorString(GLenum e) { (void)e; return (const GLubyte *)"GL error"; }
GLenum glewInit(void) { ensure_init(); return 0; }
const GLubyte *glewGetErrorString(GLenum e) { (void)e; return (const GLubyte *)"no error"; }
