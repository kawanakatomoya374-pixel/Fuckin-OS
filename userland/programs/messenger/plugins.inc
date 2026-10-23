/* plugins.inc - plugins of C-OS Message (see cos_plugin.h and cos_plugin_messenger.h): commands, filters and hooks added and removed while the app runs.
 *
 * /system/plugins/messenger/*.c-osll is scanned about once a second. /plugins lists what is loaded; "/plugins on <name>" copies a plugin that ships with the
 * system (/system/plugins/available/messenger/) into the scanned directory, "/plugins off <name>" removes it again - and the commands appear and disappear at once. */
#include "cos_plugin.h"
#include "cos_plugin_messenger.h"

#define PL_DIR   "/system/plugins/messenger"
#define PL_AVAIL "/system/plugins/available/messenger"
static struct { char name[20]; char help[100]; void (*fn)(const char *); int owner; } pl_cmd[16];
static struct { int (*fn)(char *, size_t); int owner; } pl_out[8];
static struct { void (*fn)(const char *, const char *); int owner; } pl_in[8];
static cos_plugins_t plugs;

static int pl_add_command(const char *name, const char *help, void (*fn)(const char *)) {
    if (plugs.current < 0 || !name || name[0] != '/' || !fn || strlen(name) >= sizeof pl_cmd[0].name) return -1;
    for (int i = 0; i < 16; i++) if (pl_cmd[i].fn && strcmp(pl_cmd[i].name, name) == 0) return -1;
    for (int i = 0; i < 16; i++) if (!pl_cmd[i].fn) { scopy(pl_cmd[i].name, sizeof pl_cmd[i].name, name); scopy(pl_cmd[i].help, sizeof pl_cmd[i].help, help ? help : ""); pl_cmd[i].fn = fn; pl_cmd[i].owner = plugs.current; return 0; }
    return -1;
}
static int pl_add_out(int (*fn)(char *, size_t)) {
    if (plugs.current < 0 || !fn) return -1;
    for (int i = 0; i < 8; i++) if (!pl_out[i].fn) { pl_out[i].fn = fn; pl_out[i].owner = plugs.current; return 0; }
    return -1;
}
static int pl_add_in(void (*fn)(const char *, const char *)) {
    if (plugs.current < 0 || !fn) return -1;
    for (int i = 0; i < 8; i++) if (!pl_in[i].fn) { pl_in[i].fn = fn; pl_in[i].owner = plugs.current; return 0; }
    return -1;
}
static void pl_note(const char *title, const char *body) { char b[1500]; snprintf(b, sizeof b, "%s\n%s", title ? title : "", body ? body : ""); note_add(b); }
static int pl_send(const char *text) { if (cur < 0 || !text || !*text) return -1; send_text(&ct[cur], text); return 0; }
static const char *pl_chat_name(void) { return cur >= 0 ? ct[cur].name : NULL; }
static uint32_t pl_random32(void) { static uint32_t s; if (!s) s = (uint32_t)cos_time_ms() | 1; s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
static const msgr_api_t pl_api = { MSGRP_ABI, sizeof(msgr_api_t), pl_add_command, pl_add_out, pl_add_in, pl_note, set_status, pl_send, pl_chat_name,
                                   cos_time_ms, pl_random32, cos_snprintf };

static void pl_on_unload(cos_plugins_t *ps, int idx) {                 /* everything the plugin registered goes with it */
    (void)ps;
    for (int i = 0; i < 16; i++) if (pl_cmd[i].fn && pl_cmd[i].owner == idx) pl_cmd[i].fn = NULL;
    for (int i = 0; i < 8; i++) { if (pl_out[i].fn && pl_out[i].owner == idx) pl_out[i].fn = NULL; if (pl_in[i].fn && pl_in[i].owner == idx) pl_in[i].fn = NULL; }
}
static void pl_on_change(cos_plugins_t *ps, int idx, int what) {
    char b[200];
    if (what > 0) snprintf(b, sizeof b, "[MESSENGER] plugin loaded: %s %s\n", ps->pl[idx].name, ps->pl[idx].version);
    else if (what < 0) snprintf(b, sizeof b, "[MESSENGER] plugin rejected: %s (%s)\n", ps->pl[idx].name, ps->pl[idx].why);
    else snprintf(b, sizeof b, "[MESSENGER] plugin unloaded\n");
    cos_puts(b);
    if (what > 0) { snprintf(b, sizeof b, "プラグイン「%s」を読み込みました", ps->pl[idx].name); set_status(b); }
    dirty = true;
}
static void plug_init(void) {
    plugs.app = "messenger"; plugs.dir = PL_DIR; plugs.api = &pl_api; plugs.current = -1; plugs.on_unload = pl_on_unload; plugs.on_change = pl_on_change;
}
static void plug_poll(void) { if (cos_plugins_poll(&plugs, 1000)) dirty = true; }
static bool plug_command(const char *name, const char *arg) {
    for (int i = 0; i < 16; i++) if (pl_cmd[i].fn && strcmp(pl_cmd[i].name, name) == 0) { void (*fn)(const char *) = pl_cmd[i].fn; fn(arg ? arg : ""); dirty = true; return true; }
    return false;
}
/* returns true when a plugin swallowed the message */
static bool plug_outgoing(char *text, size_t cap) {
    for (int i = 0; i < 8; i++) if (pl_out[i].fn && pl_out[i].fn(text, cap) != 0) return true;
    return false;
}
static void plug_incoming(contact_t *k, const char *text) {
    for (int i = 0; i < 8; i++) if (pl_in[i].fn) pl_in[i].fn(k->name, text);
}
static bool plug_has_out(void) { for (int i = 0; i < 8; i++) if (pl_out[i].fn) return true; return false; }

/* /plugins, /plugins on <name>, /plugins off <name> */
static void plug_copy_file(const char *from, const char *to) {
    static char buf[65536];
    ssize_t n = cos_read_file(from, buf, sizeof buf);
    if (n <= 0) { set_status("No such plugin"); return; }
    cos_mkdir("/system/plugins"); cos_mkdir(PL_DIR);
    if (cos_write_file(to, buf, (size_t)n) != n) set_status("Could not enable the plugin");
}
static void cmd_plugins(const char *arg) {
    char b[1500]; b[0] = 0;
    char name[60], path[160], src[160];
    if (arg && (!strncmp(arg, "on ", 3) || !strncmp(arg, "off ", 4))) {
        bool on = arg[1] == 'n'; const char *nm = arg + (on ? 3 : 4); while (*nm == ' ') nm++;
        scopy(name, sizeof name, nm);
        if (!strstr(name, ".c-osll")) strlcat(name, ".c-osll", sizeof name);
        snprintf(path, sizeof path, "%s/%s", PL_DIR, name); snprintf(src, sizeof src, "%s/%s", PL_AVAIL, name);
        if (on) plug_copy_file(src, path); else cos_unlink(path);
        cos_plugins_scan(&plugs);                                   /* at once: the command is there (or gone) when this returns */
        dirty = true; arg = NULL;
    }
    size_t o = 0;
    for (int k = 0; k < COS_PLUGIN_MAX; k++) {
        cos_plugin_t *p = &plugs.pl[k];
        if (p->state == 1) {
            o += (size_t)snprintf(b + o, sizeof b - o, "%s %s\t", p->name, p->version);
            for (int i = 0; i < 16; i++) if (pl_cmd[i].fn && pl_cmd[i].owner == k) o += (size_t)snprintf(b + o, sizeof b - o, "%s ", pl_cmd[i].name);
            o += (size_t)snprintf(b + o, sizeof b - o, "\n");
        } else if (p->state == 2) o += (size_t)snprintf(b + o, sizeof b - o, "%s\t(not loaded: %s)\n", p->name, p->why);
    }
    if (!o) snprintf(b, sizeof b, "(no plugins are loaded)\n");
    cos_dirent_ex_t av[16]; int na = cos_listdir_ex(PL_AVAIL, av, 16);
    o = strlen(b);
    if (na > 0) { o += (size_t)snprintf(b + o, sizeof b - o, "# available\n"); for (int i = 0; i < na && o < sizeof b - 60; i++) if (!av[i].is_dir) { av[i].name[strcspn(av[i].name, ".")] = 0; o += (size_t)snprintf(b + o, sizeof b - o, "%s\t/plugins on %s\n", av[i].name, av[i].name); } }
    char nb[1700]; snprintf(nb, sizeof nb, "Plugins\n# loaded now\n%s", b); note_add(nb);
}
