/**
 * gui_desktop_render.c - GUIコア (壁紙描画・デスクトップアイコン描画)
 * gui.c (5,588行) から分割生成。詳細は gui_internal.h を参照。
 */

#include "gui.h"
#include "gui_internal.h"
#include "memory.h"
#include "voxel_games_advanced.h"
#include "vga.h"
#include "gfx_blit.h"
#include "mouse.h"
#include "../drivers/input/mouse_minimal.h"
#include "keyboard.h"
#include "fs.h"
#include "../bios/bios.h"
#include "io.h"
#include "serial.h"
#include "timer.h"
#include "../apps/development/python_ide_gui.h"
#include "gui_render_engine.h"
#include "gui_utils.h"
#include "../apps/system/password_screen.h"
#include "../components/boot_animation.h"
#include "notification_center.h"
#include "theme_system.h"
#include <shell.h>
#include <string.h>
#include <stdio.h>

#include "../../apps/png_decoder.h"

#define WALLPAPER_OWN_MAX_W 1920
#define WALLPAPER_OWN_MAX_H 1080

static uint32_t* s_wallpaper_own_buf = NULL;  /* 0x00RRGGBB, stride = s_wallpaper_own_w - see conversion note below */
static uint64_t s_wallpaper_own_w = 0;
static uint64_t s_wallpaper_own_h = 0;
static bool s_wallpaper_own_valid = false;

/* Decodes `path` into this module's own persistent buffer - completely
 * independent from jpeg_viewer's shared singleton state, so nothing else
 * loading a different image anywhere else in the GUI can ever change what
 * the wallpaper is currently showing out from under it. PNG only for now
 * (the current wallpaper asset is a PNG); BMP wallpapers would need
 * decode_bmp() exposed from jpeg_viewer.c the same way png_decode() is. */
bool gui_wallpaper_decode_own_buffer(const char* path) {
    extern int cos_fs_read_file(const char* path, void* buffer, uint64_t size);
    if (!path || !path[0]) return false;

    if (!s_wallpaper_own_buf) {
        s_wallpaper_own_buf = (uint32_t*)kmalloc((size_t)WALLPAPER_OWN_MAX_W * WALLPAPER_OWN_MAX_H * 4);
        if (!s_wallpaper_own_buf) return false;
    }

    /* FS_MAX_DATA-sized read buffer is enough for anything this filesystem
     * can actually store (see the file-size-limit discussion elsewhere). */
    static uint8_t file_buf[64 * 1024];
    int64_t got = cos_fs_read_file(path, file_buf, sizeof(file_buf));
    if (got <= 0) return false;

    uint64_t w = 0, h = 0;
    /* png_decode() writes raw BGRA bytes - decode straight into
     * s_wallpaper_own_buf's storage (same 4-bytes-per-pixel footprint
     * either way, just reinterpreted), then convert to this file's
     * uint32_t 0x00RRGGBB convention *once* here, in place, instead
     * of once per frame the way the old per-pixel draw loop used to.
     * Forward iteration makes the in-place rewrite safe: each pixel's
     * write only touches bytes its own read has already consumed. */
    if (!png_decode(file_buf, (uint64_t)got, (uint8_t*)s_wallpaper_own_buf,
                     WALLPAPER_OWN_MAX_W, WALLPAPER_OWN_MAX_H, &w, &h)) {
        return false;
    }
    uint8_t* bytes = (uint8_t*)s_wallpaper_own_buf;
    uint64_t pixel_count = w * h;
    for (uint64_t i = 0; i < pixel_count; ++i) {
        const uint8_t* src = bytes + i * 4; /* B, G, R, A */
        s_wallpaper_own_buf[i] = ((uint32_t)src[2] << 16) | ((uint32_t)src[1] << 8) | (uint32_t)src[0];
    }
    s_wallpaper_own_w = w;
    s_wallpaper_own_h = h;
    s_wallpaper_own_valid = true;
    return true;
}

static void wallpaper_draw_own_buffer_scaled(int dst_x, int dst_y, uint64_t dst_w, uint64_t dst_h) {
    if (!s_wallpaper_own_valid || !s_wallpaper_own_buf || s_wallpaper_own_w == 0 || s_wallpaper_own_h == 0) return;
    if (dst_w == 0 || dst_h == 0) return;

    if (!backbuffer) return;

    gfx_surface_t dst = gfx_surface_make(backbuffer, (int)SCREEN_W, (int)SCREEN_H, (int)SCREEN_W);
    gfx_surface_t src = gfx_surface_make(s_wallpaper_own_buf, (int)s_wallpaper_own_w, (int)s_wallpaper_own_h, (int)s_wallpaper_own_w);
    gfx_blit_scaled(&dst, dst_x, dst_y, (int)dst_w, (int)dst_h,
                     &src, 0, 0, (int)s_wallpaper_own_w, (int)s_wallpaper_own_h,
                     GFX_BLIT_COPY, 0);
}

