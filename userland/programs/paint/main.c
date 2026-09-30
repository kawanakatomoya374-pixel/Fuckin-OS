/* paint.c-os - the Paint app, as a real ring-3 program.
 *
 * A redraw of the old kernel-space Paint (src/gui/apps/paint/gui_apps_paint.c)
 * using cos_ui: the same tool set (Pen, Brush, Eraser, Fill, Line, Rect,
 * Circle), plus two things the old version didn't have room for as
 * hand-rolled VGA code - Line/Rect/Circle now preview live as the mouse
 * drags and only commit to the canvas on release (the old version drew
 * every intermediate position permanently), and Save/Open to a small raw
 * format under /desktop so a drawing survives closing the window.
 *
 * KNOWN ISSUE (unresolved): on real hardware (QEMU), the process exits
 * immediately after its first frame - "[SYSCALL] ring3 thread exiting via
 * SYS_EXIT, code=0" appears in the serial log right after window creation,
 * even in a version of this file stripped to nothing but
 * cos_win2_create()+cos_sleep_ms(4000)+return 99 (the literal SYS_EXIT code
 * printed was still 0, not 99, and the delay was not honoured either).
 * Clock (userland/programs/clock) uses the identical launch pattern (same
 * kernel-side single-instance launcher shape, same cos_win2_create/
 * cos_sleep_ms calls, same desktop-icon double-click path through
 * gui_open_window()'s WIN_PAINT/WIN_CLOCK redirect) and runs correctly, so
 * the fault is specific to something about this program's identity (name,
 * embedding, or the WIN_PAINT path specifically) rather than a general
 * ring-3 GUI bug - narrowed but not yet found. Verified working on the host
 * build (host_build.sh) throughout. Fixing this is the next step before
 * relying on this app on real hardware.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "cos.h"
#include "cos_ui.h"

#define TOOLBAR_H 44
#define STATUS_H  24
#define FILE_MAGIC 0x434F5350u   /* "PSOC" little-endian: a C-OS Paint canvas, nothing else reads this */

typedef enum { T_PEN, T_BRUSH, T_ERASER, T_FILL, T_LINE, T_RECT, T_CIRCLE, T_COUNT } tool_t;
static const char *const TOOL_NAMES[T_COUNT] = { "Pen", "Brush", "Eraser", "Fill", "Line", "Rect", "Circle" };

static uint32_t *g_canvas = NULL;
static int g_cw = 0, g_ch = 0;

static void canvas_alloc(int w, int h) {
    if (w <= 0 || h <= 0) return;
    uint32_t *nc = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!nc) return;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            nc[(size_t)y * w + x] = (x < g_cw && y < g_ch && g_canvas) ? g_canvas[(size_t)y * g_cw + x] : 0xFFFFFFu;
    free(g_canvas);
    g_canvas = nc; g_cw = w; g_ch = h;
}
static void canvas_clear(void) { for (int i = 0; i < g_cw * g_ch; ++i) g_canvas[i] = 0xFFFFFFu; }
static void put(int x, int y, uint32_t col) { if (x >= 0 && y >= 0 && x < g_cw && y < g_ch) g_canvas[(size_t)y * g_cw + x] = col; }
static void fill_circle(int cx, int cy, int r, uint32_t col) {
    if (r < 1) r = 1;
    for (int y = -r; y <= r; ++y) for (int x = -r; x <= r; ++x) if (x * x + y * y <= r * r) put(cx + x, cy + y, col);
}
static void draw_line(int x0, int y0, int x1, int y1, int radius, uint32_t col) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 - y0; if (dy < 0) dy = -dy; dy = -dy;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fill_circle(x0, y0, radius, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void draw_rect_outline(int x0, int y0, int x1, int y1, int w, uint32_t col) {
    draw_line(x0, y0, x1, y0, w, col); draw_line(x1, y0, x1, y1, w, col);
    draw_line(x1, y1, x0, y1, w, col); draw_line(x0, y1, x0, y0, w, col);
}
static void draw_circle_outline(int cx, int cy, int r, int w, uint32_t col) {
    for (int a = 0; a < 720; ++a) {
        double t = a * 3.14159265 / 360.0;
        put(cx + (int)(r * cos(t)), cy + (int)(r * sin(t)), col);
    }
    (void)w;
}

static bool canvas_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t hdr[3] = { FILE_MAGIC, (uint32_t)g_cw, (uint32_t)g_ch };
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(g_canvas, sizeof(uint32_t), (size_t)g_cw * g_ch, f);
    fclose(f);
    return true;
}
static bool canvas_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t hdr[3];
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != FILE_MAGIC) { fclose(f); return false; }
    int w = (int)hdr[1], h = (int)hdr[2];
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { fclose(f); return false; }
    uint32_t *buf = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, sizeof(uint32_t), (size_t)w * h, f);
    fclose(f);
    if (got != (size_t)w * h) { free(buf); return false; }
    free(g_canvas); g_canvas = buf; g_cw = w; g_ch = h;
    return true;
}

