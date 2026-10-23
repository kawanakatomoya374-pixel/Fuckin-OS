/* settings.c-os - the Settings app, as a real ring-3 program.
 *
 * Full feature parity with the old kernel-space Settings panel
 * (src/gui/apps/settings/gui_apps_settings.c, 15 tabs) - a sidebar instead
 * of a cramped top tab row, cos_ui's anti-aliased drawing and the Inter
 * font at the window's own resolution, and real system data/actions via
 * SYS_SETTINGS_GET/SET/ACTION and SYS_PASSWORD_CHANGE (see cos.h) rather
 * than direct kernel calls a ring-3 process cannot make. Every control
 * that did something real in the old panel does the same real thing here
 * through those syscalls; a control that was already just a status line
 * there (e.g. Security's "Full Scan" - no scanner exists) stays that way
 * rather than inventing functionality the OS does not actually have.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cos.h"
#include "cos_ui.h"

typedef enum {
    TAB_SYSTEM, TAB_DISPLAY, TAB_NETWORK, TAB_STORAGE, TAB_USERS, TAB_SECURITY,
    TAB_ABOUT, TAB_TERMINAL, TAB_SHORTCUTS, TAB_POWER, TAB_STARTUP,
    TAB_ACCESSIBILITY, TAB_INPUT, TAB_FILES, TAB_GUI, TAB_DEVICES, TAB_EXTENSIONS, TAB_PLUGINS, TAB_UPDATE, TAB_COUNT
} tab_t;
static const char *const TAB_NAMES[TAB_COUNT] = {
    "System", "Display", "Network", "Storage", "Users", "Security",
    "About", "Terminal", "Shortcuts", "Power", "Startup",
    "Accessibility", "Input", "Files", "GUI", "Devices", "Extensions", "Plugins", "Update",
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
static bool button(row_ctx_t *r, int x, int y, int w, int h, const char *label, bool primary, int mx, int my, bool click) {
    bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
    cui_round_rect(r->c, x, y, w, h, 7, primary ? r->accent : r->btn);
    cui_text_center(r->c, r->lbl, x, y, w, h, label, primary ? 0xFFFFFF : r->text);
    return hover && click;
}
static bool chip(row_ctx_t *r, int x, int y, int w, int h, const char *label, bool selected, bool enabled, int mx, int my, bool click) {
    bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
    uint32_t fill = selected ? r->accent : (enabled ? r->btn : 0x1A1D26u);
    uint32_t txt = selected ? 0xFFFFFFu : (enabled ? r->text : r->muted);
    cui_round_rect(r->c, x, y, w, h, 6, fill);
    cui_text_center(r->c, r->lbl, x, y, w, h, label, txt);
    return enabled && hover && click;
}
/* Two-state pill, replacing the old settings_draw_toggle_button(). */
static bool toggle(row_ctx_t *r, int x, int y, int w, int h, bool on, const char *on_label, const char *off_label, int mx, int my, bool click) {
    bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
    cui_round_rect(r->c, x, y, w, h, 6, on ? r->accent : r->btn);
    cui_text_center(r->c, r->lbl, x, y, w, h, on ? on_label : off_label, on ? 0xFFFFFFu : r->text);
    return hover && click;
}
/* A tiny [-] [value] [+] stepper, for Auto-save interval. */
static int stepper(row_ctx_t *r, int x, int y, const char *value_label, int mx, int my, bool click, bool *minus, bool *plus) {
    cui_round_rect(r->c, x, y, 40, 30, 6, r->btn);
    cui_text_center(r->c, r->lbl, x, y, 40, 30, "-", r->text);
    if (minus && mx >= x && mx < x + 40 && my >= y && my < y + 30 && click) *minus = true;
    cui_round_rect(r->c, x + 50, y, 100, 30, 6, r->accent);
    cui_text_center(r->c, r->lbl, x + 50, y, 100, 30, value_label, 0xFFFFFFu);
    cui_round_rect(r->c, x + 160, y, 40, 30, 6, r->btn);
    cui_text_center(r->c, r->lbl, x + 160, y, 40, 30, "+", r->text);
    if (plus && mx >= x + 160 && mx < x + 200 && my >= y && my < y + 30 && click) *plus = true;
    return y + 40;
}

/* ---- password-change inline form state ---- */
static bool pw_open = false;
static char pw_old[64] = "", pw_new[64] = "";
static int pw_focus = 0; /* 0 = old password field, 1 = new password field */

/* ---- status line, replacing the old kernel gui_notify() toast: ring-3
 * programs have no desktop-toast syscall, so this renders the same
 * message inline under the tab header for a couple of seconds instead. */
static char status_msg[96] = "";
static uint64_t status_until_ms = 0;
static void pl_status(const char *m);
static void set_status(const char *msg) {
    snprintf(status_msg, sizeof status_msg, "%s", msg);
    status_until_ms = cos_time_ms() + 2200;
}

