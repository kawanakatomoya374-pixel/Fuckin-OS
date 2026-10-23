/* plugins.c - plugins of the File Manager (see cos_plugin.h, cos_plugin_files.h): context-menu commands and preview viewers added and removed while Files runs.
 *
 * /system/plugins/files/*.c-osll is scanned about once a second. Help > Plugins... lists what ships in /system/plugins/available/files/ and switches each on
 * (copy into the scanned folder) or off (delete it) - the menu entries appear and disappear without restarting. */
#include "files.h"
#include "cos_plugin.h"
#include "cos_plugin_files.h"

#define PL_DIR   "/system/plugins/files"
#define PL_AVAIL "/system/plugins/available/files"
#define NACT 12
#define NVIEW 6

static struct { char en[40], ja[40]; int flags; int (*fn)(const char *, char *, size_t); int owner; } act[NACT];
static struct { char exts[64]; int (*fn)(const char *, char *, size_t); int owner; } view[NVIEW];
static cos_plugins_t plugs;
static struct { char name[48]; bool on; } av[8];
static int nav;

static int p_add_action(const char *en, const char *ja, int flags, int (*fn)(const char *, char *, size_t)) {
    if (plugs.current < 0 || !fn || !en) return -1;
    for (int i = 0; i < NACT; ++i) if (!act[i].fn) {
        snprintf(act[i].en, sizeof act[i].en, "%s", en); snprintf(act[i].ja, sizeof act[i].ja, "%s", ja ? ja : en);
        act[i].flags = flags ? flags : FMP_FILES; act[i].fn = fn; act[i].owner = plugs.current; return 0;
    }
    return -1;
}
static int p_add_viewer(const char *exts, int (*fn)(const char *, char *, size_t)) {
    if (plugs.current < 0 || !fn || !exts) return -1;
    for (int i = 0; i < NVIEW; ++i) if (!view[i].fn) { snprintf(view[i].exts, sizeof view[i].exts, "%s", exts); view[i].fn = fn; view[i].owner = plugs.current; return 0; }
    return -1;
}
static void p_status(const char *m) { status("%s", m ? m : ""); }
static const char *p_cwd(void) { return G.path; }
static long p_read(const char *path, uint64_t off, void *buf, size_t cap) {
    int fd = cos_open(path, COS_O_RDONLY); if (fd < 0) return -1;
    if (off && cos_lseek(fd, (int64_t)off) < 0) { cos_close(fd); return -1; }
    ssize_t n = cos_fd_read(fd, buf, cap); cos_close(fd); return (long)n;
}
static int p_write(const char *path, const void *buf, size_t n) { return cos_write_file(path, buf, n) == (ssize_t)n ? 0 : -1; }
static int64_t p_size(const char *path) { cos_stat_ex_t st; return cos_stat_ex(path, &st) == 0 ? (int64_t)st.size : -1; }
static const fmp_api_t api = { FMP_ABI, sizeof(fmp_api_t), p_add_action, p_add_viewer, p_status, p_cwd, p_read, p_write, p_size, snprintf, cos_time_ms };

static void on_unload(cos_plugins_t *ps, int idx) {
    (void)ps;
    for (int i = 0; i < NACT; ++i) if (act[i].fn && act[i].owner == idx) act[i].fn = NULL;
    for (int i = 0; i < NVIEW; ++i) if (view[i].fn && view[i].owner == idx) view[i].fn = NULL;
    G.pv_text_path[0] = 0;                                                  /* a viewer may be gone: redo the preview */
}
static void on_change(cos_plugins_t *ps, int idx, int what) {
    char b[200];
    if (what > 0) snprintf(b, sizeof b, "[FILES] plugin loaded: %s %s\n", ps->pl[idx].name, ps->pl[idx].version);
    else if (what < 0) snprintf(b, sizeof b, "[FILES] plugin rejected: %s (%s)\n", ps->pl[idx].name, ps->pl[idx].why);
    else snprintf(b, sizeof b, "[FILES] plugin unloaded\n");
    cos_puts(b);
    G.pv_text_path[0] = 0; G.dirty = true;
}
void fplug_init(void) {
    plugs.app = "files"; plugs.dir = PL_DIR; plugs.api = &api; plugs.current = -1; plugs.on_unload = on_unload; plugs.on_change = on_change;
}
void fplug_poll(void) { if (cos_plugins_poll(&plugs, 1000)) G.dirty = true; }
void fplug_shutdown(void) { cos_plugins_unload_all(&plugs); }

