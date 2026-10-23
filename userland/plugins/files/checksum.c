/* checksum.c - plugin of Files: "Checksum (CRC-32)" in the right-click menu of a file. */
#include "cos_plugin.h"
#include "cos_plugin_files.h"
static const fmp_api_t *A;
static uint32_t table[256];
static int act(const char *path, char *out, size_t cap) {
    static uint8_t buf[4096];
    uint32_t crc = 0xFFFFFFFFu; uint64_t off = 0; long n;
    while ((n = A->read_file(path, off, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
        off += (uint64_t)n;
        if (n < (long)sizeof buf) break;
    }
    if (n < 0) { A->snprintf(out, cap, "Cannot read the file"); return 1; }
    A->snprintf(out, cap, "CRC-32: %08X\nSize: %llu bytes", ~crc, (unsigned long long)off);
    return 0;
}
static int init(const cos_plugin_host_t *h) {
    A = (const fmp_api_t *)h->api;
    if (!A || A->abi != FMP_ABI) return -1;
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
    return A->add_action("Checksum (CRC-32)", "チェックサム (CRC-32)", FMP_FILES, act);
}
static void fini(void) { A = 0; }
COS_PLUGIN("files", "checksum", "1.0", init, fini);
