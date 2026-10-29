#include "screenshot.h"
#include "vga.h"
#include "mouse.h"
#include "timer.h"
#include "fs.h"
#include "gui.h"
#include "screenshot_enc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum { SS_IDLE, SS_CHOOSE, SS_REGION } ss_mode_t;
static ss_mode_t mode = SS_IDLE;
static int sx, sy;
static bool pending;
static int px0, py0, px1, py1;
static unsigned serial_no;

/* the file format: PNG (lossless, default), JPG (small), WebP (lossless), BMP; remembered in the settings as gui.screenshot_format */
enum { FMT_PNG, FMT_JPG, FMT_WEBP, FMT_BMP, FMT_N };
static const char *const k_fmt_name[FMT_N] = { "png", "jpg", "webp", "bmp" };
static const char *const k_fmt_label[FMT_N] = { "PNG", "JPG", "WebP", "BMP" };
static int fmt = FMT_PNG;
extern void gui_request_redraw(void);
extern const char* config_get_string(const char* key) __attribute__((weak));
extern int config_set_string(const char* key, const char* value) __attribute__((weak));
extern int config_save_all(void) __attribute__((weak));
static void fmt_load(void) {
    if (!config_get_string) return;
    const char *v = config_get_string("gui.screenshot_format"); if (!v || !v[0]) return;
    for (int i = 0; i < FMT_N; ++i) if (!strcmp(v, k_fmt_name[i])) fmt = i;
}
static void fmt_set(int f) { fmt = f; gui_request_redraw(); if (config_set_string) { config_set_string("gui.screenshot_format", k_fmt_name[f]); if (config_save_all) config_save_all(); } }

static void le16(unsigned char *p, unsigned v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void le32(unsigned char *p, unsigned v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);p[2]=(unsigned char)(v>>16);p[3]=(unsigned char)(v>>24);}
static void clamp_point(int *x,int *y){if(*x<0)*x=0;if(*y<0)*y=0;if((uint64_t)*x>=SCREEN_W)*x=(int)SCREEN_W-1;if((uint64_t)*y>=SCREEN_H)*y=(int)SCREEN_H-1;}
static void request_capture(int x0,int y0,int x1,int y1){clamp_point(&x0,&y0);clamp_point(&x1,&y1);px0=x0< x1?x0:x1;px1=x0< x1?x1:x0;py0=y0< y1?y0:y1;py1=y0< y1?y1:y0;pending=true;mode=SS_IDLE;gui_request_redraw();}

