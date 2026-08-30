/**
 * gui_apps_memory.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * メモリマネージャアプリ
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
 * MEMORY MANAGER APPLICATION
 * ============================================================ */
void draw_memory_manager_app(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x, cy = w->y, cw = w->w, ch = w->h;
    
    draw_window_frame(idx);
    
     /* Tab bar */
    const char* tabs[] = {"Overview", "Processes", "Performance", "Settings"};
    int tab_count = 4;
    int tab_w = (cw - 40) / tab_count;
    
    for (int i = 0; i < tab_count; i++) {
        int tx = cx + 20 + i * tab_w;
        int ty = cy + TITLEBAR_H + 8;
        bool active = (i == w->mem_mgr_tab);
        bool hover = (_get_mouse()->x >= tx && _get_mouse()->x < tx + tab_w - 8 &&
                     _get_mouse()->y >= ty && _get_mouse()->y < ty + 24);
        
        uint64_t bg = active ? C_SELECTED_BG : (hover ? C_HOVER_BG : C_WIN_BG);
        vga_fill_rect(tx, ty, tab_w - 8, 24, bg);
        vga_draw_rect(tx, ty, tab_w - 8, 24, C_BORDER);
        vga_draw_string(tx + (tab_w - 8 - slen(tabs[i]) * FONT_W) / 2, ty + 4, tabs[i], 
                      active ? C_ACCENT : C_TEXT, 0xFFFFFFFF);
    }
    
    int content_y = cy + TITLEBAR_H + 40;
    
    switch (w->mem_mgr_tab) {
                case 0:
            {
                 /* Memory usage visualization */
                int bar_y = content_y + 20;
                int bar_h = 60;
                int bar_w = cw - 80;
                int bar_x = cx + 40;
                uint64_t total_pages = 0, free_pages = 0, used_pages = 0;
                phys_mem_stats_t* pst = phys_memory_get_stats();
                if (pst) {
                    total_pages = pst->total_pages;
                    free_pages = pst->free_pages;
                    used_pages = pst->used_pages;
                }
                uint64_t total_bytes = total_pages * 4096ULL;
                uint64_t used_bytes = used_pages * 4096ULL;
                uint64_t free_bytes = free_pages * 4096ULL;
                if (total_bytes == 0) total_bytes = phys_get_total_memory();
                if (total_bytes == 0) total_bytes = 1;

                vga_draw_rect(bar_x, bar_y, bar_w, bar_h, C_BORDER);
                vga_fill_rect(bar_x + 1, bar_y + 1, bar_w - 2, bar_h - 2, C_WIN_BG);
                int used_w = (int)((used_bytes * (uint64_t)(bar_w - 4)) / total_bytes);
                vga_fill_rect(bar_x + 2, bar_y + 2, used_w, bar_h - 4, C_ACCENT);

                 /* Memory info */
                char info[96], total_str[32], used_str[32], free_str[32];
                format_size_bytes(used_bytes, used_str, 31);
                format_size_bytes(total_bytes, total_str, 31);
                format_size_bytes(free_bytes, free_str, 31);
                scopy(info, used_str, 95); scat(info, " used of ", 95); scat(info, total_str, 95);

                vga_draw_string(cx + 40, bar_y - 20, "Memory Usage:", C_TEXT, 0xFFFFFFFF);
                vga_draw_string(cx + 40, bar_y + bar_h + 10, info, C_TEXT_GRAY, 0xFFFFFFFF);

                 /* Memory blocks */
                vga_draw_string(cx + 40, bar_y + bar_h + 40, "Memory Blocks:", C_TEXT, 0xFFFFFFFF);
                for (int i = 0; i < 8; i++) {
                    int by = bar_y + bar_h + 60 + i * 20;
                    char block_info[64];
                    scopy(block_info, "Page set ", 63);
                    char tmp[16];
                    int_to_str(i, tmp, 10);
                    scat(block_info, tmp, 63);
                    scat(block_info, ": ", 63);
                    if (pst) {
                        int_to_str((i < 4) ? (int)used_pages : (int)free_pages, tmp, 10);
                        scat(block_info, tmp, 63);
                        scat(block_info, " pages", 63);
                    } else {
                        scat(block_info, "unavailable", 63);
                    }
                    vga_draw_string(cx + 60, by, block_info, C_TEXT_GRAY, 0xFFFFFFFF);
                }
                vga_draw_string(cx + 40, bar_y + bar_h + 210, free_str, C_TEXT_GRAY, 0xFFFFFFFF);
            }
            break;
            
                case 1:
            {
                vga_draw_string(cx + 40, content_y + 20, "Active Processes:", C_TEXT, 0xFFFFFFFF);
                for (int i = 0; i < window_count; i++) {
                    int py = content_y + 50 + i * 25;
                    char proc_info[64];
                    scopy(proc_info, windows[i].title, 63);
                    scat(proc_info, " (", 63);
                    int_to_str(windows[i].kind, proc_info + slen(proc_info), 10);
                    scat(proc_info, ")", 63);
                    vga_draw_string(cx + 60, py, proc_info, C_TEXT_GRAY, 0xFFFFFFFF);
                }
            }
            break;
            
                case 2:
            {
                vga_draw_string(cx + 40, content_y + 20, "Performance Monitor:", C_TEXT, 0xFFFFFFFF);
                 /* Simple performance graph */
                int graph_y = content_y + 50;
                int graph_h = 100;
                int graph_w = cw - 80;
                int graph_x = cx + 40;
                
                vga_draw_rect(graph_x, graph_y, graph_w, graph_h, C_BORDER);
                vga_fill_rect(graph_x + 1, graph_y + 1, graph_w - 2, graph_h - 2, rgb(240, 240, 240));
                
                 /* Performance bars */
                for (int i = 0; i < 5; i++) {
                    int bar_x = graph_x + 10 + i * 30;
                    int bar_h = (gui_frame_counter % 100) * (graph_h - 20) / 100;
                    vga_fill_rect(bar_x, graph_y + graph_h - bar_h - 10, 20, bar_h, C_ACCENT);
                }
            }
            break;
            
                case 3:
            {
                vga_draw_string(cx + 40, content_y + 20, "Memory Settings:", C_TEXT, 0xFFFFFFFF);
                
                 /* Auto refresh checkbox */
                int check_x = cx + 60;
                int check_y = content_y + 50;
                vga_draw_rect(check_x, check_y, 12, 12, C_BORDER);
                if (w->mem_auto_refresh) {
                    vga_fill_rect(check_x + 2, check_y + 2, 8, 8, C_ACCENT);
                }
                vga_draw_string(check_x + 20, check_y, "Auto Refresh", C_TEXT, 0xFFFFFFFF);
                
                 /* Refresh interval */
                vga_draw_string(check_x, check_y + 30, "Refresh Interval (ms):", C_TEXT, 0xFFFFFFFF);
                char interval_str[16];
                int_to_str(w->mem_refresh_interval, interval_str, 10);
                vga_draw_string(check_x + 180, check_y + 30, interval_str, C_TEXT_GRAY, 0xFFFFFFFF);
            }
            break;
    }
    
     /* Status bar */
    char status[64];
    scopy(status, "Memory Manager - ", 63);
    char used_str[32];
    format_size_bytes(kmemory_used(), used_str, 31);
    scat(status, used_str, 63);
    draw_statusbar(cx, cy + ch - C_STATUSBAR_H, cw, status, NULL);
}

