/*
 * tinygl_viewer.c - TinyGL-backed 3D Viewer for C-OS
 *
 * The image is rasterised by the vendored TinyGL software renderer into a
 * private RGBA framebuffer, then copied into the C-OS VGA backbuffer.  This
 * intentionally exercises TinyGL's projection, model-view, depth buffer and
 * triangle rasterisation rather than using C-OS's 2D primitives to fake 3D.
 */
#include "gui.h"
#include "vga.h"
#include "memory.h"
#include "keyboard.h"
#include "tinygl_os.h"
#include <GL/gl.h>
#include <string.h>

#define TINYGL_VIEW_W 320
#define TINYGL_VIEW_H 240

static tinygl_os_context_t* s_ctx;
static uint32_t* s_pixels;
static float s_yaw = 24.0f;
static float s_pitch = -18.0f;
static float s_spin = 0.0f;
static bool s_ready;

static void tinygl_viewer_init(void) {
    if (s_ready) return;
    s_pixels = (uint32_t*)kmalloc((size_t)TINYGL_VIEW_W * TINYGL_VIEW_H * sizeof(uint32_t));
    if (!s_pixels) return;
    memset(s_pixels, 0, (size_t)TINYGL_VIEW_W * TINYGL_VIEW_H * sizeof(uint32_t));
    s_ctx = tinygl_os_create(s_pixels, TINYGL_VIEW_W, TINYGL_VIEW_H);
    if (!s_ctx) {
        kfree(s_pixels);
        s_pixels = NULL;
        return;
    }
    s_ready = true;
}

static void tinygl_viewer_blit(int x, int y) {
    for (int sy = 0; sy < TINYGL_VIEW_H; ++sy) {
        for (int sx = 0; sx < TINYGL_VIEW_W; ++sx) {
            /* TinyGL's frame buffer is bottom-up. C-OS's VGA coordinates are
             * top-down, so reverse source scanlines during the copy. */
            uint32_t p = s_pixels[(TINYGL_VIEW_H - 1 - sy) * TINYGL_VIEW_W + sx];
            uint64_t color = ((uint64_t)((p >> 16) & 0xFF) << 16) |
                             ((uint64_t)((p >> 8) & 0xFF) << 8) |
                             (uint64_t)(p & 0xFF);
            vga_set_pixel(x + sx, y + sy, color);
        }
    }
}

static void tinygl_viewer_render_scene(void) {
    if (!s_ready || !s_ctx) return;
    tinygl_os_begin_frame(s_ctx, 0.0f, 0.0f, 7.0f, s_yaw, s_pitch);
    glRotatef(s_spin, 0.30f, 1.00f, 0.16f);

    /* A central coloured cube and four smaller reference cubes make it easy
     * to visually verify perspective, occlusion and depth testing. */
    tinygl_os_draw_box(-1.4f, -1.4f, -1.4f, 1.4f, 1.4f, 1.4f, rgb(62, 159, 255));
    tinygl_os_draw_box(-4.0f, -1.0f, -2.6f, -2.3f, 0.7f, -0.9f, rgb(255, 107, 92));
    tinygl_os_draw_box( 2.3f, -1.0f, -2.6f,  4.0f, 0.7f, -0.9f, rgb(113, 220, 154));
    tinygl_os_draw_box(-0.8f, -2.4f, -2.2f,  0.8f, -0.8f, -0.6f, rgb(255, 211, 92));
    s_spin += 0.75f;
    if (s_spin >= 360.0f) s_spin -= 360.0f;
}

void tinygl_viewer_draw(int idx) {
    window_t* w = &windows[idx];
    tinygl_viewer_init();
    int x = w->x + 18;
    int y = w->y + 48;
    int content_w = w->w - 36;
    int content_h = w->h - 64;
    vga_fill_rect(w->x, w->y + 32, w->w, w->h - 32, rgb(15, 20, 32));
    if (!s_ready) {
        vga_draw_string(x, y, "TinyGL context could not be created.", rgb(255, 160, 160), 0);
        return;
    }
    tinygl_viewer_render_scene();
    int panel_x = x + (content_w - TINYGL_VIEW_W) / 2;
    int panel_y = y + 26;
    if (panel_x < x) panel_x = x;
    vga_fill_rect(panel_x - 2, panel_y - 2, TINYGL_VIEW_W + 4, TINYGL_VIEW_H + 4, rgb(69, 90, 122));
    tinygl_viewer_blit(panel_x, panel_y);
    vga_draw_string(x, y, "TinyGL 3D Viewer", rgb(225, 237, 255), 0);
    vga_draw_string(x, panel_y + TINYGL_VIEW_H + 12,
                    "Arrow keys: orbit  |  Space: reset view  |  Software depth buffer active",
                    rgb(170, 192, 220), 0);
}

void tinygl_viewer_handle_key(int idx, const keyboard_event_t* ev) {
    (void)idx;
    if (!ev || !ev->pressed) return;
    switch (ev->scancode) {
        case 0x48: s_pitch -= 5.0f; break; /* Up */
        case 0x50: s_pitch += 5.0f; break; /* Down */
        case 0x4B: s_yaw   -= 5.0f; break; /* Left */
        case 0x4D: s_yaw   += 5.0f; break; /* Right */
        case 0x39: s_yaw = 24.0f; s_pitch = -18.0f; s_spin = 0.0f; break;
        default: break;
    }
    if (s_pitch < -80.0f) s_pitch = -80.0f;
    if (s_pitch > 80.0f) s_pitch = 80.0f;
}

void tinygl_viewer_handle_click(int idx, int mx, int my) {
    (void)idx; (void)mx; (void)my;
    s_spin += 25.0f;
    if (s_spin >= 360.0f) s_spin -= 360.0f;
}
