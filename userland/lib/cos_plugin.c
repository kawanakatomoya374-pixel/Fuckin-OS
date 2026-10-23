/* cos_plugin.c - the program side of cos_plugin.h: finding, loading, reloading and unloading plugin libraries while the program runs. */
#include "cos.h"
#include "cos_plugin.h"

static bool ends_with(const char *s, const char *suffix) {
    size_t n = cos_strlen(s), m = cos_strlen(suffix);
    return n >= m && cos_strcmp(s + n - m, suffix) == 0;
}
static void copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = 0;
}
/* info->app is one program or a comma list ("settings,system"): a plugin may serve several programs */
static bool app_matches(const char *list, const char *app) {
    size_t n = cos_strlen(app);
    for (const char *p = list; *p; ) {
        size_t k = 0; while (p[k] && p[k] != ',') k++;
        if (k == n && !cos_memcmp(p, app, n)) return true;
        p += k; if (*p == ',') p++;
    }
    return false;
}
static void notify(cos_plugins_t *ps, int idx, int what) { if (ps->on_change) ps->on_change(ps, idx, what); }

static void unload_one(cos_plugins_t *ps, int idx) {
    cos_plugin_t *p = &ps->pl[idx];
    if (p->state == 1) {
        if (ps->on_unload) ps->on_unload(ps, idx);                     /* the program drops what the plugin registered ... */
        if (p->info && p->info->fini) p->info->fini();                 /* ... the plugin stops ... */
        cos_dlclose(p->handle);                                        /* ... and the library is unloaded (destructors, unmapping) */
    }
    int was = p->state;
    cos_memset(p, 0, sizeof *p);
    if (was == 1) notify(ps, idx, 0);
}

static void load_one(cos_plugins_t *ps, int idx, const char *path, const char *name, uint64_t size, uint64_t mtime) {
    cos_plugin_t *p = &ps->pl[idx];
    cos_memset(p, 0, sizeof *p);
    copy(p->path, sizeof p->path, path); copy(p->name, sizeof p->name, name);
    p->size = size; p->mtime = mtime; p->handle = -1;
    p->state = 2;                                                       /* until it has loaded: a failure is remembered */
    int64_t h = cos_dlopen_flags(path, COS_RTLD_LOCAL | COS_RTLD_NOW);  /* local: every plugin exports the same descriptor name */
    if (h <= 0) { const char *e = cos_dlerror(); copy(p->why, sizeof p->why, e ? e : "cannot be loaded"); notify(ps, idx, -1); return; }
    const cos_plugin_info_t *info = (const cos_plugin_info_t *)cos_dlsym(h, "cos_plugin_info");
    const char *why = 0;
    if (!info) why = "no cos_plugin_info";
    else if (info->magic != COS_PLUGIN_MAGIC) why = "not a plugin (bad magic)";
    else if (info->abi != COS_PLUGIN_ABI) why = "built for another plugin ABI";
    else if (!info->app || !app_matches(info->app, ps->app)) why = "made for another program";
    else if (!info->init) why = "no init function";
    if (why) { copy(p->why, sizeof p->why, why); cos_dlclose(h); notify(ps, idx, -1); return; }
    cos_plugin_host_t host = { COS_PLUGIN_ABI, sizeof host, ps->app, ps->api };
    p->handle = h; p->info = info;
    copy(p->name, sizeof p->name, info->name ? info->name : name); copy(p->version, sizeof p->version, info->version ? info->version : "");
    p->state = 1;                                                       /* registrations made in init belong to this index */
    ps->current = idx;
    if (ps->on_begin) ps->on_begin(ps, idx);
    int rc = info->init(&host);
    ps->current = -1;
    if (rc != 0) {
        if (ps->on_unload) ps->on_unload(ps, idx);                      /* whatever it registered before failing */
        cos_dlclose(h);
        cos_memset(p, 0, sizeof *p);
        copy(p->path, sizeof p->path, path); p->size = size; p->mtime = mtime; p->handle = -1; p->state = 2;
        copy(p->name, sizeof p->name, name); copy(p->why, sizeof p->why, "its init refused to load");
        notify(ps, idx, -1);
        return;
    }
    notify(ps, idx, 1);
}

int cos_plugins_scan(cos_plugins_t *ps) {
    static cos_dirent_ex_t ents[64];                                    /* one program scans at a time; the list is only used inside this call */
    int n = cos_listdir_ex(ps->dir, ents, 64), changes = 0;
    if (n < 0) n = 0;                                                   /* no directory: nothing is enabled */
    uint32_t seen = 0;
    for (int i = 0; i < n; ++i) {
        if (ents[i].is_dir || !ends_with(ents[i].name, ".c-osll")) continue;
        char path[160];
        cos_snprintf(path, sizeof path, "%s/%s", ps->dir, ents[i].name);
        int idx = -1, freei = -1;
        for (int k = 0; k < COS_PLUGIN_MAX; ++k) {
            if (ps->pl[k].state && cos_strcmp(ps->pl[k].path, path) == 0) idx = k;
            if (!ps->pl[k].state && freei < 0) freei = k;
        }
        if (idx >= 0) {
            seen |= 1u << idx;
            if (ps->pl[idx].size == ents[i].size && ps->pl[idx].mtime == ents[i].mtime) continue;       /* unchanged (also a rejected one: not retried) */
            unload_one(ps, idx);                                                                          /* changed: load the new version */
            load_one(ps, idx, path, ents[i].name, ents[i].size, ents[i].mtime);
            seen |= 1u << idx; changes++;
        } else if (freei >= 0) {
            load_one(ps, freei, path, ents[i].name, ents[i].size, ents[i].mtime);
            seen |= 1u << freei; changes++;
        }
    }
    for (int k = 0; k < COS_PLUGIN_MAX; ++k)
        if (ps->pl[k].state && !((seen >> k) & 1u)) { unload_one(ps, k); changes++; }                   /* the file is gone */
    return changes;
}

void cos_plugins_unload_all(cos_plugins_t *ps) {
    for (int k = COS_PLUGIN_MAX - 1; k >= 0; --k) if (ps->pl[k].state) unload_one(ps, k);
}

int cos_plugins_poll(cos_plugins_t *ps, uint32_t ms) {
    uint64_t now = cos_time_ms();
    if (ps->last_scan && now - ps->last_scan < ms) return 0;
    ps->last_scan = now ? now : 1;
    return cos_plugins_scan(ps);
}