/* what is on the screen in the rectangle, as 0x00RRGGBB */
static uint32_t *grab(int x0, int y0, int w, int h) {
    uint32_t *px = (uint32_t *)malloc((size_t)w * (size_t)h * 4u); if (!px) return NULL;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) px[(size_t)y * w + x] = (uint32_t)vga_get_pixel(x0 + x, y0 + y) & 0xFFFFFFu;
    return px;
}
static uint8_t *encode_bmp(const uint32_t *px, int w, int h, size_t *n) {
    uint32_t stride = (uint32_t)w * 4u, data = stride * (uint32_t)h, total = 54u + data;
    unsigned char *b = (unsigned char *)malloc(total); if (!b) return NULL; memset(b, 0, total);
    b[0]='B'; b[1]='M'; le32(b+2,total); le32(b+10,54); le32(b+14,40); le32(b+18,(unsigned)w); le32(b+22,(unsigned)h); le16(b+26,1); le16(b+28,32); le32(b+34,data);
    for (int y = 0; y < h; y++) { unsigned char *row = b + 54 + (size_t)y * stride; const uint32_t *src = px + (size_t)(h - 1 - y) * w; for (int x = 0; x < w; x++) { uint32_t c = src[x]; row[x*4]=(unsigned char)c; row[x*4+1]=(unsigned char)(c>>8); row[x*4+2]=(unsigned char)(c>>16); row[x*4+3]=255; } }
    *n = total; return b;
}
static void save_shot(int x0,int y0,int x1,int y1){
    int w=x1-x0+1,h=y1-y0+1; if(w<1||h<1) return;
    uint32_t *px = grab(x0, y0, w, h); if (!px) { gui_notify("Screenshot could not be saved",1800); return; }
    size_t n = 0; uint8_t *d = NULL;
    if (fmt == FMT_PNG) d = ss_encode_png(px, w, h, &n);
    else if (fmt == FMT_JPG) d = ss_encode_jpeg(px, w, h, 92, &n);
    else if (fmt == FMT_WEBP) d = ss_encode_webp(px, w, h, &n);
    else d = encode_bmp(px, w, h, &n);
    free(px);
    if (!d) { gui_notify("Screenshot could not be encoded",1800); return; }
    char name[64], path[96]; uint64_t sz; bool isd;
    do { snprintf(name, sizeof name, "Screenshot-%u.%s", ++serial_no, k_fmt_name[fmt]); snprintf(path, sizeof path, "/screenshots/%s", name); } while (fs_stat_path(path, &sz, &isd) && serial_no < 100000);
    if (!fs_write_file_at("/screenshots", name, (const char*)d, n)) gui_notify("Screenshot could not be saved",1800);
    else gui_notify("Screenshot saved to /screenshots",1800);
    free(d);
}
bool screenshot_active(void){return mode!=SS_IDLE;}
void screenshot_begin(void){(void)fs_create_dir_at("/","screenshots");fmt_load();mode=SS_CHOOSE;pending=false;gui_request_redraw();gui_notify("Screenshot: choose the file type, then Full screen or Select area",2600);}
void screenshot_handle_mouse(void){
    if (mode == SS_CHOOSE && mouse.left_click) {
        int cx = (int)SCREEN_W / 2;
        if (mouse.x >= cx - 190 && mouse.x < cx - 20 && mouse.y >= 80 && mouse.y < 130) { request_capture(0, 0, (int)SCREEN_W - 1, (int)SCREEN_H - 1); return; }
        if (mouse.x >= cx + 20 && mouse.x < cx + 190 && mouse.y >= 80 && mouse.y < 130) { mode = SS_REGION; gui_request_redraw(); gui_notify("Drag a rectangle, then release", 2200); return; }
        for (int i = 0; i < FMT_N; ++i) if (mouse.x >= cx - 190 + i * 96 && mouse.x < cx - 190 + i * 96 + 88 && mouse.y >= 140 && mouse.y < 168) { fmt_set(i); return; }
        if (mouse.x < cx - 210 || mouse.x >= cx + 210 || mouse.y < 62 || mouse.y >= 180) { mode = SS_IDLE; gui_request_redraw(); }     /* a click outside the bar closes it */
    } else if (mode == SS_REGION) {
        if (mouse.left_click) { sx = (int)mouse.x; sy = (int)mouse.y; }
        if (mouse.left_release) { request_capture(sx, sy, (int)mouse.x, (int)mouse.y); }
    }
}
void screenshot_service_before_overlay(void){if(pending){save_shot(px0,py0,px1,py1);pending=false;}}

void screenshot_draw_overlay(void){
    if (mode == SS_CHOOSE) {
        int cx = (int)SCREEN_W / 2;
        vga_fill_rect(cx - 210, 62, 420, 118, 0xE91B2430); vga_draw_rect(cx - 210, 62, 420, 118, 0xFF5B6B86);
        vga_fill_rect(cx - 190, 80, 170, 50, 0xFF2563EB); vga_fill_rect(cx + 20, 80, 170, 50, 0xFF334155);
        vga_draw_string(cx - 174, 99, "Full screen", 0xFFFFFFFF, 0xFF2563EB); vga_draw_string(cx + 38, 99, "Select area", 0xFFFFFFFF, 0xFF334155);
        for (int i = 0; i < FMT_N; ++i) { uint32_t bg = i == fmt ? 0xFF2563EB : 0xFF334155; vga_fill_rect(cx - 190 + i * 96, 140, 88, 28, bg); vga_draw_string(cx - 190 + i * 96 + 28, 148, k_fmt_label[i], 0xFFFFFFFF, bg); }
    } else if (mode == SS_REGION && mouse.left) {
        int x0 = sx < (int)mouse.x ? sx : (int)mouse.x, x1 = sx < (int)mouse.x ? (int)mouse.x : sx;
        int y0 = sy < (int)mouse.y ? sy : (int)mouse.y, y1 = sy < (int)mouse.y ? (int)mouse.y : sy;
        vga_draw_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, 0xFF38BDF8);
    }
}
