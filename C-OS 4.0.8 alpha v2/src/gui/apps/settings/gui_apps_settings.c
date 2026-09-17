/**
 * gui_apps_settings.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * 設定パネル
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
 * SETTINGS - Full settings panel
 * ============================================================ */
static const char* settings_tabs[] = {
    "System", "Display", "Network", "Storage",
    "Users", "Security", "About", "Terminal", "Shortcuts",
    "Power", "Startup", "Accessibility", "Input", "Files", NULL
};

/* Fourteen categories must remain reachable in a 768px desktop window. */
#define SETTINGS_TAB_ROW_H 26
#define SETTINGS_TAB_HIT_H 24

static const char* settings_tab_label(int tab) {
    static const char* const en[] = {
        "System", "Display", "Network", "Storage", "Users", "Security",
        "About", "Terminal", "Shortcuts", "Power", "Startup", "Accessibility",
        "Input", "Files"
    };
    static const char* const ja[] = {
        "システム", "表示", "ネットワーク", "ストレージ", "ユーザー", "セキュリティ",
        "情報", "ターミナル", "ショートカット", "電源", "起動", "アクセシビリティ",
        "入力", "ファイル"
    };
    if (tab < 0 || tab >= 14) tab = 0;
    return gui_text(en[tab], ja[tab]);
}

static void settings_format_ip(ip_addr_t ip, char* out, size_t out_size) {
    snprintf(out, out_size, "%d.%d.%d.%d", ip.addr[0], ip.addr[1], ip.addr[2], ip.addr[3]);
}

static void settings_format_mac(eth_addr_t mac, char* out, size_t out_size) {
    snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac.addr[0], mac.addr[1], mac.addr[2], mac.addr[3], mac.addr[4], mac.addr[5]);
}

static void settings_draw_toggle_button(int x, int y, int w, int h, bool on, const char* on_label, const char* off_label) {
    vga_fill_rounded_rect(x, y, w, h, 6, on ? C_ACCENT : C_TOOLBAR_BTN);
    vga_draw_rounded_rect(x, y, w, h, 6, on ? C_ACCENT : C_BORDER);
    vga_draw_string(x + 14, y + 10, on ? on_label : off_label, on ? C_TEXT_LIGHT : C_TEXT, 0xFFFFFFFF);
}


static int settings_ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool settings_contains_ci(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return true;
    if (!hay || !hay[0]) return false;
    for (int i = 0; hay[i]; ++i) {
        int j = 0;
        while (needle[j] && hay[i + j] &&
               settings_ascii_lower((unsigned char)hay[i + j]) == settings_ascii_lower((unsigned char)needle[j])) {
            ++j;
        }
        if (!needle[j]) return true;
    }
    return false;
}

static const char* settings_tab_keywords[] = {
    "system info hardware memory kernel build",
    "display resolution theme wallpaper font scale",
    "network cloud https json sync connectivity",
    "storage disk files partition ramfs ext2 fat32",
    "users account password profile login",
    "security lock password permissions access",
    "about version build license credits",
    "terminal shell console autoscroll input",
    "shortcuts hotkey keybind keyboard",
    "power restart shutdown sleep battery",
    "startup autostart boot launch login",
    "accessibility cursor contrast zoom font",
    "input mouse keyboard drag raw sensitivity",
    "files file manager search folder open"
};

static int settings_find_best_tab(const char* query, int current_tab) {
    if (!query || !query[0]) return current_tab;
    for (int i = 0; i < 14; ++i) {
        const char* name = settings_tabs[i];
        if (settings_contains_ci(name, query) || settings_contains_ci(settings_tab_keywords[i], query)) {
            return i;
        }
    }
    return current_tab;
}

int settings_scroll_max_for_window(int idx) {
    if (idx < 0 || idx >= window_count) return 0;
    window_t* w = &windows[idx];
    int viewport_h = w->h - C_TITLEBAR_H - 146;
    if (viewport_h < 120) viewport_h = 120;

    static const int est_heights[] = {
        260,  /* System */
        520,  /* Display */
        320,  /* Network */
        260,  /* Storage */
        220,  /* Users */
        240,  /* Security */
        210,  /* About */
        240,  /* Terminal */
        240,  /* Shortcuts */
        200,  /* Power */
        220,  /* Startup */
        360,  /* Accessibility */
        320,  /* Input */
        300,  /* Files */
    };
    int tab = w->settings_tab;
    if (tab < 0 || tab >= 14) tab = 1;
    int content_h = est_heights[tab];
    int max_scroll = content_h - viewport_h;
    if (max_scroll < 0) max_scroll = 0;
    if (max_scroll > 4096) max_scroll = 4096;
    return max_scroll;
}

static void settings_draw_search_box(int cx, int cy, int cw, const char* text, bool active) {
    int search_w = 260;
    if (search_w > cw - 180) search_w = cw - 180;
    if (search_w < 140) search_w = 140;
    int search_x = cx + cw - search_w - 18;
    int search_y = cy + 18;
    uint64_t fill = active ? rgb(255, 255, 255) : rgb(246, 249, 253);
    uint64_t edge = active ? C_ACCENT : rgb(189, 203, 224);
    vga_fill_rounded_rect(search_x, search_y, search_w, 28, 8, fill);
    vga_draw_rounded_rect(search_x, search_y, search_w, 28, 8, edge);
    vga_draw_string(search_x + 10, search_y + 8, text && text[0] ? text : "Search settings...", text && text[0] ? rgb(34, 54, 92) : rgb(122, 138, 160), 0xFFFFFFFF);
}

static void settings_draw_scrollbar(int idx, int cont_x, int cont_y, int cont_w, int clip_h) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    int max_scroll = settings_scroll_max_for_window(idx);
    if (w->settings_scroll < 0) w->settings_scroll = 0;
    if (w->settings_scroll > max_scroll) w->settings_scroll = max_scroll;
    if (max_scroll <= 0) return;

    int track_x = cont_x + cont_w - 14;
    int track_y = cont_y + 56;
    int track_h = clip_h;
    vga_fill_rounded_rect(track_x, track_y, 8, track_h, 4, rgb(226, 232, 240));
    vga_draw_rounded_rect(track_x, track_y, 8, track_h, 4, rgb(194, 206, 224));

    int thumb_h = (track_h * track_h) / (track_h + max_scroll);
    if (thumb_h < 24) thumb_h = 24;
    if (thumb_h > track_h) thumb_h = track_h;
    int thumb_y = track_y;
    if (max_scroll > 0) {
        thumb_y += (w->settings_scroll * (track_h - thumb_h)) / max_scroll;
    }
    vga_fill_rounded_rect(track_x + 1, thumb_y + 1, 6, thumb_h - 2, 3, C_ACCENT);
}

