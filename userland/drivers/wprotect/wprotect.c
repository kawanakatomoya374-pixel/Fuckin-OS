/* wprotect.c - a SYSTEM-level kernel extension: write protection for a part of the file system.
 *
 * It registers a filter on programs' file operations (cosys_fs_filter_register). While it is loaded, creating, writing, deleting or renaming anything under the
 * protected prefix (default "/protected/") fails for every program - including a fully privileged one - and each refusal is logged. Reading stays allowed.
 *   0x1032  set_prefix(ptr, len)   chooses the prefix (0..127 bytes; empty = protect nothing). Only a fully privileged (unsandboxed) caller may. 0, or -1
 *   0x1033  denied()               how many operations were refused since the module loaded
 * On unload it writes the number of refusals to /tmp/wprotect.last with the module's own file access (cosys_fs_write), which no sandbox or jail restricts. */
#include "cosys.h"

static char g_prefix[128] = "/protected/";
static uint32_t g_denied;
static cosys_lock_t g_lock;

static int under(const char *path, const char *pre, size_t pl) {
    if (!path || pl == 0) return 0;
    size_t i = 0;
    while (i < pl && path[i] == pre[i]) i++;
    if (i == pl) return 1;                                  /* the path starts with the prefix */
    if (i == pl - 1 && pre[pl - 1] == '/' && path[i] == '\0') return 1;      /* the protected directory itself ("/protected" for "/protected/") */
    return 0;
}
static int on_fs(uint32_t op, uint64_t pid, const char *path, const char *path2) {
    if (op == COSYS_FSOP_READ) return 0;
    char pre[sizeof g_prefix];
    uint64_t fl = cosys_lock(&g_lock);
    memcpy(pre, g_prefix, sizeof pre);
    cosys_unlock(&g_lock, fl);
    size_t pl = strlen(pre);
    if (!under(path, pre, pl) && !(op == COSYS_FSOP_RENAME && under(path2, pre, pl))) return 0;
    fl = cosys_lock(&g_lock); g_denied++; cosys_unlock(&g_lock, fl);
    cosys_log_dec("refused a file operation of pid ", pid);
    cosys_log(path);
    return 1;
}
static int64_t set_prefix(uint64_t uptr, uint64_t len, uint64_t c) {
    (void)c;
    if (cosys_current_caps() != 0xFFFFFFFFu || len >= sizeof g_prefix) return -1;
    char tmp[sizeof g_prefix];
    memset(tmp, 0, sizeof tmp);
    if (len && cosys_copy_from_user(tmp, uptr, (size_t)len) != 0) return -1;
    uint64_t fl = cosys_lock(&g_lock); memcpy(g_prefix, tmp, sizeof tmp); cosys_unlock(&g_lock, fl);
    return 0;
}
static int64_t get_denied(uint64_t a, uint64_t b, uint64_t c) { (void)a; (void)b; (void)c; return (int64_t)g_denied; }

static int wp_init(void) {
    if (cosys_fs_filter_register(on_fs) != 0) return -1;
    if (cosys_register_syscall(0x1032, set_prefix, 0) != 0 || cosys_register_syscall(0x1033, get_denied, 0) != 0) { cosys_fs_filter_unregister(on_fs); return -1; }
    cosys_log("write protection active");
    return 0;
}
static void wp_exit(void) {
    cosys_fs_filter_unregister(on_fs);
    cosys_unregister_syscall(0x1032); cosys_unregister_syscall(0x1033);
    char text[40] = "denied=";
    size_t n = 7; char digits[12]; int d = 0; uint32_t v = g_denied;
    do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (d) text[n++] = digits[--d];
    text[n++] = '\n';
    cosys_fs_write("/tmp/wprotect.last", text, n);
}
COSYS_MODULE("wprotect", "1.0", COSYS_LEVEL_SYSTEM, wp_init, wp_exit);
