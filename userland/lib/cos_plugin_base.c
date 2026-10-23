/* cos_plugin_base.c - the host table every program gives its plugins (see cos_plugin_base.h) and the settings store behind it. */
#include "cos.h"
#include "cos_plugin_base.h"

#define CFG_MAX 2048

static void cfg_path(const char *plugin, char *out, size_t cap) { cos_snprintf(out, cap, "%s/%s.cfg", COS_PLUGIN_CONFIG_DIR, plugin); }
static int valid_name(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.')) return 0;
    return 1;
}
int cos_pcfg_get(const char *plugin, const char *key, char *out, size_t cap) {
    static char buf[CFG_MAX]; char path[160];
    if (!valid_name(plugin) || !valid_name(key) || !out || !cap) return -1;
    cfg_path(plugin, path, sizeof path);
    ssize_t n = cos_read_file(path, buf, sizeof buf - 1);
    if (n <= 0) return -1;
    buf[n] = 0;
    size_t kl = cos_strlen(key);
    for (char *l = buf; *l; ) {
        char *e = l; while (*e && *e != '\n') e++;
        if ((size_t)(e - l) > kl && !cos_memcmp(l, key, kl) && l[kl] == '=') {
            size_t vl = (size_t)(e - l) - kl - 1; if (vl >= cap) vl = cap - 1;
            cos_memcpy(out, l + kl + 1, vl); out[vl] = 0; return 0;
        }
        l = *e ? e + 1 : e;
    }
    return -1;
}
int cos_pcfg_set(const char *plugin, const char *key, const char *value) {
    static char buf[CFG_MAX], nb[CFG_MAX]; char path[160];
    if (!valid_name(plugin) || !valid_name(key) || !value) return -1;
    for (const char *v = value; *v; v++) if (*v == '\n') return -1;
    cos_mkdir("/system/plugins"); cos_mkdir(COS_PLUGIN_CONFIG_DIR);
    cfg_path(plugin, path, sizeof path);
    ssize_t n = cos_read_file(path, buf, sizeof buf - 1); if (n < 0) n = 0; buf[n] = 0;
    size_t kl = cos_strlen(key), o = 0;
    for (char *l = buf; *l; ) {                                          /* keep every other line */
        char *e = l; while (*e && *e != '\n') e++;
        size_t len = (size_t)(e - l);
        int mine = len > kl && !cos_memcmp(l, key, kl) && l[kl] == '=';
        if (!mine && len && o + len + 2 < sizeof nb) { cos_memcpy(nb + o, l, len); o += len; nb[o++] = '\n'; }
        l = *e ? e + 1 : e;
    }
    size_t vl = cos_strlen(value);
    if (o + kl + vl + 3 >= sizeof nb) return -1;
    cos_memcpy(nb + o, key, kl); o += kl; nb[o++] = '='; cos_memcpy(nb + o, value, vl); o += vl; nb[o++] = '\n';
    return cos_write_file(path, nb, o) == (ssize_t)o ? 0 : -1;
}

static void (*s_status)(const char *);
static void b_status(const char *m) { if (s_status) s_status(m ? m : ""); }
static long b_read(const char *path, uint64_t off, void *buf, size_t cap) {
    int fd = cos_open(path, COS_O_RDONLY); if (fd < 0) return -1;
    if (off && cos_lseek(fd, (int64_t)off) < 0) { cos_close(fd); return -1; }
    ssize_t n = cos_fd_read(fd, buf, cap); cos_close(fd); return (long)n;
}
static int b_write(const char *path, const void *buf, size_t n) { return cos_write_file(path, buf, n) == (ssize_t)n ? 0 : -1; }
static int64_t b_size(const char *path) { cos_stat_ex_t st; return cos_stat_ex(path, &st) == 0 ? (int64_t)st.size : -1; }
static void b_log(const char *l) { cos_puts(l); }
static uint32_t b_random(void) { static uint32_t s; if (!s) s = (uint32_t)cos_time_ms() | 1; s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
static int b_datetime(int *y, int *mo, int *d, int *h, int *mi, int *s) {
    cos_datetime_t t; if (cos_rtc_now(&t)) return -1;
    if (y) *y = t.year; if (mo) *mo = t.month; if (d) *d = t.day; if (h) *h = t.hour; if (mi) *mi = t.minute; if (s) *s = t.second; return 0;
}
void cos_plugin_base_init(cos_pbase_t *o, void (*status)(const char *msg)) {
    s_status = status;
    o->abi = COS_PBASE_ABI; o->size = sizeof *o; o->status = b_status; o->now_ms = cos_time_ms; o->snprintf = cos_snprintf;
    o->read_file = b_read; o->write_file = b_write; o->file_size = b_size; o->cfg_get = cos_pcfg_get; o->cfg_set = cos_pcfg_set;
    o->log = b_log; o->random32 = b_random; o->datetime = b_datetime;
}