void draw_settings(int idx) {
    window_t* w = &windows[idx];
    if (w->settings_tab < 0 || w->settings_tab > 13) w->settings_tab = 1;

    bool dark = gui_is_dark_mode() ? true : false;
    uint64_t bg = dark ? rgb(18, 22, 30) : rgb(246, 249, 253);
    uint64_t head = dark ? rgb(30, 36, 48) : rgb(233, 241, 251);
    uint64_t sidebar = dark ? rgb(26, 31, 40) : rgb(236, 242, 249);
    uint64_t panel = dark ? rgb(22, 26, 34) : rgb(250, 252, 255);
    uint64_t edge = dark ? rgb(76, 88, 110) : rgb(209, 221, 238);
    uint64_t text = dark ? rgb(238, 243, 250) : rgb(34, 54, 92);
    uint64_t muted = dark ? rgb(170, 182, 198) : rgb(92, 110, 140);
    uint64_t line = dark ? rgb(56, 66, 82) : rgb(208, 220, 236);

    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w, ch = w->h - C_TITLEBAR_H;
    int sb_w = 160;
    int cont_x = cx + sb_w + 28;
    int cont_y = cy + 76;
    int cont_w = cw - sb_w - 52;
    int max_scroll = settings_scroll_max_for_window(idx);
    if (w->settings_scroll < 0) w->settings_scroll = 0;
    if (w->settings_scroll > max_scroll) w->settings_scroll = max_scroll;
    if (w->settings_search[0]) {
        int best = settings_find_best_tab(w->settings_search, w->settings_tab);
        if (best >= 0) w->settings_tab = best;
    }
    int iy = cont_y + 56 - w->settings_scroll;

    vga_fill_rect(cx, cy, cw, ch, bg);
    vga_fill_rounded_rect(cx + 12, cy + 10, cw - 24, 56, 12, head);
    vga_draw_rounded_rect(cx + 12, cy + 10, cw - 24, 56, 12, edge);
    vga_draw_string(cx + 24, cy + 22, gui_text("Settings", "設定"), text, 0xFFFFFFFF);
    vga_draw_string(cx + 24, cy + 40, gui_text("Controls, appearance, startup, and accessibility.", "操作・外観・起動・アクセシビリティの設定"), muted, 0xFFFFFFFF);
    settings_draw_search_box(cx + 12, cy + 10, cw - 24, w->settings_search, w->settings_search_active);

    vga_fill_rounded_rect(cx + 12, cy + 76, sb_w, ch - 88, 14, sidebar);
    vga_draw_rounded_rect(cx + 12, cy + 76, sb_w, ch - 88, 14, edge);

    const char* cats[14];
    for (int i = 0; i < 14; i++) cats[i] = settings_tab_label(i);
    for (int i = 0; i < 14; i++) {
        int ty = cy + 92 + i * SETTINGS_TAB_ROW_H;
        bool sel = (w->settings_tab == i);
        if (sel) vga_fill_rounded_rect(cx + 20, ty - 1, sb_w - 16, SETTINGS_TAB_HIT_H, 7, dark ? rgb(52, 74, 114) : rgb(214, 229, 252));
        vga_draw_string(cx + 28, ty + 5, cats[i], sel ? (dark ? rgb(160, 196, 255) : rgb(28, 64, 146)) : (dark ? rgb(226, 230, 238) : rgb(42, 58, 84)), 0xFFFFFFFF);
    }

    vga_fill_rounded_rect(cont_x, cont_y, cont_w, ch - 88, 14, panel);
    vga_draw_rounded_rect(cont_x, cont_y, cont_w, ch - 88, 14, edge);
    vga_draw_string(cont_x + 18, cont_y + 14, cats[w->settings_tab], text, 0xFFFFFFFF);
    vga_fill_rect(cont_x + 18, cont_y + 38, cont_w - 36, 1, line);

    gui_set_clip(cont_x + 18, cont_y + 56, cont_w - 36, ch - 146);
    settings_draw_scrollbar(idx, cont_x, cont_y, cont_w, ch - 146);

    switch (w->settings_tab) {
        case 0: {
            bios_system_info_t info;
            phys_mem_stats_t* pst = phys_memory_get_stats();
            uint64_t total_mem = phys_get_total_memory();
            uint64_t used_mem = kmemory_used();
            uint64_t free_mem = kmemory_free();
            char total_buf[32], used_buf[32], free_buf[32], stor_buf[32], build_buf[96];
            bios_get_system_info_copy(&info);
            format_size_bytes(total_mem, total_buf, 31);
            format_size_bytes(used_mem, used_buf, 31);
            format_size_bytes(free_mem, free_buf, 31);
            if (pst) {
                char tmp[32];
                int_to_str((int)pst->total_pages, tmp, 10);
                scopy(stor_buf, tmp, 31);
                scat(stor_buf, " pages RAM managed", 31);
            } else {
                scopy(stor_buf, "Physical stats unavailable", 31);
            }
            scopy(build_buf, COS_BOOT_MESSAGE, 95);
            vga_draw_string(cont_x + 18, iy, "Device Name:", text, 0xFFFFFFFF); scat(build_buf, info.vendor, 95);
            if (info.product[0]) { scat(build_buf, " / ", 95); scat(build_buf, info.product, 95); }
            vga_draw_string(cont_x + 140, iy, build_buf, text, 0xFFFFFFFF); iy += 30;
            vga_draw_string(cont_x + 18, iy, "Processor:", text, 0xFFFFFFFF); vga_draw_string(cont_x + 140, iy, info.cpu_vendor, text, 0xFFFFFFFF); iy += 30;
            vga_draw_string(cont_x + 18, iy, "Memory:", text, 0xFFFFFFFF); vga_draw_string(cont_x + 140, iy, total_buf, text, 0xFFFFFFFF); iy += 30;
            virt_mem_stats_t* vst = virt_memory_get_stats();
            if (vst) {
                char virt_buf[64];
                snprintf(virt_buf, 64, "Mapped: %d, Faults: %d", (int)vst->mapped_pages, (int)vst->page_faults);
                vga_draw_string(cont_x + 18, iy, "Virtual:", text, 0xFFFFFFFF);
                vga_draw_string(cont_x + 140, iy, virt_buf, text, 0xFFFFFFFF);
                iy += 30;
            }
            vga_draw_string(cont_x + 18, iy, "Used / Free:", text, 0xFFFFFFFF); vga_draw_string(cont_x + 140, iy, used_buf, text, 0xFFFFFFFF); vga_draw_string(cont_x + 260, iy, free_buf, text, 0xFFFFFFFF); iy += 30;
            vga_draw_string(cont_x + 18, iy, "Build:", text, 0xFFFFFFFF); vga_draw_string(cont_x + 140, iy, build_buf, text, 0xFFFFFFFF); iy += 30;
            vga_draw_string(cont_x + 18, iy, "Kernel:", text, 0xFFFFFFFF); vga_draw_string(cont_x + 140, iy, stor_buf, C_SUCCESS, 0xFFFFFFFF);
            break;
        }
        case 1: {
            char res_buf[64];
            int_to_str((int)SCREEN_W, res_buf, 10);
            size_t res_len = strlen(res_buf);
            res_buf[res_len] = 'x';
            int_to_str((int)SCREEN_H, res_buf + res_len + 1, 10);
            vga_draw_string(cont_x + 18, iy, gui_text("Resolution:", "解像度:"), text, 0xFFFFFFFF);
            vga_draw_string(cont_x + 140, iy, res_buf, text, 0xFFFFFFFF);
            iy += 30;
            vga_draw_string(cont_x + 18, iy, gui_text("Accent Theme:", "アクセントテーマ:"), text, 0xFFFFFFFF); iy += 24;
            const char* theme_names[] = {"Ocean", "Violet", "Forest", "Sunset"};
            /* 74pxボタン内へ確実に収まる日本語テーマ名。 */
            const char* theme_names_ja[] = {"海", "紫", "森", "夕焼"};
            for (int i = 0; i < 4; i++) {
                int bx = cont_x + 18 + i * 84;
                bool selected = (gui_get_theme_idx() == i + 1);
                vga_fill_rounded_rect(bx, iy, 74, 30, 4, selected ? C_ACCENT : C_TOOLBAR_BTN);
                vga_draw_rounded_rect(bx, iy, 74, 30, 4, selected ? C_ACCENT : C_BORDER);
                const char* theme_label = gui_text(theme_names[i], theme_names_ja[i]);
                int theme_w = gui_is_japanese() ? ((int)strlen(theme_label) / 3) * 16 : (int)strlen(theme_label) * FONT_W;
                vga_draw_string(bx + (74 - theme_w) / 2, iy + 8, theme_label, selected ? C_TEXT_LIGHT : C_TEXT, 0xFFFFFFFF);
            }
            iy += 48;
            vga_draw_string(cont_x + 18, iy, gui_text("Desktop Background:", "デスクトップ背景:"), text, 0xFFFFFFFF); iy += 24;
            int wp_count = gui_get_wallpaper_count();
            for (int i = 0; i < wp_count; i++) {
                int bx = cont_x + 18 + i * 84;
                bool selected = (w->wallpaper_idx == i);
                vga_fill_rounded_rect(bx, iy, 74, 42, 5, gui_get_wallpaper_color_idx(i));
                vga_draw_rounded_rect(bx, iy, 74, 42, 5, selected ? C_ACCENT : C_BORDER);
                if (selected) {
                    vga_fill_rect(bx + 3, iy + 3, 68, 36, rgb(255,255,255));
                    vga_fill_rect(bx + 5, iy + 5, 64, 32, gui_get_wallpaper_color_idx(i));
                }
                vga_draw_string(bx + 10, iy + 14, gui_get_wallpaper_name(i), C_TEXT_LIGHT, 0xFFFFFFFF);
            }
            iy += 58;
            vga_fill_rounded_rect(cont_x + 18, iy, 172, 30, 6, C_TOOLBAR_BTN);
            vga_draw_string(cont_x + 34, iy + 9, gui_text("Use Image Wallpaper", "画像壁紙を使う"), C_TEXT, 0xFFFFFFFF);
            vga_fill_rounded_rect(cont_x + 198, iy, 172, 30, 6, C_TOOLBAR_BTN);
            vga_draw_string(cont_x + 214, iy + 9, gui_text("Reload Image Wallpaper", "画像壁紙を再読み込み"), C_TEXT, 0xFFFFFFFF);
            iy += 46;
            vga_draw_string(cont_x + 18, iy, gui_text("Font Scale:", "文字サイズ:"), text, 0xFFFFFFFF); iy += 24;
            for (int i = 0; i < 4; i++) {
                int bx = cont_x + 18 + i * 56;
                bool selected = (gui_get_font_scale() == i + 1);
                vga_fill_rounded_rect(bx, iy, 48, 30, 4, selected ? C_ACCENT : C_TOOLBAR_BTN);
                vga_draw_rounded_rect(bx, iy, 48, 30, 4, selected ? C_ACCENT : C_BORDER);
                char scale_buf[8];
                int_to_str(i + 1, scale_buf, 10);
                scat(scale_buf, "x", 7);
                vga_draw_string(bx + 12, iy + 8, scale_buf, selected ? C_TEXT_LIGHT : C_TEXT, 0xFFFFFFFF);
            }
            break;
        }
        case 2: {
            net_iface_t* iface = net_get_default_iface();
            bool connected = net_is_connected();
            vga_draw_string(cont_x + 18, iy, connected ? "Status: Connected" : "Status: No network interface detected",
                             connected ? C_SUCCESS : muted, 0xFFFFFFFF);
            iy += 30;
            if (iface) {
                char mac_buf[32], ip_buf[32], mask_buf[32], gw_buf[32];
                settings_format_mac(iface->mac, mac_buf, sizeof(mac_buf));
                settings_format_ip(iface->ip, ip_buf, sizeof(ip_buf));
                settings_format_ip(iface->netmask, mask_buf, sizeof(mask_buf));
                settings_format_ip(iface->gateway, gw_buf, sizeof(gw_buf));

                vga_draw_string(cont_x + 18, iy, "Interface:", text, 0xFFFFFFFF);
                vga_draw_string(cont_x + 140, iy, iface->name, text, 0xFFFFFFFF); iy += 28;
                vga_draw_string(cont_x + 18, iy, "MAC Address:", text, 0xFFFFFFFF);
                vga_draw_string(cont_x + 140, iy, mac_buf, text, 0xFFFFFFFF); iy += 28;
                vga_draw_string(cont_x + 18, iy, "IPv4 Address:", text, 0xFFFFFFFF);
                vga_draw_string(cont_x + 140, iy, ip_buf, text, 0xFFFFFFFF); iy += 28;
                vga_draw_string(cont_x + 18, iy, "Subnet Mask:", text, 0xFFFFFFFF);
                vga_draw_string(cont_x + 140, iy, mask_buf, text, 0xFFFFFFFF); iy += 28;
                vga_draw_string(cont_x + 18, iy, "Gateway:", text, 0xFFFFFFFF);
                vga_draw_string(cont_x + 140, iy, gw_buf, text, 0xFFFFFFFF); iy += 40;
            } else {
                vga_draw_string(cont_x + 18, iy, "No NIC found. Add a virtio-net or e1000 device to QEMU", muted, 0xFFFFFFFF); iy += 24;
                vga_draw_string(cont_x + 18, iy, "(-nic user,model=e1000) to test networking.", muted, 0xFFFFFFFF); iy += 40;
            }
            vga_fill_rounded_rect(cont_x + 18, iy, 190, 32, 6, C_ACCENT);
            vga_draw_string(cont_x + 34, iy + 9, "Renew IP (DHCP)", C_TEXT_LIGHT, 0xFFFFFFFF);
            iy += 46;
            vga_draw_string(cont_x + 18, iy, "Transport: TCP/UDP over IPv4, DNS, DHCP client", muted, 0xFFFFFFFF);
            break;
        }
        case 3: {
            uint64_t total = storage_get_total_space();
            uint64_t used  = storage_get_used_space();
            uint64_t free_sp = storage_get_free_space();
            char stot[32], sused[32], sfree[32];
            format_size_bytes(total, stot, 31);
            format_size_bytes(used,  sused, 31);
            format_size_bytes(free_sp, sfree, 31);
            char cap_line[64];
            scopy(cap_line, "VFS Storage: ", 63); scat(cap_line, stot, 63);
            vga_draw_string(cont_x + 18, iy, cap_line, text, 0xFFFFFFFF); iy += 30;
            char used_line[64]; scopy(used_line, "Used: ", 63); scat(used_line, sused, 63);
            vga_draw_string(cont_x + 18, iy, used_line, text, 0xFFFFFFFF); iy += 20;
            int bar_total = (total > 0) ? total : 1;
            int bar_pct = (int)((uint64_t)(used * 100u) / (uint64_t)bar_total);
            if (bar_pct > 100) bar_pct = 100;
            vga_fill_rect(cont_x + 18, iy, cont_w - 36, 14, rgb(220,220,220));
            vga_fill_rect(cont_x + 18, iy, (cont_w - 36) * bar_pct / 100, 14, C_ACCENT); vga_draw_rect(cont_x + 18, iy, cont_w - 36, 14, C_BORDER); iy += 24;
            char free_line[64]; scopy(free_line, "Free: ", 63); scat(free_line, sfree, 63);
            vga_draw_string(cont_x + 18, iy, free_line, muted, 0xFFFFFFFF); iy += 30;
            char fc_line[40]; scopy(fc_line, "Files: ", 39); char fcn[16]; uitostr((uint64_t)fs_entry_count(), fcn); scat(fc_line, fcn, 39);
            vga_draw_string(cont_x + 18, iy, fc_line, muted, 0xFFFFFFFF);
            break;
        }
        case 4:
            vga_draw_string(cont_x + 18, iy, "Current User: Administrator", text, 0xFFFFFFFF); iy += 30;
            vga_draw_string(cont_x + 18, iy, "Role: System Owner", muted, 0xFFFFFFFF); iy += 40;
            vga_fill_rounded_rect(cont_x + 18, iy, 150, 32, 6, C_ACCENT); vga_draw_string(cont_x + 45, iy + 10, "Change Password", C_TEXT_LIGHT, 0xFFFFFFFF);
            break;
        case 5:
            vga_draw_string(cont_x + 18, iy, "Security Scanner: Ready", text, 0xFFFFFFFF); iy += 30;
            vga_draw_string(cont_x + 18, iy, "Virus definitions: local only", muted, 0xFFFFFFFF); iy += 30;
            vga_fill_rounded_rect(cont_x + 18, iy + 24, 180, 32, 6, C_ACCENT); vga_draw_string(cont_x + 47, iy + 34, "Full Scan", C_TEXT_LIGHT, 0xFFFFFFFF);
            break;
        case 6:
            vga_draw_string(cont_x + 18, iy, "C-OS 4.0.8 alpha", C_ACCENT, 0xFFFFFFFF); iy += 30;
            vga_draw_string(cont_x + 18, iy, "Built for experimentation and UI work.", text, 0xFFFFFFFF); iy += 30;
            vga_draw_string(cont_x + 18, iy, "All UI elements are self-contained.", muted, 0xFFFFFFFF);
            break;
        case 7:
            vga_draw_string(cont_x + 18, iy, "Terminal Auto-Scroll", text, 0xFFFFFFFF); iy += 30;
            settings_draw_toggle_button(cont_x + 18, iy, 180, 32, gui_get_terminal_autoscroll(), "Disable", "Enable");
            iy += 48;
            vga_fill_rounded_rect(cont_x + 18, iy, 150, 32, 6, C_TOOLBAR_BTN);
            vga_draw_string(cont_x + 40, iy + 10, "Open Terminal", C_TEXT, 0xFFFFFFFF);
            break;
        case 8:
            vga_draw_string(cont_x + 18, iy, "Desktop shortcuts, clipboard and window actions are now available from the context menu.", text, 0xFFFFFFFF); iy += 30;
            vga_draw_string(cont_x + 18, iy, "Tip: right click the desktop for quick actions.", muted, 0xFFFFFFFF);
            break;
        case 9:
            vga_draw_string(cont_x + 18, iy, "Power actions", text, 0xFFFFFFFF); iy += 30;
            vga_fill_rounded_rect(cont_x + 18, iy, 130, 32, 6, C_TOOLBAR_BTN); vga_draw_string(cont_x + 56, iy + 10, "Sleep", C_TEXT, 0xFFFFFFFF);
            vga_fill_rounded_rect(cont_x + 160, iy, 130, 32, 6, C_TOOLBAR_BTN); vga_draw_string(cont_x + 190, iy + 10, "Restart", C_TEXT, 0xFFFFFFFF);
            vga_fill_rounded_rect(cont_x + 302, iy, 130, 32, 6, rgb(216, 84, 84)); vga_draw_string(cont_x + 330, iy + 10, "Shutdown", C_TEXT_LIGHT, 0xFFFFFFFF);
            break;
        case 10:
            vga_draw_string(cont_x + 18, iy, "Startup shortcuts", text, 0xFFFFFFFF); iy += 30;
            settings_draw_toggle_button(cont_x + 18, iy, 210, 32, gui_get_autostart_terminal(), "Terminal autostart: On", "Terminal autostart: Off"); iy += 46;
            settings_draw_toggle_button(cont_x + 18, iy, 230, 32, gui_get_autostart_file_manager(), "File Manager autostart: On", "File Manager autostart: Off"); iy += 46;
            settings_draw_toggle_button(cont_x + 18, iy, 220, 32, gui_get_autostart_browser(), "Browser autostart: On", "Browser autostart: Off");
            break;
        case 11:
            vga_draw_string(cont_x + 18, iy, "Appearance Mode", text, 0xFFFFFFFF); iy += 24;
            vga_fill_rounded_rect(cont_x + 18, iy, 110, 32, 6, gui_is_dark_mode() ? C_ACCENT : C_TOOLBAR_BTN);
            vga_draw_string(cont_x + 46, iy + 10, "Dark", gui_is_dark_mode() ? C_TEXT_LIGHT : C_TEXT, 0xFFFFFFFF);
            vga_fill_rounded_rect(cont_x + 138, iy, 110, 32, 6, gui_is_dark_mode() ? C_TOOLBAR_BTN : C_ACCENT);
            vga_draw_string(cont_x + 168, iy + 10, "Light", gui_is_dark_mode() ? C_TEXT : C_TEXT_LIGHT, 0xFFFFFFFF);
            iy += 48;
            vga_draw_string(cont_x + 18, iy, "Window Animations", text, 0xFFFFFFFF); iy += 24;
            settings_draw_toggle_button(cont_x + 18, iy, 160, 32, gui_get_window_animations(), "On", "Off"); iy += 48;
            vga_draw_string(cont_x + 18, iy, "Desktop Notifications", text, 0xFFFFFFFF); iy += 24;
            settings_draw_toggle_button(cont_x + 18, iy, 160, 32, gui_get_notifications_enabled(), "On", "Off"); iy += 48;
            vga_draw_string(cont_x + 18, iy, "FPS Counter", text, 0xFFFFFFFF); iy += 24;
            settings_draw_toggle_button(cont_x + 18, iy, 160, 32, gui_get_fps_overlay(), "On", "Off");
            break;
        case 12:
            vga_draw_string(cont_x + 18, iy, gui_text("Mouse Sensitivity", "マウス感度"), text, 0xFFFFFFFF); iy += 24;
            for (int i = 0; i < 4; i++) {
                int bx = cont_x + 18 + i * 74;
                bool sel = (gui_get_mouse_sensitivity() == i + 1);
                vga_fill_rounded_rect(bx, iy, 66, 32, 6, sel ? C_ACCENT : C_TOOLBAR_BTN);
                vga_draw_string(bx + 26, iy + 10,
                                (i == 0) ? gui_text("Low", "低") :
                                (i == 1) ? gui_text("Med", "中") :
                                (i == 2) ? gui_text("High", "高") : gui_text("Max", "最高"),
                                sel ? C_TEXT_LIGHT : C_TEXT, 0xFFFFFFFF);
            }
            iy += 52;
            vga_draw_string(cont_x + 18, iy, gui_text("Raw Mouse Input", "マウス生入力"), text, 0xFFFFFFFF); iy += 24;
            settings_draw_toggle_button(cont_x + 18, iy, 200, 32, gui_get_mouse_raw_input(),
                                        gui_text("Raw Input: On", "生入力: オン"),
                                        gui_text("Raw Input: Reduced", "生入力: 軽減"));
            iy += 48;
            vga_draw_string(cont_x + 18, iy, gui_text("Drag Threshold", "ドラッグ開始距離"), text, 0xFFFFFFFF); iy += 24;
            for (int i = 1; i <= 10; i++) {
                int bx = cont_x + 18 + ((i - 1) % 5) * 66;
                int by = iy + ((i - 1) / 5) * 40;
                bool sel = (gui_get_mouse_drag_threshold() == i);
                vga_fill_rounded_rect(bx, by, 58, 30, 6, sel ? C_ACCENT : C_TOOLBAR_BTN);
                char buf[8];
                int_to_str(i, buf, 10);
                scat(buf, "px", 7);
                vga_draw_string(bx + 12, by + 9, buf, sel ? C_TEXT_LIGHT : C_TEXT, 0xFFFFFFFF);
            }
            iy += 96;
            vga_draw_string(cont_x + 18, iy, gui_text("Multi-cursor Mode", "マルチカーソルモード"), text, 0xFFFFFFFF); iy += 24;
            settings_draw_toggle_button(cont_x + 18, iy, 260, 32, gui_get_multi_cursor_enabled(),
                                        gui_text("Multi-cursor: On", "マルチカーソル: オン"),
                                        gui_text("Multi-cursor: Off", "マルチカーソル: オフ"));
            iy += 44;
            vga_draw_string(cont_x + 18, iy,
                            gui_text("Arrows move; Alt=left click; Fn/F12=right click; Ctrl+P+O toggles",
                                     "矢印=移動  Alt=左クリック  Fn/F12=右クリック  Ctrl+P+O=切替"),
                            muted, 0xFFFFFFFF);
            break;
        case 13: {
            vga_draw_string(cont_x + 18, iy, gui_text("New File Manager windows use these defaults:", "新規に開くファイルマネージャーの既定設定:"), muted, 0xFFFFFFFF); iy += 34;

            vga_draw_string(cont_x + 18, iy, gui_text("Show Hidden Files", "隠しファイルを表示"), text, 0xFFFFFFFF); iy += 24;
            settings_draw_toggle_button(cont_x + 18, iy, 120, 32, efm_get_default_show_hidden(), gui_text("On", "オン"), gui_text("Off", "オフ"));
            iy += 48;

            vga_draw_string(cont_x + 18, iy, gui_text("Confirm Before Delete", "削除前に確認する"), text, 0xFFFFFFFF); iy += 24;
            settings_draw_toggle_button(cont_x + 18, iy, 120, 32, efm_get_confirm_delete(), gui_text("On", "オン"), gui_text("Off", "オフ"));
            iy += 48;

            vga_draw_string(cont_x + 18, iy, gui_text("Default View", "既定の表示形式"), text, 0xFFFFFFFF); iy += 24;
            {
                const char* view_names[] = {"List", "Icons", "Details", "Thumbnails"};
                int default_view = efm_get_default_view_mode();
                for (int i = 0; i < 4; i++) {
                    int bx = cont_x + 18 + i * 84;
                    bool sel = (default_view == i);
                    vga_fill_rounded_rect(bx, iy, 76, 30, 6, sel ? C_ACCENT : C_TOOLBAR_BTN);
                    vga_draw_rounded_rect(bx, iy, 76, 30, 6, sel ? C_ACCENT : C_BORDER);
                    vga_draw_string(bx + 10, iy + 9, view_names[i], sel ? C_TEXT_LIGHT : C_TEXT, 0xFFFFFFFF);
                }
            }
            iy += 46;
            vga_draw_string(cont_x + 18, iy, gui_text("These do not affect already-open windows.", "既に開いているウィンドウには影響しません。"), muted, 0xFFFFFFFF);
            break;
        }
    }
}

