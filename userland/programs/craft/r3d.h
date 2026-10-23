/* r3d.h - the software renderer behind the C-OS port of Craft.
 *
 * Craft (github.com/fogleman/Craft, MIT) draws with four small OpenGL shader programs; C-OS has no 3D hardware path for ring-3
 * programs, so this file does what those shaders do on the CPU: block faces (texture atlas, ambient occlusion, light, fog to
 * the sky colour), the sky dome, 3-D sign text, 2-D text/menu overlays and inverted lines (selection box, crosshair).
 * Vertex layouts are Craft's own, so cube.c / sign.c / world generation are used unchanged.
 *
 * Matrices are Craft's (column-major, OpenGL clip space); the framebuffer is y-down 0x00RRGGBB.
 */
#ifndef R3D_H
#define R3D_H

#include <stdint.h>

/* textures: all RGBA8, row 0 = top of the picture */
void r3d_set_atlas(const uint8_t *rgba, int w, int h);   /* block texture atlas (16x16 tiles); magenta = transparent */
void r3d_set_font(const uint8_t *rgba, int w, int h);
void r3d_set_sign(const uint8_t *rgba, int w, int h);
void r3d_set_sky(const uint8_t *rgba, int w, int h);

/* the internal framebuffer (may be smaller than the window: r3d_present scales it up) */
int  r3d_resize(int w, int h);                            /* 0 on success */
int  r3d_width(void);
int  r3d_height(void);
void r3d_clear(uint32_t rgb);
void r3d_clear_depth(void);

/* Block faces, 10 floats per vertex: x y z  nx ny nz  u v  ao light. `count` vertices (3 per triangle). */
void r3d_blocks(const float *v, int count, const float m[16], const float camera[3],
                float timer, float daylight, float fog_distance, int ortho);
/* 3-D sign text, 5 floats per vertex: x y z u v. White texels are transparent; drawn slightly in front of the block. */
void r3d_signs(const float *v, int count, const float m[16]);
/* The sky dome, 8 floats per vertex: x y z nx ny nz u v. */
void r3d_sky(const float *v, int count, const float m[16], float timer);
/* 2-D text quads, 4 floats per vertex: x y u v (pixels, origin bottom-left like Craft's set_matrix_2d). Alpha blended. */
void r3d_text2d(const float *v, int count, const float m[16]);
/* Inverted lines (GL_COLOR_LOGIC_OP / GL_INVERT): `comps` 2 (screen) or 3 (world, depth tested); count = vertices (2 per line). */
void r3d_lines(const float *v, int comps, int count, const float m[16], int width, int depth_test);
/* 2-D overlays (text, crosshair, menus) can be drawn straight into the window's pixels at full resolution instead of the (smaller) 3-D framebuffer:
 * between r3d_overlay_begin and r3d_overlay_end, r3d_text2d / r3d_lines / r3d_fill_rect draw into `px` (w x h, stride = w). */
void r3d_overlay_begin(uint32_t *px, int w, int h);
void r3d_overlay_end(void);

/* A translucent rectangle in framebuffer pixels (top-left origin). */
void r3d_fill_rect(int x, int y, int w, int h, uint32_t rgb, int alpha /* 0..255 */);

/* Copies the framebuffer into a window's pixels, scaled up by whole factors or stretched to fit. */
void r3d_present(uint32_t *dst, int dst_stride_pixels, int dst_w, int dst_h);

/* Runs the queued block draws (r3d_blocks only queues; every other call flushes first, and r3d_present too). */
void r3d_flush(void);
int  r3d_cores(void);          /* participants the game currently uses for drawing (tuned at run time) */

/* Threads that share the pixel work (0 or 1 = draw on the calling thread). */
void r3d_set_threads(int n);   /* total participants incl. the caller (extra cores via the compute-job calls); 1 = single core */

#endif
