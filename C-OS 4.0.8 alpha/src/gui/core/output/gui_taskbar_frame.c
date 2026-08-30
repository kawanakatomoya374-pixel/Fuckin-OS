/**
 * gui_taskbar_frame.c - GUIコア (タスクバー描画)
 * gui.c (5,588行) から分割生成。詳細は gui_internal.h を参照。
 */

#include "gui.h"
#include "gui_internal.h"
#include "voxel_games_advanced.h"
#include "vga.h"
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
#include "../apps/common/gui_apps_common.h"
#include "acpi_power.h"
#include <shell.h>
#include <string.h>
#include <stdio.h>

void draw_taskbar(void) {

    int ty = (int)SCREEN_H - TASKBAR_H;
    // Calm blue taskbar
    vga_fill_rect(0, ty, (int)SCREEN_W, TASKBAR_H, C_TASKBAR);
    vga_fill_rect(0, ty, (int)SCREEN_W, 1, rgb(50, 65, 95));

        // Start button in a diamond shape
    bool sh = (mouse.x >= 0 && mouse.x < 92 && mouse.y >= ty);
    int sx = 8, sy = ty + 4;
    uint64_t start_bg = sh ? rgb(65, 95, 155) : rgb(38, 52, 80);
    uint64_t button_border = rgb(86, 118, 180);
    int sw = 86, shh = 32;
    int cx = sx + sw / 2;
    int cy = sy + shh / 2;
    for (int dy = 0; dy < shh; dy++) {
        int mid = dy <= shh / 2 ? dy : (shh - 1 - dy);
        int half = 8 + (mid * (sw - 16)) / (shh / 2 + 1);
        int x0 = cx - half / 2;
        int ww = half;
        if (x0 < sx) { ww -= (sx - x0); x0 = sx; }
        if (x0 + ww > sx + sw) ww = sx + sw - x0;
        if (ww > 0) vga_fill_rect(x0, sy + dy, ww, 1, start_bg);
    }
    vga_draw_line(sx + 1, cy, cx, sy + 1, button_border);
    vga_draw_line(cx, sy + 1, sx + sw - 2, cy, button_border);
    vga_draw_line(sx + 1, cy, cx, sy + shh - 2, button_border);
    vga_draw_line(cx, sy + shh - 2, sx + sw - 2, cy, button_border);
    vga_draw_string(sx + 22, sy + 11, gui_text("Start", "スタート"), C_TEXT_LIGHT, 0xFFFFFFFF);
    
    /* Open windows: icon-only buttons (Windows-taskbar style) - just
     * each app's badge, a hover/active highlight behind it, and a
     * small underline for the focused app, instead of a text-label
     * pill. Slot geometry (start at x=110, 46px each) is mirrored in
     * gui_input.c's taskbar click handling - keep both in sync. */
    const int win_btn_x0 = 110;
    const int win_btn_slot = 46;
    const int win_btn_icon = 32;
    int bx = win_btn_x0;
    for (int i = 0; i < window_count; i++) {
        window_t* w = &windows[i];
        if (!w->visible) continue;
        bool active = (i == active_window) && !w->minimized;
        bool hov = (mouse.x >= bx && mouse.x < bx + win_btn_slot && mouse.y >= ty);

        if (hov || active) {
            uint64_t hl = active ? C_TASKBAR_BHOV : rgb(50, 66, 96);
            vga_fill_rounded_rect(bx + 2, ty + 4, win_btn_slot - 4, TASKBAR_H - 10, 8, hl);
        }
        int icon_x = bx + (win_btn_slot - win_btn_icon) / 2;
        int icon_y = ty + (TASKBAR_H - win_btn_icon) / 2 - 2;
        gui_draw_app_icon(w->kind, icon_x, icon_y, win_btn_icon, hov);
        if (active) {
            vga_fill_rect(bx + win_btn_slot / 2 - 8, ty + TASKBAR_H - 4, 16, 3, C_ACCENT2);
        } else if (!w->minimized) {
            vga_fill_rect(bx + win_btn_slot / 2 - 5, ty + TASKBAR_H - 4, 10, 2, rgb(140, 160, 200));
        }
        bx += win_btn_slot;
    }

    char clock_str[16];
    uint64_t secs = get_timer_ticks() / 100;
    uint64_t mins = secs / 60;
    uint64_t hrs  = mins / 60;
    mins %= 60; secs %= 60;
    clock_str[0] = '0' + (hrs / 10) % 10; clock_str[1] = '0' + hrs % 10;
    clock_str[2] = ':';
    clock_str[3] = '0' + (mins / 10) % 10; clock_str[4] = '0' + mins % 10;
    clock_str[5] = ':';
    clock_str[6] = '0' + (secs / 10) % 10; clock_str[7] = '0' + secs % 10;
    clock_str[8] = 0;
    int clock_x = (int)SCREEN_W - 180;
    if (clock_x < 4) clock_x = 4;
    vga_draw_string(clock_x, ty + 12, clock_str, C_TEXT_LIGHT, 0xFFFFFFFF);


    /* ---- Notification indicator ---- */
    extern int notification_get_unread_count(void);
    int notif_count = notification_get_unread_count();
    if (notif_count > 0) {
        int nx = clock_x - 30;
        vga_fill_circle(nx + 8, ty + 18, 10, rgb(220, 50, 40));
        char nbuf[4];
        if (notif_count > 9) {
            nbuf[0] = '9'; nbuf[1] = '+'; nbuf[2] = 0;
        } else {
            nbuf[0] = '0' + notif_count; nbuf[1] = 0;
        }
        vga_draw_string(nx + 4, ty + 11, nbuf, 0x00FFFFFF, 0xFFFFFFFF);
    }

    /* ---- USB indicator ---- */
    bool usb_ready = gui_usb_recognized();
    uint64_t usb_color = usb_ready ? rgb(70, 150, 255) : rgb(255, 255, 255);
    int usb_icon_x = (int)SCREEN_W - 260;
    int usb_icon_y = ty + 10;
    if (usb_icon_x < 0) usb_icon_x = 0;
    gui_draw_usb_taskbar_icon(usb_icon_x, usb_icon_y, usb_color);
    vga_draw_string(usb_icon_x + 22, ty + 11, "USB", usb_color, 0xFFFFFFFF);

    /* ---- Battery gauge ---- */
    uint64_t battery_pct = acpi_power_get_battery_percent();
    bool charging = acpi_power_is_charging();
    uint64_t batt_color = (battery_pct >= 80) ? rgb(80, 200, 120)
                       : (battery_pct >= 50) ? rgb(35, 155, 75)
                       : (battery_pct >= 30) ? rgb(240, 160, 60)
                                             : rgb(235, 90, 90);
    int batt_bar_w = 42;
    int batt_bar_h = 10;
    int batt_bar_x = (int)SCREEN_W - 74;
    int batt_bar_y = ty + 14;
    int batt_fill = (int)((battery_pct * (uint64_t)batt_bar_w) / 100);
    const char* batt_src = acpi_power_get_source_label();
    if (!batt_src || batt_src[0] == 0) batt_src = "BAT";

    int batt_icon_x = (int)SCREEN_W - 18;
    int batt_icon_y = batt_bar_y - 1;
    vga_fill_rect(batt_icon_x, batt_icon_y, 14, 12, rgb(45, 55, 75));
    vga_draw_rect(batt_icon_x, batt_icon_y, 14, 12, rgb(120, 140, 170));
    vga_fill_rect(batt_icon_x + 14, batt_icon_y + 3, 2, 6, rgb(120, 140, 170));
    int icon_fill = (int)((battery_pct * 10u) / 100u);
    if (icon_fill > 10) icon_fill = 10;
    if (icon_fill > 0) vga_fill_rect(batt_icon_x + 2, batt_icon_y + 2, icon_fill, 8, batt_color);
    if (charging) {
        vga_fill_rect(batt_icon_x + 6, batt_icon_y + 2, 2, 8, rgb(255, 255, 255));
        vga_fill_rect(batt_icon_x + 4, batt_icon_y + 5, 6, 2, rgb(255, 255, 255));
    }

    /* batt source label removed to avoid overlap */
    vga_fill_rect(batt_bar_x, batt_bar_y, batt_bar_w, batt_bar_h, rgb(45, 55, 75));
    if (batt_fill > 0) vga_fill_rect(batt_bar_x, batt_bar_y, batt_fill, batt_bar_h, batt_color);
    vga_draw_rect(batt_bar_x, batt_bar_y, batt_bar_w, batt_bar_h, rgb(120, 140, 170));
    char batt_pct_str[8];
    if (battery_pct >= 100) {
        batt_pct_str[0] = '1'; batt_pct_str[1] = '0'; batt_pct_str[2] = '0';
        batt_pct_str[3] = '%'; batt_pct_str[4] = 0;
    } else if (battery_pct >= 10) {
        batt_pct_str[0] = ' ';
        batt_pct_str[1] = '0' + (int)(battery_pct / 10) % 10;
        batt_pct_str[2] = '0' + (int)(battery_pct % 10);
        batt_pct_str[3] = '%'; batt_pct_str[4] = 0;
    } else {
        batt_pct_str[0] = ' '; batt_pct_str[1] = ' '; batt_pct_str[2] = '0' + (int)(battery_pct % 10);
        batt_pct_str[3] = '%'; batt_pct_str[4] = 0;
    }
    vga_draw_string(batt_bar_x + batt_bar_w + 4, ty + 10, batt_pct_str, C_TEXT_LIGHT, 0xFFFFFFFF);

    /* Hover tooltip: show source + remaining time without crowding the taskbar */
    uint64_t batt_minutes = acpi_power_get_estimated_minutes_remaining();
    char batt_time[16];
    if (charging) {
        snprintf(batt_time, sizeof(batt_time), "AC");
    } else if (batt_minutes == 0) {
        snprintf(batt_time, sizeof(batt_time), "--");
    } else if (batt_minutes >= 60) {
        snprintf(batt_time, sizeof(batt_time), "%lluh%02llum",
                 (unsigned long long)(batt_minutes / 60ULL),
                 (unsigned long long)(batt_minutes % 60ULL));
    } else {
        snprintf(batt_time, sizeof(batt_time), "%llum", (unsigned long long)batt_minutes);
    }
    char tip[32];
    snprintf(tip, sizeof(tip), "%s %llu%% %s", batt_src,
             (unsigned long long)battery_pct, batt_time);
    int tooltip_w = (int)strlen(tip) * 8 + 12;
    if (tooltip_w < 72) tooltip_w = 72;
    int tooltip_h = 18;
    int tooltip_x = batt_icon_x;
    if (tooltip_x + tooltip_w > (int)SCREEN_W - 4) tooltip_x = (int)SCREEN_W - 4 - tooltip_w;
    if (tooltip_x < 4) tooltip_x = 4;
    int tooltip_y = batt_icon_y - tooltip_h - 2;
    if (tooltip_y < 0) tooltip_y = batt_icon_y + 14;
    bool hover_batt = (mouse.x >= batt_icon_x && mouse.x <= batt_bar_x + batt_bar_w + 16 &&
                       mouse.y >= batt_icon_y - 2 && mouse.y <= batt_icon_y + 12);
    if (hover_batt) {
        vga_fill_rect(tooltip_x, tooltip_y, tooltip_w, tooltip_h, rgb(18, 24, 34));
        vga_draw_rect(tooltip_x, tooltip_y, tooltip_w, tooltip_h, rgb(130, 150, 180));
        vga_draw_string(tooltip_x + 6, tooltip_y + 5, tip, C_TEXT_LIGHT, 0xFFFFFFFF);
    }

    /* ---- Live memory usage bar ---- */
    uint64_t mem_used = kmemory_used();
    uint64_t mem_free = kmemory_free();
    uint64_t mem_total = mem_used + mem_free;
    int mem_bar_w = 112;
    int mem_bar_h = 10;
    int mem_bar_x = (int)SCREEN_W - 390;
    int mem_bar_y = ty + 14;

    if (mem_total > 0) {
        int used_w = (int)((mem_used * (uint64_t)mem_bar_w) / mem_total);

        vga_draw_string(mem_bar_x - 54, ty + 10, gui_text("MEM", "メモリ"), C_TEXT_LIGHT, 0xFFFFFFFF);

        vga_fill_rect(mem_bar_x, mem_bar_y, mem_bar_w, mem_bar_h, rgb(45, 55, 75));
        vga_fill_rect(mem_bar_x, mem_bar_y, used_w, mem_bar_h, rgb(80, 200, 120));
        vga_draw_rect(mem_bar_x, mem_bar_y, mem_bar_w, mem_bar_h, rgb(120, 140, 170));
    }

    /* ---- Dock: pinned quick-launch icons ---- */
    int dock_x = (int)SCREEN_W - 320;
    if (dock_x < 56) dock_x = 56;
    /* C-OS 4.0.8 alpha: NetSurf quick-launch button enabled in dock.
     * WIN_BROWSER opens the NetSurf window (gui_apps_browser.c) which
     * now calls cos_netsurf_load_url_sync_nowait() for real. */
    struct { int kind; } dock_items[] = {
        { WIN_BROWSER },
        { WIN_CALC },
        { 0 }
    };
    for (int d = 0; dock_items[d].kind; d++) {
        int dx = dock_x + d * 36;
        int dy = ty + 4;
        bool dhov = (mouse.x >= dx && mouse.x < dx + 30 && mouse.y >= dy && mouse.y < dy + 32);
        if (dhov) {
            vga_fill_rounded_rect(dx - 2, dy - 2, 34, 36, 8, rgb(52, 68, 98));
        }
        gui_draw_app_icon(dock_items[d].kind, dx, dy, 30, dhov);
    }
}

