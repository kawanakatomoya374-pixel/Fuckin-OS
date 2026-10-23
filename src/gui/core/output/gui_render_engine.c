#include "gui_render_engine.h"
#include "vga.h"
#include <stddef.h>

static struct {
    int x, y, w, h;
    bool enabled;
} g_clip = {0, 0, 0, 0, false};



static void gui_render_clip_rect(int* x, int* y, int* w, int* h) {
    if (!x || !y || !w || !h) return;
    if (*w <= 0 || *h <= 0) return;
    if (!g_clip.enabled) return;

    int x1 = *x;
    int y1 = *y;
    int x2 = *x + *w;
    int y2 = *y + *h;
    int cx1 = g_clip.x;
    int cy1 = g_clip.y;
    int cx2 = g_clip.x + g_clip.w;
    int cy2 = g_clip.y + g_clip.h;

    if (x1 < cx1) x1 = cx1;
    if (y1 < cy1) y1 = cy1;
    if (x2 > cx2) x2 = cx2;
    if (y2 > cy2) y2 = cy2;

    if (x2 <= x1 || y2 <= y1) {
        *w = 0;
        *h = 0;
        return;
    }

    *x = x1;
    *y = y1;
    *w = x2 - x1;
    *h = y2 - y1;
}

int gui_render_init(void) {
    g_clip.x = 0;
    g_clip.y = 0;
    g_clip.w = SCREEN_W;
    g_clip.h = SCREEN_H;
    g_clip.enabled = false;
    return 0;
}







void gui_render_fill_rect(int x, int y, int w, int h, uint64_t color) {
    gui_render_clip_rect(&x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    vga_fill_rect(x, y, w, h, color);
}











void gui_render_draw_text(int x, int y, const char* text, uint64_t fg, uint64_t bg) {
    if (!text) return;
    vga_draw_string(x, y, text, fg, bg);
}