static void pl_status(const char *m) { set_status(m); }
static void draw_masked(row_ctx_t *r, int x, int y, int w, int h, const char *text, bool focused) {
    cui_round_rect(r->c, x, y, w, h, 6, focused ? 0x26314A : r->btn);
    char masked[64]; size_t n = strlen(text); if (n > 60) n = 60;
    for (size_t i = 0; i < n; ++i) masked[i] = '*';
    masked[n] = 0;
    cui_text(r->c, r->val, x + 10, y + (h - 14) / 2, masked[0] ? masked : " ", r->text);
    if (focused) cui_fill(r->c, x + 10 + (int)n * 9, y + 6, 2, h - 12, r->accent);
}


/* ---- Update page: what cosupdate (docs/app_update.md) and the taskbar keep in /system/update ---- */
static bool up_read(const char *path, char *buf, size_t cap) {
    cos_stat_t s2; if (cos_stat(path, &s2) != 0 || s2.is_dir || s2.size == 0 || s2.size >= cap) return false;
    ssize_t n = cos_read_file(path, buf, cap - 1); if (n <= 0) return false; buf[n] = 0; return true;
}
static bool up_field(const char *text, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key); out[0] = 0;
    for (const char *p = text; *p; ) {
        const char *nl = strchr(p, '\n'); size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == ' ') { size_t n = len - kl - 1; if (n >= cap) n = cap - 1; memcpy(out, p + kl + 1, n); out[n] = 0; return true; }
        if (!nl) break; p = nl + 1;
    }
    return false;
}
static void up_run(const char *cmd) { const char *argv[3] = { "cosupdate.c-os", cmd, NULL }; cos_spawn_argv("/bin/cosupdate.c-os", argv, NULL); }
static char up_url[300]; static bool up_url_focus, up_url_loaded;
static void up_url_save(void) {
    cos_unlink("/system/update/url.txt");
    if (up_url[0]) cos_write_file("/system/update/url.txt", up_url, strlen(up_url));
}

#include "plugins.inc"