void draw_window_frame(int idx) {
    window_t* w = &windows[idx];
    if (!w->visible || w->minimized) return;
    bool is_active = (idx == active_window);
    
    // Soft shadow
    for (int i = 4; i > 0; i--) {
        vga_fill_rect(w->x + i * 2, w->y + i * 2, w->w, w->h, rgb(25 + i * 8, 35 + i * 8, 55 + i * 8));
    }
    
    // Window body with subtle gradient
    uint64_t win_top = rgb(250, 252, 255);
    uint64_t win_bottom = rgb(240, 245, 250);
    for (int gy = 0; gy < w->h; gy++) {
        uint64_t grad = (gy < TITLEBAR_H + 20) ? win_top : win_bottom;
        vga_fill_rect(w->x, w->y + gy, w->w, 1, grad);
    }
    
    vga_fill_rounded_rect(w->x, w->y, w->w, w->h, 8, 0);
    vga_draw_rounded_rect(w->x, w->y, w->w, w->h, 8, C_BORDER);
    
    // Title bar - calm blue
    uint64_t tb_acrylic = is_active ? C_TITLEBAR : rgb(130, 140, 160);
    uint64_t tb_acrylic_light = is_active ? C_TITLEBAR_HOV : rgb(150, 160, 180);
    
    for (int i = 0; i < TITLEBAR_H; i++) {
        uint64_t grad_color = (i < TITLEBAR_H / 2) ? tb_acrylic_light : tb_acrylic;
        vga_fill_rect(w->x + 2, w->y + i, w->w - 4, 1, grad_color);
    }
    
    // Title text
    int tw = (int)strlen(w->title) * FONT_W;
    vga_draw_string(w->x + w->w / 2 - tw / 2, w->y + 8, w->title, C_TEXT_LIGHT, 0xFFFFFFFF);
    
    // Window controls
    int btn_y = w->y + 6;
    int close_x = w->x + w->w - 32;
    int max_x = w->x + w->w - 62;
    int min_x = w->x + w->w - 92;
    
    uint64_t glass_bg = is_active ? rgb(255, 255, 255) : rgb(220, 225, 235);
    uint64_t glass_border = is_active ? rgb(160, 180, 210) : rgb(150, 155, 170);
    
    // Minimize button
    bool hov_min = (mouse.x >= min_x && mouse.x < min_x + 24 && mouse.y >= btn_y && mouse.y < btn_y + 24);
    uint64_t min_color = hov_min ? rgb(100, 160, 230) : glass_bg;
    vga_fill_rounded_rect(min_x, btn_y, 24, 24, 6, min_color);
    vga_draw_rounded_rect(min_x, btn_y, 24, 24, 6, glass_border);
    vga_fill_rect(min_x + 6, btn_y + 11, 12, 2, hov_min ? rgb(255, 255, 255) : rgb(80, 90, 120));

    // Maximize / restore button
    bool hov_max = (mouse.x >= max_x && mouse.x < max_x + 24 && mouse.y >= btn_y && mouse.y < btn_y + 24);
    uint64_t max_color = hov_max ? rgb(100, 160, 230) : glass_bg;
    uint64_t max_glyph = hov_max ? rgb(255, 255, 255) : rgb(80, 90, 120);
    vga_fill_rounded_rect(max_x, btn_y, 24, 24, 6, max_color);
    vga_draw_rounded_rect(max_x, btn_y, 24, 24, 6, glass_border);
    if (w->maximized) {
        // Restore glyph: two overlapping squares
        vga_draw_rect(max_x + 9, btn_y + 6, 8, 8, max_glyph);
        vga_fill_rect(max_x + 6, btn_y + 9, 9, 9, max_color);
        vga_draw_rect(max_x + 6, btn_y + 9, 8, 8, max_glyph);
    } else {
        // Maximize glyph: single square outline
        vga_draw_rect(max_x + 6, btn_y + 6, 12, 12, max_glyph);
    }
    
    // Close button
    bool hov_close = (mouse.x >= close_x && mouse.x < close_x + 24 && mouse.y >= btn_y && mouse.y < btn_y + 24);
    uint64_t close_color = hov_close ? rgb(220, 80, 80) : glass_bg;
    vga_fill_rounded_rect(close_x, btn_y, 24, 24, 6, close_color);
    vga_draw_rounded_rect(close_x, btn_y, 24, 24, 6, hov_close ? rgb(180, 60, 60) : glass_border);
    uint64_t x_color = hov_close ? rgb(255, 255, 255) : rgb(80, 90, 120);
    vga_draw_line(close_x + 6, btn_y + 6, close_x + 18, btn_y + 18, x_color);
    vga_draw_line(close_x + 18, btn_y + 6, close_x + 6, btn_y + 18, x_color);

    // Resize grip hint (bottom-right corner) - three short diagonal
    // strokes, the conventional "drag here to resize" affordance.
    // There's no cursor-shape system in this codebase to swap the
    // pointer for a resize arrow, so this is the visual cue instead.
    if (!w->maximized) {
        uint64_t grip_color = is_active ? rgb(140, 155, 180) : rgb(170, 175, 185);
        int gx = w->x + w->w - 4;
        int gy = w->y + w->h - 4;
        for (int k = 1; k <= 3; k++) {
            vga_draw_line(gx - k * 4, gy, gx, gy - k * 4, grip_color);
        }
    }
    
    // Handle minimize button click - through the real function so
    // this picks up its extra bookkeeping (voxel-game state save,
    // window-state snapshot) instead of only setting the flag.
    if (hov_min && mouse.left_click) {
        gui_minimize_window(idx);
    }
    // Handle maximize/restore button click
    if (hov_max && mouse.left_click) {
        if (w->maximized) gui_restore_window(idx);
        else gui_maximize_window(idx);
    }
}