void draw_wallpaper(void) {
    int dh = (int)SCREEN_H - TASKBAR_H;
    int dw = (int)SCREEN_W;
    const wallpaper_preset_t* wp = &wallpaper_presets[current_wallpaper];

    if (wp->is_jpeg) {
        if (!gui_wallpaper_image_loaded) {
            gui_reload_image_wallpaper();
        }
        if (gui_wallpaper_image_loaded) {
            wallpaper_draw_own_buffer_scaled(0, 0, (uint64_t)dw, (uint64_t)dh);
            return;
        }
    }

    for (int y = 0; y < dh; y++) {
        int t = (dh > 1) ? (y * 255 / (dh - 1)) : 0;
        uint8_t tr = (wp->top >> 16) & 0xFF;
        uint8_t tg = (wp->top >>  8) & 0xFF;
        uint8_t tb = (wp->top      ) & 0xFF;
        uint8_t br = (wp->bottom >> 16) & 0xFF;
        uint8_t bg = (wp->bottom >>  8) & 0xFF;
        uint8_t bb = (wp->bottom      ) & 0xFF;
        uint8_t r = (uint8_t)((tr * (255 - t) + br * t) / 255);
        uint8_t g = (uint8_t)((tg * (255 - t) + bg * t) / 255);
        uint8_t b = (uint8_t)((tb * (255 - t) + bb * t) / 255);
        vga_fill_rect(0, y, dw, 1, rgb(r, g, b));
    }

}



static bool gui_desktop_ext_eq_ci(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

static bool gui_desktop_path_looks_text(const char* path) {
    if (!path || !path[0]) return false;
    const char* dot = strrchr(path, '.');
    if (!dot || !dot[1]) return false;
    const char* ext = dot + 1;
    return gui_desktop_ext_eq_ci(ext, "txt") || gui_desktop_ext_eq_ci(ext, "md") ||
           gui_desktop_ext_eq_ci(ext, "c")   || gui_desktop_ext_eq_ci(ext, "h")  ||
           gui_desktop_ext_eq_ci(ext, "cpp") || gui_desktop_ext_eq_ci(ext, "hpp")||
           gui_desktop_ext_eq_ci(ext, "py")  || gui_desktop_ext_eq_ci(ext, "sh") ||
           gui_desktop_ext_eq_ci(ext, "json")|| gui_desktop_ext_eq_ci(ext, "xml")||
           gui_desktop_ext_eq_ci(ext, "html")|| gui_desktop_ext_eq_ci(ext, "css")||
           gui_desktop_ext_eq_ci(ext, "js")  || gui_desktop_ext_eq_ci(ext, "csv")||
           gui_desktop_ext_eq_ci(ext, "ini") || gui_desktop_ext_eq_ci(ext, "cfg")||
           gui_desktop_ext_eq_ci(ext, "log");
}

/* ============================================================
 * App icon badges + glyphs
 * ------------------------------------------------------------
 * Every app icon used to be drawn as TWO nested rounded-rect
 * cards: draw_desktop_icons() painted an outer "tile" background,
 * and then each per-app function below painted its own separate
 * inner frame + drop shadow via gui_draw_icon_frame()/
 * gui_draw_icon_shadow() before drawing its glyph on top of that.
 * Combined with vga_draw_rounded_rect() never actually drawing its
 * corner arcs (see the vga.c fix), that's what made every icon
 * look like a "rounded box melted into another rounded box."
 *
 * This replaces that with ONE flat rounded-square badge per icon,
 * in a color unique to that app, with a single bold glyph on top.
 * gui_draw_app_icon() is the single entry point - it draws the
 * badge at whatever `size` is asked for and dispatches to the
 * right glyph, so desktop icons, taskbar buttons and the dock all
 * share the exact same art, just at different sizes. Every glyph
 * below is defined proportionally to `size` rather than in fixed
 * pixel offsets, so - unlike the old per-icon art - it now also
 * scales cleanly with the desktop icon-size setting. */

typedef struct {
    uint64_t top;
    uint64_t bot;
    uint64_t border;
} icon_badge_t;

static icon_badge_t gui_icon_badge_for_kind(int kind, bool hov) {
    uint64_t top, bot;
    switch (kind) {
        case WIN_FILE_MGR:        top = rgb(250, 191, 92);  bot = rgb(221, 152, 46);  break;
        case WIN_TEXT_EDITOR:     top = rgb(104, 168, 246); bot = rgb(52, 118, 213);  break;
        case WIN_TERMINAL:        top = rgb(58, 64, 78);    bot = rgb(24, 27, 36);    break;
        case WIN_SETTINGS:        top = rgb(146, 158, 176); bot = rgb(98, 110, 128);  break;
        case WIN_ABOUT:           top = rgb(84, 190, 205);  bot = rgb(40, 143, 160);  break;
        case WIN_CALC:            top = rgb(78, 88, 114);   bot = rgb(42, 49, 68);    break;
        case WIN_STORAGE:         top = rgb(154, 168, 188); bot = rgb(104, 120, 142); break;
        case WIN_BROWSER:         top = rgb(94, 162, 238);  bot = rgb(34, 111, 199);  break;
        case WIN_TASK_MGR:        top = rgb(233, 122, 106); bot = rgb(196, 72, 64);   break;
        case WIN_PAINT:           top = rgb(232, 130, 180); bot = rgb(190, 68, 132);  break;
        case WIN_MUSIC:           top = rgb(168, 130, 240); bot = rgb(116, 76, 204);  break;
        case WIN_CLOCK:           top = rgb(240, 168, 92);  bot = rgb(206, 120, 44);  break;
        case WIN_SYSINFO:         top = rgb(80, 194, 172);  bot = rgb(36, 148, 128);  break;
        case WIN_PYTHON_IDE:      top = rgb(76, 130, 212);  bot = rgb(38, 86, 162);   break;
        case WIN_CALC_GRAPH:      top = rgb(100, 154, 224); bot = rgb(52, 106, 180);  break;
        case WIN_SHEET:           top = rgb(106, 190, 124); bot = rgb(56, 146, 80);   break;
        case WIN_VOXEL_GAME:      top = rgb(134, 144, 228); bot = rgb(88, 98, 182);   break;
        case WIN_JPEG:            top = rgb(108, 194, 228); bot = rgb(54, 150, 192);  break;
        case WIN_MEMORY_MGR:      top = rgb(144, 172, 212); bot = rgb(90, 120, 166);  break;
        case WIN_HTTP_DOWNLOADER: top = rgb(100, 190, 154); bot = rgb(50, 146, 110);  break;
        default:                  top = rgb(154, 166, 186); bot = rgb(106, 120, 142); break;
    }
    if (hov) { top = lighten(top, 18); bot = lighten(bot, 14); }
    icon_badge_t b;
    b.top = top;
    b.bot = bot;
    b.border = darken(bot, 22);
    return b;
}

/* Vertical 2-tone gradient fill masked to a rounded-rect silhouette.
 * Built from the same band + corner-circle decomposition
 * vga_fill_rounded_rect() uses internally (rather than filling a
 * plain rect and letting a solid rounded-rect overwrite it, which is
 * what the old draw_desktop_icons() did - drawing its gradient loop
 * and then immediately covering it with a flat vga_fill_rounded_rect(),
 * so the gradient never actually showed), so the gradient's shape
 * matches the badge exactly. */
static void gui_fill_rounded_rect_vgrad(int x, int y, int w, int h, int r, uint64_t top, uint64_t bot, int split) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    for (int row = 0; row < h; row++) {
        vga_fill_rect(x + r, y + row, w - 2 * r, 1, (row < split) ? top : bot);
    }
    for (int row = r; row < h - r; row++) {
        uint64_t c = (row < split) ? top : bot;
        vga_fill_rect(x, y + row, r, 1, c);
        vga_fill_rect(x + w - r, y + row, r, 1, c);
    }
    vga_fill_circle(x + r, y + r, r, top);
    vga_fill_circle(x + w - r - 1, y + r, r, top);
    vga_fill_circle(x + r, y + h - r - 1, r, bot);
    vga_fill_circle(x + w - r - 1, y + h - r - 1, r, bot);
}

