/**
 * gui_apps_birthday.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * バースデー記念アプリ
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
 * BIRTHDAY CELEBRATION APPLICATION
 * ============================================================ */
void draw_birthday_app(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x, cy = w->y, cw = w->w, ch = w->h;
    
    draw_window_frame(idx);
    
    int content_y = cy + TITLEBAR_H + 10;
    int content_h = ch - TITLEBAR_H - 20 - C_STATUSBAR_H;
    
     /* Animated birthday celebration */
    if (w->birthday_animation) {
        uint64_t time = gui_frame_counter - w->birthday_start_time;
        
         /* Confetti animation */
        for (int i = 0; i < 20; i++) {
            int fx = cx + (time * 3 + i * 47) % (cw - 20) + 10;
            int fy = cy + ((time * 2 + i * 31) % (content_h - 20)) + 10;
            uint64_t colors[] = {rgb(255, 0, 0), rgb(0, 255, 0), rgb(0, 0, 255), 
                               rgb(255, 255, 0), rgb(255, 0, 255), rgb(0, 255, 255)};
            uint64_t color = colors[i % 6];
            vga_fill_rect(fx, fy, 8, 8, color);
        }
        
         /* Birthday cake */
        int cake_x = cx + cw / 2 - 30;
        int cake_y = content_y + content_h / 2 - 40;
        
         /* Cake base */
        vga_fill_rect(cake_x, cake_y + 30, 60, 40, rgb(139, 69, 19));
        vga_draw_rect(cake_x, cake_y + 30, 60, 40, rgb(101, 67, 33));
        
         /* Cake layers */
        vga_fill_rect(cake_x + 5, cake_y + 20, 50, 10, rgb(255, 182, 193));
        vga_fill_rect(cake_x + 10, cake_y + 10, 40, 10, rgb(255, 218, 185));
        vga_fill_rect(cake_x + 15, cake_y, 30, 10, rgb(255, 255, 255));
        
         /* Candles */
        for (int i = 0; i < 5; i++) {
            int cxandle = cake_x + 20 + i * 8;
            vga_fill_rect(cxandle, cake_y - 10, 2, 10, rgb(255, 255, 0));
             /* Flame animation */
            if (time % 20 < 10) {
                vga_fill_rect(cxandle - 1, cake_y - 12, 4, 2, rgb(255, 100, 0));
            }
        }
        
         /* Birthday message */
        const char* message = "Happy Birthday!";
        int msg_x = cx + (cw - slen(message) * FONT_W) / 2;
        int msg_y = cake_y + 90;
        vga_draw_string(msg_x, msg_y, message, rgb(255, 0, 0), 0xFFFFFFFF);
        
         /* Sparkles around */
        for (int i = 0; i < 12; i++) {
            int sx = cx + 20 + (i * 53) % (cw - 40);
            int sy = content_y + 20 + (i * 37) % (content_h - 40);
            if ((time + i * 7) % 30 < 15) {
                vga_fill_rect(sx, sy, 4, 4, rgb(255, 215, 0));
                vga_fill_rect(sx + 1, sy + 1, 2, 2, rgb(255, 255, 255));
            }
        }
    } else {
         /* Static birthday card */
        vga_draw_string(cx + cw/2 - 60, content_y + 20, "🎉 Birthday Celebration 🎉", C_ACCENT, 0xFFFFFFFF);
        vga_draw_string(cx + cw/2 - 80, content_y + 60, "Click 'Start Celebration' to begin!", C_TEXT_GRAY, 0xFFFFFFFF);
        
         /* Start button */
        int btn_x = cx + cw/2 - 60;
        int btn_y = content_y + 100;
        int btn_w = 120;
        int btn_h = 30;
        
        bool hover = (_get_mouse()->x >= btn_x && _get_mouse()->x < btn_x + btn_w &&
                     _get_mouse()->y >= btn_y && _get_mouse()->y < btn_y + btn_h);
        
        uint64_t btn_color = hover ? C_SELECTED : C_ACCENT;
        vga_fill_rect(btn_x, btn_y, btn_w, btn_h, btn_color);
        vga_draw_rect(btn_x, btn_y, btn_w, btn_h, C_BORDER);
        
        vga_draw_string(btn_x + (btn_w - 80) / 2, btn_y + 8, "Start Celebration", C_TEXT_LIGHT, 0xFFFFFFFF);
    }
    
     /* Theme selector */
    vga_draw_string(cx + 20, content_y + content_h - 60, "Theme:", C_TEXT, 0xFFFFFFFF);
    const char* themes[] = {"Classic", "Modern", "Colorful"};
    for (int i = 0; i < 3; i++) {
        int tx = cx + 80 + i * 80;
        int ty = content_y + content_h - 60;
        bool selected = (i == w->birthday_theme);
        bool hover = (_get_mouse()->x >= tx && _get_mouse()->x < tx + 70 &&
                     _get_mouse()->y >= ty && _get_mouse()->y < ty + 20);
        
        uint64_t bg = selected ? C_SELECTED_BG : (hover ? C_HOVER_BG : C_WIN_BG);
        vga_fill_rect(tx, ty, 70, 20, bg);
        vga_draw_rect(tx, ty, 70, 20, C_BORDER);
        vga_draw_string(tx + (70 - slen(themes[i]) * FONT_W) / 2, ty + 2, themes[i], 
                      selected ? C_ACCENT : C_TEXT, 0xFFFFFFFF);
    }
    
     /* Status bar */
    char status[64];
    if (w->birthday_animation) {
        scopy(status, "Celebration in progress!", 63);
    } else {
        scopy(status, "Ready to celebrate", 63);
    }
    draw_statusbar(cx, cy + ch - C_STATUSBAR_H, cw, status, NULL);
}