void handle_settings_click(int idx, int mx, int my2) {
    window_t* w = &windows[idx];
    if (w->settings_tab < 0 || w->settings_tab > 13) w->settings_tab = 1;
    int cy = w->y + C_TITLEBAR_H;
    int sb_w = 160;
    int cont_x = w->x + sb_w + 28;
    int cont_y = cy + 76;
    int cw = w->w;
    int ch = w->h - C_TITLEBAR_H;
    int search_w = 260;
    if (search_w > cw - 180) search_w = cw - 180;
    if (search_w < 140) search_w = 140;
    int search_x = w->x + cw - search_w - 18;
    int search_y = cy + 18;
    if (mx >= search_x && mx < search_x + search_w && my2 >= search_y && my2 < search_y + 28) {
        w->settings_search_active = TRUE;
        gui_request_redraw();
        return;
    }

    for (int i = 0; settings_tabs[i]; i++) {
        int ty = cy + 92 + i * SETTINGS_TAB_ROW_H;
        if (mx >= w->x + 12 && mx < w->x + 12 + sb_w && my2 >= ty - 1 && my2 < ty - 1 + SETTINGS_TAB_HIT_H) {
            w->settings_tab = i;
            w->settings_search_active = FALSE;
            w->settings_scroll = 0;
            gui_request_redraw();
            return;
        }
    }

    int max_scroll = settings_scroll_max_for_window(idx);
    int scrollbar_x = w->x + cw - 14;
    int scrollbar_y = cy + 56;
    int scrollbar_h = ch - 146;
    if (max_scroll > 0 && mx >= scrollbar_x && mx < scrollbar_x + 8 && my2 >= scrollbar_y && my2 < scrollbar_y + scrollbar_h) {
        int thumb_h = (scrollbar_h * scrollbar_h) / (scrollbar_h + max_scroll);
        if (thumb_h < 24) thumb_h = 24;
        if (thumb_h > scrollbar_h) thumb_h = scrollbar_h;
        int rel = my2 - scrollbar_y - thumb_h / 2;
        int max_pos = scrollbar_h - thumb_h;
        if (max_pos < 1) max_pos = 1;
        if (rel < 0) rel = 0;
        if (rel > max_pos) rel = max_pos;
        w->settings_scroll = (rel * max_scroll) / max_pos;
        gui_request_redraw();
        return;
    }

    switch (w->settings_tab) {
        case 1: {
            int theme_y = cy + 185 - w->settings_scroll;
            for (int i = 0; i < 4; i++) {
                int tx = cont_x + 18 + i * 84;
                if (mx >= tx && mx < tx + 74 && my2 >= theme_y && my2 < theme_y + 30) {
                    gui_set_theme_idx(i + 1);
                    w->settings_theme_idx = gui_get_theme_idx();
                    gui_notify("Accent theme changed", 1600);
                    return;
                }
            }
            int bg_y = theme_y + 83;
            int wp_count = gui_get_wallpaper_count();
            for (int i = 0; i < wp_count; i++) {
                int bx = cont_x + 18 + i * 84;
                if (mx >= bx && mx < bx + 74 && my2 >= bg_y && my2 < bg_y + 42) {
                    w->wallpaper_idx = i;
                    gui_set_wallpaper(i);
                    gui_notify("Desktop background updated", 1800);
                    return;
                }
            }
            int btn_y = bg_y + 58;
            if (mx >= cont_x + 18 && mx < cont_x + 190 && my2 >= btn_y && my2 < btn_y + 30) {
                gui_set_wallpaper(gui_get_wallpaper_count() - 1);
                gui_notify("Image wallpaper enabled", 1500);
                return;
            }
            if (mx >= cont_x + 198 && mx < cont_x + 370 && my2 >= btn_y && my2 < btn_y + 30) {
                gui_set_wallpaper(gui_get_wallpaper_count() - 1);
                gui_notify("Image wallpaper reloaded", 1500);
                return;
            }
            int fs_y = bg_y + 106;
            for (int i = 0; i < 4; i++) {
                int bx = cont_x + 18 + i * 56;
                if (mx >= bx && mx < bx + 48 && my2 >= fs_y && my2 < fs_y + 30) {
                    gui_set_font_scale(i + 1);
                    gui_notify("Font scale changed", 1500);
                    return;
                }
            }
            int ff_y = fs_y + 78;
            for (int i = 0; i < 6; i++) {
                int col = i % 3;
                int row = i / 3;
                int bx = cont_x + 18 + col * 154;
                int by = ff_y + row * 42;
                if (mx >= bx && mx < bx + 142 && my2 >= by && my2 < by + 32) {
                    gui_set_font_family(i);
                    gui_notify("Font family changed", 1500);
                    return;
                }
            }
            break;
        }
        case 2: {
            net_iface_t* iface = net_get_default_iface();
            int btn_y_rel = iface ? 314 : 226;
            int btn_y = cy + btn_y_rel - w->settings_scroll;
            if (mx >= cont_x + 18 && mx < cont_x + 208 && my2 >= btn_y && my2 < btn_y + 32) {
                if (iface) {
                    dhcp_discover();
                    gui_notify("Requesting new IP via DHCP...", 1800);
                } else {
                    gui_notify("No network interface available", 1800);
                }
                return;
            }
            break;
        }
        case 4: {
            int iy = cy + 130 - w->settings_scroll;
            if (mx >= cont_x + 18 && mx < cont_x + 168 && my2 >= iy && my2 < iy + 32) {
                if (password_change_screen_show()) gui_notify("Password updated", 2000);
                else gui_notify("Password change cancelled", 2000);
                return;
            }
            break;
        }
        case 5: {
            int iy = cy + 170 - w->settings_scroll;
            if (mx >= cont_x + 18 && mx < cont_x + 198 && my2 >= iy && my2 < iy + 32) {
                gui_notify("Full scan started", 2000);
                return;
            }
            break;
        }
        case 7: {
            int auto_y = cy + 86 - w->settings_scroll;
            int open_y = cy + 134 - w->settings_scroll;
            if (mx >= cont_x + 18 && mx < cont_x + 198 && my2 >= auto_y && my2 < auto_y + 32) {
                gui_set_terminal_autoscroll(!gui_get_terminal_autoscroll());
                gui_notify(gui_get_terminal_autoscroll() ? "Terminal auto-scroll enabled" : "Terminal auto-scroll disabled", 1700);
                return;
            }
            if (mx >= cont_x + 18 && mx < cont_x + 168 && my2 >= open_y && my2 < open_y + 32) {
                gui_open_window(WIN_TERMINAL, gui_text("Terminal", "ターミナル"), 120, 120, 840, 560);
                return;
            }
            break;
        }
        case 9: {
            int py = cy + 86 - w->settings_scroll;
            if (mx >= cont_x + 18 && mx < cont_x + 148 && my2 >= py && my2 < py + 32) {
                gui_notify("Sleep requested", 1500);
                return;
            }
            if (mx >= cont_x + 160 && mx < cont_x + 290 && my2 >= py && my2 < py + 32) {
                gui_notify("Restart requested", 1800);
                return;
            }
            if (mx >= cont_x + 302 && mx < cont_x + 432 && my2 >= py && my2 < py + 32) {
                gui_notify("Shutdown requested", 1800);
                return;
            }
            break;
        }
        case 10: {
            int sy = cy + 86 - w->settings_scroll;
            if (mx >= cont_x + 18 && mx < cont_x + 228 && my2 >= sy && my2 < sy + 32) {
                gui_set_autostart_terminal(!gui_get_autostart_terminal());
                gui_notify(gui_get_autostart_terminal() ? "Terminal autostart enabled" : "Terminal autostart disabled", 1500);
                return;
            }
            if (mx >= cont_x + 18 && mx < cont_x + 248 && my2 >= sy + 46 && my2 < sy + 78) {
                gui_set_autostart_file_manager(!gui_get_autostart_file_manager());
                gui_notify(gui_get_autostart_file_manager() ? "File Manager autostart enabled" : "File Manager autostart disabled", 1500);
                return;
            }
            if (mx >= cont_x + 18 && mx < cont_x + 238 && my2 >= sy + 92 && my2 < sy + 124) {
                gui_set_autostart_browser(!gui_get_autostart_browser());
                gui_notify(gui_get_autostart_browser() ? "Browser autostart enabled" : "Browser autostart disabled", 1500);
                return;
            }
            break;
        }
        case 11: {
            int sy = cy + 86 - w->settings_scroll;
            if (mx >= cont_x + 18 && mx < cont_x + 128 && my2 >= sy && my2 < sy + 32) {
                gui_set_dark_mode(true);
                gui_notify("Dark mode enabled", 1500);
                return;
            }
            if (mx >= cont_x + 138 && mx < cont_x + 248 && my2 >= sy && my2 < sy + 32) {
                gui_set_dark_mode(false);
                gui_notify("Light mode enabled", 1500);
                return;
            }
            if (mx >= cont_x + 18 && mx < cont_x + 178 && my2 >= sy + 72 && my2 < sy + 104) {
                gui_set_window_animations(!gui_get_window_animations());
                gui_notify(gui_get_window_animations() ? "Animations enabled" : "Animations disabled", 1500);
                return;
            }
            if (mx >= cont_x + 18 && mx < cont_x + 178 && my2 >= sy + 144 && my2 < sy + 176) {
                gui_set_notifications_enabled(!gui_get_notifications_enabled());
                gui_notify(gui_get_notifications_enabled() ? "Notifications enabled" : "Notifications disabled", 1500);
                return;
            }
            if (mx >= cont_x + 18 && mx < cont_x + 178 && my2 >= sy + 216 && my2 < sy + 248) {
                gui_set_fps_overlay(!gui_get_fps_overlay());
                gui_notify(gui_get_fps_overlay() ? "FPS counter enabled" : "FPS counter disabled", 1500);
                return;
            }
            break;
        }
        case 12: {
            /* Keep hit regions in lockstep with draw_settings(): the first
             * sensitivity control is cont_y+80, then the subsequent rows
             * are spaced by the exact increments used by the renderer. */
            int sensitivity_y = cont_y + 80 - w->settings_scroll;
            for (int i = 0; i < 4; i++) {
                int bx = cont_x + 18 + i * 74;
                if (mx >= bx && mx < bx + 66 && my2 >= sensitivity_y && my2 < sensitivity_y + 32) {
                    gui_set_mouse_sensitivity(i + 1);
                    gui_notify("Mouse sensitivity updated", 1500);
                    return;
                }
            }
            int raw_input_y = sensitivity_y + 76;
            if (mx >= cont_x + 18 && mx < cont_x + 218 && my2 >= raw_input_y && my2 < raw_input_y + 32) {
                gui_set_mouse_raw_input(!gui_get_mouse_raw_input());
                gui_notify(gui_get_mouse_raw_input() ? "Raw mouse input enabled" : "Raw mouse input reduced", 1500);
                return;
            }
            int drag_y = sensitivity_y + 148;
            for (int i = 1; i <= 10; ++i) {
                int bx = cont_x + 18 + ((i - 1) % 5) * 66;
                int by = drag_y + ((i - 1) / 5) * 40;
                if (mx >= bx && mx < bx + 58 && my2 >= by && my2 < by + 30) {
                    gui_set_mouse_drag_threshold(i);
                    gui_notify("Drag threshold updated", 1500);
                    return;
                }
            }
            int multi_cursor_y = sensitivity_y + 268;
            if (mx >= cont_x + 18 && mx < cont_x + 278 && my2 >= multi_cursor_y && my2 < multi_cursor_y + 32) {
                gui_toggle_multi_cursor_mode();
                gui_notify(gui_get_multi_cursor_enabled()
                               ? gui_text("Multi-cursor mode enabled", "マルチカーソルモードを有効にしました")
                               : gui_text("Multi-cursor mode disabled", "マルチカーソルモードを無効にしました"),
                           1800);
                return;
            }
            break;
        }
        case 13: {
            int base = cy + 190 - w->settings_scroll;
            /* Show Hidden Files トグル */
            if (mx >= cont_x + 18 && mx < cont_x + 138 && my2 >= base && my2 < base + 32) {
                efm_set_default_show_hidden(!efm_get_default_show_hidden());
                gui_notify(efm_get_default_show_hidden() ? "Hidden files will be shown by default" : "Hidden files will be hidden by default", 1800);
                return;
            }
            /* Confirm Before Delete トグル */
            int confirm_y = cy + 262 - w->settings_scroll;
            if (mx >= cont_x + 18 && mx < cont_x + 138 && my2 >= confirm_y && my2 < confirm_y + 32) {
                efm_set_confirm_delete(!efm_get_confirm_delete());
                gui_notify(efm_get_confirm_delete() ? "Delete confirmation enabled" : "Delete confirmation disabled", 1800);
                return;
            }
            /* Default View ボタン群 */
            int view_y = cy + 334 - w->settings_scroll;
            for (int i = 0; i < 4; i++) {
                int bx = cont_x + 18 + i * 84;
                if (mx >= bx && mx < bx + 76 && my2 >= view_y && my2 < view_y + 30) {
                    efm_set_default_view_mode(i);
                    gui_notify("Default file manager view updated", 1500);
                    return;
                }
            }
            break;
        }
    }
}