static int gui_icon_badge_radius(int size) {
    int r = size / 6;
    if (r < 4) r = 4;
    if (r > 18) r = 18;
    return r;
}

static void gui_draw_icon_badge(int x, int y, int size, icon_badge_t c) {
    int r = gui_icon_badge_radius(size);
    int shadow_off = size / 24;
    if (shadow_off < 1) shadow_off = 1;
    /* single soft drop shadow (the old version stacked 3 of these,
     * which just muddied the edge further once the stroke bug above
     * is fixed) */
    vga_fill_rounded_rect(x + shadow_off, y + shadow_off * 2, size, size, r, rgb(10, 13, 20));
    gui_fill_rounded_rect_vgrad(x, y, size, size, r, c.top, c.bot, size * 2 / 5);
    vga_draw_rounded_rect(x, y, size, size, r, c.border);
    int hi_inset = r > 2 ? r : 2;
    if (size - 2 * hi_inset > 0) {
        vga_fill_rect(x + hi_inset, y + 1, size - 2 * hi_inset, 1, lighten(c.top, 26));
    }
}

static void gui_glyph_folder(int x, int y, int size, icon_badge_t b) {
    uint64_t body = rgb(255, 255, 255);
    uint64_t shade = blend(body, b.bot, 190);
    int pad = size / 8;
    int tab_w = size * 2 / 5;
    int tab_h = size / 7;
    int body_y = y + size * 2 / 5;
    int body_h = size - size * 2 / 5 - pad;
    int r = size / 12; if (r < 2) r = 2;
    vga_fill_rounded_rect(x + pad, body_y - tab_h, tab_w, tab_h * 2, r, body);
    vga_fill_rounded_rect(x + pad, body_y, size - 2 * pad, body_h, r + 1, body);
    vga_fill_rect(x + pad, body_y, size - 2 * pad, body_h / 3, shade);
}

static void gui_glyph_document(int x, int y, int size, icon_badge_t b) {
    uint64_t page = rgb(255, 255, 255);
    uint64_t bar = blend(page, b.bot, 60);
    uint64_t line = blend(page, b.bot, 150);
    int pad = size / 6;
    int pw = size - 2 * pad, ph = size - 2 * pad;
    int px = x + pad, py = y + pad;
    int r = size / 14; if (r < 2) r = 2;
    vga_fill_rounded_rect(px, py, pw, ph, r, page);
    vga_fill_rect(px, py, pw, ph / 6, bar);
    int lh = size / 28; if (lh < 1) lh = 1;
    int gap = ph / 5;
    for (int i = 1; i <= 3; i++) {
        int ly = py + ph / 6 + i * gap - gap / 2;
        int lw = (i == 3) ? pw * 2 / 5 : pw * 3 / 5;
        vga_fill_rect(px + pw / 6, ly, lw, lh, line);
    }
}

static void gui_glyph_file(int x, int y, int size, icon_badge_t b) {
    uint64_t page = rgb(255, 255, 255);
    uint64_t fold = blend(page, b.bot, 130);
    int pad = size / 6;
    int pw = size - 2 * pad, ph = size - 2 * pad;
    int px = x + pad, py = y + pad;
    int r = size / 14; if (r < 2) r = 2;
    vga_fill_rounded_rect(px, py, pw, ph, r, page);
    int fs = pw / 3;
    for (int i = 0; i < fs; i++) {
        vga_fill_rect(px + pw - fs + i, py + i, fs - i, 1, fold);
    }
}