int main(int argc, char **argv) {
    int32_t sw = 900, sh = 680;
    cos_win_info_t wi;
    int64_t h = cos_win2_create("Paint", sw, sh, &wi);
    if (h <= 0) return 1;
    cui_canvas c;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);

    cui_font *f_lbl = cui_font_default(12);
    cui_font *f_status = cui_font_default(12);

    const uint32_t BAR = 0x1E2433, BAR_BORDER = 0x2C3448, WIN_BG = 0xF4F5F7;
    const uint32_t BTN = 0x2A3244, BTN_SEL = 0x4D9FFF;
    const uint32_t TXT = 0xE8ECF5, TXT_DIM = 0x9AA3B8;
    static const uint32_t PALETTE[12] = {
        0x000000, 0xFFFFFF, 0xE94141, 0x33B24C, 0x3B82F6, 0xF2C21B,
        0xE04FD0, 0x33C4C4, 0x7A1F1F, 0x1F6B2E, 0x1F3E8C, 0x8A7A1F,
    };

    tool_t tool = T_PEN;
    uint32_t color = 0x000000;
    int brush = 3;
    bool dragging = false;
    int drag_ox = 0, drag_oy = 0, drag_lx = 0, drag_ly = 0;
    uint32_t *preview_backup = NULL;

    char save_path[256] = "/desktop/painting.coscanvas";
    if (argc > 1 && argv[1][0]) snprintf(save_path, sizeof save_path, "%s", argv[1]);
    canvas_load(save_path);   /* silently starts blank if the file doesn't exist yet */

    char status[96] = "";
    bool dirty = true;

    for (;;) {
        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, dragging ? 16 : 200);
        bool acted = false;

        int canvas_x = 0, canvas_y = TOOLBAR_H;
        int canvas_w = c.w, canvas_h = c.h - TOOLBAR_H - STATUS_H;
        if (canvas_w < 1) canvas_w = 1;
        if (canvas_h < 1) canvas_h = 1;
        if (canvas_w != g_cw || canvas_h != g_ch) { canvas_alloc(canvas_w, canvas_h); dirty = true; }

        if (r > 0) {
            if (ev.type == COS_EV_CLOSE) { canvas_save(save_path); break; }
            if (ev.type == COS_EV_KEY && (ev.mods & COS_MOD_CTRL) && (ev.ascii == 's' || ev.ascii == 'S')) {
                canvas_save(save_path);
                snprintf(status, sizeof status, "Saved to %s", save_path);
                acted = true;
            }
            if (ev.type == COS_EV_KEY && (ev.mods & COS_MOD_CTRL) && (ev.ascii == 'n' || ev.ascii == 'N')) {
                canvas_clear(); snprintf(status, sizeof status, "Cleared"); acted = true;
            }
            if (ev.type == COS_EV_KEY && ev.ascii == '[') { if (brush > 1) --brush; acted = true; }
            if (ev.type == COS_EV_KEY && ev.ascii == ']') { if (brush < 40) ++brush; acted = true; }

            if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) {
                int tx = 10;
                bool hit_ui = false;
                for (int i = 0; i < T_COUNT; ++i) {
                    if (ev.x >= tx && ev.x < tx + 62 && ev.y >= 6 && ev.y < 38) { tool = (tool_t)i; hit_ui = true; }
                    tx += 66;
                }
                int px = tx + 14;
                for (int i = 0; i < 12; ++i) {
                    int bx = px + (i % 6) * 26, by = 6 + (i / 6) * 16;
                    if (ev.x >= bx && ev.x < bx + 22 && ev.y >= by && ev.y < by + 14) { color = PALETTE[i]; hit_ui = true; }
                }
                int bxm = px + 6 * 26 + 16;
                if (ev.x >= bxm && ev.x < bxm + 24 && ev.y >= 6 && ev.y < 38) { if (brush > 1) --brush; hit_ui = true; }
                if (ev.x >= bxm + 28 && ev.x < bxm + 52 && ev.y >= 6 && ev.y < 38) { if (brush < 40) ++brush; hit_ui = true; }
                int sxb = c.w - 150;
                if (ev.x >= sxb && ev.x < sxb + 66 && ev.y >= 6 && ev.y < 38) { canvas_save(save_path); snprintf(status, sizeof status, "Saved"); hit_ui = true; }
                if (ev.x >= sxb + 74 && ev.x < sxb + 140 && ev.y >= 6 && ev.y < 38) { canvas_clear(); hit_ui = true; }

                if (!hit_ui && ev.x >= canvas_x && ev.x < canvas_x + canvas_w && ev.y >= canvas_y && ev.y < canvas_y + canvas_h) {
                    dragging = true;
                    drag_ox = drag_lx = ev.x - canvas_x; drag_oy = drag_ly = ev.y - canvas_y;
                    if (tool == T_LINE || tool == T_RECT || tool == T_CIRCLE) {
                        free(preview_backup);
                        preview_backup = (uint32_t *)malloc((size_t)g_cw * g_ch * sizeof(uint32_t));
                        if (preview_backup) memcpy(preview_backup, g_canvas, (size_t)g_cw * g_ch * sizeof(uint32_t));
                    } else if (tool == T_FILL) {
                        uint32_t fc = color;
                        for (int i = 0; i < g_cw * g_ch; ++i) g_canvas[i] = fc;
                        dragging = false;
                    }
                }
                acted = true;
            } else if (ev.type == COS_EV_MOUSE_MOVE && dragging) {
                int px0 = ev.x - canvas_x, py0 = ev.y - canvas_y;
                uint32_t draw_col = (tool == T_ERASER) ? 0xFFFFFFu : color;
                int size = (tool == T_BRUSH) ? brush * 2 : (tool == T_ERASER) ? brush * 2 : brush;
                if (tool == T_PEN || tool == T_BRUSH || tool == T_ERASER) {
                    draw_line(drag_lx, drag_ly, px0, py0, size, draw_col);
                    drag_lx = px0; drag_ly = py0;
                } else if (preview_backup) {
                    memcpy(g_canvas, preview_backup, (size_t)g_cw * g_ch * sizeof(uint32_t));
                    if (tool == T_LINE) draw_line(drag_ox, drag_oy, px0, py0, brush, color);
                    else if (tool == T_RECT) draw_rect_outline(drag_ox, drag_oy, px0, py0, brush, color);
                    else if (tool == T_CIRCLE) { int dx = px0 - drag_ox, dy = py0 - drag_oy; int rr = (int)(sqrt((double)(dx*dx+dy*dy))); draw_circle_outline(drag_ox, drag_oy, rr, brush, color); }
                }
                acted = true;
            } else if (ev.type == COS_EV_MOUSE_UP) {
                dragging = false;
                free(preview_backup); preview_backup = NULL;
                acted = true;
            }
        }

        if (!acted && !dirty) continue;
        dirty = false;

        cui_fill(&c, 0, 0, c.w, c.h, WIN_BG);
        cui_fill(&c, 0, 0, c.w, TOOLBAR_H, BAR);
        cui_fill(&c, 0, TOOLBAR_H - 1, c.w, 1, BAR_BORDER);

        int tx = 10;
        for (int i = 0; i < T_COUNT; ++i) {
            bool sel = (tool == (tool_t)i);
            cui_round_rect(&c, tx, 6, 60, 32, 6, sel ? BTN_SEL : BTN);
            cui_text_center(&c, f_lbl, tx, 6, 60, 32, TOOL_NAMES[i], sel ? 0xFFFFFF : TXT);
            tx += 66;
        }
        int px = tx + 14;
        for (int i = 0; i < 12; ++i) {
            int bx = px + (i % 6) * 26, by = 6 + (i / 6) * 16;
            cui_round_rect(&c, bx, by, 22, 14, 3, PALETTE[i]);
            if (PALETTE[i] == color) cui_round_rect_outline(&c, bx - 2, by - 2, 26, 18, 5, 2.0f, BTN_SEL);
        }
        int bxm = px + 6 * 26 + 16;
        cui_round_rect(&c, bxm, 6, 24, 32, 6, BTN);
        cui_text_center(&c, f_lbl, bxm, 6, 24, 32, "-", TXT);
        cui_round_rect(&c, bxm + 28, 6, 24, 32, 6, BTN);
        cui_text_center(&c, f_lbl, bxm + 28, 6, 24, 32, "+", TXT);
        char bsz[8]; snprintf(bsz, sizeof bsz, "%d", brush);
        cui_text_center(&c, f_lbl, bxm - 30, 6, 26, 32, bsz, TXT_DIM);

        int sxb = c.w - 150;
        cui_round_rect(&c, sxb, 6, 66, 32, 6, BTN);
        cui_text_center(&c, f_lbl, sxb, 6, 66, 32, "Save", TXT);
        cui_round_rect(&c, sxb + 74, 6, 66, 32, 6, BTN);
        cui_text_center(&c, f_lbl, sxb + 74, 6, 66, 32, "Clear", TXT);

        if (g_canvas) {
            for (int y = 0; y < canvas_h && y < g_ch; ++y) {
                const uint32_t *row = g_canvas + (size_t)y * g_cw;
                uint32_t *dst = c.px + (size_t)(canvas_y + y) * c.stride + canvas_x;
                for (int x = 0; x < canvas_w && x < g_cw; ++x) dst[x] = row[x];
            }
        }

        cui_fill(&c, 0, c.h - STATUS_H, c.w, STATUS_H, BAR);
        char statusline[352];
        snprintf(statusline, sizeof statusline, "%s  |  size %d  |  %s", TOOL_NAMES[tool], brush, status);
        cui_text(&c, f_status, 10, c.h - STATUS_H + 5, statusline, TXT_DIM);

        cos_win2_present(h);
    }
    canvas_save(save_path);
    cos_win2_close(h);
    return 0;
}
