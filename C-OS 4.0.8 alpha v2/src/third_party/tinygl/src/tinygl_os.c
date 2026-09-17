#include "tinygl_os.h"

#include "memory.h"
#include "serial.h"
#include "zbuffer.h"
#include <GL/gl.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.141592653589793238462643383279502884
#endif

static void tinygl_set_color(uint64_t color) {
    float r = (float)((color >> 16) & 0xFF) / 255.0f;
    float g = (float)((color >> 8) & 0xFF) / 255.0f;
    float b = (float)(color & 0xFF) / 255.0f;
    glColor3f(r, g, b);
}

tinygl_os_context_t *tinygl_os_create(uint32_t *framebuffer, int width, int height) {
    if (!framebuffer || width <= 0 || height <= 0) {
        return NULL;
    }

    tinygl_os_context_t *ctx = (tinygl_os_context_t *)kmalloc(sizeof(tinygl_os_context_t));
    if (!ctx) {
        serial_puts("[TinyGL] context allocation failed\n");
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));

    ZBuffer *zb = ZB_open(width, height, ZB_MODE_RGBA, 0, NULL, NULL, framebuffer);
    if (!zb) {
        serial_puts("[TinyGL] ZB_open failed\n");
        kfree(ctx);
        return NULL;
    }

    glInit(zb);

    ctx->zbuffer = zb;
    ctx->framebuffer = framebuffer;
    ctx->width = width;
    ctx->height = height;
    ctx->initialized = true;

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glShadeModel(GL_SMOOTH);

    return ctx;
}

void tinygl_os_destroy(tinygl_os_context_t *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->zbuffer) {
        ZB_close((ZBuffer *)ctx->zbuffer);
    }
    glClose();
    kfree(ctx);
}

void tinygl_os_begin_frame(tinygl_os_context_t *ctx,
                           float eye_x, float eye_y, float eye_z,
                           float yaw_deg, float pitch_deg) {
    if (!ctx || !ctx->initialized) {
        return;
    }

    glViewport(0, 0, ctx->width, ctx->height);
    glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    float aspect = (ctx->height > 0) ? ((float)ctx->width / (float)ctx->height) : 1.0f;
    float near_z = 0.12f;
    float top = near_z * 0.7f;
    float right = top * aspect;
    glFrustum(-right, right, -top, top, near_z, 128.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glRotatef(-pitch_deg, 1.0f, 0.0f, 0.0f);
    glRotatef(-yaw_deg,   0.0f, 1.0f, 0.0f);
    glTranslatef(-eye_x, -eye_y, -eye_z);
}

void tinygl_os_draw_box(float x0, float y0, float z0,
                        float x1, float y1, float z1,
                        uint64_t color) {
    tinygl_set_color(color);

    glBegin(GL_QUADS);

    /* +Y face */
    glVertex3f(x0, y1, z0); glVertex3f(x1, y1, z0); glVertex3f(x1, y1, z1); glVertex3f(x0, y1, z1);
    /* -Y face */
    glVertex3f(x0, y0, z0); glVertex3f(x0, y0, z1); glVertex3f(x1, y0, z1); glVertex3f(x1, y0, z0);
    /* +X face */
    glVertex3f(x1, y0, z0); glVertex3f(x1, y0, z1); glVertex3f(x1, y1, z1); glVertex3f(x1, y1, z0);
    /* -X face */
    glVertex3f(x0, y0, z0); glVertex3f(x0, y1, z0); glVertex3f(x0, y1, z1); glVertex3f(x0, y0, z1);
    /* +Z face */
    glVertex3f(x0, y0, z1); glVertex3f(x0, y1, z1); glVertex3f(x1, y1, z1); glVertex3f(x1, y0, z1);
    /* -Z face */
    glVertex3f(x0, y0, z0); glVertex3f(x1, y0, z0); glVertex3f(x1, y1, z0); glVertex3f(x0, y1, z0);

    glEnd();
}
