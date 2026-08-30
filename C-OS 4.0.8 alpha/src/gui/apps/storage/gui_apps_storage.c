/**
 * gui_apps_storage.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * ストレージ管理アプリ
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
 * STORAGE APP - Enhanced Storage Manager with FAT32 support
 * ============================================================ */
void draw_storage_app(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w, ch = w->h - C_TITLEBAR_H;

    vga_fill_rect(cx, cy, cw, ch, C_WIN_BG);
    
     /* Title area */
    vga_draw_string(cx+16, cy+16, "Storage Manager", C_ACCENT, 0xFFFFFFFF);
    vga_draw_string(cx+16, cy+38, "C-OS 4.0.8 alpha Dual Filesystem", C_TEXT_GRAY, 0xFFFFFFFF);

    int device_count = unified_storage_get_device_count ? unified_storage_get_device_count() : 0;
    uint64_t logical_total = unified_storage_get_total_capacity ? unified_storage_get_total_capacity() : 0;
    uint64_t logical_free = unified_storage_get_free_capacity ? unified_storage_get_free_capacity() : 0;
    char dev_buf[32], cap_buf[32], free_buf[32];
    format_size_bytes(logical_total, cap_buf, 31);
    format_size_bytes(logical_free, free_buf, 31);
    uitostr((uint64_t)device_count, dev_buf);
    char topinfo[128];
    scopy(topinfo, "Devices: ", sizeof(topinfo)-1); scat(topinfo, dev_buf, sizeof(topinfo)-1);
    scat(topinfo, "  Total: ", sizeof(topinfo)-1); scat(topinfo, cap_buf, sizeof(topinfo)-1);
    scat(topinfo, "  Free: ", sizeof(topinfo)-1); scat(topinfo, free_buf, sizeof(topinfo)-1);
    vga_draw_string(cx+16, cy+54, topinfo, C_TEXT_GRAY, 0xFFFFFFFF);

    int py = cy + 78;
    int half_w = (cw - 48) / 2;

     /* ===== FAT32 Partition Card ===== */
    vga_fill_rounded_rect(cx+16, py, half_w, 140, 8, C_WIN_BG2);
    vga_draw_rounded_rect(cx+16, py, half_w, 140, 8, C_BORDER);
    
     /* Persistent storage card */
    vga_fill_rect(cx+16, py, half_w, 28, C_ACCENT);
    vga_draw_string(cx+28, py+7, "Persistent Storage", C_TEXT_LIGHT, 0xFFFFFFFF);
    
    uint64_t fat32_total = storage_get_total_space();
    uint64_t fat32_free  = storage_get_free_space();
    uint64_t fat32_used  = storage_get_used_space();
    
    char sz_fat32_total[32], sz_fat32_used[32], sz_fat32_free[32];
    format_size_bytes(fat32_total, sz_fat32_total, 31);
    format_size_bytes(fat32_used, sz_fat32_used, 31);
    format_size_bytes(fat32_free, sz_fat32_free, 31);
    
     /* Size info */
    vga_draw_string(cx+28, py+38, sz_fat32_total, C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx+28+100, py+38, "logical VFS space", C_TEXT_GRAY, 0xFFFFFFFF);
    
     /* Progress bar */
    int bar_w = half_w - 24;
    int fat32_pct = (fat32_total > 0) ? (int)(fat32_used * 100 / fat32_total) : 0;
    if (fat32_pct > 100) fat32_pct = 100;
    
    vga_fill_rect(cx+28, py+62, bar_w, 18, rgb(220,220,220));
    if (fat32_pct > 0) {
        uint64_t bar_color = (fat32_pct > 90) ? C_ERROR_COL : 
                             (fat32_pct > 70) ? C_WARNING : C_SUCCESS;
        vga_fill_rect(cx+28, py+62, bar_w * fat32_pct / 100, 18, bar_color);
    }
    vga_draw_rect(cx+28, py+62, bar_w, 18, C_BORDER);
    
    char fat32_pct_str[16]; uitostr((uint64_t)fat32_pct, fat32_pct_str); 
    scat(fat32_pct_str, "%", 15);
    vga_draw_string(cx+28+bar_w+6, py+64, fat32_pct_str, C_TEXT_GRAY, 0xFFFFFFFF);
    
     /* Used/Free */
    char fat32_used_str[48]; scopy(fat32_used_str, "Used: ", 47); scat(fat32_used_str, sz_fat32_used, 47);
    char fat32_free_str[48]; scopy(fat32_free_str, "Free: ", 47); scat(fat32_free_str, sz_fat32_free, 47);
    vga_draw_string(cx+28, py+88, fat32_used_str, C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx+28, py+108, fat32_free_str, C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx+28+half_w/2, py+88, storage_has_password() ? "Password: set" : "Password: none", C_TEXT_GRAY, 0xFFFFFFFF);

     /* ===== RAMFS Partition Card ===== */
    int px2 = cx + 16 + half_w + 16;
    vga_fill_rounded_rect(px2, py, half_w, 140, 8, C_WIN_BG2);
    vga_draw_rounded_rect(px2, py, half_w, 140, 8, C_BORDER);
    
     /* RAMFS Header */
    vga_fill_rect(px2, py, half_w, 28, rgb(0, 150, 180));
    vga_draw_string(px2+12, py+7, "RAM Filesystem", C_TEXT_LIGHT, 0xFFFFFFFF);
    
    uint64_t ramfs_total = storage_get_total_space();
    uint64_t ramfs_used  = storage_get_used_space();
    uint64_t ramfs_free  = storage_get_free_space();
    int file_cnt = fs_entry_count();
    
    char sz_ramfs_total[32], sz_ramfs_used[32], sz_ramfs_free[32];
    format_size_bytes(ramfs_total, sz_ramfs_total, 31);
    format_size_bytes(ramfs_used, sz_ramfs_used, 31);
    format_size_bytes(ramfs_free, sz_ramfs_free, 31);
    
     /* RAMFS Size info */
    vga_draw_string(px2+12, py+38, sz_ramfs_total, C_TEXT, 0xFFFFFFFF);
    vga_draw_string(px2+12+100, py+38, "Volatile RAM", C_TEXT_GRAY, 0xFFFFFFFF);
    
     /* RAMFS Progress bar */
    int ramfs_pct = (ramfs_total > 0) ? (int)(ramfs_used * 100 / ramfs_total) : 0;
    if (ramfs_pct > 100) ramfs_pct = 100;
    
    vga_fill_rect(px2+12, py+62, bar_w, 18, rgb(220,220,220));
    if (ramfs_pct > 0) {
        uint64_t bar_color = (ramfs_pct > 90) ? C_ERROR_COL : 
                             (ramfs_pct > 70) ? C_WARNING : C_SUCCESS;
        vga_fill_rect(px2+12, py+62, bar_w * ramfs_pct / 100, 18, bar_color);
    }
    vga_draw_rect(px2+12, py+62, bar_w, 18, C_BORDER);
    
    char ramfs_pct_str[16]; uitostr((uint64_t)ramfs_pct, ramfs_pct_str); 
    scat(ramfs_pct_str, "%", 15);
    vga_draw_string(px2+12+bar_w+6, py+64, ramfs_pct_str, C_TEXT_GRAY, 0xFFFFFFFF);
    
     /* RAMFS Used/Free */
    char ramfs_used_str[48]; scopy(ramfs_used_str, "Used: ", 47); scat(ramfs_used_str, sz_ramfs_used, 47);
    char ramfs_free_str[48]; scopy(ramfs_free_str, "Free: ", 47); scat(ramfs_free_str, sz_ramfs_free, 47);
    vga_draw_string(px2+12, py+88, ramfs_used_str, C_TEXT, 0xFFFFFFFF);
    vga_draw_string(px2+12, py+108, ramfs_free_str, C_TEXT, 0xFFFFFFFF);
    
    char files_str[32]; scopy(files_str, "Files: ", 31); 
    char fcstr[12]; uitostr((uint64_t)file_cnt, fcstr); scat(files_str, fcstr, 31);
    vga_draw_string(px2+12+half_w/2, py+88, files_str, C_TEXT_GRAY, 0xFFFFFFFF);
    vga_draw_string(px2+12+half_w/2, py+108, storage_has_password() ? "Persistent password set" : "Password not set", C_TEXT_GRAY, 0xFFFFFFFF);

     /* ===== File List Section ===== */
    int list_y = py + 155;
    vga_draw_string(cx+16, list_y, "Files on RAMFS:", C_TEXT, 0xFFFFFFFF);
    list_y += 22;

    fs_entry_t* entries = fs_list_dir("/");
    int total_entries = file_cnt;
    if (total_entries > 8) total_entries = 8;

    for (int i = 0; i < total_entries; i++) {
        int ey = list_y + i * 22;
        if (ey > cy + ch - C_STATUSBAR_H - 22) break;
        
        bool hov = (_get_mouse()->x >= cx+16 && _get_mouse()->x < cx+cw-16 &&
                    _get_mouse()->y >= ey      && _get_mouse()->y < ey+22);
        if (hov) vga_fill_rect(cx+16, ey, cw-32, 22, C_HOVER_BG);

        uint64_t icon_col = entries[i].is_dir ? rgb(0,120,212) : rgb(80,80,80);
        vga_fill_rect(cx+22, ey+4, 12, 14, icon_col);
        vga_draw_rect(cx+22, ey+4, 12, 14, rgb(0,60,120));
        if (!entries[i].is_dir) vga_fill_rect(cx+26, ey+7, 4, 2, rgb(220,220,220));

        vga_draw_string(cx+42, ey+4, entries[i].name, C_TEXT, 0xFFFFFFFF);

        if (!entries[i].is_dir && entries[i].size > 0) {
            char fsz[24]; format_size_bytes(entries[i].size, fsz, 23);
            int fw = slen(fsz) * FONT_W;
            vga_draw_string(cx+cw-fw-32, ey+4, fsz, C_TEXT_GRAY, 0xFFFFFFFF);
        }
    }

     /* Status bar */
    char status[80];
    scopy(status, "FAT32: ", 79); scat(status, sz_fat32_free, 79);
    scat(status, " free | RAMFS: ", 79); scat(status, sz_ramfs_free, 79);
    scat(status, " free", 79);
    draw_statusbar(cx, cy+ch-C_STATUSBAR_H, cw, status, NULL);
}

