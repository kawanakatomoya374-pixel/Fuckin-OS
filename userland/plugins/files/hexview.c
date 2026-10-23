/* hexview.c - plugin of Files: the preview pane shows a hex dump of binary files (.bin .dat .c-os .c-osll .c-osys). */
#include "cos_plugin.h"
#include "cos_plugin_files.h"
static const fmp_api_t *A;
static int view(const char *path, char *out, size_t cap) {
    uint8_t b[64];
    long n = A->read_file(path, 0, b, sizeof b);
    if (n < 0) return 1;
    size_t o = 0;
    for (long row = 0; row < n && o + 60 < cap; row += 8) {
        o += (size_t)A->snprintf(out + o, cap - o, "%04lX ", row);
        for (long i = row; i < row + 8; i++) o += (size_t)A->snprintf(out + o, cap - o, i < n ? "%02X " : "   ", b[i]);
        for (long i = row; i < row + 8 && i < n; i++) o += (size_t)A->snprintf(out + o, cap - o, "%c", b[i] >= 32 && b[i] < 127 ? b[i] : '.');
        o += (size_t)A->snprintf(out + o, cap - o, "\n");
    }
    return 0;
}
static int init(const cos_plugin_host_t *h) {
    A = (const fmp_api_t *)h->api;
    if (!A || A->abi != FMP_ABI) return -1;
    return A->add_viewer(".bin;.dat;.c-os;.c-osll;.c-osys", view);
}
static void fini(void) { A = 0; }
COS_PLUGIN("files", "hexview", "1.0", init, fini);