static void gui_glyph_terminal(int x, int y, int size, icon_badge_t b) {
    /* The former free-floating diagonal pixels looked like a broken glyph at
     * desktop, taskbar, and small-menu scales.  Keep every terminal mark in a
     * bounded screen silhouette so the icon remains recognisable down to 16px. */
    uint64_t frame = rgb(235, 242, 248);
    uint64_t screen = blend(rgb(20, 28, 38), b.bot, 32);
    uint64_t prompt = rgb(122, 234, 158);
    uint64_t cursor = rgb(232, 238, 246);
    int pad = size / 6; if (pad < 2) pad = 2;
    int sx = x + pad, sy = y + pad;
    int sw = size - pad * 2, sh = size - pad * 2;
    int radius = size / 12; if (radius < 2) radius = 2;
    int stroke = size / 13; if (stroke < 2) stroke = 2;

    vga_fill_rounded_rect(sx, sy, sw, sh, radius, screen);
    vga_draw_rounded_rect(sx, sy, sw, sh, radius, frame);

    int px = sx + sw / 4;
    int py = sy + sh / 3;
    /* Thick, compact '>_' prompt: no unsupported diagonal font glyphs. */
    vga_fill_rect(px, py, stroke, stroke, prompt);
    vga_fill_rect(px + stroke, py + stroke, stroke, stroke, prompt);
    vga_fill_rect(px, py + stroke * 2, stroke, stroke, prompt);
    int line_x = px + stroke * 4;
    int line_y = sy + sh * 2 / 3;
    int line_w = sw - (line_x - sx) - stroke;
    if (line_w < stroke) line_w = stroke;
    vga_fill_rect(line_x, line_y, line_w, stroke, cursor);
}

static void gui_glyph_gear(int x, int y, int size, icon_badge_t b) {
    uint64_t g = rgb(255, 255, 255);
    uint64_t hole = blend(g, b.bot, 200);
    int cx = x + size / 2, cy = y + size / 2;
    int r = size * 3 / 10;
    int tw = size / 6, th = size / 9;
    if (th < 2) th = 2;
    vga_fill_rect(cx - tw / 2, y + size / 12, tw, th, g);
    vga_fill_rect(cx - tw / 2, y + size - size / 12 - th, tw, th, g);
    vga_fill_rect(x + size / 12, cy - tw / 2, th, tw, g);
    vga_fill_rect(x + size - size / 12 - th, cy - tw / 2, th, tw, g);
    vga_fill_circle(cx, cy, r, g);
    vga_fill_circle(cx, cy, r * 2 / 5, hole);
}

static void gui_glyph_about(int x, int y, int size, icon_badge_t b) {
    (void)b;
    uint64_t g = rgb(255, 255, 255);
    int cx = x + size / 2, cy = y + size / 2;
    int r = size * 3 / 8;
    vga_draw_circle(cx, cy, r, g);
    vga_draw_circle(cx, cy, r - 1, g);
    int barw = size / 9; if (barw < 2) barw = 2;
    vga_fill_rect(cx - barw / 2, cy - size / 28, barw, size * 3 / 10, g);
    vga_fill_circle(cx, cy - size / 5, barw * 2 / 3, g);
}

static void gui_glyph_calc(int x, int y, int size, icon_badge_t b) {
    uint64_t body = rgb(255, 255, 255);
    uint64_t screen = blend(body, b.bot, 210);
    uint64_t key = blend(body, b.bot, 90);
    uint64_t accent = rgb(240, 170, 90);
    int pad = size / 6;
    int bw = size - 2 * pad, bh = size - 2 * pad;
    int bx = x + pad, by = y + pad;
    int r = size / 14; if (r < 2) r = 2;
    vga_fill_rounded_rect(bx, by, bw, bh, r, body);
    vga_fill_rounded_rect(bx + bw / 8, by + bh / 8, bw * 3 / 4, bh / 4, r > 1 ? r - 1 : 1, screen);
    int kx0 = bx + bw / 8, ky0 = by + bh * 9 / 20;
    int kw = bw * 3 / 16, kh = bh / 6, gap = bw / 16;
    if (kw < 1) kw = 1;
    if (kh < 1) kh = 1;
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 3; col++) {
            uint64_t kc = (row == 1 && col == 2) ? accent : key;
            vga_fill_rect(kx0 + col * (kw + gap), ky0 + row * (kh + gap), kw, kh, kc);
        }
    }
}

static void gui_glyph_drive(int x, int y, int size, icon_badge_t b) {
    (void)b;
    uint64_t body = rgb(255, 255, 255);
    uint64_t led = rgb(120, 230, 150);
    int pad = size / 6;
    int dw = size - 2 * pad, dh = size / 3;
    int dx = x + pad, dy = y + size / 2 - dh / 2;
    int r = size / 16; if (r < 2) r = 2;
    vga_fill_rounded_rect(dx, dy, dw, dh, r, body);
    int ledr = size / 20; if (ledr < 2) ledr = 2;
    for (int i = 0; i < 3; i++) {
        vga_fill_circle(dx + dw * (i + 1) / 4, dy + dh / 2, ledr, led);
    }
}

static void gui_glyph_browser(int x, int y, int size, icon_badge_t b) {
    uint64_t g = rgb(255, 255, 255);
    uint64_t soft = blend(g, b.bot, 130);
    int cx = x + size / 2, cy = y + size / 2;
    int r = size * 3 / 8;
    vga_fill_circle(cx, cy, r, g);
    vga_draw_circle(cx, cy, r, soft);
    int ir = r * 3 / 5;
    vga_draw_circle(cx, cy, ir, soft);
    vga_draw_line(cx - r, cy, cx + r, cy, soft);
    vga_draw_line(cx, cy - r, cx, cy + r, soft);
}

