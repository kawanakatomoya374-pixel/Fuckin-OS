/**
 * gui_apps_clock.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * 時計アプリ
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
 * CLOCK APP
 * ============================================================ */
void draw_clock_app(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w, ch = w->h - C_TITLEBAR_H;

     /* Dark gradient background */
    for (int y = 0; y < ch; y++) {
        int t2 = y * 256 / ch;
        uint8_t rb = (uint8_t)(15 + t2 * 10 / 256);
        uint8_t gb = (uint8_t)(18 + t2 * 12 / 256);
        uint8_t bb = (uint8_t)(35 + t2 * 25 / 256);
        vga_fill_rect(cx, cy + y, cw, 1, rgb(rb, gb, bb));
    }

    rtc_time_t t; t = rtc_get_datetime();

    int ox = cx + cw/2;
    int oy = cy + ch/2 - 20;
    int r = (cw < ch ? cw : ch) / 2 - 30;
    if (r < 60) r = 60;

     /* Outer glow rings */
    vga_draw_circle(ox, oy, r+4, rgb(20, 40, 80));
    vga_draw_circle(ox, oy, r+3, rgb(30, 60, 120));
    vga_draw_circle(ox, oy, r+2, rgb(50, 90, 160));

     /* Clock face */
    vga_fill_circle(ox, oy, r, rgb(18, 22, 38));
    vga_draw_circle(ox, oy, r, rgb(80, 120, 200));
    vga_draw_circle(ox, oy, r-1, rgb(50, 80, 140));

     /* Tick marks using trig */
    for (int i = 0; i < 60; i++) {
        int angle = i * 6 - 90;
        int inner_r = (i % 5 == 0) ? r - 14 : r - 8;
        int outer_r = r - 3;
        int x1 = ox + vga_icos(angle) * inner_r / 1024;
        int y1 = oy + vga_isin(angle) * inner_r / 1024;
        int x2 = ox + vga_icos(angle) * outer_r / 1024;
        int y2 = oy + vga_isin(angle) * outer_r / 1024;
        if (i % 5 == 0) {
            vga_draw_line(x1, y1, x2, y2, rgb(200, 220, 255));
            vga_draw_line(x1+1, y1, x2+1, y2, rgb(200, 220, 255));
        } else {
            vga_put_pixel(x2, y2, rgb(80, 100, 140));
        }
    }

     /* Hour numbers */
    static const char* hlabels[12] = {"12","1","2","3","4","5","6","7","8","9","10","11"};
    for (int i = 0; i < 12; i++) {
        int angle = i * 30 - 90;
        int nr = r - 26;
        int hx2 = ox + vga_icos(angle) * nr / 1024 - slen(hlabels[i]) * FONT_W / 2;
        int hy2 = oy + vga_isin(angle) * nr / 1024 - FONT_H / 2;
        vga_draw_string(hx2, hy2, hlabels[i], rgb(160, 190, 240), 0xFFFFFFFF);
    }

     /* Clock hands */
    int hour_angle   = (int)(t.hour % 12) * 30 + (int)t.minute / 2 - 90;
    int minute_angle = (int)t.minute * 6 + (int)t.second / 10 - 90;
    int second_angle = (int)t.second * 6 - 90;

    int hour_len   = r * 55 / 100;
    int minute_len = r * 80 / 100;
    int second_len = r * 88 / 100;

     /* Hour hand (thick) */
    int hx3 = ox + vga_icos(hour_angle)   * hour_len   / 1024;
    int hy3 = oy + vga_isin(hour_angle)   * hour_len   / 1024;
    vga_draw_line_thick(ox, oy, hx3, hy3, 5, rgb(220, 230, 255));

     /* Minute hand */
    int mx3 = ox + vga_icos(minute_angle) * minute_len / 1024;
    int my3 = oy + vga_isin(minute_angle) * minute_len / 1024;
    vga_draw_line_thick(ox, oy, mx3, my3, 3, rgb(100, 180, 255));

     /* Second hand + counterweight */
    int sx2 = ox + vga_icos(second_angle) * second_len / 1024;
    int sy2 = oy + vga_isin(second_angle) * second_len / 1024;
    int stx = ox - vga_icos(second_angle) * (second_len/5) / 1024;
    int sty = oy - vga_isin(second_angle) * (second_len/5) / 1024;
    vga_draw_line(stx, sty, sx2, sy2, rgb(255, 60, 60));

     /* Center pivot */
    vga_fill_circle(ox, oy, 6, rgb(200, 220, 255));
    vga_fill_circle(ox, oy, 3, rgb(255, 60, 60));

     /* Digital time */
    char time_str[16];
    time_str[0] = '0' + (t.hour/10)%10;   time_str[1] = '0' + t.hour%10;
    time_str[2] = ':';
    time_str[3] = '0' + (t.minute/10)%10; time_str[4] = '0' + t.minute%10;
    time_str[5] = ':';
    time_str[6] = '0' + (t.second/10)%10; time_str[7] = '0' + t.second%10;
    time_str[8] = 0;
    int tw2 = slen(time_str)*FONT_W;
    vga_draw_string(ox - tw2/2, oy + r + 16, time_str, rgb(200, 220, 255), 0xFFFFFFFF);

     /* Date */
    char date_str[32]; char tmp2[8];
    uitostr(t.year, tmp2); scopy(date_str, tmp2, 31); scat(date_str, "/", 31);
    uitostr(t.month, tmp2); if(slen(tmp2)<2){scat(date_str,"0",31);} scat(date_str,tmp2,31);
    scat(date_str, "/", 31);
    uitostr(t.day, tmp2); if(slen(tmp2)<2){scat(date_str,"0",31);} scat(date_str,tmp2,31);
    int dw2 = slen(date_str)*FONT_W;
    vga_draw_string(ox - dw2/2, oy + r + 36, date_str, rgb(150, 180, 220), 0xFFFFFFFF);
}

