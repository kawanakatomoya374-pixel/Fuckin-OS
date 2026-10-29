/* PSXFunkin on C-OS: the graphics backend. A software renderer for the PlayStation GPU calls the game makes (boot/gfx.h):
 * rectangles, sprites and textured quads with a colour modulation (0x80 is neutral) and the four semi-transparency modes.
 * The game submits primitives into an ordering table that the GPU draws in the reverse order of submission, so the commands are kept
 * and replayed backwards at Gfx_Flip(). Textures are TIM images kept as index data + palette (a palette entry of 0x0000 is transparent). */
#include "gfx.h"
#include "mem.h"
#include "main.h"
#include "../plat.h"

#define MAX_TEX 1024
#define MAX_CMD 8192

typedef struct { u16 w, h; u8 bpp; u8 used; u8 *idx; u32 *rgba; u32 pal[256]; } Tex;     /* idx for 4/8 bpp, rgba for 16 bpp */
static Tex tex_tab[MAX_TEX]; static int tex_n;

enum { C_RECT, C_RECT_BLEND, C_SPRITE, C_QUAD, C_QUAD_BLEND };
typedef struct { u8 kind, mode, r, g, b; u16 tex; s16 sx, sy, sw, sh; s16 x[4], y[4]; } Cmd;
static Cmd cmds[MAX_CMD]; static int ncmd;

static u32 fb[PLAT_W * PLAT_H];
static u8 clr_r, clr_g, clr_b; static int clr_on = 1;

/* ---- textures --------------------------------------------------------------------------------------------------------------------- */
static u32 rgb555(u16 c) { return c == 0 ? 0u : 0xFF000000u | ((u32)(c & 31) << 3 << 16) | ((u32)((c >> 5) & 31) << 3 << 8) | ((u32)((c >> 10) & 31) << 3); }
static Tex *tex_of(const Gfx_Tex *t) { u16 id = t->tpage_x; return id && id <= MAX_TEX && t->tpage_y == (u16)(id ^ 0x5A5Au) ? &tex_tab[id - 1] : NULL; }

void Gfx_LoadTex(Gfx_Tex *tex, IO_Data data, Gfx_LoadTex_Flag flag)
{
	if (data == NULL) { sprintf(error_msg, "[Gfx_LoadTex] data is NULL"); ErrorLock(); }
	const u8 *p = (const u8*)data;
	u32 flags = p[4] | (p[5] << 8) | (p[6] << 16) | ((u32)p[7] << 24);
	int mode = flags & 3; p += 8;
	Tex *t = tex ? tex_of(tex) : NULL;
	if (tex && !t) {
		if (tex_n >= MAX_TEX) { sprintf(error_msg, "[Gfx_LoadTex] too many textures"); ErrorLock(); }
		t = &tex_tab[tex_n]; memset(t, 0, sizeof *t); tex->tpage_x = (u16)(++tex_n); tex->tpage_y = (u16)(tex->tpage_x ^ 0x5A5Au);
	}
	u32 pal[256]; memset(pal, 0, sizeof pal);
	if (flags & 8) {                                                       /* CLUT block: u32 size, u16 x y w h, then w*h words */
		u32 sz = p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); int cw = p[8] | (p[9] << 8), ch = p[10] | (p[11] << 8);
		if (!(flag & GFX_LOADTEX_NOCLUT) && t) { int n = cw * ch; if (n > 256) n = 256; for (int i = 0; i < n; ++i) pal[i] = rgb555((u16)(p[12 + i * 2] | (p[13 + i * 2] << 8))); memcpy(t->pal, pal, sizeof pal); }
		p += sz;
	}
	u32 sz = p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); int pw = p[8] | (p[9] << 8), ph = p[10] | (p[11] << 8);
	const u8 *pix = p + 12;
	if (t && !(flag & GFX_LOADTEX_NOTEX)) {
		int w = mode == 0 ? pw * 4 : mode == 1 ? pw * 2 : pw;
		t->w = (u16)w; t->h = (u16)ph; t->bpp = mode == 0 ? 4 : mode == 1 ? 8 : 16;
		free(t->idx); free(t->rgba); t->idx = NULL; t->rgba = NULL;
		if (mode == 2) {
			t->rgba = (u32*)malloc((size_t)w * ph * 4);
			for (int i = 0; i < w * ph; ++i) t->rgba[i] = rgb555((u16)(pix[i * 2] | (pix[i * 2 + 1] << 8)));
		} else {
			t->idx = (u8*)malloc((size_t)w * ph);
			if (mode == 1) memcpy(t->idx, pix, (size_t)w * ph);
			else for (int i = 0; i < w * ph; i += 2) { u8 b = pix[i >> 1]; t->idx[i] = b & 15; t->idx[i + 1] = b >> 4; }
		}
		t->used = 1;
	}
	(void)sz;
	if (flag & GFX_LOADTEX_FREE) Mem_Free(data);
}

