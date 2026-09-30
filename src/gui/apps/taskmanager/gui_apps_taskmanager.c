/**
 * gui_apps_taskmanager.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * タスクマネージャ
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
#include "smp.h"

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

static void taskmgr_draw_process_rows(int cx, int cont_y, int cw, int ch) {
    (void)ch;
    sched_stats_t stats = {0};
    scheduler_get_stats(&stats);

    vga_fill_rect(cx, cont_y, cw, 52, C_TOOLBAR);
    vga_fill_rect(cx, cont_y+51, cw, 1, C_BORDER);

    char buf[64];
    vga_draw_string(cx+8, cont_y+8, "Live scheduler stats", C_TEXT, 0xFFFFFFFF);
    scopy(buf, "Load: ", 63); uitostr(stats.current_load, buf + 6); scat(buf, "%", 63);
    vga_draw_string(cx+8, cont_y+26, buf, C_TEXT, 0xFFFFFFFF);

    char ts[64];
    scopy(ts, "Uptime: ", 63); uitostr(scheduler_get_uptime(), ts + 8); scat(ts, " ticks", 63);
    vga_draw_string(cx+180, cont_y+26, ts, C_TEXT, 0xFFFFFFFF);

    char cs[64];
    scopy(cs, "Ctx switches: ", 63); uitostr(stats.context_switches, cs + 14);
    vga_draw_string(cx+380, cont_y+26, cs, C_TEXT, 0xFFFFFFFF);

    vga_fill_rect(cx, cont_y + 52, cw, 28, C_TOOLBAR);
    vga_fill_rect(cx, cont_y + 79, cw, 1, C_BORDER);
    vga_draw_string(cx+8, cont_y+58, "Window", C_TEXT_GRAY, 0xFFFFFFFF);
    vga_draw_string(cx+280, cont_y+58, "Kind", C_TEXT_GRAY, 0xFFFFFFFF);
    vga_draw_string(cx+440, cont_y+58, "State", C_TEXT_GRAY, 0xFFFFFFFF);

    int row = 0;
    for (int i = 0; i < window_count && row < 8; i++) {
        window_t* win = &windows[i];
        if (!win->visible && !win->minimized) continue;
        int ry = cont_y + 80 + row * 30;
        bool hov = (_get_mouse()->x >= cx && _get_mouse()->x < cx+cw && _get_mouse()->y >= ry && _get_mouse()->y < ry+30);
        vga_fill_rect(cx, ry, cw, 30, hov ? C_HOVER_BG : (row % 2 == 0 ? C_WIN_BG : C_WIN_BG2));
        vga_fill_rect(cx, ry+29, cw, 1, rgb(235,235,235));
        vga_draw_string(cx+8, ry+7, win->title[0] ? win->title : "Untitled", C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx+280, ry+7, window_kind_name(win->kind), C_TEXT, 0xFFFFFFFF);
        const char* st = win->minimized ? "Minimized" : (win->focused ? "Focused" : "Running");
        vga_draw_string(cx+440, ry+7, st, win->focused ? C_ACCENT : C_TEXT, 0xFFFFFFFF);
        row++;
    }

    if (row == 0) {
        vga_draw_string(cx+8, cont_y+98, "No visible application windows.", C_TEXT_DIM, 0xFFFFFFFF);
    }
}

static void taskmgr_draw_hw_tab(int cx, int cont_y, int cw) {
    (void)cw;
    bios_system_info_t info; bios_get_system_info_copy(&info);
    char line[160];

    vga_draw_string(cx + 16, cont_y + 18, "Hardware & firmware", C_ACCENT, 0xFFFFFFFF);

    scopy(line, "CPUs: ", 159); uitostr(info.num_cpus ? info.num_cpus : 1, line + 6);
    scat(line, " | APIC: ", 159); scat(line, info.has_apic ? "yes" : "no", 159);
    scat(line, " | ACPI: ", 159); scat(line, info.has_acpi ? "yes" : "no", 159);
    vga_draw_string(cx + 16, cont_y + 48, line, C_TEXT, 0xFFFFFFFF);

    scopy(line, "SMP platform: ", 159);
    uitostr(smp_online_cpu_count(), line + strlen(line));
    scat(line, " online / ", 159);
    uitostr(smp_possible_cpu_count(), line + strlen(line));
    scat(line, " possible | AP startup: ", 159);
    scat(line, smp_secondary_startup_ready() ? "ready" : "deferred safely", 159);
    vga_draw_string(cx + 16, cont_y + 78, line, C_TEXT_DIM, 0xFFFFFFFF);

    scopy(line, "CPU vendor: ", 159); scat(line, info.cpu_vendor, 159);
    vga_draw_string(cx + 16, cont_y + 108, line, C_TEXT, 0xFFFFFFFF);

    scopy(line, "BIOS: ", 159); scat(line, info.bios_vendor, 159); scat(line, " ", 159); scat(line, info.bios_version, 159);
    vga_draw_string(cx + 16, cont_y + 138, line, C_TEXT, 0xFFFFFFFF);

    if (usb_is_initialized()) {
        char usb_line[160];
        usb_get_status(usb_line, sizeof(usb_line));
        vga_draw_string(cx + 16, cont_y + 206, usb_line, C_TEXT, 0xFFFFFFFF);
        if (!usb_has_usb2()) {
            vga_draw_string(cx + 16, cont_y + 206, "USB2/EHCI controller not detected on this machine.", C_ERROR_COL, 0xFFFFFFFF);
        }
    } else {
        vga_draw_string(cx + 16, cont_y + 206, "USB subsystem not initialized.", C_ERROR_COL, 0xFFFFFFFF);
    }

    const char* last_err = usb_get_last_error();
    if (last_err) {
        vga_draw_string(cx + 16, cont_y + 206, "Last USB error:", C_WARNING, 0xFFFFFFFF);
        vga_draw_string(cx + 16, cont_y + 232, last_err, C_ERROR_COL, 0xFFFFFFFF);
    }
}

/* ============================================================
 * TASK MANAGER
 * ============================================================ */