static void gui_glyph_taskmgr(int x, int y, int size, icon_badge_t b) {
    (void)b;
    uint64_t g = rgb(255, 255, 255);
    int pad = size / 5;
    int baseline = y + size - pad;
    int bw = size / 8; if (bw < 3) bw = 3;
    int gap = bw * 2 / 3;
    int heights[4];
    heights[0] = size * 2 / 5;
    heights[1] = size * 3 / 5;
    heights[2] = size / 3;
    heights[3] = size * 7 / 10;
    int bx = x + pad;
    for (int i = 0; i < 4; i++) {
        vga_fill_rect(bx, baseline - heights[i], bw, heights[i], g);
        bx += bw + gap;
    }
}

static void gui_glyph_paint(int x, int y, int size, icon_badge_t b) {
    uint64_t body = rgb(255, 255, 255);
    int cx = x + size / 2, cy = y + size / 2;
    int r = size * 3 / 8;
    vga_fill_circle(cx, cy, r, body);
    vga_fill_circle(cx + r / 3, cy + r / 3, r / 3, b.bot);
    int dr = size / 14; if (dr < 2) dr = 2;
    vga_fill_circle(cx - r / 2, cy - r / 4, dr, rgb(240, 120, 110));
    vga_fill_circle(cx, cy - r / 2, dr, rgb(110, 190, 240));
    vga_fill_circle(cx + r / 2, cy - r / 5, dr, rgb(140, 220, 140));
}

static void gui_glyph_music(int x, int y, int size, icon_badge_t b) {
    (void)b;
    uint64_t g = rgb(255, 255, 255);
    int nr = size / 7; if (nr < 3) nr = 3;
    int x1 = x + size * 3 / 8, x2 = x + size * 5 / 8;
    int y1 = y + size * 2 / 3, y2 = y + size * 7 / 12;
    int sw = size / 14; if (sw < 2) sw = 2;
    int beam_y = y + size / 5;
    vga_fill_rect(x1 + nr - sw, beam_y, sw, y1 - beam_y, g);
    vga_fill_rect(x2 + nr - sw, y + size / 8, sw, y2 - (y + size / 8), g);
    vga_fill_rect(x1 + nr - sw, beam_y, (x2 - x1) + sw, size / 10, g);
    vga_fill_circle(x1, y1, nr, g);
    vga_fill_circle(x2, y2, nr, g);
}

static void gui_glyph_clock(int x, int y, int size, icon_badge_t b) {
    (void)b;
    uint64_t g = rgb(255, 255, 255);
    int cx = x + size / 2, cy = y + size / 2;
    int r = size * 3 / 8;
    vga_draw_circle(cx, cy, r, g);
    vga_draw_circle(cx, cy, r - 1, g);
    int mw = size / 20; if (mw < 1) mw = 1;
    vga_fill_rect(cx - mw / 2, cy - r * 3 / 5, mw, r * 3 / 5, g);
    vga_fill_rect(cx, cy - mw, r * 2 / 5, mw * 2, g);
    vga_fill_circle(cx, cy, size / 24 > 1 ? size / 24 : 1, g);
}

static void gui_glyph_sysinfo(int x, int y, int size, icon_badge_t b) {
    (void)b;
    uint64_t g = rgb(255, 255, 255);
    int midy = y + size / 2;
    int x0 = x + size / 6;
    int seg = size / 8;
    for (int t = -1; t <= 1; t++) {
        vga_draw_line(x0, midy + t, x0 + seg, midy + t, g);
        vga_draw_line(x0 + seg, midy + t, x0 + seg * 2, midy - size / 4 + t, g);
        vga_draw_line(x0 + seg * 2, midy - size / 4 + t, x0 + seg * 3, midy + size / 4 + t, g);
        vga_draw_line(x0 + seg * 3, midy + size / 4 + t, x0 + seg * 4, midy + t, g);
        vga_draw_line(x0 + seg * 4, midy + t, x0 + seg * 5, midy + t, g);
    }
}

static void gui_glyph_python(int x, int y, int size, icon_badge_t b) {
    (void)b;
    uint64_t blue = rgb(90, 150, 235);
    uint64_t yellow = rgb(240, 205, 90);
    uint64_t eye = rgb(255, 255, 255);
    int u = size * 3 / 8;
    int r = size / 16; if (r < 2) r = 2;
    int er = size / 16; if (er < 2) er = 2;
    vga_fill_rounded_rect(x + size / 6, y + size / 6, u, u, r, blue);
    vga_fill_rounded_rect(x + size - size / 6 - u, y + size - size / 6 - u, u, u, r, yellow);
    vga_fill_circle(x + size / 6 + u * 2 / 3, y + size / 6 + u / 3, er, eye);
    vga_fill_circle(x + size - size / 6 - u * 2 / 3, y + size - size / 6 - u / 3, er, eye);
}

static void gui_glyph_graph(int x, int y, int size, icon_badge_t b) {
    (void)b;
    uint64_t g = rgb(255, 255, 255);
    int pad = size / 5;
    int ox = x + pad, oy = y + size - pad;
    vga_draw_line(ox, oy, ox, y + pad, g);
    vga_draw_line(ox, oy, x + size - pad, oy, g);
    vga_draw_line(ox, oy - size / 6, ox + size / 5, oy - size * 2 / 5, g);
    vga_draw_line(ox + size / 5, oy - size * 2 / 5, ox + size * 2 / 5, oy - size / 4, g);
    vga_draw_line(ox + size * 2 / 5, oy - size / 4, x + size - pad, y + pad + size / 8, g);
    vga_fill_circle(x + size - pad, y + pad + size / 8, size / 18 > 1 ? size / 18 : 1, g);
}