/* ---- actions ---- */
int  fplug_action_slots(void) { return NACT; }
bool fplug_action_live(int i) { return i >= 0 && i < NACT && act[i].fn; }
const char *fplug_action_label(int i) { return G.jp ? act[i].ja : act[i].en; }
bool fplug_action_enabled(int i) {
    if (!fplug_action_live(i) || sel_count() != 1) return false;
    for (int k = 0; k < G.nvis; ++k) { Ent *e = ent_at(k); if (e->sel) return (e->is_dir ? FMP_DIRS : FMP_FILES) & act[i].flags; }
    return false;
}
void fplug_run_action(int i) {
    if (!fplug_action_live(i)) return;
    for (int k = 0; k < G.nvis; ++k) {
        Ent *e = ent_at(k);
        if (!e->sel) continue;
        char p[PATHN]; path_join(p, sizeof p, G.path, e->name);
        snprintf(G.pg_title, sizeof G.pg_title, "%s", fplug_action_label(i));
        G.pg_text[0] = 0;
        int r = act[i].fn(p, G.pg_text, sizeof G.pg_text);
        if (!G.pg_text[0]) snprintf(G.pg_text, sizeof G.pg_text, "%s", r ? "Failed" : "Done");
        { char lb[200]; snprintf(lb, sizeof lb, "[FILES] plugin action '%s' rc=%d: %.60s\n", act[i].en, r, G.pg_text); for (char *c = lb; *c; ++c) if (*c == '\n' && c[1]) *c = '|'; cos_puts(lb); }
        G.pg_text[sizeof G.pg_text - 1] = 0;
        dir_load(true); open_dialog(D_TEXT);
        return;
    }
}

/* ---- viewers ---- */
const char *fplug_view(const char *path, char *out, size_t cap) {
    size_t pl = strlen(path);
    for (int i = 0; i < NVIEW; ++i) {
        if (!view[i].fn) continue;
        const char *s = view[i].exts;
        while (*s) {
            size_t n = strcspn(s, ";");
            if (n && n <= pl && strncasecmp(path + pl - n, s, n) == 0) { out[0] = 0; if (view[i].fn(path, out, cap) == 0) { out[cap - 1] = 0; cos_puts("[FILES] plugin viewer used\n"); return out; } }
            s += n; if (*s == ';') s++;
        }
    }
    return NULL;
}
bool fplug_has_viewer(const char *name) {
    char tmp[2]; (void)tmp; size_t pl = strlen(name);
    for (int i = 0; i < NVIEW; ++i) {
        if (!view[i].fn) continue;
        const char *s = view[i].exts;
        while (*s) { size_t n = strcspn(s, ";"); if (n && n <= pl && strncasecmp(name + pl - n, s, n) == 0) return true; s += n; if (*s == ';') s++; }
    }
    return false;
}

/* ---- the manager dialog ---- */
void fplug_refresh(void) {
    cos_dirent_ex_t d[16]; int n = cos_listdir_ex(PL_AVAIL, d, 16); nav = 0;
    for (int i = 0; i < n && nav < 8; ++i) {
        if (d[i].is_dir) continue;
        char *dot = strstr(d[i].name, ".c-osll"); if (!dot) continue; *dot = 0;
        snprintf(av[nav].name, sizeof av[nav].name, "%s", d[i].name); av[nav].on = false;
        for (int k = 0; k < COS_PLUGIN_MAX; ++k) if (plugs.pl[k].state == 1 && !strcmp(plugs.pl[k].name, av[nav].name)) av[nav].on = true;
        nav++;
    }
}
int fplug_avail_count(void) { return nav; }
const char *fplug_avail_name(int i) { return av[i].name; }
bool fplug_avail_on(int i) { return av[i].on; }
void fplug_toggle(int i) {
    if (i < 0 || i >= nav) return;
    char from[160], to[160]; static char buf[65536];
    snprintf(from, sizeof from, "%s/%s.c-osll", PL_AVAIL, av[i].name); snprintf(to, sizeof to, "%s/%s.c-osll", PL_DIR, av[i].name);
    if (av[i].on) cos_unlink(to);
    else {
        ssize_t n = cos_read_file(from, buf, sizeof buf);
        if (n <= 0) { status("No such plugin"); return; }
        cos_mkdir("/system/plugins"); cos_mkdir(PL_DIR);
        if (cos_write_file(to, buf, (size_t)n) != n) { status("Could not enable the plugin"); return; }
    }
    cos_plugins_scan(&plugs);
    fplug_refresh(); G.dirty = true;
}
