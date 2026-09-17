#ifndef TINYGL_OS_H
#define TINYGL_OS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct tinygl_os_context {
    void *zbuffer;
    uint32_t *framebuffer;
    int width;
    int height;
    bool initialized;
} tinygl_os_context_t;

tinygl_os_context_t *tinygl_os_create(uint32_t *framebuffer, int width, int height);
void tinygl_os_destroy(tinygl_os_context_t *ctx);
void tinygl_os_begin_frame(tinygl_os_context_t *ctx,
                           float eye_x, float eye_y, float eye_z,
                           float yaw_deg, float pitch_deg);
void tinygl_os_draw_box(float x0, float y0, float z0,
                        float x1, float y1, float z1,
                        uint64_t color);

#endif