static void gui_glyph_sheet(int x, int y, int size, icon_badge_t b) {
    uint64_t g = rgb(255, 255, 255);
    uint64_t line = blend(g, b.bot, 140);
    int pad = size / 6;
    int gw = size - 2 * pad, gh = size - 2 * pad;
    int gx = x + pad, gy = y + pad;
    int r = size / 16; if (r < 2) r = 2;
    int lt = size / 26; if (lt < 1) lt = 1;
    vga_fill_rounded_rect(gx, gy, gw, gh, r, g);
    vga_fill_rect(gx, gy + gh / 3, gw, lt, line);
    vga_fill_rect(gx, gy + gh * 2 / 3, gw, lt, line);
    vga_fill_rect(gx + gw / 3, gy, lt, gh, line);
    vga_fill_rect(gx + gw * 2 / 3, gy, lt, gh, line);
    vga_fill_rect(gx, gy, gw / 3, gh / 3, blend(g, b.bot, 60));
}

static void gui_glyph_game(int x, int y, int size, icon_badge_t b) {
    uint64_t g = rgb(255, 255, 255);
    uint64_t dpad = blend(g, b.bot, 120);
    int pad = size / 6;
    int bw = size - 2 * pad, bh = size * 2 / 5;
    int bx = x + pad, by = y + size / 2 - bh / 2;
    vga_fill_rounded_rect(bx, by, bw, bh, bh / 2, g);
    int dcx = bx + bw / 4, dcy = by + bh / 2;
    int dl = bh / 3;
    vga_fill_rect(dcx - dl / 2, dcy - 2, dl, 4, dpad);
    vga_fill_rect(dcx - 2, dcy - dl / 2, 4, dl, dpad);
    int br = size / 16; if (br < 2) br = 2;
    vga_fill_circle(bx + bw * 3 / 4, dcy - bh / 6, br, rgb(232, 110, 100));
    vga_fill_circle(bx + bw * 3 / 4 + size / 8, dcy + bh / 8, br, rgb(110, 190, 230));
}

static void gui_glyph_image(int x, int y, int size, icon_badge_t b) {
    uint64_t frame = rgb(255, 255, 255);
    int pad = size / 6;
    int fw = size - 2 * pad, fh = size - 2 * pad;
    int fx = x + pad, fy = y + pad;
    int r = size / 16; if (r < 2) r = 2;
    vga_fill_rounded_rect(fx, fy, fw, fh, r, frame);
    int ix = fx + size / 22, iy = fy + size / 22;
    int iw = fw - size / 11, ih = fh - size / 11;
    if (iw < 2) iw = 2;
    if (ih < 2) ih = 2;
    vga_fill_rect(ix, iy, iw, ih, blend(frame, b.bot, 60));
    int sr = size / 16; if (sr < 2) sr = 2;
    vga_fill_circle(ix + iw / 4, iy + ih / 4, sr, rgb(250, 206, 90));
    int gh = ih / 3; if (gh < 1) gh = 1;
    vga_fill_rect(ix, iy + ih - gh, iw, gh, blend(rgb(90, 190, 120), b.bot, 40));
    int peak = ih / 3;
    for (int i = 0; i < peak; i++) {
        vga_fill_rect(ix + iw / 2 - i / 2, iy + ih - gh - i, i + 1, 2, blend(rgb(120, 150, 190), b.bot, 30));
    }
}

static void gui_glyph_memory(int x, int y, int size, icon_badge_t b) {
    uint64_t g = rgb(255, 255, 255);
    uint64_t pin = blend(g, b.bot, 130);
    int pad = size / 5;
    int cw = size - 2 * pad, ch = size * 3 / 5;
    int cx = x + pad, cy = y + size / 2 - ch / 2;
    int r = size / 16; if (r < 2) r = 2;
    vga_fill_rounded_rect(cx, cy, cw, ch, r, g);
    int pins = 4;
    int pw = cw / (pins * 2); if (pw < 1) pw = 1;
    int ph = size / 12; if (ph < 1) ph = 1;
    for (int i = 0; i < pins; i++) {
        vga_fill_rect(cx + (i * 2 + 1) * pw - pw / 2, cy + ch, pw, ph, pin);
    }
}

static void gui_glyph_download(int x, int y, int size, icon_badge_t b) {
    (void)b;
    uint64_t g = rgb(255, 255, 255);
    int cx = x + size / 2;
    int top = y + size / 5, mid = y + size * 3 / 5;
    int sw = size / 10; if (sw < 2) sw = 2;
    vga_fill_rect(cx - sw / 2, top, sw, mid - top, g);
    int aw = size / 4;
    for (int i = 0; i < aw / 2; i++) {
        vga_fill_rect(cx - i, mid + i, i * 2, 2, g);
    }
    int trayw = size * 3 / 5;
    int trayh = size / 12; if (trayh < 2) trayh = 2;
    vga_fill_rect(x + size / 2 - trayw / 2, y + size * 4 / 5, trayw, trayh, g);
}

static void gui_glyph_generic(int x, int y, int size, icon_badge_t b) {
    (void)b;
    uint64_t g = rgb(255, 255, 255);
    int cx = x + size / 2, cy = y + size / 2;
    int r = size / 8; if (r < 2) r = 2;
    vga_fill_circle(cx - size / 5, cy, r, g);
    vga_fill_circle(cx, cy, r, g);
    vga_fill_circle(cx + size / 5, cy, r, g);
}

/* Single entry point: draws the badge for `kind` at (x,y,size) and
 * the matching glyph on top. Shared by desktop icons, taskbar
 * buttons, the dock, and (indirectly, via gui_draw_ctx_icon_art
 * below) context-menu icons. */
