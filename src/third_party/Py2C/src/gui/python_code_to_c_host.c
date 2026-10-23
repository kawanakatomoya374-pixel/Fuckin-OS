#include <stdio.h>

#include "gui/python_code_to_c_host.h"

int p2c_host_open(P2C_Host *h, uint32_t width, uint32_t height) {
    if (!h) return -1;
    h->head = 0u;
    h->count = 0u;
    if (width == 0u) width = P2C_GUI_DEFAULT_WIDTH;
    if (height == 0u) height = P2C_GUI_DEFAULT_HEIGHT;
    return p2c_fb_open_memory(&h->canvas, width, height, 0u);
}

int p2c_host_wrap(P2C_Host *h, uint32_t *pixels, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!h) return -1;
    h->head = 0u;
    h->count = 0u;
    return p2c_fb_wrap(&h->canvas, pixels, width, height, pitch);
}

void p2c_host_close(P2C_Host *h) {
    if (!h) return;
    p2c_fb_close(&h->canvas);
    h->head = 0u;
    h->count = 0u;
}

int p2c_host_push(P2C_Host *h, P2C_Event ev) {
    if (!h || h->count >= P2C_HOST_QUEUE_CAPACITY) return 0;
    unsigned tail = (h->head + h->count) % P2C_HOST_QUEUE_CAPACITY;
    h->queue[tail] = ev;
    h->count++;
    return 1;
}

int p2c_host_poll(P2C_Host *h, P2C_Event *out) {
    if (!h || !out || h->count == 0u) return 0;
    *out = h->queue[h->head];
    h->head = (h->head + 1u) % P2C_HOST_QUEUE_CAPACITY;
    h->count--;
    return 1;
}

int p2c_host_present_ppm(const P2C_Host *h, const char *path) {
    if (!h || !path || !h->canvas.pixels) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned w = (unsigned)h->canvas.width, ht = (unsigned)h->canvas.height;
    fprintf(f, "P6\n%u %u\n255\n", w, ht);
    for (unsigned y = 0; y < ht; y++) {
        for (unsigned x = 0; x < w; x++) {
            uint32_t px = p2c_fb_get_pixel(&h->canvas, x, y);
            unsigned char rgb[3] = {
                (unsigned char)((px >> 16) & 0xFFu),
                (unsigned char)((px >> 8) & 0xFFu),
                (unsigned char)(px & 0xFFu)
            };
            if (fwrite(rgb, 1u, 3u, f) != 3u) { fclose(f); return -1; }
        }
    }
    return fclose(f) == 0 ? 0 : -1;
}