void handle_settings_key(int idx, char ascii, int key, bool ctrl) {
    window_t* w = &windows[idx];
    if (w->settings_tab < 0 || w->settings_tab > 13) w->settings_tab = 1;

    if (ctrl && (ascii == 'f' || ascii == 'F')) {
        w->settings_search_active = TRUE;
        gui_request_redraw();
        return;
    }

    if (key == KEY_ESC) {
        if (w->settings_search_active) {
            w->settings_search_active = FALSE;
            gui_request_redraw();
            return;
        }
        if (w->settings_search[0]) {
            w->settings_search[0] = 0;
            gui_request_redraw();
            return;
        }
    }

    if (key == KEY_ENTER) {
        if (w->settings_search[0]) {
            int best = settings_find_best_tab(w->settings_search, w->settings_tab);
            if (best >= 0) w->settings_tab = best;
        }
        w->settings_search_active = FALSE;
        gui_request_redraw();
        return;
    }

    if (key == KEY_BACKSPACE || ascii == '\b') {
        if (w->settings_search_active && w->settings_search[0]) {
            int len = (int)strlen(w->settings_search);
            if (len > 0) w->settings_search[len - 1] = 0;
            if (w->settings_search[0]) {
                int best = settings_find_best_tab(w->settings_search, w->settings_tab);
                if (best >= 0) w->settings_tab = best;
            }
            w->settings_scroll = 0;
            gui_request_redraw();
            return;
        }
        if (key == KEY_UP) {
            int max_scroll = settings_scroll_max_for_window(idx);
            if (w->settings_scroll < max_scroll) w->settings_scroll += 24;
            if (w->settings_scroll > max_scroll) w->settings_scroll = max_scroll;
            gui_request_redraw();
            return;
        }
    }

    if (w->settings_search_active && ascii >= 32 && ascii < 127) {
        int len = (int)strlen(w->settings_search);
        if (len < 63) {
            w->settings_search[len] = ascii;
            w->settings_search[len + 1] = 0;
            int best = settings_find_best_tab(w->settings_search, w->settings_tab);
            if (best >= 0) w->settings_tab = best;
            w->settings_scroll = 0;
            gui_request_redraw();
        }
        return;
    }

    int max_scroll = settings_scroll_max_for_window(idx);
    if (key == KEY_PAGEUP) {
        w->settings_scroll += 96;
    } else if (key == KEY_PAGEDOWN) {
        if (w->settings_scroll >= 96) w->settings_scroll -= 96; else w->settings_scroll = 0;
    } else if (key == KEY_HOME) {
        w->settings_scroll = 0;
    } else if (key == KEY_END) {
        w->settings_scroll = max_scroll;
    } else if (key == KEY_UP) {
        if (w->settings_scroll >= 24) w->settings_scroll -= 24; else w->settings_scroll = 0;
    } else if (key == KEY_DOWN) {
        w->settings_scroll += 24;
    } else if (ctrl && ascii == 'l') {
        w->settings_search[0] = 0;
        w->settings_search_active = TRUE;
        gui_request_redraw();
        return;
    } else {
        return;
    }

    if (w->settings_scroll < 0) w->settings_scroll = 0;
    if (w->settings_scroll > max_scroll) w->settings_scroll = max_scroll;
    gui_request_redraw();
}