//  text_buf[33..48] = operand A string (double as text)
//  text_buf[49]     = waiting_for_rhs flag ('1' or '0')

static void calc_init_state(window_t* w) {
    if (w->text_buf[0] == 0) {
        strcpy(w->text_buf, "0");           // display
        w->text_buf[32] = 0;               // no operator
        w->text_buf[33] = 0;               // operand A
        w->text_buf[49] = '0';             // not waiting
    }
}

static void calc_press(window_t* w, char key) {
    char* disp = w->text_buf;         // [0..31]
    char* op   = &w->text_buf[32];    // [32]
    char* opA  = &w->text_buf[33];    // [33..48]
    char* wait = &w->text_buf[49];    // [49]

    if (key >= '0' && key <= '9') {
        if (*wait == '1' || strcmp(disp, "0") == 0) {
            disp[0] = key; disp[1] = 0; *wait = '0';
        } else {
            int l = (int)strlen(disp);
            if (l < 14) { disp[l] = key; disp[l+1] = 0; }
        }
    } else if (key == '.') {
        if (*wait == '1') { strcpy(disp, "0."); *wait = '0'; return; }
        bool has = FALSE;
        for (int i = 0; disp[i]; i++) if (disp[i] == '.') { has = TRUE; break; }
        if (!has) { int l = (int)strlen(disp); disp[l] = '.'; disp[l+1] = 0; }
    } else if (key == 'C') {
        strcpy(disp, "0"); *op = 0; *opA = 0; *wait = '0';
    } else if (key == 'B') { // backspace
        int l = (int)strlen(disp);
        if (l > 1) disp[l-1] = 0; else strcpy(disp, "0");
    } else if (key == '+' || key == '-' || key == '*' || key == '/') {
        strcpy(opA, disp); *op = key; *wait = '1';
    } else if (key == '=') {
        if (*op && *opA) {
            // Simple integer arithmetic (avoid floats in kernel)
            int a = atoi(opA), b = atoi(disp), result = 0;
            if (*op == '+') result = a + b;
            else if (*op == '-') result = a - b;
            else if (*op == '*') result = a * b;
            else if (*op == '/') result = (b != 0) ? a / b : 0;
            cos_itoa(result, disp, 10);
            *op = 0; *opA = 0; *wait = '1';
        }
    } else if (key == 'N') { // negate
        if (disp[0] == '-') {
            int l = (int)strlen(disp);
            for (int i = 0; i < l; i++) disp[i] = disp[i+1];
        } else {
            int l = (int)strlen(disp);
            for (int i = l; i >= 0; i--) disp[i+1] = disp[i];
            disp[0] = '-';
        }
    }
}


// ============================================================
// Context Menu - Enhanced with icons, separators, animations
// ============================================================

/* Submenu kinds */
#define SUBMENU_KIND_NONE      0
#define SUBMENU_KIND_LANGUAGE  1
#define SUBMENU_KIND_OPEN      2
#define SUBMENU_KIND_SETTINGS  3
#define SUBMENU_KIND_POWER     4
#define SUBMENU_KIND_WINDOW    5
#define SUBMENU_KIND_NEW       6
#define SUBMENU_KIND_AUDIO      7
#define SUBMENU_KIND_WALLPAPER  8
#define SUBMENU_KIND_DESKTOP_ICON_SIZE  9
#define SUBMENU_KIND_APP_ICON         10