void gui_draw_app_icon(int kind, int x, int y, int size, bool hov) {
    icon_badge_t badge = gui_icon_badge_for_kind(kind, hov);
    gui_draw_icon_badge(x, y, size, badge);
    switch (kind) {
        case WIN_FILE_MGR:        gui_glyph_folder(x, y, size, badge);   break;
        case WIN_TEXT_EDITOR:     gui_glyph_document(x, y, size, badge); break;
        case WIN_TERMINAL:        gui_glyph_terminal(x, y, size, badge); break;
        case WIN_SETTINGS:        gui_glyph_gear(x, y, size, badge);     break;
        case WIN_ABOUT:           gui_glyph_about(x, y, size, badge);    break;
        case WIN_CALC:            gui_glyph_calc(x, y, size, badge);     break;
        case WIN_STORAGE:         gui_glyph_drive(x, y, size, badge);    break;
        case WIN_BROWSER:         gui_glyph_browser(x, y, size, badge);  break;
        case WIN_TASK_MGR:        gui_glyph_taskmgr(x, y, size, badge);  break;
        case WIN_PAINT:           gui_glyph_paint(x, y, size, badge);    break;
        case WIN_MUSIC:           gui_glyph_music(x, y, size, badge);    break;
        case WIN_CLOCK:           gui_glyph_clock(x, y, size, badge);    break;
        case WIN_SYSINFO:         gui_glyph_sysinfo(x, y, size, badge);  break;
        case WIN_PYTHON_IDE:      gui_glyph_python(x, y, size, badge);   break;
        case WIN_CALC_GRAPH:      gui_glyph_graph(x, y, size, badge);    break;
        case WIN_SHEET:           gui_glyph_sheet(x, y, size, badge);    break;
        case WIN_VOXEL_GAME:      gui_glyph_game(x, y, size, badge);     break;
        case WIN_JPEG:            gui_glyph_image(x, y, size, badge);    break;
        case WIN_MEMORY_MGR:      gui_glyph_memory(x, y, size, badge);   break;
        case WIN_HTTP_DOWNLOADER: gui_glyph_download(x, y, size, badge); break;
        default:                  gui_glyph_generic(x, y, size, badge);  break;
    }
}

/* Dynamic desktop file/folder icons reuse gui_draw_app_icon() for
 * every case except "a file that isn't recognized as text", which
 * gets a neutral badge (kind -1 falls through to the default color)
 * plus the plain-file glyph rather than any specific app's color. */
static void gui_draw_desktop_icon_art(const desktop_icon_t* ic, int x, int y, int size, bool hov) {
    if (!ic) return;
    if (ic->is_dynamic) {
        if (ic->is_file) {
            if (ic->win_kind == WIN_TEXT_EDITOR) {
                if (gui_desktop_path_looks_text(ic->path)) {
                    gui_draw_app_icon(WIN_TEXT_EDITOR, x, y, size, hov);
                } else {
                    icon_badge_t badge = gui_icon_badge_for_kind(-1, hov);
                    gui_draw_icon_badge(x, y, size, badge);
                    gui_glyph_file(x, y, size, badge);
                }
                return;
            }
            if (ic->win_kind == WIN_JPEG) {
                gui_draw_app_icon(WIN_JPEG, x, y, size, hov);
                return;
            }
        } else if (ic->win_kind == WIN_FILE_MGR) {
            gui_draw_app_icon(WIN_FILE_MGR, x, y, size, hov);
            return;
        }
    }
    gui_draw_app_icon(ic->win_kind, x, y, size, hov);
}

/* Small icons used in right-click context menus. Real app entries
 * (New File, New Folder, Terminal, ...) reuse gui_draw_app_icon() at
 * menu-row scale so they match the app's real icon; window/system
 * actions (minimize, maximize, close, power) aren't apps and keep
 * small dedicated glyphs on a plain neutral chip. (The previous
 * version called the full-size desktop art functions - designed for
 * a ~40-48px canvas - directly into a 12px menu slot, badly
 * overflowing into neighboring menu rows.) */
void gui_draw_ctx_icon_art(int icon_id, int x, int y, bool hov) {
    switch (icon_id) {
        case 1:  gui_draw_app_icon(WIN_TEXT_EDITOR, x, y, 16, hov); return;
        case 2:  gui_draw_app_icon(WIN_FILE_MGR,    x, y, 16, hov); return;
        case 4:  gui_draw_app_icon(WIN_FILE_MGR,    x, y, 16, hov); return;
        case 5:  gui_draw_app_icon(WIN_SHEET,       x, y, 16, hov); return;
        case 6:  gui_draw_app_icon(WIN_VOXEL_GAME,  x, y, 16, hov); return;
        case 7:  gui_draw_app_icon(WIN_ABOUT,       x, y, 16, hov); return;
        case 11: gui_draw_app_icon(WIN_SETTINGS,    x, y, 16, hov); return;
        case 12: gui_draw_app_icon(WIN_MUSIC,       x, y, 16, hov); return;
        case 13: gui_draw_app_icon(WIN_PYTHON_IDE,  x, y, 16, hov); return;
        case 15: gui_draw_app_icon(WIN_BROWSER,     x, y, 16, hov); return;
        default: break;
    }

    uint64_t bg = hov ? rgb(240, 246, 255) : rgb(230, 238, 248);
    uint64_t edge = hov ? rgb(150, 176, 220) : rgb(176, 190, 212);
    uint64_t accent = hov ? rgb(72, 144, 232) : rgb(88, 130, 206);
    uint64_t light = rgb(248, 250, 255);
    vga_fill_rounded_rect(x + 1, y + 1, 14, 14, 4, bg);
    vga_draw_rounded_rect(x + 1, y + 1, 14, 14, 4, edge);
    switch (icon_id) {
        case 3: /* Terminal */
            vga_fill_rect(x + 4, y + 5, 8, 6, rgb(28, 30, 40));
            vga_fill_rect(x + 5, y + 6, 2, 1, rgb(110, 224, 148));
            vga_fill_rect(x + 8, y + 6, 2, 1, light);
            vga_fill_rect(x + 5, y + 9, 5, 1, rgb(170, 182, 200));
            break;
        case 8: /* Minimize */
            vga_fill_rect(x + 4, y + 10, 8, 2, accent);
            break;
        case 9: /* Maximize */
            vga_draw_rect(x + 4, y + 4, 7, 7, accent);
            vga_fill_rect(x + 4, y + 4, 7, 2, accent);
            break;
        case 10: /* Close */
            vga_draw_line(x + 4, y + 4, x + 11, y + 11, rgb(224, 84, 84));
            vga_draw_line(x + 11, y + 4, x + 4, y + 11, rgb(224, 84, 84));
            break;
        case 14: /* Power / Restart */
            vga_draw_circle(x + 8, y + 8, 5, rgb(224, 84, 84));
            vga_fill_rect(x + 7, y + 3, 2, 5, rgb(224, 84, 84));
            break;
        default:
            vga_fill_rect(x + 5, y + 5, 6, 6, accent);
            break;
    }
}



