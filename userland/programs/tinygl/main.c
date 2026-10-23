/* tinygl.c-os - the TinyGL 3D viewer as a ring-3 program. The vendored TinyGL software renderer (src/third_party/tinygl)
 * is compiled into the program itself and rasterises into a private RGBA buffer, which is scaled into the window. This
 * exercises TinyGL's projection, model-view, depth buffer and triangle rasteriser rather than faking 3D with 2D
 * primitives. Arrow keys orbit, space resets, a click adds spin; the window is resizable-looking via the 2x scale. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "cos.h"
#include "cos_ui.h"
#include <GL/gl.h>
#include "zbuffer.h"

#define RW 360
#define RH 270
#define SCALE 2

static uint32_t *fb;
static float yaw = 24.0f, pitch = -18.0f, spin;

static void box(float x0, float y0, float z0, float x1, float y1, float z1, uint32_t color) {
    glColor3f(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f);
    glBegin(GL_QUADS);
    glVertex3f(x0, y1, z0); glVertex3f(x1, y1, z0); glVertex3f(x1, y1, z1); glVertex3f(x0, y1, z1);
    glVertex3f(x0, y0, z0); glVertex3f(x0, y0, z1); glVertex3f(x1, y0, z1); glVertex3f(x1, y0, z0);
    glVertex3f(x1, y0, z0); glVertex3f(x1, y0, z1); glVertex3f(x1, y1, z1); glVertex3f(x1, y1, z0);
    glVertex3f(x0, y0, z0); glVertex3f(x0, y1, z0); glVertex3f(x0, y1, z1); glVertex3f(x0, y0, z1);
    glVertex3f(x0, y0, z1); glVertex3f(x0, y1, z1); glVertex3f(x1, y1, z1); glVertex3f(x1, y0, z1);
    glVertex3f(x0, y0, z0); glVertex3f(x1, y0, z0); glVertex3f(x1, y1, z0); glVertex3f(x0, y1, z0);
    glEnd();
}

static void render_scene(void) {
    glViewport(0, 0, RW, RH);
    glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float near_z = 0.12f, top = near_z * 0.7f, right = top * (float)RW / (float)RH;
    glFrustum(-right, right, -top, top, near_z, 128.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glRotatef(-pitch, 1, 0, 0);
    glRotatef(-yaw, 0, 1, 0);
    glTranslatef(0, 0, -7.0f);
    glRotatef(spin, 0.30f, 1.00f, 0.16f);
    box(-1.4f, -1.4f, -1.4f, 1.4f, 1.4f, 1.4f, 0x3E9FFF);
    box(-4.0f, -1.0f, -2.6f, -2.3f, 0.7f, -0.9f, 0xFF6B5C);
    box(2.3f, -1.0f, -2.6f, 4.0f, 0.7f, -0.9f, 0x71DC9A);
    box(-0.8f, -2.4f, -2.2f, 0.8f, -0.8f, -0.6f, 0xFFD35C);
    spin += 0.75f;
    if (spin >= 360.0f) spin -= 360.0f;
}

int main(void) {
    cos_win_info_t wi;
    int64_t h = cos_win2_create("TinyGL 3D Viewer", RW * SCALE + 36, RH * SCALE + 90, &wi);
    if (h <= 0) return 1;
    cui_canvas c;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    cui_font *f = cui_font_default(14), *fb_ = cui_font_bold(16);

    fb = calloc((size_t)RW * RH, sizeof(uint32_t));
    ZBuffer *zb = fb ? ZB_open(RW, RH, ZB_MODE_RGBA, 0, NULL, NULL, NULL) : NULL;
    if (!zb) {
        cui_fill(&c, 0, 0, c.w, c.h, 0x0F1420);
        cui_text(&c, f, 20, 20, "TinyGL context could not be created.", 0xFFA0A0);
        cos_win2_present(h);
        cos_win_event_t ev;
        while (cos_win2_wait(h, &ev, 1000) <= 0 || ev.type != COS_EV_CLOSE) { }
        cos_win2_close(h);
        return 1;
    }
    glInit(zb);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glShadeModel(GL_SMOOTH);

    uint64_t frames = 0, t0 = cos_time_ms();
    char fps[48] = "";
    for (;;) {
        render_scene();
        ZB_copyFrameBuffer(zb, fb, RW * 4);   /* TinyGL renders RGB565 internally; this expands it to 32-bit pixels */
        cui_fill(&c, 0, 0, c.w, c.h, 0x0F1420);
        cui_text(&c, fb_, 18, 14, "TinyGL 3D Viewer", 0xE1EDFF);
        int ox = 18, oy = 44;
        cui_fill(&c, ox - 2, oy - 2, RW * SCALE + 4, RH * SCALE + 4, 0x455A7A);
        /* TinyGL's frame buffer is bottom-up and 2x nearest-neighbour scaled into the window */
        for (int y = 0; y < RH * SCALE; y++) {
            const uint32_t *src = fb + (RH - 1 - y / SCALE) * RW;
            uint32_t *dst = c.px + (oy + y) * c.stride + ox;
            for (int x = 0; x < RW * SCALE; x++) dst[x] = src[x / SCALE] & 0xFFFFFF;
        }
        cui_text(&c, f, 18, oy + RH * SCALE + 12, "Arrow keys: orbit   Space: reset view   Click: spin   (software depth buffer)", 0xAAC0DC);
        if (fps[0]) cui_text(&c, f, c.w - 18 - cui_text_width(f, fps), 16, fps, 0x78E0A0);
        cos_win2_present(h);
        if (++frames % 20 == 0) { uint64_t t = cos_time_ms(); snprintf(fps, sizeof fps, "%.1f fps", 20000.0 / (double)(t - t0 ? t - t0 : 1)); t0 = t; }

        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, 16);
        while (r > 0) {
            if (ev.type == COS_EV_CLOSE) goto done;
            if (ev.type == COS_EV_KEY) {
                if (ev.special == COS_KEY_UP) pitch -= 5.0f;
                else if (ev.special == COS_KEY_DOWN) pitch += 5.0f;
                else if (ev.special == COS_KEY_LEFT) yaw -= 5.0f;
                else if (ev.special == COS_KEY_RIGHT) yaw += 5.0f;
                else if (ev.ascii == ' ') { yaw = 24.0f; pitch = -18.0f; spin = 0.0f; }
                if (pitch < -80.0f) pitch = -80.0f;
                if (pitch > 80.0f) pitch = 80.0f;
            } else if (ev.type == COS_EV_MOUSE_DOWN) { spin += 25.0f; if (spin >= 360.0f) spin -= 360.0f; }
            r = cos_win2_poll(h, &ev);
        }
    }
done:
    glClose();
    ZB_close(zb);
    cos_win2_close(h);
    return 0;
}