/* ---- state ------------------------------------------------------------------------------------------------------------------------ */
void Gfx_Init(void) { ncmd = 0; clr_r = clr_g = clr_b = 0; clr_on = 1; }
void Gfx_Quit(void) { }
void Gfx_SetClear(u8 r, u8 g, u8 b) { clr_r = r; clr_g = g; clr_b = b; }
void Gfx_EnableClear(void) { clr_on = 1; }
void Gfx_DisableClear(void) { clr_on = 0; }

static Cmd *cmd_new(u8 kind) { if (ncmd >= MAX_CMD) return NULL; Cmd *c = &cmds[ncmd++]; memset(c, 0, sizeof *c); c->kind = kind; return c; }

void Gfx_DrawRect(const RECT *r, u8 R, u8 G, u8 B) { Cmd *c = cmd_new(C_RECT); if (!c) return; c->x[0] = (s16)r->x; c->y[0] = (s16)r->y; c->sw = (s16)r->w; c->sh = (s16)r->h; c->r = R; c->g = G; c->b = B; }
void Gfx_BlendRect(const RECT *r, u8 R, u8 G, u8 B, u8 mode) { Cmd *c = cmd_new(C_RECT_BLEND); if (!c) return; c->x[0] = (s16)r->x; c->y[0] = (s16)r->y; c->sw = (s16)r->w; c->sh = (s16)r->h; c->r = R; c->g = G; c->b = B; c->mode = mode; }
void Gfx_BlitTexCol(Gfx_Tex *t, const RECT *s, s32 x, s32 y, u8 R, u8 G, u8 B) {
	Cmd *c = cmd_new(C_SPRITE); if (!c) return; c->tex = t->tpage_x; c->sx = (s16)s->x; c->sy = (s16)s->y; c->sw = (s16)s->w; c->sh = (s16)s->h; c->x[0] = (s16)x; c->y[0] = (s16)y; c->r = R; c->g = G; c->b = B;
}
void Gfx_BlitTex(Gfx_Tex *t, const RECT *s, s32 x, s32 y) { Gfx_BlitTexCol(t, s, x, y, 0x80, 0x80, 0x80); }
void Gfx_DrawTexCol(Gfx_Tex *t, const RECT *s, const RECT *d, u8 R, u8 G, u8 B) {
	Cmd *c = cmd_new(C_QUAD); if (!c) return; c->tex = t->tpage_x; c->sx = (s16)s->x; c->sy = (s16)s->y; c->sw = (s16)s->w; c->sh = (s16)s->h; c->r = R; c->g = G; c->b = B;
	c->x[0] = c->x[2] = (s16)d->x; c->x[1] = c->x[3] = (s16)(d->x + d->w); c->y[0] = c->y[1] = (s16)d->y; c->y[2] = c->y[3] = (s16)(d->y + d->h);
}
void Gfx_DrawTex(Gfx_Tex *t, const RECT *s, const RECT *d) { Gfx_DrawTexCol(t, s, d, 0x80, 0x80, 0x80); }
void Gfx_DrawTexArbCol(Gfx_Tex *t, const RECT *s, const POINT *p0, const POINT *p1, const POINT *p2, const POINT *p3, u8 R, u8 G, u8 B) {
	Cmd *c = cmd_new(C_QUAD); if (!c) return; c->tex = t->tpage_x; c->sx = (s16)s->x; c->sy = (s16)s->y; c->sw = (s16)s->w; c->sh = (s16)s->h; c->r = R; c->g = G; c->b = B;
	c->x[0] = p0->x; c->y[0] = p0->y; c->x[1] = p1->x; c->y[1] = p1->y; c->x[2] = p2->x; c->y[2] = p2->y; c->x[3] = p3->x; c->y[3] = p3->y;
}
void Gfx_DrawTexArb(Gfx_Tex *t, const RECT *s, const POINT *p0, const POINT *p1, const POINT *p2, const POINT *p3) { Gfx_DrawTexArbCol(t, s, p0, p1, p2, p3, 0x80, 0x80, 0x80); }
void Gfx_BlendTexArbCol(Gfx_Tex *t, const RECT *s, const POINT *p0, const POINT *p1, const POINT *p2, const POINT *p3, u8 R, u8 G, u8 B, u8 mode) {
	Cmd *c = cmd_new(C_QUAD_BLEND); if (!c) return; c->tex = t->tpage_x; c->sx = (s16)s->x; c->sy = (s16)s->y; c->sw = (s16)s->w; c->sh = (s16)s->h; c->r = R; c->g = G; c->b = B; c->mode = mode;
	c->x[0] = p0->x; c->y[0] = p0->y; c->x[1] = p1->x; c->y[1] = p1->y; c->x[2] = p2->x; c->y[2] = p2->y; c->x[3] = p3->x; c->y[3] = p3->y;
}
void Gfx_BlendTexArb(Gfx_Tex *t, const RECT *s, const POINT *p0, const POINT *p1, const POINT *p2, const POINT *p3, u8 mode) { Gfx_BlendTexArbCol(t, s, p0, p1, p2, p3, 0x80, 0x80, 0x80, mode); }