static void gui_desktop_grid_metrics(int* origin_x, int* origin_y, int* cell_w, int* cell_h, int* cols, int* rows) {
    int box = gui_get_desktop_icon_render_size();
    int ox = 16;
    int oy = 20;
    int cw = box + 28;
    int ch = box + 36;
    int min_cw = box + 16;
    int min_ch = box + 28;
    int usable_w = (int)SCREEN_W - ox - 16;
    int usable_h = (int)SCREEN_H - TASKBAR_H - oy - 16;

    if (usable_w < 1) usable_w = 1;
    if (usable_h < 1) usable_h = 1;
    if (cw < min_cw) cw = min_cw;
    if (ch < min_ch) ch = min_ch;

    int c = usable_w / cw;
    int r = usable_h / ch;
    if (c < 1) c = 1;
    if (r < 1) r = 1;

    while (c * r < MAX_ICONS && (cw > min_cw || ch > min_ch)) {
        if (cw > min_cw) cw -= 4;
        if (ch > min_ch) ch -= 4;
        c = usable_w / cw;
        r = usable_h / ch;
        if (c < 1) c = 1;
        if (r < 1) r = 1;
    }

    if (c * r < MAX_ICONS) {
        c = usable_w / min_cw;
        if (c < 1) c = 1;
        r = (MAX_ICONS + c - 1) / c;
        while (r * min_ch > usable_h && c < MAX_ICONS) {
            c++;
            r = (MAX_ICONS + c - 1) / c;
        }
        cw = min_cw;
        ch = min_ch;
    }

    if (origin_x) *origin_x = ox;
    if (origin_y) *origin_y = oy;
    if (cell_w) *cell_w = cw;
    if (cell_h) *cell_h = ch;
    if (cols) *cols = c;
    if (rows) *rows = r;
}

static void draw_desktop_grid(void) {
    int origin_x = 0;
    int origin_y = 0;
    int cell_w = 0;
    int cell_h = 0;
    int cols = 0;
    int rows = 0;
    gui_desktop_grid_metrics(&origin_x, &origin_y, &cell_w, &cell_h, &cols, &rows);

    uint64_t line = rgb(72, 92, 122);
    uint64_t line_soft = rgb(48, 62, 86);

    for (int c = 0; c <= cols; ++c) {
        int x = origin_x + c * cell_w;
        if (x >= 0 && x < (int)SCREEN_W) {
            vga_fill_rect(x, origin_y, 1, rows * cell_h, (c % 2 == 0) ? line : line_soft);
        }
    }
    for (int r = 0; r <= rows; ++r) {
        int y = origin_y + r * cell_h;
        if (y >= 0 && y < (int)SCREEN_H - TASKBAR_H) {
            vga_fill_rect(origin_x, y, cols * cell_w, 1, (r % 2 == 0) ? line : line_soft);
        }
    }
}

void draw_desktop_icons(void) {
    int origin_x = 0, origin_y = 0, cell_w = 0, cell_h = 0, cols = 0, rows = 0;
    gui_desktop_grid_metrics(&origin_x, &origin_y, &cell_w, &cell_h, &cols, &rows);

    for (int i = 0; i < desktop_icon_count; i++) {
        desktop_icon_t* ic = &desktop_icons[i];
        int x = ic->x, y = ic->y;
        int box = gui_get_desktop_icon_render_size();
        int label_y = y + box + 4;
        int icon_h = box + 22;
        bool hov = (mouse.x >= x && mouse.x < x + box && mouse.y >= y && mouse.y < y + icon_h);

        gui_draw_desktop_icon_art(ic, x, y, box, hov);

        char label_buf[96];
        int label_max_w = cell_w - 10;
        if (label_max_w < box) label_max_w = box;
        gui_fit_text_to_width(ic->label, label_buf, sizeof(label_buf), label_max_w);
        int lw = (int)strlen(label_buf) * FONT_W;
        int label_x = x + (box / 2) - (lw / 2);
        int label_min_x = x - (cell_w - box) / 2;
        int label_max_x = x + cell_w - lw;
        if (label_x < label_min_x) label_x = label_min_x;
        if (label_x > label_max_x) label_x = label_max_x;
        if (label_x < x) label_x = x;
        vga_draw_string(label_x, label_y, label_buf, C_TEXT_LIGHT, 0xFFFFFFFF);
    }
}

