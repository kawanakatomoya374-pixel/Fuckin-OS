/**
 * gui_apps_about.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * Aboutダイアログ
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
 * ABOUT
 * ============================================================ */
void draw_about(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w, ch = w->h - C_TITLEBAR_H;

     /* Gradient background */
    for (int y = 0; y < ch; y++) {
        uint8_t r = (uint8_t)(0  + y*20/ch);
        uint8_t g = (uint8_t)(80 + y*40/ch);
        uint8_t b = (uint8_t)(180+ y*40/ch);
        vga_fill_rect(cx, cy+y, cw, 1, rgb(r,g,b));
    }

     /* Logo */
    vga_fill_rounded_rect(cx+cw/2-60, cy+20, 120, 120, 20, rgb(0,100,200));
    vga_draw_string(cx+cw/2-24, cy+70, "C-OS 4.0.8 alpha", C_TEXT_LIGHT, 0xFFFFFFFF);
    vga_draw_string(cx+cw/2-16, cy+90, "v4.0.8 alpha", rgb(200,230,255), 0xFFFFFFFF);

    vga_draw_string(cx+cw/2-60, cy+160, "C-OS 4.0.8 alpha Operating System", C_TEXT_LIGHT, 0xFFFFFFFF);
    vga_draw_string(cx+cw/2-48, cy+184, "Version 4.0.8 alpha (2026)", rgb(200,230,255), 0xFFFFFFFF);
    {
        char about_mem[48], about_stor[48], about_line[96];
        format_size_bytes(phys_get_total_memory(), about_mem, 47);
        format_size_bytes(storage_get_total_space(), about_stor, 47);
        scopy(about_line, about_mem, 95);
        scat(about_line, " RAM | ", 95);
        scat(about_line, about_stor, 95);
        scat(about_line, " Storage", 95);
        vga_draw_string(cx+cw/2-((slen(about_line)*FONT_W)/2), cy+212, about_line, rgb(180,220,255), 0xFFFFFFFF);
    }
    vga_draw_string(cx+cw/2-64, cy+236, "Fluent Design UI System", rgb(180,220,255), 0xFFFFFFFF);
    vga_draw_string(cx+cw/2-72, cy+260, "x86 64-bit Bare Metal Kernel", rgb(180,220,255), 0xFFFFFFFF);

     /* OK button */
    bool hov = (_get_mouse()->x >= cx+cw/2-50 && _get_mouse()->x < cx+cw/2+50 && _get_mouse()->y >= cy+ch-60 && _get_mouse()->y < cy+ch-30);
    vga_fill_rounded_rect(cx+cw/2-50, cy+ch-60, 100, 30, 6, hov ? rgb(0,100,180) : C_ACCENT);
    vga_draw_string(cx+cw/2-8, cy+ch-50, "OK", C_TEXT_LIGHT, 0xFFFFFFFF);
}


