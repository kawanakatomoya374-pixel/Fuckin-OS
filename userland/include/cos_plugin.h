/* cos_plugin.h - plugins for running programs: features added (and taken away) while the program is open.
 *
 * A plugin is a shared library (.c-osll) in /system/plugins/<app>/. The program scans that directory while it runs: a library that appears is loaded and
 * initialised (its constructors run, its init function registers what it adds - slash commands, menu items, viewers), one that disappears is shut down and
 * unloaded again (its registrations are removed first, then its destructors run and its memory is returned), one that changes is reloaded. Nothing is
 * restarted. What a plugin may do is what the program's own table of functions (`api`) offers, and nothing more: it runs inside the program, with the program's
 * capabilities, so the directory is the trust boundary - only the system can put files there.
 *
 *   plugin side:    #include "cos_plugin.h"  and  "cos_plugin_files.h" (or _messenger)
 *                   static int  my_init(const cos_plugin_host_t *h) { const fmp_api_t *api = h->api; ... return 0; }
 *                   static void my_fini(void) {}
 *                   COS_PLUGIN("files", "checksum", "1.0", my_init, my_fini);
 *   program side:   static cos_plugins_t plugins = { .app = "files", .dir = "/system/plugins/files", .api = &my_api, .on_unload = drop_registrations, ... };
 *                   ... cos_plugins_scan(&plugins) every second or so ...
 */
#ifndef COS_PLUGIN_H
#define COS_PLUGIN_H
#include <stdint.h>
#include <stddef.h>

#define COS_PLUGIN_ABI    1u
#define COS_PLUGIN_MAGIC  0x474C5043u          /* "CPLG" */
#define COS_PLUGIN_MAX    8
#define COS_PLUGIN_ROOT   "/system/plugins"    /* + "/<app>" is scanned; "/system/plugins/available/<app>" holds the ones that ship with the system */

typedef struct cos_plugin_host {
    uint32_t    abi, size;
    const char *app;                            /* the program's name, e.g. "files" */
    const void *api;                            /* the program's own table of functions (see the program's cos_plugin_<app>.h) */
} cos_plugin_host_t;

typedef struct cos_plugin_info {
    uint32_t    magic, abi;
    const char *app, *name, *version;
    int  (*init)(const cos_plugin_host_t *host);   /* 0 = fine; anything else: the plugin is not loaded */
    void (*fini)(void);                            /* before the library is unloaded: stop timers, release what it holds */
} cos_plugin_info_t;

/* in a plugin: exports the descriptor the program looks for */
#define COS_PLUGIN(appname, plugname, plugversion, initfn, finifn) \
    __attribute__((visibility("default"), used)) const cos_plugin_info_t cos_plugin_info = { COS_PLUGIN_MAGIC, COS_PLUGIN_ABI, appname, plugname, plugversion, initfn, finifn }

/* ---- program side ---- */
typedef struct {
    char     path[160], name[48], version[16];
    int64_t  handle;
    const cos_plugin_info_t *info;
    uint64_t size, mtime;                       /* what the file was like when it was loaded (a change reloads it) */
    int8_t   state;                             /* 0 free, 1 loaded, 2 rejected (not retried until the file changes) */
    char     why[80];                           /* for a rejected one */
} cos_plugin_t;

typedef struct cos_plugins {
    const char *app, *dir;
    const void *api;
    /* index `idx` is about to run its init (registrations made now belong to it) / is going away (drop what it registered) / has been loaded / changed state */
    void (*on_begin)(struct cos_plugins *ps, int idx);
    void (*on_unload)(struct cos_plugins *ps, int idx);
    void (*on_change)(struct cos_plugins *ps, int idx, int loaded);     /* after a load (1), an unload (0) or a rejection (-1) - for log lines and redraws */
    int  current;                               /* the plugin whose init runs now, or -1 */
    cos_plugin_t pl[COS_PLUGIN_MAX];
    uint64_t last_scan;
} cos_plugins_t;

/* Looks at the directory once: loads what is new, reloads what changed, unloads what is gone. Returns the number of changes. Cheap enough to call every second. */
int  cos_plugins_scan(cos_plugins_t *ps);
/* At exit (or to switch plugins off): unloads everything. */
void cos_plugins_unload_all(cos_plugins_t *ps);
/* scan at most every `ms` milliseconds; returns the number of changes (0 when it was too early) */
int  cos_plugins_poll(cos_plugins_t *ps, uint32_t ms);
#endif
