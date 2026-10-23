/* Photo.c-os - the single C-OS image viewer for BMP/JPEG/PNG/WebP. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cos.h"
#include "cos_ui.h"

#define MAX_IMAGE_BYTES (100ull * 1024ull * 1024ull)
#define MAX_IMAGE_PIXELS (1920u * 1080u)
#define WIN_W 900
#define WIN_H 640

static int64_t win;
static cos_win_info_t wi;
static cui_canvas cv;
static uint32_t *pixels;
static int iw, ih;
static char path[256], status[160];

static void say(const char *s) { snprintf(status, sizeof status, "%s", s); }
static void clear_image(void) {
    if (pixels) free(pixels);
    pixels = NULL;
    iw = ih = 0;
}
static const char *format_name(const char *p) {
    const char *dot = strrchr(p ? p : "", '.');
    if (!dot) return "image";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "JPEG";
    if (!strcmp(dot, ".png")) return "PNG";
    if (!strcmp(dot, ".bmp")) return "BMP";
    if (!strcmp(dot, ".webp")) return "WebP";
    return "image";
}
static bool load_image(const char *p) {
    clear_image();
    cos_stat_t st;
    if (!p || cos_stat(p, &st) < 0 || st.is_dir) { say("Cannot open image"); return false; }
    if (st.size > MAX_IMAGE_BYTES) { say("Image is larger than 100 MiB"); return false; }
    pixels = (uint32_t *)malloc((size_t)MAX_IMAGE_PIXELS * sizeof(uint32_t));
    if (!pixels) { say("Not enough memory for image surface"); return false; }
    cos_image_info_t info;
    int rc = cos_image_decode(p, pixels, (size_t)MAX_IMAGE_PIXELS * sizeof(uint32_t), &info);
    if (rc != 0 || info.width == 0 || info.height == 0 || info.width > 1920 || info.height > 1080) {
        clear_image(); say("Unsupported or corrupt BMP/JPEG/PNG/WebP image"); return false;
    }
    iw = (int)info.width; ih = (int)info.height;
    snprintf(path, sizeof path, "%s", p);
    snprintf(status, sizeof status, "%dx%d  %s", iw, ih, format_name(p));
    return true;
}
static void draw(void) {
    cui_fill(&cv, 0, 0, cv.w, cv.h, 0x0D1119);
    cui_fill(&cv, 0, 0, cv.w, 56, 0x1A2333);
    cui_text(&cv, cui_font_bold(17), 20, 17, "Photo.c-os", 0xF3F6FF);
    cui_text(&cv, cui_font_default(11), 145, 21, "BMP / JPEG / PNG / WebP", 0x9BA9C2);
    int x = 16, y = 72, w = cv.w - 32, h = cv.h - 130;
    cui_round_rect(&cv, x, y, w, h, 8, 0x151C28);
    if (pixels && iw > 0 && ih > 0) {
        float sx = (float)(w - 20) / iw, sy = (float)(h - 20) / ih, sc = sx < sy ? sx : sy;
        int dw = (int)(iw * sc), dh = (int)(ih * sc), ox = x + (w - dw) / 2, oy = y + (h - dh) / 2;
        for (int yy = 0; yy < dh; ++yy) {
            int syy = yy * ih / dh;
            for (int xx = 0; xx < dw; ++xx) {
                int sxx = xx * iw / dw;
                cv.px[(oy + yy) * cv.stride + ox + xx] = pixels[(size_t)syy * (size_t)iw + (size_t)sxx];
            }
        }
    } else {
        cui_text_center(&cv, cui_font_default(14), x, y, w, h,
                        "Open BMP, JPEG, PNG or WebP from Files, or drop it here", 0xAEBBD0);
    }
    cui_text_fit(&cv, cui_font_default(10), 20, cv.h - 42, cv.w - 40,
                 path[0] ? path : status, 0x9BA9C2);
}
static void present(void) {
    cui_canvas_init(&cv, wi.pixels, wi.width, wi.height, wi.stride);
    draw(); cos_win2_present(win);
}
int main(int argc, char **argv) {
    win = cos_win2_create("Photo.c-os", WIN_W, WIN_H, &wi);
    if (win < 0) return 1;
    if (argc > 1 && argv[1]) load_image(argv[1]); else say("BMP/JPEG/PNG/WebP ready");
    present();
    for (;;) {
        cos_win_event_t e;
        while (cos_win2_poll(win, &e) > 0) {
            if (e.type == COS_EV_CLOSE) { clear_image(); cos_win2_close(win); return 0; }
            if (e.type == COS_EV_OPEN || e.type == COS_EV_DROP) {
                char p[256];
                if (cos_win2_get_path(win, p, sizeof p) >= 0) { load_image(p); present(); }
            }
        }
        cos_yield(); cos_sleep_ms(10);
    }
}
