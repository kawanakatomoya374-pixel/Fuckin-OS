/**
 * gui_apps_paint.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * ペイントアプリ
 *
 * 元は単一の gui_apps.c (11,638行) に含まれていたコードを、
 * 保守性向上のため機能単位に分割したものの一部。
 */
#include "gui.h"
#include "mk_desktop.h"
#include "system/password_screen.h"
#include "vga.h"
#include "mk_mp3.h"
#include "../../../apps/jpeg_viewer.h"
#include "string.h"
#include "serial.h"

#ifndef KEY_PAGEUP
#define KEY_PAGEUP   0x49
#endif
#ifndef KEY_PAGEDOWN
#define KEY_PAGEDOWN 0x51
#endif
#ifndef KEY_HOME
#define KEY_HOME     0x47
#endif
#ifndef KEY_END
#define KEY_END      0x4F
#endif
#include "memory.h"
#include "memory_physical.h"
#include "cos_version.h"
#include "rtc.h"
#include "scheduler.h"
#include "../../bios/bios.h"
#include "../../kernel/drivers/usb.h"
#include "../../kernel/drivers/pci.h"
#include "fs.h"
#include "keyboard.h"
#include "../../drivers/disk/storage.h"
#include "../../drivers/input/mouse_minimal.h"
#include <shell.h>
extern const char* fs_read_file_at(const char* path, const char* name);
extern const char* config_get_string(const char* key);
extern void gui_snapshot_save_desktop(void);
extern bool settings_set_desktop_icon_size(uint32_t size) __attribute__((weak));
extern void gui_normalize_desktop_icons(void);
#include "gui_apps_common.h"

/* ============================================================
 * PAINT APP
 * ============================================================ */
#define PAINT_CANVAS_BG rgb(255, 255, 255)
static uint32_t* g_paint_canvas = NULL;
static size_t g_paint_canvas_pixels = 0;
static int g_paint_canvas_w = 0;
static int g_paint_canvas_h = 0;
static bool g_paint_stroke_active = false;
static int g_paint_last_x = 0;
static int g_paint_last_y = 0;

static void paint_canvas_clear(int w, int h) {
    if (w <= 0 || h <= 0) return;
    size_t need = (size_t)w * (size_t)h;
    if (need == 0) return;
    if (g_paint_canvas_pixels < need) {
        uint32_t* buf = (uint32_t*)kmalloc(need * sizeof(uint32_t));
        if (!buf) return;
        if (g_paint_canvas) kfree(g_paint_canvas);
        g_paint_canvas = buf;
        g_paint_canvas_pixels = need;
    }
    g_paint_canvas_w = w;
    g_paint_canvas_h = h;
    for (size_t i = 0; i < need; ++i) {
        g_paint_canvas[i] = (uint32_t)PAINT_CANVAS_BG;
    }
}

static void paint_canvas_ensure(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!g_paint_canvas || g_paint_canvas_w != w || g_paint_canvas_h != h) {
        paint_canvas_clear(w, h);
    }
}

static inline void paint_canvas_put(int x, int y, uint32_t color) {
    if (!g_paint_canvas || x < 0 || y < 0 || x >= g_paint_canvas_w || y >= g_paint_canvas_h) return;
    g_paint_canvas[(size_t)y * (size_t)g_paint_canvas_w + (size_t)x] = color;
}

static void paint_canvas_fill_circle(int cx, int cy, int r, uint32_t color) {
    if (r < 1) r = 1;
    int rr = r * r;
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            if (x * x + y * y <= rr) paint_canvas_put(cx + x, cy + y, color);
        }
    }
}

