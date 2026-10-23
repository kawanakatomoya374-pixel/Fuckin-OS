/* cos_plugin_base.h - the part of the host table every program gives its plugins: messages, file access, a small settings store, and the log.
 *
 * A plugin library has no libc of its own (it is a freestanding .c-osll), so everything it needs comes through these tables.
 * Each program's own table (cos_plugin_terminal.h, cos_plugin_studio.h, ...) starts with a `cos_pbase_t base`.
 *
 * Settings store: /system/plugins/config/<plugin>.cfg holds "key=value" lines. The Settings app's extension pages and the plugins themselves use it,
 * so a value set in Settings is what the plugin reads (and what survives a restart).
 */
#ifndef COS_PLUGIN_BASE_H
#define COS_PLUGIN_BASE_H
#include <stdint.h>
#include <stddef.h>

#define COS_PBASE_ABI 1u
#define COS_PLUGIN_CONFIG_DIR "/system/plugins/config"

typedef struct cos_pbase {
    uint32_t abi, size;
    void     (*status)(const char *msg);                                           /* a one-line message in the program's own status area */
    uint64_t (*now_ms)(void);
    int      (*snprintf)(char *buf, size_t cap, const char *fmt, ...);
    long     (*read_file)(const char *path, uint64_t off, void *buf, size_t cap);  /* bytes read, -1 on error */
    int      (*write_file)(const char *path, const void *buf, size_t n);           /* creates or truncates; 0 on success */
    int64_t  (*file_size)(const char *path);                                       /* -1 when missing */
    int      (*cfg_get)(const char *plugin, const char *key, char *out, size_t cap);   /* 0 when the key exists */
    int      (*cfg_set)(const char *plugin, const char *key, const char *value);       /* 0 on success */
    void     (*log)(const char *line);                                             /* the serial log */
    uint32_t (*random32)(void);
    int      (*datetime)(int *y, int *mo, int *d, int *h, int *mi, int *s);        /* wall clock; 0 on success */
} cos_pbase_t;

/* program side (libcos): the table; `status` is the program's own function (may be NULL) */
void cos_plugin_base_init(cos_pbase_t *out, void (*status)(const char *msg));
int  cos_pcfg_get(const char *plugin, const char *key, char *out, size_t cap);
int  cos_pcfg_set(const char *plugin, const char *key, const char *value);

/* plugin side helpers (header only) */
static inline int pcfg_int(const cos_pbase_t *b, const char *plugin, const char *key, int def) {
    char v[32]; if (b->cfg_get(plugin, key, v, sizeof v) != 0) return def;
    int neg = 0, n = 0, i = 0; if (v[0] == '-') { neg = 1; i = 1; }
    for (; v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + (v[i] - '0');
    return neg ? -n : n;
}
static inline int pcfg_set_int(const cos_pbase_t *b, const char *plugin, const char *key, int value) {
    char v[16]; b->snprintf(v, sizeof v, "%d", value); return b->cfg_set(plugin, key, v);
}
#endif