/* ---- drawing ---------------------------------------------------------------------------------------------------------------------- */
static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
/* the GPU's four semi-transparency modes: 0 B/2+F/2, 1 B+F, 2 B-F, 3 B+F/4 (B = what is there, F = the new colour) */
static inline u32 mix(u32 d, int fr, int fg, int fb, int mode) {
	int br = (d >> 16) & 255, bg = (d >> 8) & 255, bb = d & 255, r, g, b;
	switch (mode) {
	case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
	case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
	case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
	default: r = br + (fr >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2); break;
	}
	return 0xFF000000u | ((u32)clamp255(r) << 16) | ((u32)clamp255(g) << 8) | (u32)clamp255(b);
}
static void fill_rect(int x, int y, int w, int h, int r, int g, int b, int blend, int mode) {
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > PLAT_W ? PLAT_W : x + w, y1 = y + h > PLAT_H ? PLAT_H : y + h;
	for (int yy = y0; yy < y1; ++yy) {
		u32 *row = fb + yy * PLAT_W;
		for (int xx = x0; xx < x1; ++xx) row[xx] = blend ? mix(row[xx], r, g, b, mode) : (0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b);
	}
}
static inline u32 texel(const Tex *t, int u, int v) {
	if (u < 0) u = 0; else if (u >= t->w) u = t->w - 1; if (v < 0) v = 0; else if (v >= t->h) v = t->h - 1;
	return t->rgba ? t->rgba[v * t->w + u] : t->pal[t->idx[v * t->w + u]];
}
static inline void put_texel(u32 *dst, u32 c, int r, int g, int b, int blend, int mode) {
	if ((c >> 24) == 0) return;                                              /* palette entry 0x0000: transparent */
	int fr = clamp255((int)(((c >> 16) & 255) * r) >> 7), fg = clamp255((int)(((c >> 8) & 255) * g) >> 7), fb = clamp255((int)((c & 255) * b) >> 7);
	*dst = blend ? mix(*dst, fr, fg, fb, mode) : (0xFF000000u | ((u32)fr << 16) | ((u32)fg << 8) | (u32)fb);
}
/* a parallelogram p0 p1 p2 (p3 = p1 + p2 - p0): the source rectangle is mapped on it with an affine map (every quad of the game is a scale / rotation of a rectangle) */
static void draw_quad(const Cmd *c, int blend) {
	const Tex *t = (c->tex && c->tex <= MAX_TEX) ? &tex_tab[c->tex - 1] : NULL; if (!t || !t->used) return;
	float x0 = c->x[0], y0 = c->y[0], e1x = (float)(c->x[1] - c->x[0]), e1y = (float)(c->y[1] - c->y[0]), e2x = (float)(c->x[2] - c->x[0]), e2y = (float)(c->y[2] - c->y[0]);
	float det = e1x * e2y - e1y * e2x; if (det > -0.001f && det < 0.001f) return;
	float minx = c->x[0], maxx = minx, miny = c->y[0], maxy = miny;
	for (int i = 1; i < 4; ++i) { if (c->x[i] < minx) minx = c->x[i]; if (c->x[i] > maxx) maxx = c->x[i]; if (c->y[i] < miny) miny = c->y[i]; if (c->y[i] > maxy) maxy = c->y[i]; }
	int ix0 = (int)minx, ix1 = (int)maxx + 1, iy0 = (int)miny, iy1 = (int)maxy + 1;
	if (ix0 < 0) ix0 = 0; if (iy0 < 0) iy0 = 0; if (ix1 > PLAT_W) ix1 = PLAT_W; if (iy1 > PLAT_H) iy1 = PLAT_H;
	float inv = 1.0f / det;
	for (int y = iy0; y < iy1; ++y) {
		u32 *row = fb + y * PLAT_W; float py = (float)y + 0.5f - y0;
		for (int x = ix0; x < ix1; ++x) {
			float px = (float)x + 0.5f - x0;
			float s = (px * e2y - py * e2x) * inv, tt = (e1x * py - e1y * px) * inv;
			if (s < 0.0f || s >= 1.0f || tt < 0.0f || tt >= 1.0f) continue;
			put_texel(&row[x], texel(t, c->sx + (int)(s * c->sw), c->sy + (int)(tt * c->sh)), c->r, c->g, c->b, blend, c->mode);
		}
	}
}
static void draw_sprite(const Cmd *c) {
	const Tex *t = (c->tex && c->tex <= MAX_TEX) ? &tex_tab[c->tex - 1] : NULL; if (!t || !t->used) return;
	for (int yy = 0; yy < c->sh; ++yy) {
		int y = c->y[0] + yy; if (y < 0 || y >= PLAT_H) continue; u32 *row = fb + y * PLAT_W;
		for (int xx = 0; xx < c->sw; ++xx) { int x = c->x[0] + xx; if (x < 0 || x >= PLAT_W) continue; put_texel(&row[x], texel(t, c->sx + xx, c->sy + yy), c->r, c->g, c->b, 0, 0); }
	}
}

extern void Audio_Pump(void);
void Gfx_Flip(void)
{
	/* clear, then draw the commands of this frame from the last submitted to the first (the GPU's ordering-table order) */
	if (clr_on) { u32 c = 0xFF000000u | ((u32)clr_r << 16) | ((u32)clr_g << 8) | clr_b; for (int i = 0; i < PLAT_W * PLAT_H; ++i) fb[i] = c; }
	for (int i = ncmd - 1; i >= 0; --i) {
		const Cmd *c = &cmds[i];
		switch (c->kind) {
		case C_RECT: fill_rect(c->x[0], c->y[0], c->sw, c->sh, c->r, c->g, c->b, 0, 0); break;
		case C_RECT_BLEND: fill_rect(c->x[0], c->y[0], c->sw, c->sh, c->r, c->g, c->b, 1, c->mode); break;
		case C_SPRITE: draw_sprite(c); break;
		case C_QUAD: draw_quad(c, 0); break;
		case C_QUAD_BLEND: draw_quad(c, 1); break;
		}
	}
	ncmd = 0;
	Audio_Pump();
	plat_present(fb);
}
