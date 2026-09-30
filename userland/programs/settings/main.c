/* settings.c-os - the Settings app, as a real ring-3 program.
 *
 * A redraw of the old kernel-space Settings panel (src/gui/apps/settings/
 * gui_apps_settings.c, 1183 lines across 15 tabs) with a sidebar instead of
 * a cramped top tab row, cos_ui's anti-aliased drawing and the Inter font at
 * the window's own resolution, and real system data via the new
 * SYS_SETTINGS_GET/SET syscalls (see cos.h) rather than direct kernel calls
 * a ring-3 process cannot make. System/Display/Storage/Network/About read
 * genuine live numbers; the remaining categories (Users, Security, Terminal,
 * Shortcuts, Power, Startup, Accessibility, Input, Files) carry the same
 * scope the original gave them - several were already just a status line
 * and a placeholder button there, which this keeps rather than invents
 * functionality the OS does not actually have.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "cos.h"
#include "cos_ui.h"

typedef enum {
    TAB_SYSTEM, TAB_DISPLAY, TAB_NETWORK, TAB_STORAGE, TAB_USERS, TAB_SECURITY,
    TAB_ABOUT, TAB_TERMINAL, TAB_SHORTCUTS, TAB_POWER, TAB_STARTUP,
    TAB_ACCESSIBILITY, TAB_INPUT, TAB_FILES, TAB_GUI, TAB_COUNT
} tab_t;
static const char *const TAB_NAMES[TAB_COUNT] = {
    "System", "Display", "Network", "Storage", "Users", "Security",
    "About", "Terminal", "Shortcuts", "Power", "Startup",
    "Accessibility", "Input", "Files", "GUI",
};

#define SIDEBAR_W 190

static void fmt_bytes(uint64_t v, char *out, size_t cap) {
    const char *unit = "B"; double d = (double)v;
    if (d >= 1073741824.0) { d /= 1073741824.0; unit = "GB"; }
    else if (d >= 1048576.0) { d /= 1048576.0; unit = "MB"; }
    else if (d >= 1024.0) { d /= 1024.0; unit = "KB"; }
    snprintf(out, cap, "%.1f %s", d, unit);
}

typedef struct {
    cui_canvas *c;
    cui_font *lbl, *val, *hdr, *small;
    uint32_t text, muted, accent, panel, border, btn, btn_sel;
    int x, y, w;
} row_ctx_t;

static int row(row_ctx_t *r, const char *label, const char *value, int y) {
    cui_text(r->c, r->lbl, r->x, y, label, r->muted);
    cui_text(r->c, r->val, r->x + 190, y, value, r->text);
    return y + 30;
}
static int section(row_ctx_t *r, const char *title, int y) {
    cui_text(r->c, r->hdr, r->x, y, title, r->text);
    cui_fill(r->c, r->x, y + 26, r->w - r->x - 20, 1, r->border);
    return y + 40;
}
static bool button(row_ctx_t *r, int x, int y, int w, int h, const char *label, bool primary, int mx, int my, bool click, bool *out_clicked) {
    bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
    cui_round_rect(r->c, x, y, w, h, 7, primary ? r->accent : r->btn);
    cui_text_center(r->c, r->lbl, x, y, w, h, label, primary ? 0xFFFFFF : r->text);
    if (hover && click && out_clicked) *out_clicked = true;
    (void)hover;
    return hover;
}
static bool chip(row_ctx_t *r, int x, int y, int w, int h, const char *label, bool selected, int mx, int my, bool click) {
    bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
    cui_round_rect(r->c, x, y, w, h, 6, selected ? r->accent : r->btn);
    cui_text_center(r->c, r->lbl, x, y, w, h, label, selected ? 0xFFFFFF : r->text);
    return hover && click;
}

int main(void) {
    int32_t sw = 980, sh = 680;
    cos_win_info_t wi;
    int64_t h = cos_win2_create("Settings", sw, sh, &wi);
    if (h <= 0) return 1;
    cui_canvas c;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);

    cui_font *f_side = cui_font_default(15);
    cui_font *f_hdr  = cui_font_bold(20);
    cui_font *f_lbl  = cui_font_default(14);
    cui_font *f_val  = cui_font_default(14);
    cui_font *f_small= cui_font_default(12);

    const uint32_t BG = 0x14171F, SIDEBAR = 0x1A1E28, PANEL = 0x1E222D;
    const uint32_t BORDER = 0x2C3140, TEXT = 0xE8ECF5, MUTED = 0x8A93A8;
    const uint32_t ACCENT = 0x4D9FFF, BTN = 0x2A3040, SEL = 0x232A3D;

    cos_settings_t st;
    cos_settings_get(&st);
    tab_t tab = TAB_SYSTEM;
    int mx = -1, my = -1;
    bool click = false;
    bool dirty = true;

    for (;;) {
        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, 300);
        click = false;
        if (r > 0) {
            if (ev.type == COS_EV_CLOSE) break;
            if (ev.type == COS_EV_MOUSE_MOVE) { mx = ev.x; my = ev.y; dirty = true; }
            if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) { mx = ev.x; my = ev.y; click = true; dirty = true; }
        } else {
            cos_settings_get(&st);   /* live numbers (memory/storage/network) stay current even with no input */
            dirty = true;
        }
        if (!dirty) continue;
        dirty = false;

        cui_fill(&c, 0, 0, c.w, c.h, BG);
        cui_fill(&c, 0, 0, SIDEBAR_W, c.h, SIDEBAR);
        cui_fill(&c, SIDEBAR_W - 1, 0, 1, c.h, BORDER);

        cui_text(&c, f_hdr, 20, 20, "Settings", TEXT);
        int sy = 66;
        for (int i = 0; i < TAB_COUNT; ++i) {
            bool sel = (tab == (tab_t)i);
            bool hov = mx >= 8 && mx < SIDEBAR_W - 8 && my >= sy && my < sy + 34;
            if (sel || hov) cui_round_rect(&c, 8, sy, SIDEBAR_W - 16, 34, 7, sel ? SEL : 0x20242F);
            if (sel) cui_fill(&c, 8, sy + 6, 3, 22, ACCENT);
            cui_text(&c, f_side, 24, sy + 8, TAB_NAMES[i], sel ? TEXT : MUTED);
            if (hov && click) tab = (tab_t)i;
            sy += 38;
        }

        row_ctx_t rc = { &c, f_lbl, f_val, f_hdr, f_small, TEXT, MUTED, ACCENT, PANEL, BORDER, BTN, SEL, SIDEBAR_W + 30, 20, c.w };
        int y = 30;
        cui_text(&c, f_hdr, rc.x, y, TAB_NAMES[tab], TEXT); y += 44;

        char buf1[32], buf2[32], buf3[32];
        switch (tab) {
        case TAB_SYSTEM: {
            y = section(&rc, "Hardware", y);
            y = row(&rc, "Processor", st.cpu_vendor, y);
            fmt_bytes(st.mem_total, buf1, sizeof buf1);
            y = row(&rc, "Memory", buf1, y);
            fmt_bytes(st.mem_used, buf2, sizeof buf2); fmt_bytes(st.mem_free, buf3, sizeof buf3);
            char memline[64]; snprintf(memline, sizeof memline, "%s used / %s free", buf2, buf3);
            y = row(&rc, "Memory usage", memline, y);
            y += 10;
            y = section(&rc, "System", y);
            y = row(&rc, "OS Version", st.os_version, y);
            y = row(&rc, "Build", st.build_info, y);
            break;
        }
        case TAB_DISPLAY: {
            char resline[32]; snprintf(resline, sizeof resline, "%d x %d", st.screen_w, st.screen_h);
            y = section(&rc, "Screen", y);
            y = row(&rc, "Resolution", resline, y);
            y += 14;
            y = section(&rc, "Accent Theme", y);
            static const char *const themes[4] = { "Ocean", "Violet", "Forest", "Sunset" };
            for (int i = 0; i < 4; ++i) {
                bool sel = st.theme_idx == i + 1;
                if (chip(&rc, rc.x + i * 90, y, 80, 32, themes[i], sel, mx, my, click)) { cos_settings_set(COS_SET_THEME, i + 1); cos_settings_get(&st); }
            }
            y += 48;
            y = section(&rc, "Font Scale", y);
            for (int i = 0; i < 4; ++i) {
                bool sel = st.font_scale == i + 1;
                char lab[4]; snprintf(lab, sizeof lab, "%dx", i + 1);
                if (chip(&rc, rc.x + i * 60, y, 50, 32, lab, sel, mx, my, click)) { cos_settings_set(COS_SET_FONT_SCALE, i + 1); cos_settings_get(&st); }
            }
            y += 48;
            y = section(&rc, "Wallpaper", y);
            for (int i = 0; i < st.wallpaper_count && i < 6; ++i) {
                bool sel = st.wallpaper_idx == i;
                char lab[16]; snprintf(lab, sizeof lab, "Style %d", i + 1);
                if (chip(&rc, rc.x + i * 96, y, 86, 34, lab, sel, mx, my, click)) { cos_settings_set(COS_SET_WALLPAPER, i); cos_settings_get(&st); }
            }
            y += 50;
            bool dk = st.dark_mode != 0;
            if (chip(&rc, rc.x, y, 140, 32, dk ? "Dark Mode: On" : "Dark Mode: Off", dk, mx, my, click)) { cos_settings_set(COS_SET_DARK_MODE, dk ? 0 : 1); cos_settings_get(&st); }
            break;
        }
        case TAB_NETWORK: {
            y = section(&rc, "Connection", y);
            y = row(&rc, "Status", st.net_connected ? "Connected" : "No interface detected", y);
            if (st.net_connected) {
                y = row(&rc, "Interface", st.net_iface, y);
                y = row(&rc, "IPv4 Address", st.net_ip, y);
                y = row(&rc, "MAC Address", st.net_mac, y);
            } else {
                cui_text(&c, f_small, rc.x, y, "Add a virtio-net or e1000 device to test networking.", MUTED); y += 24;
            }
            break;
        }
        case TAB_STORAGE: {
            fmt_bytes(st.storage_total, buf1, sizeof buf1);
            fmt_bytes(st.storage_used, buf2, sizeof buf2);
            fmt_bytes(st.storage_free, buf3, sizeof buf3);
            y = section(&rc, "Disk", y);
            y = row(&rc, "Capacity", buf1, y);
            int barw = rc.w - rc.x - 30;
            uint64_t pct = st.storage_total ? (st.storage_used * 100 / st.storage_total) : 0;
            cui_round_rect(&c, rc.x, y, barw, 16, 8, BTN);
            cui_round_rect(&c, rc.x, y, (int)(barw * pct / 100), 16, 8, ACCENT);
            y += 30;
            char usedline[64]; snprintf(usedline, sizeof usedline, "%s used", buf2);
            y = row(&rc, "Used", usedline, y);
            y = row(&rc, "Free", buf3, y);
            char fc[16]; snprintf(fc, sizeof fc, "%d", st.file_count);
            y = row(&rc, "Files indexed", fc, y);
            break;
        }
        case TAB_ABOUT: {
            y = section(&rc, "C-OS", y);
            y = row(&rc, "Version", st.os_version, y);
            y = row(&rc, "Build", st.build_info, y);
            y = row(&rc, "Architecture", "x86-64", y);
            y = row(&rc, "Boot Mode", "GRUB2 Multiboot", y);
            break;
        }
        case TAB_ACCESSIBILITY: {
            bool jp = st.japanese != 0;
            y = section(&rc, "Language", y);
            if (chip(&rc, rc.x, y, 110, 32, jp ? "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" : "English", true, mx, my, false)) {}
            if (chip(&rc, rc.x + 118, y, 110, 32, "Switch", false, mx, my, click)) { cos_settings_set(COS_SET_LANGUAGE, jp ? 0 : 1); cos_settings_get(&st); }
            break;
        }
        case TAB_GUI: {
            y = section(&rc, "Desktop Icons", y);
            static const int sizes[3] = { 48, 64, 80 };
            static const char *const names[3] = { "Small", "Medium", "Large" };
            for (int i = 0; i < 3; ++i) {
                bool sel = st.icon_size == sizes[i];
                if (chip(&rc, rc.x + i * 96, y, 86, 32, names[i], sel, mx, my, click)) { cos_settings_set(COS_SET_ICON_SIZE, sizes[i]); cos_settings_get(&st); }
            }
            break;
        }
        case TAB_USERS:
            y = section(&rc, "Account", y);
            y = row(&rc, "Current User", "Administrator", y);
            y = row(&rc, "Role", "System Owner", y);
            y += 10;
            button(&rc, rc.x, y, 170, 34, "Change Password", true, mx, my, click, NULL);
            break;
        case TAB_SECURITY:
            y = section(&rc, "Protection", y);
            y = row(&rc, "Security Scanner", "Ready", y);
            y = row(&rc, "Definitions", "Local only", y);
            y += 10;
            button(&rc, rc.x, y, 150, 34, "Full Scan", true, mx, my, click, NULL);
            break;
        case TAB_TERMINAL:
            y = section(&rc, "Shell", y);
            y = row(&rc, "Default shell", "/bin/shell", y);
            y = row(&rc, "Startup script", "none", y);
            break;
        case TAB_SHORTCUTS:
            y = section(&rc, "Keyboard", y);
            y = row(&rc, "Open Studio", "Ctrl+Alt+S", y);
            y = row(&rc, "Open File Manager", "Ctrl+Alt+E", y);
            y = row(&rc, "Lock Screen", "Ctrl+Alt+L", y);
            break;
        case TAB_POWER:
            y = section(&rc, "Power", y);
            button(&rc, rc.x, y, 130, 34, "Restart", false, mx, my, click, NULL);
            button(&rc, rc.x + 140, y, 130, 34, "Shut Down", false, mx, my, click, NULL);
            break;
        case TAB_STARTUP:
            y = section(&rc, "Startup Programs", y);
            cui_text(&c, f_small, rc.x, y, "Nothing is configured to launch automatically.", MUTED); y += 26;
            break;
        case TAB_INPUT:
            y = section(&rc, "Mouse & Keyboard", y);
            y = row(&rc, "Pointer speed", "Normal", y);
            y = row(&rc, "Keyboard layout", "US QWERTY", y);
            break;
        case TAB_FILES:
            y = section(&rc, "File Manager", y);
            cui_text(&c, f_small, rc.x, y, "Files are browsed and edited directly from File Manager.", MUTED); y += 26;
            break;
        default: break;
        }

        cos_win2_present(h);
    }
    cos_win2_close(h);
    return 0;
}
