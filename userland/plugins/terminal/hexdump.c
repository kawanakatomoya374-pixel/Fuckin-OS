/* hexdump.c - plugin of the Terminal: hexdump FILE [OFFSET [LENGTH]], and crc32 / sum for checking files. */
#include "cos_plugin.h"
#include "cos_plugin_terminal.h"
#include "cos_plugin_util.h"
static const term_api_t *A;
static int cmd_hexdump(int argc, char **argv, const term_io_t *io) {
    if (argc < 2) { io->print("usage: hexdump FILE [OFFSET [LENGTH]]"); return 1; }
    char abs[256]; io->resolve(argv[1], abs, sizeof abs);
    long off = argc > 2 ? pu_atol(argv[2]) : 0, len = argc > 3 ? pu_atol(argv[3]) : 256;
    if (len > 4096) len = 4096;
    static uint8_t b[4096]; long n = A->base.read_file(abs, (uint64_t)off, b, (size_t)len);
    if (n < 0) { io->print("hexdump: cannot read"); return 1; }
    for (long row = 0; row < n; row += 16) {
        char l[120]; size_t o = (size_t)A->base.snprintf(l, sizeof l, "%08lX ", off + row);
        for (long i = row; i < row + 16; i++) o += (size_t)A->base.snprintf(l + o, sizeof l - o, i < n ? "%02X " : "   ", b[i]);
        l[o++] = ' ';
        for (long i = row; i < row + 16 && i < n; i++) l[o++] = b[i] >= 32 && b[i] < 127 ? (char)b[i] : '.';
        l[o] = 0; io->print(l);
    }
    return 0;
}
static int cmd_crc32(int argc, char **argv, const term_io_t *io) {
    if (argc < 2) { io->print("usage: crc32 FILE..."); return 1; }
    for (int i = 1; i < argc; i++) {
        char abs[256]; io->resolve(argv[i], abs, sizeof abs);
        static uint8_t b[4096]; uint32_t crc = 0xFFFFFFFFu; uint64_t off = 0; long n; int bad = 0;
        while ((n = A->base.read_file(abs, off, b, sizeof b)) > 0) { crc = pu_crc32_update(crc, b, (size_t)n); off += (uint64_t)n; if (n < (long)sizeof b) break; }
        if (n < 0) bad = 1;
        char m[300]; if (bad) A->base.snprintf(m, sizeof m, "%s: cannot read", argv[i]); else A->base.snprintf(m, sizeof m, "%08X  %llu  %s", ~crc, (unsigned long long)off, argv[i]);
        io->print(m);
    }
    return 0;
}
static int init(const cos_plugin_host_t *h) {
    A = (const term_api_t *)h->api;
    if (!A || A->abi != TERMP_ABI) return -1;
    if (A->add_command("hexdump", "hexdump FILE [OFFSET [LENGTH]]   bytes in hex and text", cmd_hexdump)) return -1;
    A->add_command("crc32", "crc32 FILE...   CRC-32 and size of files", cmd_crc32);
    return 0;
}
static void fini(void) { A = 0; }
COS_PLUGIN("terminal", "hexdump", "1.0", init, fini);
