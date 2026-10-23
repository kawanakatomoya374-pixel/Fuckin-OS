/* plugins.c - plugins of C-OS Studio (cos_plugin.h, cos_plugin_studio.h): commands in the Tools menu that edit the open document.
 * /system/plugins/studio/*.c-osll is scanned about once a second. Tools > Plugins... lists what ships in /system/plugins/available/studio and
 * switches each one on (copy) or off (delete). */
#include "studio.h"
#include "cos_plugin.h"
#include "cos_plugin_studio.h"

#define PL_DIR   "/system/plugins/studio"
#define PL_AVAIL "/system/plugins/available/studio"
#define NCMD 24
static struct { char label[48]; void (*fn)(void); int owner; } cmd[NCMD];
static cos_plugins_t plugs;
static studio_api_t api;
static uint32_t gen = 1;                                        /* bumps when the set of commands changes (the menu is rebuilt) */

static int p_add_command(const char *label, void (*fn)(void)) {
    if (plugs.current < 0 || !label || !fn) return -1;
    for (int i = 0; i < NCMD; ++i) if (!cmd[i].fn) { snprintf(cmd[i].label, sizeof cmd[i].label, "%s", label); cmd[i].fn = fn; cmd[i].owner = plugs.current; gen++; return 0; }
    return -1;
}
static int p_has_doc(void) { return app_doc() != NULL; }
static const char *p_doc_path(void) { Doc *d = app_doc(); return d && !d->untitled ? d->path : ""; }
static size_t p_doc_length(void) { Doc *d = app_doc(); return d ? buf_len(&d->b) : 0; }
static size_t p_doc_read(size_t pos, char *out, size_t n) {
    Doc *d = app_doc(); if (!d) return 0;
    size_t len = buf_len(&d->b); if (pos >= len) return 0; if (n > len - pos) n = len - pos;
    buf_copy(&d->b, pos, n, out); return n;
}
static void p_doc_replace(size_t pos, size_t del, const char *ins, size_t n) {
    Doc *d = app_doc(); if (!d) return;
    size_t len = buf_len(&d->b); if (pos > len) pos = len; if (del > len - pos) del = len - pos;
    doc_replace(d, pos, del, ins, n, false); G.dirty_frame = true;
}
static size_t p_caret(void) { Doc *d = app_doc(); return d ? d->caret : 0; }
static size_t p_sel_start(void) { Doc *d = app_doc(); return d ? (d->caret < d->anchor ? d->caret : d->anchor) : 0; }
static size_t p_sel_end(void) { Doc *d = app_doc(); return d ? (d->caret < d->anchor ? d->anchor : d->caret) : 0; }
static void p_set_selection(size_t a, size_t b) { Doc *d = app_doc(); if (!d) return; size_t len = buf_len(&d->b); if (a > len) a = len; if (b > len) b = len; d->anchor = a; d->caret = b; G.dirty_frame = true; }
static size_t p_line_count(void) { Doc *d = app_doc(); return d ? buf_lines(&d->b) : 0; }
static size_t p_line_start(size_t l) { Doc *d = app_doc(); return d ? buf_line_start(&d->b, l) : 0; }
static size_t p_line_of(size_t pos) { Doc *d = app_doc(); return d ? buf_line_of(&d->b, pos) : 0; }
static void p_output(const char *line) { G.panel_open = true; G.panel_tab = PANEL_OUTPUT; app_output_addf("%s\n", line); }
static int p_open_file(const char *path, int line) { app_open_file(path, line); return 0; }
static const char *p_project_root(void) { return G.proj.loaded ? G.proj.root : ""; }
static void p_status(const char *m) { app_status("%s", m); }