void draw_sysinfo_app(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w, ch = w->h - C_TITLEBAR_H;

    vga_fill_rect(cx, cy, cw, ch, C_WIN_BG);
    vga_fill_rect(cx, cy, cw, 60, rgb(0,120,212));
    vga_draw_string(cx+16, cy+10, "System Information", C_TEXT_LIGHT, 0xFFFFFFFF);
    vga_draw_string(cx+16, cy+30, "C-OS 4.0.8 alpha 64-bit", rgb(180,220,255), 0xFFFFFFFF);

    bios_system_info_t sys;
    bios_get_system_info_copy(&sys);

    char mem_total_str[32], mem_used_str[32], mem_free_str[32], storage_str[32];
    format_size_bytes(phys_get_total_memory(), mem_total_str, 31);
    format_size_bytes(kmemory_used(), mem_used_str, 31);
    format_size_bytes(kmemory_free(), mem_free_str, 31);
    format_size_bytes(storage_get_total_space(), storage_str, 31);

    struct { const char* key; const char* val; } info[] = {
        {"OS Name:",      COS_FULL_NAME},
        {"OS Version:",   COS_VERSION_STRING},
        {"Architecture:", "x86-64"},
        {"Kernel:",       COS_BOOT_MESSAGE},
        {"Boot Mode:",    "GRUB2 Multiboot"},
        {"CPU:",          (sys.cpu_vendor[0] ? sys.cpu_vendor : "x86 Compatible")},
        {"Total Memory:", mem_total_str},
        {"Used Memory:",  mem_used_str},
        {"Free Memory:",  mem_free_str},
        {"Storage:",      storage_str},
        {NULL, NULL}
    };

    int iy = cy + 76;
    for (int i = 0; info[i].key; i++) {
        int row_y = iy + i * 26;
        if (row_y > cy + ch - C_STATUSBAR_H - 26) break;
        if (i % 2 == 0) vga_fill_rect(cx, row_y, cw, 26, rgb(248,248,248));
        vga_draw_string(cx+16, row_y+5, info[i].key, C_TEXT_GRAY, 0xFFFFFFFF);
        const char* val = info[i].val;
        if (i == 6) val = mem_used_str;
        else if (i == 7) val = mem_free_str;
        vga_draw_string(cx+220, row_y+5, val, C_TEXT, 0xFFFFFFFF);
    }

    vga_draw_string(cx+16, cy+ch-C_STATUSBAR_H-20, "Files are browsed and edited directly from File Manager.", C_TEXT_GRAY, 0xFFFFFFFF);
}

