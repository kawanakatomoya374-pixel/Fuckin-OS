/* plugins.inc - plugins of the Settings app (cos_plugin_settings.h): items on the "Extensions" tab, and the "Plugins" tab that switches the plugins of
 * every program on and off (copy from /system/plugins/available/<app>/ into /system/plugins/<app>/ and back). */
#include <stdlib.h>
#include "cos_plugin.h"
#include "cos_plugin_settings.h"

#define PL_DIR "/system/plugins/settings"
enum { IK_LABEL, IK_TOGGLE, IK_SLIDER, IK_CHOICE, IK_COLOR, IK_TEXT, IK_BUTTON };
#define NITEM 48
static struct {
    uint8_t kind, owner; char key[24], label[120]; int min, max, step, val; char opts[96]; char text[64];
    void (*cb)(const char *, int); void (*cbt)(const char *, const char *); void (*fn)(void);
} it[NITEM];
static int nit;
static cos_plugins_t plugs;
static settings_api_t pl_api;
static int ext_focus = -1;                 /* the text item being typed into */

static const char *cur_plugin(void) { return plugs.current >= 0 ? plugs.pl[plugs.current].name : NULL; }
static int new_item(int kind, const char *key, const char *label) {
    if (plugs.current < 0 || nit >= NITEM) return -1;
    int i = nit++; memset(&it[i], 0, sizeof it[i]);
    it[i].kind = (uint8_t)kind; it[i].owner = (uint8_t)plugs.current;
    snprintf(it[i].key, sizeof it[i].key, "%s", key ? key : ""); snprintf(it[i].label, sizeof it[i].label, "%s", label ? label : "");
    return i;
}
static void drop_item(int i) { nit--; memset(&it[i], 0, sizeof it[i]); }
static int stored_int(int i, int def) { char v[32]; if (cos_pcfg_get(cur_plugin(), it[i].key, v, sizeof v)) return def; return atoi(v); }
static void store_int(int i, int v) { char s[16]; snprintf(s, sizeof s, "%d", v); cos_pcfg_set(plugs.pl[it[i].owner].name, it[i].key, s); }
static int p_add_label(const char *t) { int i = new_item(IK_LABEL, "", t); return i < 0 ? -1 : 0; }
static int p_add_toggle(const char *k, const char *l, int def, void (*cb)(const char *, int)) {
    int i = new_item(IK_TOGGLE, k, l); if (i < 0) return -1;
    it[i].val = stored_int(i, def) != 0; it[i].cb = cb; if (cb) cb(it[i].key, it[i].val); return 0;
}
static int p_add_slider(const char *k, const char *l, int mn, int mx, int step, int def, void (*cb)(const char *, int)) {
    int i = new_item(IK_SLIDER, k, l); if (i < 0) return -1;
    it[i].min = mn; it[i].max = mx; it[i].step = step > 0 ? step : 1;
    int v = stored_int(i, def); if (v < mn) v = mn; if (v > mx) v = mx; it[i].val = v; it[i].cb = cb; if (cb) cb(it[i].key, v); return 0;
}
static int p_add_choice(const char *k, const char *l, const char *opts, int def, void (*cb)(const char *, int)) {
    int i = new_item(IK_CHOICE, k, l); if (i < 0) return -1;
    snprintf(it[i].opts, sizeof it[i].opts, "%s", opts ? opts : ""); int n = 1; for (const char *p = it[i].opts; *p; p++) if (*p == '|') n++;
    int v = stored_int(i, def); if (v < 0 || v >= n) v = 0; it[i].val = v; it[i].cb = cb; if (cb) cb(it[i].key, v); return 0;
}
static int p_add_color(const char *k, const char *l, uint32_t def, void (*cbc)(const char *, uint32_t)) {
    int i = new_item(IK_COLOR, k, l); if (i < 0) return -1;
    it[i].val = stored_int(i, (int)(def & 0xFFFFFF)) & 0xFFFFFF; it[i].cb = (void (*)(const char *, int))cbc; if (cbc) cbc(it[i].key, (uint32_t)it[i].val); return 0;
}
static int p_add_text(const char *k, const char *l, const char *def, void (*cb)(const char *, const char *)) {
    int i = new_item(IK_TEXT, k, l); if (i < 0) return -1;
    if (cos_pcfg_get(cur_plugin(), it[i].key, it[i].text, sizeof it[i].text)) snprintf(it[i].text, sizeof it[i].text, "%s", def ? def : "");
    it[i].cbt = cb; if (cb) cb(it[i].key, it[i].text); return 0;
}
static int p_add_button(const char *l, void (*fn)(void)) { int i = new_item(IK_BUTTON, "", l); if (i < 0) return -1; it[i].fn = fn; return 0; }
static int p_sys_set(int key, int value) { return cos_settings_set(key, value); }
static int p_sys_get(int key) {
    cos_settings_t s; if (cos_settings_get(&s)) return -1;
    switch (key) {
        case SETP_THEME: return s.theme_idx; case SETP_FONT_SCALE: return s.font_scale; case SETP_WALLPAPER: return s.wallpaper_idx; case SETP_DARK_MODE: return s.dark_mode;
        case SETP_LANGUAGE: return s.japanese; case SETP_ICON_SIZE: return s.icon_size; case SETP_DESKTOP_COLOR: return s.desktop_color; case SETP_DESKTOP_COLOR2: return s.desktop_color2;
        default: return -1;
    }
}
static void pl_status(const char *m);
static void pl_on_unload(cos_plugins_t *ps, int idx) {
    (void)ps;
    for (int i = nit - 1; i >= 0; i--) if (it[i].owner == idx) { if (ext_focus == i) ext_focus = -1; for (int k = i; k + 1 < nit; k++) it[k] = it[k + 1]; nit--; memset(&it[nit], 0, sizeof it[nit]); }
    ext_focus = -1;
}
static void pl_on_change(cos_plugins_t *ps, int idx, int what) {
    char b[200];
    if (what > 0) snprintf(b, sizeof b, "[SETTINGS] plugin loaded: %s %s\n", ps->pl[idx].name, ps->pl[idx].version);
    else if (what < 0) snprintf(b, sizeof b, "[SETTINGS] plugin rejected: %s (%s)\n", ps->pl[idx].name, ps->pl[idx].why);
    else snprintf(b, sizeof b, "[SETTINGS] plugin unloaded\n");
    cos_puts(b);
}
static void plug_init(void) {
    cos_plugin_base_init(&pl_api.base, pl_status);
    pl_api.abi = SETP_ABI; pl_api.size = sizeof pl_api; pl_api.add_label = p_add_label; pl_api.add_toggle = p_add_toggle; pl_api.add_slider = p_add_slider;
    pl_api.add_choice = p_add_choice; pl_api.add_color = p_add_color; pl_api.add_text = p_add_text; pl_api.add_button = p_add_button; pl_api.sys_set = p_sys_set; pl_api.sys_get = p_sys_get;
    plugs.app = "settings"; plugs.dir = PL_DIR; plugs.api = &pl_api; plugs.current = -1; plugs.on_unload = pl_on_unload; plugs.on_change = pl_on_change;
}
/* the items of one plugin go under a section with its name */
static int ext_draw(row_ctx_t *r, int y, int mx, int my, bool click, bool *changed) {
    int shown = 0;
    for (int k = 0; k < COS_PLUGIN_MAX; k++) {
        if (plugs.pl[k].state != 1) continue;
        bool any = false; for (int i = 0; i < nit; i++) if (it[i].owner == k) any = true;
        if (!any) continue;
        char t[100]; snprintf(t, sizeof t, "%s  %s", plugs.pl[k].name, plugs.pl[k].version); y = section(r, t, y); shown++;
        for (int i = 0; i < nit; i++) {
            if (it[i].owner != k) continue;
            switch (it[i].kind) {
            case IK_LABEL: cui_text(r->c, r->lbl, r->x, y, it[i].label, r->muted); y += 28; break;
            case IK_TOGGLE:
                cui_text(r->c, r->lbl, r->x, y + 6, it[i].label, r->text);
                if (toggle(r, r->x + 300, y, 120, 30, it[i].val != 0, "On", "Off", mx, my, click)) { it[i].val = !it[i].val; store_int(i, it[i].val); if (it[i].cb) it[i].cb(it[i].key, it[i].val); *changed = true; }
                y += 40; break;
            case IK_SLIDER: {
                cui_text(r->c, r->lbl, r->x, y + 6, it[i].label, r->text);
                char lab[16]; snprintf(lab, sizeof lab, "%d", it[i].val); bool minus = false, plus = false;
                stepper(r, r->x + 300, y, lab, mx, my, click, &minus, &plus);
                int v = it[i].val;
                if (minus && v - it[i].step >= it[i].min) v -= it[i].step; else if (minus) v = it[i].min;
                if (plus && v + it[i].step <= it[i].max) v += it[i].step; else if (plus) v = it[i].max;
                if (v != it[i].val) { it[i].val = v; store_int(i, v); if (it[i].cb) it[i].cb(it[i].key, v); *changed = true; }
                y += 42; break; }
            case IK_CHOICE: {
                cui_text(r->c, r->lbl, r->x, y + 6, it[i].label, r->text);
                int x = r->x + 300, idx = 0; const char *p = it[i].opts;
                while (*p) {
                    char o[24]; int n = 0; while (p[n] && p[n] != '|' && n < 23) { o[n] = p[n]; n++; } o[n] = 0;
                    int w = cui_text_width(r->lbl, o) + 24;
                    if (chip(r, x, y, w, 30, o, it[i].val == idx, true, mx, my, click)) { it[i].val = idx; store_int(i, idx); if (it[i].cb) it[i].cb(it[i].key, idx); *changed = true; }
                    x += w + 8; idx++; p += n; if (*p == '|') p++;
                }
                y += 40; break; }
            case IK_COLOR: {
                cui_text(r->c, r->lbl, r->x, y + 6, it[i].label, r->text);
                uint32_t col = (uint32_t)it[i].val; int x = r->x + 300;
                cui_round_rect(r->c, x, y, 44, 30, 6, col); cui_round_rect_outline(r->c, x, y, 44, 30, 6, 1.0f, r->border);
                static const uint32_t pre[8] = { 0x14171F, 0x1E3A5F, 0x2D5A3D, 0x5A2D4A, 0x6B3A1E, 0x3A3A3A, 0x0F2A44, 0xE8ECF5 };
                for (int s = 0; s < 8; s++) {
                    int sx = x + 56 + s * 30; bool hov = mx >= sx && mx < sx + 24 && my >= y && my < y + 30;
                    cui_round_rect(r->c, sx, y, 24, 30, 5, pre[s]); if (hov) cui_round_rect_outline(r->c, sx, y, 24, 30, 5, 1.5f, r->accent);
                    if (hov && click) { it[i].val = (int)pre[s]; store_int(i, it[i].val); if (it[i].cb) ((void (*)(const char *, uint32_t))it[i].cb)(it[i].key, pre[s]); *changed = true; }
                }
                y += 38;
                static const char *const nm[3] = { "Red", "Green", "Blue" };
                for (int ch = 0; ch < 3; ch++) {
                    int sh = 16 - ch * 8; int v = (it[i].val >> sh) & 0xFF; bool minus = false, plus = false;
                    cui_text(r->c, r->lbl, r->x + 16, y + 6, nm[ch], r->muted);
                    char lab[8]; snprintf(lab, sizeof lab, "%d", v); stepper(r, r->x + 300, y, lab, mx, my, click, &minus, &plus);
                    int nv = v; if (minus) nv = v >= 16 ? v - 16 : 0; if (plus) nv = v <= 239 ? v + 16 : 255;
                    if (nv != v) { it[i].val = (it[i].val & ~(0xFF << sh)) | (nv << sh); store_int(i, it[i].val); if (it[i].cb) ((void (*)(const char *, uint32_t))it[i].cb)(it[i].key, (uint32_t)it[i].val); *changed = true; }
                    y += 38;
                }
                y += 4; break; }
            case IK_TEXT: {
                cui_text(r->c, r->lbl, r->x, y + 6, it[i].label, r->text);
                bool foc = ext_focus == i; int x = r->x + 300;
                cui_round_rect(r->c, x, y, 300, 30, 6, foc ? 0x26314A : r->btn);
                cui_text(r->c, r->val, x + 10, y + 7, it[i].text[0] ? it[i].text : " ", r->text);
                if (foc) cui_fill(r->c, x + 10 + cui_text_width(r->val, it[i].text), y + 5, 2, 20, r->accent);
                if (click && mx >= x && mx < x + 300 && my >= y && my < y + 30) ext_focus = i;
                y += 40; break; }
            case IK_BUTTON:
                if (button(r, r->x, y, 220, 32, it[i].label, false, mx, my, click) && it[i].fn) { it[i].fn(); *changed = true; }
                y += 42; break;
            }
        }
        y += 8;
    }
    if (!shown) { cui_text(r->c, r->lbl, r->x, y, "No extension has added settings yet.", r->muted); y += 26; cui_text(r->c, r->small, r->x, y, "Switch plugins on in the Plugins tab, or copy a .c-osll into /system/plugins/settings/.", r->muted); y += 24; }
    return y;
}
static void ext_key(const cos_win_event_t *ev) {
    if (ext_focus < 0 || ext_focus >= nit || it[ext_focus].kind != IK_TEXT) return;
    char *t = it[ext_focus].text; size_t n = strlen(t);
    if (ev->ascii == '\r' || ev->ascii == '\n') { cos_pcfg_set(plugs.pl[it[ext_focus].owner].name, it[ext_focus].key, t); if (it[ext_focus].cbt) it[ext_focus].cbt(it[ext_focus].key, t); ext_focus = -1; }
    else if (ev->ascii == 27) ext_focus = -1;
    else if (ev->ascii == 8 || ev->ascii == 127) { if (n) t[n - 1] = 0; }
    else if (ev->ascii >= 32 && ev->ascii < 127 && n < sizeof it[0].text - 1) { t[n] = ev->ascii; t[n + 1] = 0; }
}

