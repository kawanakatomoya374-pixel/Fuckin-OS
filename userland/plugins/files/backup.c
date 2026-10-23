/* backup.c - plugin of Files: "Make backup copy" writes <name>.bak next to the file (works on files up to 64 KiB). */
#include "cos_plugin.h"
#include "cos_plugin_files.h"
static const fmp_api_t *A;
static int act(const char *path, char *out, size_t cap) {
    static uint8_t buf[65536];
    int64_t sz = A->file_size(path);
    if (sz < 0 || sz > (int64_t)sizeof buf) { A->snprintf(out, cap, "Too large for a quick backup (limit 64 KiB)"); return 1; }
    long n = sz ? A->read_file(path, 0, buf, sizeof buf) : 0;
    if (n < 0) { A->snprintf(out, cap, "Cannot read the file"); return 1; }
    char dst[512]; A->snprintf(dst, sizeof dst, "%s.bak", path);
    if (A->write_file(dst, buf, (size_t)n) != 0) { A->snprintf(out, cap, "Cannot write %s", dst); return 1; }
    A->snprintf(out, cap, "Saved a copy:\n%s\n(%ld bytes)", dst, n);
    return 0;
}
static int init(const cos_plugin_host_t *h) {
    A = (const fmp_api_t *)h->api;
    if (!A || A->abi != FMP_ABI) return -1;
    return A->add_action("Make backup copy", "バックアップを作成", FMP_FILES, act);
}
static void fini(void) { A = 0; }
COS_PLUGIN("files", "backup", "1.0", init, fini);