static void paint_canvas_draw_line(int x0, int y0, int x1, int y1, int radius, uint32_t color) {
    int dx = x1 - x0;
    if (dx < 0) dx = -dx;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 - y0;
    if (dy < 0) dy = -dy;
    dy = -dy;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        paint_canvas_fill_circle(x0, y0, radius, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void paint_canvas_blit(int x, int y, int w, int h) {
    if (!g_paint_canvas || w <= 0 || h <= 0) return;
    if (w > g_paint_canvas_w) w = g_paint_canvas_w;
    if (h > g_paint_canvas_h) h = g_paint_canvas_h;
    for (int yy = 0; yy < h; ++yy) {
        const uint32_t* row = g_paint_canvas + (size_t)yy * (size_t)g_paint_canvas_w;
        for (int xx = 0; xx < w; ++xx) {
            vga_put_pixel(x + xx, y + yy, (uint64_t)row[xx]);
        }
    }
}

void draw_paint_app(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w, ch = w->h - C_TITLEBAR_H;

    vga_fill_rect(cx, cy, cw, ch, C_WIN_BG);

     /* Toolbar */
    vga_fill_rect(cx, cy, cw, 40, C_TOOLBAR);
    vga_fill_rect(cx, cy+39, cw, 1, C_BORDER);

    const char* tools[] = {"Pen", "Brush", "Eraser", "Fill", "Line", "Rect", "Circle", "Text", NULL};
    int tx = cx + 8;
    for (int i = 0; tools[i]; i++) {
        bool sel = (w->paint_tool == i);
        minimal_mouse_t* ms = _get_mouse();
        bool hov = (ms && ms->x >= tx && ms->x < tx+52 && ms->y >= cy+4 && ms->y < cy+36);
        draw_tbtn(tx, cy+4, 52, 32, tools[i], hov, sel);
        tx += 56;
    }

     /* Color palette */
    uint64_t palette[] = {
        rgb(0,0,0), rgb(255,255,255), rgb(255,0,0), rgb(0,255,0),
        rgb(0,0,255), rgb(255,255,0), rgb(255,0,255), rgb(0,255,255),
        rgb(128,0,0), rgb(0,128,0), rgb(0,0,128), rgb(128,128,0)
    };
    int px = tx + 16;
    for (int i = 0; i < 12; i++) {
        int bx = px + (i%6)*22;
        int by = cy + 4 + (i/6)*14;
        vga_fill_rect(bx, by, 18, 12, palette[i]);
        vga_draw_rect(bx, by, 18, 12, rgb(100,100,100));
        if (palette[i] == w->paint_color)
            vga_draw_rect(bx-1, by-1, 20, 14, C_ACCENT);
    }

     /* Canvas */
    int canvas_y = cy + 40;
    int canvas_h = ch - 40 - C_STATUSBAR_H;
    if (canvas_h < 1) canvas_h = 1;
    paint_canvas_ensure(cw, canvas_h);
    paint_canvas_blit(cx, canvas_y, cw, canvas_h);

    if (!g_paint_canvas || g_paint_canvas_w != cw || g_paint_canvas_h != canvas_h) {
        vga_fill_rect(cx, canvas_y, cw, canvas_h, rgb(255,255,255));
    }

     /* Canvas hint */
    if (g_paint_canvas) {
        uint64_t center_pixel = vga_get_pixel(cx + cw/2, canvas_y + canvas_h/2);
        if (center_pixel == 0xFFFFFF || center_pixel == 0 || center_pixel == rgb(255,255,255)) {
            vga_draw_string(cx+cw/2-80, canvas_y+canvas_h/2-8, "Paint Canvas - Click and drag to draw", C_TEXT_DIM, 0xFFFFFFFF);
        }
    }

     /* Color swatch */
    uint64_t show_color = (w->paint_color != 0) ? w->paint_color : rgb(0,0,0);
    vga_fill_rect(cx+cw-60, cy+8, 40, 24, show_color);
    vga_draw_rect(cx+cw-60, cy+8, 40, 24, rgb(100,100,100));
     /* Status */
    char status_buf[64]; scopy(status_buf, "Tool: ", 63);
    const char* tool_names[] = {"Pen","Brush","Eraser","Fill","Line","Rect","Circle","Text"};
    if (w->paint_tool >= 0 && w->paint_tool < 8) scat(status_buf, tool_names[w->paint_tool], 63);
    scat(status_buf, " | Size: ", 63);
    char sz_buf[8]; uitostr((uint64_t)(w->paint_size > 0 ? w->paint_size : 2), sz_buf);
    scat(status_buf, sz_buf, 63);
    draw_statusbar(cx, cy+ch-C_STATUSBAR_H, cw, status_buf, NULL);
}

void handle_paint_mouse(int idx, int mx, int my2, bool clicked) {
    window_t* w = &windows[idx];
    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w, ch = w->h - C_TITLEBAR_H;
    int canvas_y = cy + 40;
    int canvas_h = ch - 40 - C_STATUSBAR_H;
    if (canvas_h < 1) canvas_h = 1;
    paint_canvas_ensure(cw, canvas_h);

     /* Tool bar clicks */
    const char* tools[] = {"Pen","Brush","Eraser","Fill","Line","Rect","Circle","Text",NULL};
    int tx = cx + 8;
    for (int i = 0; tools[i]; i++) {
        if (clicked && mx >= tx && mx < tx+52 && my2 >= cy+4 && my2 < cy+36) {
            w->paint_tool = i;
            g_paint_stroke_active = false;
            return;
        }
        tx += 56;
    }
     /* Color palette */
    uint64_t palette[] = {
        rgb(0,0,0), rgb(255,255,255), rgb(255,0,0), rgb(0,200,0),
        rgb(0,0,255), rgb(255,255,0), rgb(255,0,255), rgb(0,255,255),
        rgb(128,0,0), rgb(0,128,0), rgb(0,0,128), rgb(128,128,0)
    };
    int px = tx + 16;
    for (int i = 0; i < 12; i++) {
        int bx = px + (i%6)*22;
        int by = cy + 4 + (i/6)*14;
        if (clicked && mx >= bx && mx < bx+18 && my2 >= by && my2 < by+12) {
            w->paint_color = palette[i];
            return;
        }
    }

    minimal_mouse_t* ms = _get_mouse();
    bool left_down = ms ? ms->left : false;
    bool in_canvas = (mx >= cx && mx < cx + cw && my2 >= canvas_y && my2 < canvas_y + canvas_h);
    if (!clicked && !left_down) {
        g_paint_stroke_active = false;
        return;
    }
    if (!in_canvas) {
        if (!left_down) g_paint_stroke_active = false;
        return;
    }

    int size = (w->paint_size > 0) ? w->paint_size : 2;
    uint32_t draw_color = (uint32_t)((w->paint_tool == 2) ? rgb(255,255,255) : w->paint_color);
    if (draw_color == 0) draw_color = (uint32_t)rgb(0,0,0);

    int px0 = mx - cx;
    int py0 = my2 - canvas_y;

    if (!g_paint_stroke_active) {
        g_paint_stroke_active = true;
        g_paint_last_x = px0;
        g_paint_last_y = py0;
    }

    if (w->paint_tool == 3) {
        for (int y = 0; y < canvas_h; ++y) {
            for (int x = 0; x < cw; ++x) {
                paint_canvas_put(x, y, draw_color);
            }
        }
        g_paint_stroke_active = false;
        return;
    }

    if (w->paint_tool == 0) {
        paint_canvas_draw_line(g_paint_last_x, g_paint_last_y, px0, py0, size, draw_color);
    } else if (w->paint_tool == 1) {
        paint_canvas_draw_line(g_paint_last_x, g_paint_last_y, px0, py0, size * 2, draw_color);
    } else if (w->paint_tool == 2) {
        paint_canvas_draw_line(g_paint_last_x, g_paint_last_y, px0, py0, size * 2, (uint32_t)rgb(255,255,255));
    } else if (w->paint_tool == 4) {
        paint_canvas_draw_line(g_paint_last_x, g_paint_last_y, px0, py0, 1, draw_color);
    } else if (w->paint_tool == 5) {
        paint_canvas_draw_line(g_paint_last_x, g_paint_last_y, px0, g_paint_last_y, 1, draw_color);
        paint_canvas_draw_line(px0, g_paint_last_y, px0, py0, 1, draw_color);
        paint_canvas_draw_line(px0, py0, g_paint_last_x, py0, 1, draw_color);
        paint_canvas_draw_line(g_paint_last_x, py0, g_paint_last_x, g_paint_last_y, 1, draw_color);
    } else if (w->paint_tool == 6) {
        int dx = px0 - g_paint_last_x;
        int dy = py0 - g_paint_last_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        int r = dx;
        if (dy > r) r = dy;
        if (r < 1) r = 1;
        for (int y = -r; y <= r; ++y) {
            for (int x = -r; x <= r; ++x) {
                if (x * x + y * y <= r * r) paint_canvas_put(px0 + x, py0 + y, draw_color);
            }
        }
    } else if (w->paint_tool == 7) {
        vga_draw_string(mx, my2, "Text tool not yet implemented", C_TEXT_GRAY, 0xFFFFFFFF);
    }

    g_paint_last_x = px0;
    g_paint_last_y = py0;
}