/* ---- the Plugins tab: every program's shipped plugins, switched on/off from here ---- */
static const char *const MGR_APPS[] = { "messenger", "files", "terminal", "studio", "py2c", "settings", "system" };
static const char *const MGR_TITLES[] = { "Message", "File Manager", "Terminal", "C-OS Studio", "Py2C Studio", "Settings (Extensions tab)", "System (applied at every start)" };
static int mgr_draw(row_ctx_t *r, int y, int mx, int my, bool click) {
    for (int a = 0; a < 7; a++) {
        char dir[96], ldir[96]; snprintf(dir, sizeof dir, "/system/plugins/available/%s", MGR_APPS[a]); snprintf(ldir, sizeof ldir, "/system/plugins/%s", MGR_APPS[a]);
        cos_dirent_ex_t av[16], on[16]; int na = cos_listdir_ex(dir, av, 16), no = cos_listdir_ex(ldir, on, 16);
        if (na <= 0) continue;
        y = section(r, MGR_TITLES[a], y);
        int x = r->x;
        for (int i = 0; i < na; i++) {
            if (av[i].is_dir || !strstr(av[i].name, ".c-osll")) continue;
            bool enabled = false; for (int k = 0; k < no; k++) if (!strcmp(on[k].name, av[i].name)) enabled = true;
            char nm[40]; snprintf(nm, sizeof nm, "%s", av[i].name); *strstr(nm, ".c-osll") = 0;
            int w = cui_text_width(r->lbl, nm) + 28;
            if (x + w > r->w - 30) { x = r->x; y += 38; }
            if (chip(r, x, y, w, 30, nm, enabled, true, mx, my, click)) {
                char from[160], to[160]; static char buf[65536];
                snprintf(from, sizeof from, "%s/%s", dir, av[i].name); snprintf(to, sizeof to, "%s/%s", ldir, av[i].name);
                if (enabled) cos_unlink(to);
                else { ssize_t n = cos_read_file(from, buf, sizeof buf); if (n > 0) { cos_mkdir("/system/plugins"); cos_mkdir(ldir); cos_write_file(to, buf, (size_t)n); } }
                cos_plugins_scan(&plugs);
            }
            x += w + 8;
        }
        y += 44;
    }
    return y;
}