void draw_task_manager(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w, ch = w->h - C_TITLEBAR_H;

    vga_fill_rect(cx, cy, cw, ch, C_WIN_BG);

     /* Tab bar */
    const char* tabs[] = {"Processes", "Performance", "Details", "Hardware", NULL};
    int tx = cx + 8;
    for (int i = 0; tabs[i]; i++) {
        bool sel = (w->taskmgr_tab == i);
        bool hov = (_get_mouse()->x >= tx && _get_mouse()->x < tx+100 && _get_mouse()->y >= cy && _get_mouse()->y < cy+32);
        vga_fill_rect(tx, cy, 100, 32, sel ? C_WIN_BG : (hov ? C_HOVER_BG : C_TOOLBAR));
        if (sel) vga_fill_rect(tx, cy+29, 100, 3, C_ACCENT);
        vga_draw_string(tx+8, cy+8, tabs[i], sel ? C_ACCENT : C_TEXT, 0xFFFFFFFF);
        tx += 108;
    }
    vga_fill_rect(cx, cy+31, cw, 1, C_BORDER);

    int cont_y = cy + 32;

    if (w->taskmgr_tab == 0) {
        taskmgr_draw_process_rows(cx, cont_y, cw, ch);
    } else if (w->taskmgr_tab == 1) {
         /* Performance */
        int gx = cx+16, gy = cont_y+16, gw = cw-32, gh = 120;
        sched_stats_t s = {0}; scheduler_get_stats(&s);
        uint64_t cpu_pct = s.current_load;
        if (cpu_pct > 100) cpu_pct = 100;

         /* CPU graph */
        vga_draw_string(gx, gy-4, "CPU Usage", C_TEXT, 0xFFFFFFFF);
        vga_fill_rect(gx, gy+12, gw, gh, rgb(10,10,10));
        vga_draw_rect(gx, gy+12, gw, gh, C_BORDER);
        for (int i = 0; i < gw-4; i++) {
            int load = (int)cpu_pct;
            int bar_h = load * gh / 100;
            uint64_t bar_col = (load < 50) ? rgb(0, 200, 100) : (load < 80) ? rgb(220, 180, 0) : rgb(200, 60, 40);
            vga_fill_rect(gx+2+i, gy+12+gh-bar_h, 1, bar_h, bar_col);
        }
        char cpu_pct_str[32];
        uitostr(cpu_pct, cpu_pct_str); scat(cpu_pct_str, "% CPU", 31);
        vga_draw_string(gx+gw-56, gy+14, cpu_pct_str, rgb(0,200,100), 0xFFFFFFFF);

         /* Memory graph */
        gy += gh + 32;
        vga_draw_string(gx, gy-4, "Memory Usage", C_TEXT, 0xFFFFFFFF);
        vga_fill_rect(gx, gy+12, gw, gh, rgb(10,10,10));
        vga_draw_rect(gx, gy+12, gw, gh, C_BORDER);
        uint64_t mem_used = kmemory_used();
        uint64_t mem_total = kmemory_used() + kmemory_free();
        if (mem_total == 0) mem_total = 1;
        int mem_bar = (int)((gw - 4) * mem_used / mem_total);
        vga_fill_rect(gx+2, gy+14, mem_bar, gh-4, rgb(0,120,212));
        char mem_str[64];
        format_size_bytes(mem_used, mem_str, 63);
        scat(mem_str, " used of ", 63);
        char mem_total_str[32];
        format_size_bytes(mem_total, mem_total_str, 31);
        scat(mem_str, mem_total_str, 63);
        vga_draw_string(gx+8, gy+gh/2, mem_str, C_TEXT_LIGHT, 0xFFFFFFFF);

         /* Storage graph */
        gy += gh + 32;
        vga_draw_string(gx, gy-4, "Storage Usage", C_TEXT, 0xFFFFFFFF);
        vga_fill_rect(gx, gy+12, gw, 32, rgb(220,220,220));
        vga_draw_rect(gx, gy+12, gw, 32, C_BORDER);
        {
            uint64_t stot  = storage_get_total_space();
            uint64_t sused = storage_get_used_space();
            int spct = (stot > 0) ? (int)((sused * 100ULL) / stot) : 0;
            if (spct > 100) spct = 100;
            if (spct > 0) vga_fill_rect(gx+2, gy+14, gw*spct/100, 28, rgb(0,180,80));
            char stor_used[32], stor_tot[32], sl[96];
            format_size_bytes(sused, stor_used, 31);
            format_size_bytes(stot,  stor_tot,  31);
            scopy(sl, stor_used, 95); scat(sl, " used of ", 95); scat(sl, stor_tot, 95);
            vga_draw_string(gx+8, gy+22, sl, C_TEXT, 0xFFFFFFFF);
        }
    } else if (w->taskmgr_tab == 2) {
         /* Details */
        vga_draw_string(cx + 16, cont_y + 18, "System Bridge", C_ACCENT, 0xFFFFFFFF);
        vga_draw_string(cx + 16, cont_y + 52, "Shell backend:", C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx + 180, cont_y + 52,  "", C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx + 16, cont_y + 82, "File manager:", C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx + 180, cont_y + 82,  "", C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx + 16, cont_y + 112, "Text editor:", C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx + 180, cont_y + 112,  "", C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx + 16, cont_y + 142, "Task manager:", C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx + 180, cont_y + 142,  "", C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx + 16, cont_y + 172, "Transport:", C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx + 180, cont_y + 172,  "", C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx + 16, cont_y + 202, "Sync:", C_TEXT, 0xFFFFFFFF);
        vga_draw_string(0, 0, "Offline", C_SUCCESS, 0xFFFFFFFF);
        vga_draw_string(cx + 16, cont_y + 232, "Packages:", C_TEXT, 0xFFFFFFFF);
        {
            char pkg[16];
            scopy(pkg, "0", sizeof(pkg)-1);
            vga_draw_string(cx + 180, cont_y + 232, pkg, C_TEXT, 0xFFFFFFFF);
        }
    } else if (w->taskmgr_tab == 3) {
        taskmgr_draw_hw_tab(cx, cont_y, cw);
    }

     /* Status bar */
    char status[96];
    sched_stats_t stats = {0};
    scheduler_get_stats(&stats);
    scopy(status, "Windows: ", 95);
    char tmp[32]; uitostr((uint64_t)window_count, tmp);
    scat(status, tmp, 95); scat(status, " | CPU: ", 95);
    char cpu_tmp[16]; uitostr((uint64_t)stats.current_load, cpu_tmp);
    scat(status, cpu_tmp, 95); scat(status, "% | Memory: ", 95);
    format_size_bytes((uint64_t)kmemory_used(), tmp, 31);
    scat(status, tmp, 95); scat(status, "/", 95);
    format_size_bytes((uint64_t)kmemory_used() + (uint64_t)kmemory_free(), tmp, 31);
    scat(status, tmp, 95);
    draw_statusbar(cx, cy+ch-C_STATUSBAR_H, cw, status, NULL);
}