int main(void) {
    int32_t sw = 1020, sh = 720;
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
    plug_init();
    tab_t tab = TAB_SYSTEM;
    int mx = -1, my = -1;
    bool click = false;
    bool dirty = true;
    cos_win_event_t pending; bool have_pending = false;
    int dev_first = 0;   /* Devices tab: index of the first device shown (mouse wheel scrolls by device) */

    for (;;) {
        cos_win_event_t ev;
        int r;
        if (have_pending) { ev = pending; have_pending = false; r = 1; }
        else r = cos_win2_wait(h, &ev, 300);
        /* a burst of pointer moves is one move: drawing the whole window for each would make the click that follows wait (and a full queue drops it) */
        if (r > 0 && ev.type == COS_EV_MOUSE_MOVE) {
            cos_win_event_t e2;
            while (cos_win2_poll(h, &e2) > 0) { if (e2.type == COS_EV_MOUSE_MOVE) ev = e2; else { pending = e2; have_pending = true; break; } }
        }
        click = false;
        if (cos_plugins_poll(&plugs, 1000)) dirty = true;
        bool key_event = false;
        if (r > 0) {
            if (ev.type == COS_EV_CLOSE) break;
            if (ev.type == COS_EV_MOUSE_MOVE) { mx = ev.x; my = ev.y; dirty = true; }
            if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) { mx = ev.x; my = ev.y; click = true; dirty = true; }
            if (ev.type == COS_EV_WHEEL && tab == TAB_DEVICES) { dev_first -= ev.wheel > 0 ? 1 : -1; if (dev_first < 0) dev_first = 0; dirty = true; }
            if (ev.type == COS_EV_KEY) { key_event = true; dirty = true; }
        } else {
            cos_settings_get(&st);   /* live numbers (memory/storage/network) stay current even with no input */
            dirty = true;
        }
        if (key_event && tab == TAB_EXTENSIONS) ext_key(&ev);
        if (key_event && tab == TAB_UPDATE && up_url_focus) {
            size_t ul = strlen(up_url);
            if (ev.ascii == 8 || ev.ascii == 127) { if (ul) up_url[ul - 1] = 0; }
            else if (ev.ascii == '\r' || ev.ascii == '\n') { up_url_save(); up_url_focus = false; set_status("Update source saved"); }
            else if (ev.ascii == 27) up_url_focus = false;
            else if (ev.ascii >= 32 && ev.ascii < 127 && ul < sizeof up_url - 1) { up_url[ul] = ev.ascii; up_url[ul + 1] = 0; }
        }
        if (status_msg[0] && cos_time_ms() > status_until_ms) { status_msg[0] = 0; dirty = true; }

        /* Password form keyboard handling (works regardless of which tab
         * is open, since the form only exists while pw_open is true). */
        if (key_event && pw_open) {
            char *field = pw_focus == 0 ? pw_old : pw_new;
            size_t len = strlen(field);
            if (ev.ascii == '\t') { pw_focus = !pw_focus; }
            else if (ev.ascii == 27) { pw_open = false; pw_old[0] = pw_new[0] = 0; }
            else if (ev.ascii == 8 || ev.ascii == 127) { if (len) field[len - 1] = 0; }
            else if (ev.ascii == '\r' || ev.ascii == '\n') {
                if (!st.has_password || pw_old[0] || pw_focus == 1) {
                    if (cos_password_change(pw_old, pw_new) == 0) {
                        set_status(st.has_password ? "Password updated" : "Password set");
                        pw_open = false; pw_old[0] = pw_new[0] = 0;
                        cos_settings_get(&st);
                    } else {
                        set_status("Current password is incorrect");
                    }
                }
            } else if (ev.ascii >= 32 && ev.ascii < 127 && len < sizeof(pw_old) - 1) {
                field[len] = ev.ascii; field[len + 1] = 0;
            }
        }

        if (!dirty) continue;
        dirty = false;

        cui_fill(&c, 0, 0, c.w, c.h, BG);
        cui_fill(&c, 0, 0, SIDEBAR_W, c.h, SIDEBAR);
        cui_fill(&c, SIDEBAR_W - 1, 0, 1, c.h, BORDER);

        cui_text(&c, f_hdr, 20, 20, "Settings", TEXT);
        int sy = 60;
        for (int i = 0; i < TAB_COUNT; ++i) {
            bool sel = (tab == (tab_t)i);
            bool hov = mx >= 8 && mx < SIDEBAR_W - 8 && my >= sy && my < sy + 30;
            if (sel || hov) cui_round_rect(&c, 8, sy, SIDEBAR_W - 16, 30, 7, sel ? SEL : 0x20242F);
            if (sel) cui_fill(&c, 8, sy + 4, 3, 22, ACCENT);
            cui_text(&c, f_side, 24, sy + 6, TAB_NAMES[i], sel ? TEXT : MUTED);
            if (hov && click) { tab = (tab_t)i; pw_open = false; }
            sy += 32;
        }

        row_ctx_t rc = { &c, f_lbl, f_val, f_hdr, f_small, TEXT, MUTED, ACCENT, PANEL, BORDER, BTN, SEL, SIDEBAR_W + 30, 20, c.w };
        int y = 30;
        cui_text(&c, f_hdr, rc.x, y, TAB_NAMES[tab], TEXT); y += 40;
        if (status_msg[0]) { cui_text(&c, f_small, rc.x, y, status_msg, ACCENT); }
        y += 18;

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
            y += 14;
            y = section(&rc, "Developer", y);
            cui_text(&c, f_lbl, rc.x, y, "Developer Mode", TEXT); y += 24;
            if (toggle(&rc, rc.x, y, 140, 32, st.developer_mode != 0, "Enabled", "Disabled", mx, my, click)) {
                cos_settings_set(COS_SET_DEVELOPER_MODE, !st.developer_mode); cos_settings_get(&st);
                set_status(st.developer_mode ? "Developer mode enabled" : "Developer mode disabled");
            }
            y += 46;
            cui_text(&c, f_lbl, rc.x, y, "Auto-save interval", TEXT); y += 24;
            {
                char lab[16]; snprintf(lab, sizeof lab, "%us", (unsigned)st.auto_save_interval);
                bool minus = false, plus = false;
                y = stepper(&rc, rc.x, y, lab, mx, my, click, &minus, &plus);
                if (minus && st.auto_save_interval > 5) { cos_settings_set(COS_SET_AUTO_SAVE_INTERVAL, (int)st.auto_save_interval - 5); cos_settings_get(&st); set_status("Auto-save interval updated"); }
                if (plus && st.auto_save_interval < 3600) { cos_settings_set(COS_SET_AUTO_SAVE_INTERVAL, (int)st.auto_save_interval + 5); cos_settings_get(&st); set_status("Auto-save interval updated"); }
            }
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
                if (chip(&rc, rc.x + i * 90, y, 80, 32, themes[i], sel, true, mx, my, click)) { cos_settings_set(COS_SET_THEME, i + 1); cos_settings_get(&st); set_status("Accent theme changed"); }
            }
            y += 48;
            y = section(&rc, "Font Scale", y);
            for (int i = 0; i < 4; ++i) {
                bool sel = st.font_scale == i + 1;
                char lab[4]; snprintf(lab, sizeof lab, "%dx", i + 1);
                if (chip(&rc, rc.x + i * 60, y, 50, 32, lab, sel, true, mx, my, click)) { cos_settings_set(COS_SET_FONT_SCALE, i + 1); cos_settings_get(&st); set_status("Font scale changed"); }
            }
            y += 48;
            y = section(&rc, "Font Family", y);
            for (int i = 0; i < 6; ++i) {
                int bx = rc.x + (i % 3) * 154, by = y + (i / 3) * 38;
                bool sel = st.font_family == i;
                if (chip(&rc, bx, by, 142, 32, cos_font_family_name(i), sel, true, mx, my, click)) { cos_settings_set(COS_SET_FONT_FAMILY, i); cos_settings_get(&st); set_status("Font family changed"); }
            }
            y += 84;
            y = section(&rc, "Wallpaper", y);
            for (int i = 0; i < st.wallpaper_count && i < 6; ++i) {
                bool sel = st.wallpaper_idx == i;
                char lab[16]; snprintf(lab, sizeof lab, "Style %d", i + 1);
                if (chip(&rc, rc.x + i * 96, y, 86, 34, lab, sel, true, mx, my, click)) { cos_settings_set(COS_SET_WALLPAPER, i); cos_settings_get(&st); set_status("Desktop background updated"); }
            }
            y += 50;
            if (toggle(&rc, rc.x, y, 140, 32, st.dark_mode != 0, "Dark Mode: On", "Dark Mode: Off", mx, my, click)) { cos_settings_set(COS_SET_DARK_MODE, !st.dark_mode); cos_settings_get(&st); set_status(st.dark_mode ? "Dark mode enabled" : "Light mode enabled"); }
            y += 48;
            y = section(&rc, "Language", y);
            for (int i = 0; i < 2; ++i) {
                bool sel = st.japanese == i;
                if (chip(&rc, rc.x + i * 100, y, 88, 32, i ? "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" : "English", sel, true, mx, my, click)) { cos_settings_set(COS_SET_LANGUAGE, i); cos_settings_get(&st); set_status("Language changed"); }
            }
            break;
        }
        case TAB_NETWORK: {
            y = section(&rc, "Connection", y);
            y = row(&rc, "Status", st.net_connected ? "Connected" : "No interface detected", y);
            if (st.net_connected) {
                y = row(&rc, "Interface", st.net_iface, y);
                y = row(&rc, "IPv4 Address", st.net_ip, y);
                y = row(&rc, "Subnet Mask", st.net_netmask, y);
                y = row(&rc, "Gateway", st.net_gateway, y);
                y = row(&rc, "MAC Address", st.net_mac, y);
                y += 10;
                if (button(&rc, rc.x, y, 190, 32, "Renew IP (DHCP)", true, mx, my, click)) {
                    set_status(cos_settings_action(COS_ACTION_NET_DHCP_RENEW, 0) == 0 ? "Requesting new IP via DHCP..." : "DHCP renew failed");
                }
                y += 46;
            } else {
                cui_text(&c, f_small, rc.x, y, "Add a virtio-net or e1000 device to test networking.", MUTED); y += 24;
            }
            cui_text(&c, f_small, rc.x, y, "Transport: TCP/UDP over IPv4, DNS, DHCP client", MUTED);
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
        case TAB_USERS: {
            y = section(&rc, "Account", y);
            y = row(&rc, "Current User", "Administrator", y);
            y = row(&rc, "Role", "System Owner", y);
            y = row(&rc, "Password", st.has_password ? "Set" : "Not set", y);
            y += 10;
            if (!pw_open) {
                if (button(&rc, rc.x, y, 170, 34, "Change Password", true, mx, my, click)) { pw_open = true; pw_focus = st.has_password ? 0 : 1; pw_old[0] = pw_new[0] = 0; }
            } else {
                if (st.has_password) {
                    cui_text(&c, f_lbl, rc.x, y, "Current password", MUTED); y += 20;
                    draw_masked(&rc, rc.x, y, 260, 32, pw_old, pw_focus == 0);
                    if (mx >= rc.x && mx < rc.x + 260 && my >= y && my < y + 32 && click) pw_focus = 0;
                    y += 44;
                }
                cui_text(&c, f_lbl, rc.x, y, "New password", MUTED); y += 20;
                draw_masked(&rc, rc.x, y, 260, 32, pw_new, pw_focus == 1);
                if (mx >= rc.x && mx < rc.x + 260 && my >= y && my < y + 32 && click) pw_focus = 1;
                y += 44;
                cui_text(&c, f_small, rc.x, y, "Tab to switch fields, Enter to save, Esc to cancel.", MUTED); y += 28;
                if (button(&rc, rc.x, y, 100, 32, "Save", true, mx, my, click)) {
                    if (cos_password_change(pw_old, pw_new) == 0) { set_status(st.has_password ? "Password updated" : "Password set"); pw_open = false; pw_old[0] = pw_new[0] = 0; cos_settings_get(&st); }
                    else set_status("Current password is incorrect");
                }
                if (button(&rc, rc.x + 110, y, 100, 32, "Cancel", false, mx, my, click)) { pw_open = false; pw_old[0] = pw_new[0] = 0; }
            }
            break;
        }
        case TAB_SECURITY:
            y = section(&rc, "Protection", y);
            y = row(&rc, "Security Scanner", "Ready", y);
            y = row(&rc, "Definitions", "Local only", y);
            y += 10;
            if (button(&rc, rc.x, y, 150, 34, "Full Scan", true, mx, my, click)) { cos_settings_action(COS_ACTION_SECURITY_FULL_SCAN, 0); set_status("Full scan started"); }
            break;
        case TAB_TERMINAL:
            y = section(&rc, "Shell", y);
            cui_text(&c, f_lbl, rc.x, y, "Terminal Auto-Scroll", TEXT); y += 24;
            if (toggle(&rc, rc.x, y, 140, 32, st.terminal_autoscroll != 0, "Enabled", "Disabled", mx, my, click)) { cos_settings_set(COS_SET_TERMINAL_AUTOSCROLL, !st.terminal_autoscroll); cos_settings_get(&st); set_status(st.terminal_autoscroll ? "Terminal auto-scroll enabled" : "Terminal auto-scroll disabled"); }
            y += 46;
            if (button(&rc, rc.x, y, 150, 32, "Open Terminal", false, mx, my, click)) { cos_settings_action(COS_ACTION_OPEN_TERMINAL, 0); }
            break;
        case TAB_SHORTCUTS:
            y = section(&rc, "Keyboard", y);
            y = row(&rc, "Open Studio", "Ctrl+Alt+S", y);
            y = row(&rc, "Open File Manager", "Ctrl+Alt+E", y);
            y = row(&rc, "Lock Screen", "Ctrl+Alt+L", y);
            y += 10;
            cui_text(&c, f_small, rc.x, y, "Right-click the desktop for quick actions.", MUTED);
            break;
        case TAB_POWER:
            y = section(&rc, "Power", y);
            if (button(&rc, rc.x, y, 130, 34, "Sleep", false, mx, my, click)) {
                set_status(cos_settings_action(COS_ACTION_POWER_SLEEP, 0) == 0 ? "Sleep requested" : "Sleep is not supported by this platform");
            }
            if (button(&rc, rc.x + 140, y, 130, 34, "Restart", false, mx, my, click)) { cos_settings_action(COS_ACTION_POWER_RESTART, 0); }
            if (button(&rc, rc.x + 280, y, 130, 34, "Shut Down", false, mx, my, click)) { cos_settings_action(COS_ACTION_POWER_SHUTDOWN, 0); }
            break;
        case TAB_STARTUP:
            y = section(&rc, "Startup Programs", y);
            if (toggle(&rc, rc.x, y, 230, 32, st.autostart_terminal != 0, "Terminal autostart: On", "Terminal autostart: Off", mx, my, click)) { cos_settings_set(COS_SET_AUTOSTART_TERMINAL, !st.autostart_terminal); cos_settings_get(&st); set_status(st.autostart_terminal ? "Terminal autostart enabled" : "Terminal autostart disabled"); }
            y += 46;
            if (toggle(&rc, rc.x, y, 250, 32, st.autostart_file_manager != 0, "File Manager autostart: On", "File Manager autostart: Off", mx, my, click)) { cos_settings_set(COS_SET_AUTOSTART_FILE_MANAGER, !st.autostart_file_manager); cos_settings_get(&st); set_status(st.autostart_file_manager ? "File Manager autostart enabled" : "File Manager autostart disabled"); }
            y += 46;
            if (toggle(&rc, rc.x, y, 230, 32, st.autostart_browser != 0, "Browser autostart: On", "Browser autostart: Off", mx, my, click)) { cos_settings_set(COS_SET_AUTOSTART_BROWSER, !st.autostart_browser); cos_settings_get(&st); set_status(st.autostart_browser ? "Browser autostart enabled" : "Browser autostart disabled"); }
            break;
        case TAB_ACCESSIBILITY: {
            y = section(&rc, "Appearance Mode", y);
            if (chip(&rc, rc.x, y, 110, 32, "Dark", st.dark_mode != 0, true, mx, my, click)) { cos_settings_set(COS_SET_DARK_MODE, 1); cos_settings_get(&st); set_status("Dark mode enabled"); }
            if (chip(&rc, rc.x + 118, y, 110, 32, "Light", st.dark_mode == 0, true, mx, my, click)) { cos_settings_set(COS_SET_DARK_MODE, 0); cos_settings_get(&st); set_status("Light mode enabled"); }
            y += 48;
            y = section(&rc, "Window Animations", y);
            if (toggle(&rc, rc.x, y, 100, 32, st.window_animations != 0, "On", "Off", mx, my, click)) { cos_settings_set(COS_SET_WINDOW_ANIMATIONS, !st.window_animations); cos_settings_get(&st); set_status(st.window_animations ? "Animations enabled" : "Animations disabled"); }
            y += 48;
            y = section(&rc, "Desktop Notifications", y);
            if (toggle(&rc, rc.x, y, 100, 32, st.notifications_enabled != 0, "On", "Off", mx, my, click)) { cos_settings_set(COS_SET_NOTIFICATIONS, !st.notifications_enabled); cos_settings_get(&st); set_status(st.notifications_enabled ? "Notifications enabled" : "Notifications disabled"); }
            y += 48;
            y = section(&rc, "FPS Counter", y);
            if (toggle(&rc, rc.x, y, 100, 32, st.fps_overlay != 0, "On", "Off", mx, my, click)) { cos_settings_set(COS_SET_FPS_OVERLAY, !st.fps_overlay); cos_settings_get(&st); set_status(st.fps_overlay ? "FPS counter enabled" : "FPS counter disabled"); }
            break;
        }
        case TAB_INPUT: {
            y = section(&rc, "Mouse Sensitivity", y);
            static const char *const sens[4] = { "Low", "Med", "High", "Max" };
            for (int i = 0; i < 4; ++i) {
                bool sel = st.mouse_sensitivity == i + 1;
                if (chip(&rc, rc.x + i * 74, y, 66, 32, sens[i], sel, true, mx, my, click)) { cos_settings_set(COS_SET_MOUSE_SENSITIVITY, i + 1); cos_settings_get(&st); set_status("Mouse sensitivity updated"); }
            }
            y += 48;
            y = section(&rc, "Raw Mouse Input", y);
            if (toggle(&rc, rc.x, y, 200, 32, st.mouse_raw_input != 0, "Raw Input: On", "Raw Input: Reduced", mx, my, click)) { cos_settings_set(COS_SET_MOUSE_RAW_INPUT, !st.mouse_raw_input); cos_settings_get(&st); set_status(st.mouse_raw_input ? "Raw mouse input enabled" : "Raw mouse input reduced"); }
            y += 48;
            y = section(&rc, "Drag Threshold", y);
            for (int i = 1; i <= 10; ++i) {
                int bx = rc.x + ((i - 1) % 5) * 66, by = y + ((i - 1) / 5) * 40;
                bool sel = st.mouse_drag_threshold == i;
                char lab[8]; snprintf(lab, sizeof lab, "%dpx", i);
                if (chip(&rc, bx, by, 58, 30, lab, sel, true, mx, my, click)) { cos_settings_set(COS_SET_MOUSE_DRAG_THRESHOLD, i); cos_settings_get(&st); set_status("Drag threshold updated"); }
            }
            y += 96;
            y = section(&rc, "Multi-cursor Mode", y);
            if (toggle(&rc, rc.x, y, 220, 32, st.multi_cursor != 0, "Multi-cursor: On", "Multi-cursor: Off", mx, my, click)) { cos_settings_set(COS_SET_MULTI_CURSOR, 0); cos_settings_get(&st); set_status(st.multi_cursor ? "Multi-cursor mode enabled" : "Multi-cursor mode disabled"); }
            y += 44;
            cui_text(&c, f_small, rc.x, y, "Arrows move; Alt=left click; Fn/F12=right click; Ctrl+P+O toggles", MUTED);
            break;
        }
        case TAB_FILES: {
            y = section(&rc, "New File Manager windows use these defaults", y);
            cui_text(&c, f_lbl, rc.x, y, "Show Hidden Files", TEXT); y += 24;
            if (toggle(&rc, rc.x, y, 100, 32, st.efm_show_hidden != 0, "On", "Off", mx, my, click)) { cos_settings_set(COS_SET_EFM_SHOW_HIDDEN, !st.efm_show_hidden); cos_settings_get(&st); set_status(st.efm_show_hidden ? "Hidden files will be shown by default" : "Hidden files will be hidden by default"); }
            y += 48;
            cui_text(&c, f_lbl, rc.x, y, "Confirm Before Delete", TEXT); y += 24;
            if (toggle(&rc, rc.x, y, 100, 32, st.efm_confirm_delete != 0, "On", "Off", mx, my, click)) { cos_settings_set(COS_SET_EFM_CONFIRM_DELETE, !st.efm_confirm_delete); cos_settings_get(&st); set_status(st.efm_confirm_delete ? "Delete confirmation enabled" : "Delete confirmation disabled"); }
            y += 48;
            cui_text(&c, f_lbl, rc.x, y, "Default View", TEXT); y += 24;
            {
                static const char *const views[4] = { "List", "Icons", "Details", "Thumbnails" };
                for (int i = 0; i < 4; ++i) {
                    bool sel = st.efm_default_view == i;
                    if (chip(&rc, rc.x + i * 84, y, 76, 30, views[i], sel, true, mx, my, click)) { cos_settings_set(COS_SET_EFM_DEFAULT_VIEW, i); cos_settings_get(&st); set_status("Default file manager view updated"); }
                }
            }
            y += 46;
            cui_text(&c, f_small, rc.x, y, "These do not affect already-open windows.", MUTED);
            break;
        }
        case TAB_GUI: {
            y = section(&rc, "Rendering", y);
            bool sel_cpu = st.render_backend == 0, sel_gpu = st.render_backend == 1;
            if (chip(&rc, rc.x, y, 170, 34, "CPU (Software)", sel_cpu, true, mx, my, click)) { cos_settings_set(COS_SET_RENDER_BACKEND, 0); cos_settings_get(&st); set_status("Switched to CPU rendering"); }
            if (chip(&rc, rc.x + 182, y, 170, 34, "GPU (VirtIO)", sel_gpu, st.render_backend_gpu_available != 0, mx, my, click)) {
                if (cos_settings_set(COS_SET_RENDER_BACKEND, 1) == 0) set_status("Switched to GPU (VirtIO) rendering"); else set_status("VirtIO-GPU is not available");
                cos_settings_get(&st);
            }
            bool full_selectable = sel_gpu && st.virgl_available != 0;
            if (chip(&rc, rc.x + 364, y, 170, 34, sel_gpu && st.gpu_full_draw ? "GPU (Full): On" : "GPU (Full)", sel_gpu && st.gpu_full_draw != 0, full_selectable || !sel_gpu, mx, my, click)) {
                if (!sel_gpu) set_status("Select GPU (VirtIO) first");
                else if (st.gpu_full_draw) { cos_settings_set(COS_SET_GPU_FULL_DRAW, 0); set_status("Switched to GPU (present only)"); cos_settings_get(&st); }
                else if (cos_settings_set(COS_SET_GPU_FULL_DRAW, 1) == 0) { set_status("Switched to GPU (Full) rendering"); cos_settings_get(&st); }
                else set_status("VIRGL 3D is not available on this host");
            }
            y += 56;
            y = section(&rc, "Font Size", y);
            {
                char lab[16]; snprintf(lab, sizeof lab, "%dpx", st.font_pixel_width);
                bool minus = false, plus = false;
                int sy2 = stepper(&rc, rc.x, y, lab, mx, my, click, &minus, &plus);
                if (button(&rc, rc.x + 204, y, 110, 30, "Reset (8px)", false, mx, my, click)) { cos_settings_set(COS_SET_FONT_PIXEL_WIDTH, 8); cos_settings_get(&st); }
                if (minus && st.font_pixel_width > 1) { cos_settings_set(COS_SET_FONT_PIXEL_WIDTH, st.font_pixel_width - 1); cos_settings_get(&st); }
                if (plus && st.font_pixel_width < 32) { cos_settings_set(COS_SET_FONT_PIXEL_WIDTH, st.font_pixel_width + 1); cos_settings_get(&st); }
                y = sy2;
            }
            y += 16;
            y = section(&rc, "Desktop Icons", y);
            static const int sizes[3] = { 48, 64, 80 };
            static const char *const names[3] = { "Small", "Medium", "Large" };
            for (int i = 0; i < 3; ++i) {
                bool sel = st.icon_size == sizes[i];
                if (chip(&rc, rc.x + i * 96, y, 86, 32, names[i], sel, true, mx, my, click)) { cos_settings_set(COS_SET_ICON_SIZE, sizes[i]); cos_settings_get(&st); }
            }
            break;
        }
        case TAB_DEVICES: {
            static cos_device_info_t devs[24];
            int ndev = cos_devices_list(devs, 24);   /* cheap; rescanned every frame so hot-plug shows up on its own */
            if (ndev < 0) ndev = 0;
            if (dev_first > ndev - 1) dev_first = ndev > 0 ? ndev - 1 : 0;
            { char cnt[48]; snprintf(cnt, sizeof cnt, "%d devices - scroll for more", ndev); cui_text(&c, f_small, rc.x + 300, 36, cnt, MUTED); }
            static const char *const kinds[6] = { "", "PS/2 controller", "PS/2 keyboard", "PS/2 mouse", "USB controller", "USB device" };
            static const char *const spd[5] = { "", "Full speed 12 Mb/s", "Low speed 1.5 Mb/s", "High speed 480 Mb/s", "SuperSpeed 5 Gb/s" };
            for (int i = dev_first; i < ndev && y < c.h - 40; ++i) {
                const cos_device_info_t *d = &devs[i];
                char l1[160], l2[200];
                snprintf(l1, sizeof l1, "%s  -  %s", kinds[d->kind < 6 ? d->kind : 0], d->name);
                cui_text(&c, f_lbl, rc.x, y, l1, TEXT); y += 22;
                if (d->kind == COS_DEV_USB_DEVICE) {
                    snprintf(l2, sizeof l2, "%s  %04X:%04X  USB %x.%02x  %s  addr %u%s", d->vendor[0] ? d->vendor : "unknown vendor",
                             d->vid, d->pid, d->bcd_usb >> 8, d->bcd_usb & 0xFF, spd[d->usb_speed < 5 ? d->usb_speed : 0], d->usb_addr,
                             d->usb_hub_addr ? " (via hub)" : "");
                    cui_text(&c, f_small, rc.x + 16, y, l2, MUTED); y += 18;
                    if (d->serial[0]) { snprintf(l2, sizeof l2, "Serial %s", d->serial); cui_text(&c, f_small, rc.x + 16, y, l2, MUTED); y += 18; }
                }
                if (d->detail[0]) { cui_text(&c, f_small, rc.x + 16, y, d->detail, MUTED); y += 18; }
                y += 8;
            }
            if (ndev == 0) cui_text(&c, f_lbl, rc.x, y, "No devices detected", MUTED);
            break;
        }
        case TAB_EXTENSIONS: {
            bool changed = false;
            y = ext_draw(&rc, y, mx, my, click, &changed);
            if (changed) { cos_settings_get(&st); set_status("Saved"); }
            break;
        }
        case TAB_PLUGINS:
            cui_text(&c, f_small, rc.x, y, "Click a plugin to switch it on or off - the program picks it up while it is running.", MUTED);
            y = mgr_draw(&rc, y + 26, mx, my, click);
            break;
        case TAB_UPDATE: {
            char t1[700], v1[200], v2[200];
            if (!up_url_loaded) { up_url_loaded = true; if (up_read("/system/update/url.txt", t1, sizeof t1)) { char *nl = strchr(t1, '\n'); if (nl) *nl = 0; snprintf(up_url, sizeof up_url, "%s", t1); } }
            bool avail = up_read("/system/update/available.txt", t1, sizeof t1);
            bool running = false, failed = false; int pct = 0; char msg[160] = "";
            char pt[500];
            if (up_read("/system/update/progress.txt", pt, sizeof pt)) {
                up_field(pt, "state", v1, sizeof v1);
                if (!strcmp(v1, "running")) { running = true; up_field(pt, "percent", v2, sizeof v2); pct = atoi(v2); }
                else if (!strcmp(v1, "failed")) { failed = true; up_field(pt, "message", msg, sizeof msg); }
            }
            y = section(&rc, "C-OS Update", y);
            y = row(&rc, "OS version", st.os_version, y);
            {   char sb[700]; const char *rel = "(the one this image came with)";
                if (up_read("/system/update/state.txt", sb, sizeof sb) && up_field(sb, "os", v1, sizeof v1) && v1[0]) rel = v1;
                y = row(&rc, "Installed release", rel, y);
            }
            const char *status = running ? "Installing..." : avail ? "An update is available" : failed ? "The last update failed" : "Up to date";
            y = row(&rc, "Status", status, y);
            if (running) { char l[40]; snprintf(l, sizeof l, "%d%%", pct); y = row(&rc, "Progress", l, y); }
            else if (failed && msg[0]) y = row(&rc, "Reason", msg, y);
            if (avail && !running) {
                if (up_field(t1, "os", v1, sizeof v1)) y = row(&rc, "Release", v1, y);
                if (up_field(t1, "notes", v1, sizeof v1) && v1[0]) y = row(&rc, "What is new", v1, y);
                char a[80]; up_field(t1, "items", v1, sizeof v1); up_field(t1, "bytes", v2, sizeof v2);
                snprintf(a, sizeof a, "%s file(s), %.1f MB", v1, atof(v2) / 1048576.0); y = row(&rc, "Download", a, y);
            }
            { char lc[200]; if (up_read("/system/update/lastcheck.txt", lc, sizeof lc)) {
                  char when[60] = "", res[100] = ""; up_field(lc, "time", when, sizeof when); up_field(lc, "result", res, sizeof res);
                  char l[200]; snprintf(l, sizeof l, "%s  (%s)", when, !strncmp(res, "ok", 2) ? "ok" : res);
                  y = row(&rc, "Last checked", l, y);
              } else y = row(&rc, "Last checked", "never", y); }
            y += 6;
            if (button(&rc, rc.x, y, 150, 34, "Check now", false, mx, my, click)) { up_run("poll"); set_status("Checking for updates..."); }
            if (button(&rc, rc.x + 160, y, 190, 34, avail && !running ? "Install the update" : "Nothing to install", avail && !running, mx, my, click) && avail && !running) { up_run("install"); set_status("The update has started"); }
            y += 52;
            y = section(&rc, "Automatic check", y);
            {   char ab[80] = ""; bool on = true; long mins = 30;
                if (up_read("/system/update/auto.txt", ab, sizeof ab)) { if (!strncmp(ab, "off", 3)) on = false; const char *sp = strchr(ab, ' '); if (sp && atol(sp + 1) > 0) mins = atol(sp + 1); }
                if (toggle(&rc, rc.x, y, 150, 32, on, "Check: on", "Check: off", mx, my, click)) { const char *argv[4] = { "cosupdate.c-os", "auto", on ? "off" : "on", NULL }; cos_spawn_argv("/bin/cosupdate.c-os", argv, NULL); }
                static const long opts[3] = { 15, 30, 60 };
                for (int i = 0; i < 3; ++i) {
                    char lab[24]; snprintf(lab, sizeof lab, "%ld min", opts[i]);
                    if (chip(&rc, rc.x + 170 + i * 84, y, 76, 32, lab, mins == opts[i], on, mx, my, click)) { char m[12]; snprintf(m, sizeof m, "%ld", opts[i]); const char *argv[4] = { "cosupdate.c-os", "auto", m, NULL }; cos_spawn_argv("/bin/cosupdate.c-os", argv, NULL); }
                }
            }
            y += 50;
            y = section(&rc, "Update source", y);
            {   int fx = rc.x, fw = c.w - rc.x - 130;
                bool over = mx >= fx && mx < fx + fw && my >= y && my < y + 32;
                if (click) up_url_focus = over;
                cui_round_rect(&c, fx, y, fw, 32, 6, up_url_focus ? 0x26314A : BTN);
                cui_text_fit(&c, f_val, fx + 10, y + 8, fw - 20, up_url[0] ? up_url : "http://server/updates/   (a folder that holds manifest.cosup)", up_url[0] ? TEXT : MUTED);
                if (up_url_focus && ((cos_time_ms() / 500) & 1)) cui_fill(&c, fx + 10 + cui_text_width(f_val, up_url), y + 6, 2, 20, ACCENT);
                if (button(&rc, fx + fw + 10, y, 100, 32, "Save", false, mx, my, click)) { up_url_save(); up_url_focus = false; set_status("Update source saved"); }
            }
            y += 46;
            cui_text(&c, f_small, rc.x, y, "Updates are signed: a package that was not signed with this system's key is refused.", MUTED);
            break;
        }
        default: break;
        }

        cos_win2_present(h);
    }
    cos_plugins_unload_all(&plugs);
    cos_win2_close(h);
    return 0;
}
