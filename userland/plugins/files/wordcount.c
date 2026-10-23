/* wordcount.c - plugin of Files: "Count lines and words" for text files. */
#include "cos_plugin.h"
#include "cos_plugin_files.h"
static const fmp_api_t *A;
static int act(const char *path, char *out, size_t cap) {
    static uint8_t buf[4096];
    unsigned long lines = 0, words = 0, bytes = 0; int inword = 0; uint64_t off = 0; long n;
    while ((n = A->read_file(path, off, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (c == '\n') lines++;
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') inword = 0; else if (!inword) { inword = 1; words++; }
        }
        bytes += (unsigned long)n; off += (uint64_t)n;
        if (n < (long)sizeof buf) break;
    }
    if (n < 0) { A->snprintf(out, cap, "Cannot read the file"); return 1; }
    A->snprintf(out, cap, "Lines: %lu\nWords: %lu\nBytes: %lu", lines, words, bytes);
    return 0;
}
static int init(const cos_plugin_host_t *h) {
    A = (const fmp_api_t *)h->api;
    if (!A || A->abi != FMP_ABI) return -1;
    return A->add_action("Count lines and words", "行数と単語数を数える", FMP_FILES, act);
}
static void fini(void) { A = 0; }
COS_PLUGIN("files", "wordcount", "1.0", init, fini);