static void on_unload(cos_plugins_t *ps, int idx) {
    (void)ps;
    for (int i = 0; i < NCMD; ++i) if (cmd[i].fn && cmd[i].owner == idx) { cmd[i].fn = NULL; gen++; }
}
static void on_change(cos_plugins_t *ps, int idx, int what) {
    char b[200];
    if (what > 0) snprintf(b, sizeof b, "[STUDIO] plugin loaded: %s %s\n", ps->pl[idx].name, ps->pl[idx].version);
    else if (what < 0) snprintf(b, sizeof b, "[STUDIO] plugin rejected: %s (%s)\n", ps->pl[idx].name, ps->pl[idx].why);
    else snprintf(b, sizeof b, "[STUDIO] plugin unloaded\n");
    cos_puts(b); G.dirty_frame = true;
}
void splug_init(void) {
    cos_plugin_base_init(&api.base, p_status);
    api.abi = STUDIOP_ABI; api.size = sizeof api; api.add_command = p_add_command; api.has_doc = p_has_doc; api.doc_path = p_doc_path; api.doc_length = p_doc_length;
    api.doc_read = p_doc_read; api.doc_replace = p_doc_replace; api.caret = p_caret; api.sel_start = p_sel_start; api.sel_end = p_sel_end; api.set_selection = p_set_selection;
    api.line_count = p_line_count; api.line_start = p_line_start; api.line_of = p_line_of; api.output = p_output; api.open_file = p_open_file; api.project_root = p_project_root;
    plugs.app = "studio"; plugs.dir = PL_DIR; plugs.api = &api; plugs.current = -1; plugs.on_unload = on_unload; plugs.on_change = on_change;
}
void splug_poll(void) { cos_plugins_poll(&plugs, 1000); }
void splug_shutdown(void) { cos_plugins_unload_all(&plugs); }
uint32_t splug_generation(void) { return gen; }
int  splug_slots(void) { return NCMD; }
bool splug_live(int i) { return i >= 0 && i < NCMD && cmd[i].fn; }
const char *splug_label(int i) { return cmd[i].label; }
void splug_run(int i) { if (splug_live(i)) { char lb[100]; snprintf(lb, sizeof lb, "[STUDIO] plugin command: %s\n", cmd[i].label); cos_puts(lb); void (*fn)(void) = cmd[i].fn; fn(); G.dirty_frame = true; } }

/* the "Plugins..." list: available plugins with their state; toggled by number */
static struct { char name[48]; bool on; } av[12]; static int nav;
void splug_refresh(void) {
    cos_dirent_ex_t d[16]; int n = cos_listdir_ex(PL_AVAIL, d, 16); nav = 0;
    for (int i = 0; i < n && nav < 12; ++i) {
        if (d[i].is_dir) continue; char *dot = strstr(d[i].name, ".c-osll"); if (!dot) continue; *dot = 0;
        snprintf(av[nav].name, sizeof av[nav].name, "%s", d[i].name); av[nav].on = false;
        for (int k = 0; k < COS_PLUGIN_MAX; ++k) if (plugs.pl[k].state == 1 && !strcmp(plugs.pl[k].name, av[nav].name)) av[nav].on = true;
        nav++;
    }
}
int splug_avail_count(void) { return nav; }
const char *splug_avail_name(int i) { return av[i].name; }
bool splug_avail_on(int i) { return av[i].on; }
void splug_toggle(int i) {
    if (i < 0 || i >= nav) return;
    char from[160], to[160]; static char buf[65536];
    snprintf(from, sizeof from, "%s/%s.c-osll", PL_AVAIL, av[i].name); snprintf(to, sizeof to, "%s/%s.c-osll", PL_DIR, av[i].name);
    if (av[i].on) cos_unlink(to);
    else {
        ssize_t n = cos_read_file(from, buf, sizeof buf);
        if (n <= 0) { app_status("No such plugin"); return; }
        cos_mkdir("/system/plugins"); cos_mkdir(PL_DIR);
        if (cos_write_file(to, buf, (size_t)n) != n) { app_status("Could not enable the plugin"); return; }
    }
    cos_plugins_scan(&plugs); splug_refresh(); G.dirty_frame = true;
}
