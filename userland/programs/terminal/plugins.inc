/* plugins.inc - plugins of the Terminal (cos_plugin_terminal.h): commands and line rewriters added and removed while it runs.
 * /system/plugins/terminal/*.c-osll is scanned about once a second. "plugin" lists them; "plugin on|off NAME" switches one of the shipped plugins
 * (/system/plugins/available/terminal/) on or off. */
#include "cos_plugin.h"
#include "cos_plugin_terminal.h"

#define PL_DIR   "/system/plugins/terminal"
#define PL_AVAIL "/system/plugins/available/terminal"
static struct { char name[24], help[110]; int (*fn)(int, char **, const term_io_t *); int owner; } pl_cmd[24];
static struct { int (*fn)(const char *, char *, size_t); int owner; } pl_rw[4];
static cos_plugins_t plugs;

static int pl_add_command(const char *name, const char *help, int (*fn)(int, char **, const term_io_t *)) {
    if (plugs.current < 0 || !name || !*name || !fn || strlen(name) >= sizeof pl_cmd[0].name) return -1;
    for (int i = 0; i < 24; i++) if (pl_cmd[i].fn && !strcmp(pl_cmd[i].name, name)) return -1;
    for (int i = 0; i < 24; i++) if (!pl_cmd[i].fn) { snprintf(pl_cmd[i].name, sizeof pl_cmd[i].name, "%s", name); snprintf(pl_cmd[i].help, sizeof pl_cmd[i].help, "%s", help ? help : ""); pl_cmd[i].fn = fn; pl_cmd[i].owner = plugs.current; return 0; }
    return -1;
}
static int pl_add_rewriter(int (*fn)(const char *, char *, size_t)) {
    if (plugs.current < 0 || !fn) return -1;
    for (int i = 0; i < 4; i++) if (!pl_rw[i].fn) { pl_rw[i].fn = fn; pl_rw[i].owner = plugs.current; return 0; }
    return -1;
}
static term_api_t pl_api;
static void pl_status(const char *m) { log_line(m); }
static void pl_resolve(const char *in, char *out, size_t cap) { path_join(in, out, cap); }
static void pl_set_cwd(const char *abs) { snprintf(cwd, sizeof cwd, "%s", abs); }
static void pl_on_unload(cos_plugins_t *ps, int idx) {
    (void)ps;
    for (int i = 0; i < 24; i++) if (pl_cmd[i].fn && pl_cmd[i].owner == idx) pl_cmd[i].fn = NULL;
    for (int i = 0; i < 4; i++) if (pl_rw[i].fn && pl_rw[i].owner == idx) pl_rw[i].fn = NULL;
}
static void pl_on_change(cos_plugins_t *ps, int idx, int what) {
    char b[200];
    if (what > 0) snprintf(b, sizeof b, "[TERMINAL] plugin loaded: %s %s\n", ps->pl[idx].name, ps->pl[idx].version);
    else if (what < 0) snprintf(b, sizeof b, "[TERMINAL] plugin rejected: %s (%s)\n", ps->pl[idx].name, ps->pl[idx].why);
    else snprintf(b, sizeof b, "[TERMINAL] plugin unloaded\n");
    cos_puts(b); redraw();
}
static void plug_init(void) {
    cos_plugin_base_init(&pl_api.base, pl_status);
    pl_api.abi = TERMP_ABI; pl_api.size = sizeof pl_api; pl_api.add_command = pl_add_command; pl_api.add_rewriter = pl_add_rewriter;
    plugs.app = "terminal"; plugs.dir = PL_DIR; plugs.api = &pl_api; plugs.current = -1; plugs.on_unload = pl_on_unload; plugs.on_change = pl_on_change;
}
static void plug_poll(void) { cos_plugins_poll(&plugs, 1000); }
/* a line before it runs: the rewriters may change it */
static void plug_rewrite(const char *in, char *out, size_t cap) {
    snprintf(out, cap, "%s", in);
    for (int i = 0; i < 4; i++) if (pl_rw[i].fn) { char t[CMD_CAP]; if (pl_rw[i].fn(out, t, sizeof t)) snprintf(out, cap, "%s", t); }
}
static void io_print(const char *s) { log_text(s); char lb[600]; snprintf(lb, sizeof lb, "[TERMINAL] pout: %.500s\n", s); for (char *c = lb; *c; c++) if (*c == '\n' && c[1]) *c = '|'; cos_puts(lb); }
static bool plug_command(int n, char **v) {
    for (int i = 0; i < 24; i++) if (pl_cmd[i].fn && !strcmp(pl_cmd[i].name, v[0])) {
        term_io_t io = { io_print, cwd, pl_resolve, pl_set_cwd };
        int rc = pl_cmd[i].fn(n, v, &io);
        if (rc) logf("%s: failed (%d)", v[0], rc);
        return true;
    }
    return false;
}
static void plug_help(void) { for (int i = 0; i < 24; i++) if (pl_cmd[i].fn) logf("%-16s %s", pl_cmd[i].name, pl_cmd[i].help); }
static void plug_manage(int n, char **v) {
    if (n > 2 && (!strcmp(v[1], "on") || !strcmp(v[1], "off"))) {
        bool on = v[1][1] == 'n'; char name[64], from[160], to[160]; static char buf[65536];
        snprintf(name, sizeof name, "%s%s", v[2], strstr(v[2], ".c-osll") ? "" : ".c-osll");
        snprintf(from, sizeof from, "%s/%s", PL_AVAIL, name); snprintf(to, sizeof to, "%s/%s", PL_DIR, name);
        if (on) {
            ssize_t r = cos_read_file(from, buf, sizeof buf);
            if (r <= 0) log_line("plugin: no such plugin (plugin list)");
            else { cos_mkdir("/system/plugins"); cos_mkdir(PL_DIR); if (cos_write_file(to, buf, (size_t)r) != r) log_line("plugin: cannot enable it"); }
        } else if (cos_unlink(to) != 0) log_line("plugin: it is not enabled");
        cos_plugins_scan(&plugs);
    }
    log_line("loaded:");
    for (int k = 0; k < COS_PLUGIN_MAX; k++) {
        cos_plugin_t *p = &plugs.pl[k];
        if (p->state == 1) { logf("  %s %s", p->name, p->version); for (int i = 0; i < 24; i++) if (pl_cmd[i].fn && pl_cmd[i].owner == k) logf("      %s", pl_cmd[i].name); }
        else if (p->state == 2) logf("  %s (not loaded: %s)", p->name, p->why);
    }
    cos_dirent_ex_t av[24]; int na = cos_listdir_ex(PL_AVAIL, av, 24);
    log_line("available (plugin on NAME / plugin off NAME):");
    for (int i = 0; i < na; i++) if (!av[i].is_dir) { av[i].name[strcspn(av[i].name, ".")] = 0; logf("  %s", av[i].name); }
}
