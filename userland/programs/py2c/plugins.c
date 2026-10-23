/* plugins.c - plugins of Py2C Studio (cos_plugin_py2c.h): toolbar buttons and filters around the conversion, added and removed while it runs.
 * the folder /system/plugins/py2c is scanned about once a second; Run > Plugins... (or the Plugins button) switches the ones that ship in
 * /system/plugins/available/py2c on and off. */
#include "p2.h"
#include "cos_plugin.h"
#include "cos_plugin_py2c.h"

#define PL_DIR   "/system/plugins/py2c"
#define PL_AVAIL "/system/plugins/available/py2c"
#define PL_NBTN 10
static struct { char label[24]; void (*fn)(void); int owner; } pl_btn[PL_NBTN];
static struct { int (*fn)(char *, size_t); int owner; } pl_src[4], pl_out[4];
static cos_plugins_t plugs;
static py2c_api_t pl_api;
static struct { char name[40]; int on; } pl_av[10]; static int pl_nav;

static int pl_add_button(const char *label, void (*fn)(void)) {
    if (plugs.current < 0 || !label || !fn) return -1;
    for (int i = 0; i < PL_NBTN; i++) if (!pl_btn[i].fn) { snprintf(pl_btn[i].label, sizeof pl_btn[i].label, "%s", label); pl_btn[i].fn = fn; pl_btn[i].owner = plugs.current; return 0; }
    return -1;
}
static int pl_add_src(int (*fn)(char *, size_t)) { if (plugs.current < 0 || !fn) return -1; for (int i = 0; i < 4; i++) if (!pl_src[i].fn) { pl_src[i].fn = fn; pl_src[i].owner = plugs.current; return 0; } return -1; }
static int pl_add_out(int (*fn)(char *, size_t)) { if (plugs.current < 0 || !fn) return -1; for (int i = 0; i < 4; i++) if (!pl_out[i].fn) { pl_out[i].fn = fn; pl_out[i].owner = plugs.current; return 0; } return -1; }
static const char *pl_source(void) { Doc *d = app_doc(); return d ? d->t : ""; }
static size_t pl_source_len(void) { Doc *d = app_doc(); return d ? d->len : 0; }
static void pl_set_source(const char *t, size_t n) { Doc *d = app_doc(); if (!d || d->readonly) return; doc_group_begin(d); doc_replace(d, 0, d->len, t, n, false); doc_group_end(d); d->caret = d->anchor = n < d->len ? n : d->len; A.dirty = true; }
static void pl_insert(const char *t) { Doc *d = app_doc(); if (!d || d->readonly) return; size_t a, b; doc_sel(d, &a, &b); doc_replace(d, a, b - a, t, strlen(t), false); view_ensure_caret(&A.ed); A.dirty = true; }
static const char *pl_output(void) { return A.cdoc && A.conv_ok == 1 ? A.cdoc->t : ""; }
static const char *pl_path(void) { Doc *d = app_doc(); return d ? d->path : ""; }
static void pl_convert(void) { conv_run(); }
static void pl_status(const char *m) { app_status("%s", m); }
static void pl_on_unload(cos_plugins_t *ps, int idx) {
    (void)ps;
    for (int i = 0; i < PL_NBTN; i++) if (pl_btn[i].fn && pl_btn[i].owner == idx) pl_btn[i].fn = NULL;
    for (int i = 0; i < 4; i++) { if (pl_src[i].fn && pl_src[i].owner == idx) pl_src[i].fn = NULL; if (pl_out[i].fn && pl_out[i].owner == idx) pl_out[i].fn = NULL; }
}
static void pl_on_change(cos_plugins_t *ps, int idx, int what) {
    char b[200];
    if (what > 0) snprintf(b, sizeof b, "[PY2C] plugin loaded: %s %s\n", ps->pl[idx].name, ps->pl[idx].version);
    else if (what < 0) snprintf(b, sizeof b, "[PY2C] plugin rejected: %s (%s)\n", ps->pl[idx].name, ps->pl[idx].why);
    else snprintf(b, sizeof b, "[PY2C] plugin unloaded\n");
    cos_puts(b); A.dirty = true;
}
void plug_init(void) {
    cos_plugin_base_init(&pl_api.base, pl_status);
    pl_api.abi = PY2CP_ABI; pl_api.size = sizeof pl_api; pl_api.add_button = pl_add_button; pl_api.add_source_filter = pl_add_src; pl_api.add_output_filter = pl_add_out;
    pl_api.source = pl_source; pl_api.source_len = pl_source_len; pl_api.set_source = pl_set_source; pl_api.insert = pl_insert; pl_api.output = pl_output; pl_api.path = pl_path; pl_api.convert = pl_convert;
    plugs.app = "py2c"; plugs.dir = PL_DIR; plugs.api = &pl_api; plugs.current = -1; plugs.on_unload = pl_on_unload; plugs.on_change = pl_on_change;
}
void plug_poll(void) { cos_plugins_poll(&plugs, 1000); }
void plug_shutdown(void) { cos_plugins_unload_all(&plugs); }
bool plug_filter_src(char *py, size_t cap) { bool ch = false; for (int i = 0; i < 4; i++) if (pl_src[i].fn && pl_src[i].fn(py, cap)) ch = true; return ch; }
void plug_filter_out(char *c, size_t cap) { for (int i = 0; i < 4; i++) if (pl_out[i].fn) pl_out[i].fn(c, cap); }
int plug_btn_slots(void) { return PL_NBTN; }
bool plug_btn_live(int i) { return i >= 0 && i < PL_NBTN && pl_btn[i].fn; }
const char *plug_btn_label(int i) { return pl_btn[i].label; }
void plug_btn_run(int i) {
    if (!plug_btn_live(i)) return;
    char lb[80]; snprintf(lb, sizeof lb, "[PY2C] plugin button: %s\n", pl_btn[i].label); cos_puts(lb);
    void (*fn)(void) = pl_btn[i].fn; fn(); A.dirty = true;
}
void plug_refresh(void) {
    cos_dirent_ex_t d[16]; int n = cos_listdir_ex(PL_AVAIL, d, 16); pl_nav = 0;
    for (int i = 0; i < n && pl_nav < 10; i++) {
        if (d[i].is_dir) continue; char *dot = strstr(d[i].name, ".c-osll"); if (!dot) continue; *dot = 0;
        snprintf(pl_av[pl_nav].name, sizeof pl_av[pl_nav].name, "%s", d[i].name); pl_av[pl_nav].on = 0;
        for (int k = 0; k < COS_PLUGIN_MAX; k++) if (plugs.pl[k].state == 1 && !strcmp(plugs.pl[k].name, pl_av[pl_nav].name)) pl_av[pl_nav].on = 1;
        pl_nav++;
    }
}
int plug_avail_count(void) { return pl_nav; }
const char *plug_avail_name(int i) { return pl_av[i].name; }
bool plug_avail_on(int i) { return pl_av[i].on != 0; }
void plug_toggle(int i) {
    if (i < 0 || i >= pl_nav) return;
    char from[160], to[160]; static char buf[65536];
    snprintf(from, sizeof from, "%s/%s.c-osll", PL_AVAIL, pl_av[i].name); snprintf(to, sizeof to, "%s/%s.c-osll", PL_DIR, pl_av[i].name);
    if (pl_av[i].on) cos_unlink(to);
    else {
        ssize_t n = cos_read_file(from, buf, sizeof buf); if (n <= 0) { app_status("No such plugin"); return; }
        cos_mkdir("/system/plugins"); cos_mkdir(PL_DIR);
        if (cos_write_file(to, buf, (size_t)n) != n) { app_status("Could not enable the plugin"); return; }
    }
    cos_plugins_scan(&plugs); plug_refresh(); A.dirty = true;
}
