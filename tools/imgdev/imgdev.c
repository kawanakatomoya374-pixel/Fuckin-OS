/* imgdev.c - decodes the test images in /system/imgdev through the kernel's decoders (SYS_IMAGE_DECODE, the path the Photo app and
 * the Image Viewer use) and reports, per file, the result code, size, time and a 4x4 grid of average colours: "IMGDEV <name> rc=.. w=.. h=.. ms=.. grid=RRGGBB,...".
 * "imgdev leak" decodes the big files repeatedly and reports the kernel heap growth. tools/imgdev/peer.py compares with the Pillow reference. */
#include "cos.h"
#define DIR "/system/imgdev/"
#define BUFPIX (1920u * 1080u)
static uint32_t *buf;

static void report(const char *name, int rc, uint64_t w, uint64_t h, uint64_t ms) {
    char g[16 * 7 + 4]; g[0] = 0;
    if (rc == 0 && w && h && w * h <= BUFPIX) {
        size_t o = 0;
        for (int gy = 0; gy < 4; gy++) for (int gx = 0; gx < 4; gx++) {
            uint64_t x0 = w * gx / 4, x1 = w * (gx + 1) / 4, y0 = h * gy / 4, y1 = h * (gy + 1) / 4, n = 0, r = 0, gg = 0, b = 0;
            for (uint64_t y = y0; y < y1; y++) for (uint64_t x = x0; x < x1; x++) { uint32_t p = buf[y * w + x]; r += (p >> 16) & 255; gg += (p >> 8) & 255; b += p & 255; n++; }
            if (!n) n = 1;
            o += (size_t)cos_snprintf(g + o, sizeof g - o, "%02x%02x%02x%s", (unsigned)((r + n / 2) / n), (unsigned)((gg + n / 2) / n), (unsigned)((b + n / 2) / n), gx + gy < 6 ? "," : "");
        }
    }
    cos_printf("IMGDEV %s rc=%d w=%llu h=%llu ms=%llu grid=%s\n", name, rc, (unsigned long long)w, (unsigned long long)h, (unsigned long long)ms, g);
}
static int decode_one(const char *name, int quiet) {
    char path[160]; cos_snprintf(path, sizeof path, DIR "%s", name);
    cos_image_info_t info = {0, 0};
    for (size_t i = 0; i < BUFPIX; i++) buf[i] = 0xDEADBEEF;
    uint64_t t0 = cos_time_ms();
    int rc = cos_image_decode(path, buf, BUFPIX * 4, &info);
    if (!quiet) report(name, rc, info.width, info.height, cos_time_ms() - t0);
    return rc;
}
static int names(char out[][48], int max) {
    static char txt[2048]; ssize_t n = cos_read_file(DIR "names.txt", txt, sizeof txt - 1);
    int k = 0; if (n <= 0) return 0;
    txt[n] = 0;
    for (char *p = txt; *p && k < max; ) { char *e = p; while (*e && *e != '\n') e++; size_t l = (size_t)(e - p); if (l && l < 47) { memcpy(out[k], p, l); out[k][l] = 0; k++; } p = *e ? e + 1 : e; }
    return k;
}
int main(int argc, char **argv) {
    buf = cos_malloc(BUFPIX * 4);
    if (!buf) { cos_puts("IMGDEV FAIL no memory\n"); return 1; }
    static char nm[40][48]; int n = names(nm, 40);
    if (n == 0) { cos_puts("IMGDEV FAIL no test images in /system/imgdev\n"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "leak")) {
        cos_sysinfo_t a, b;
        for (int i = 0; i < n; i++) decode_one(nm[i], 1);          /* warm up: caches, first-use allocations */
        cos_sysinfo(&a);
        for (int round = 0; round < 3; round++) for (int i = 0; i < n; i++) decode_one(nm[i], 1);
        cos_sysinfo(&b);
        cos_printf("IMGDEV LEAK kheap_used before=%llu after=%llu delta=%lld\n", (unsigned long long)a.kheap_used, (unsigned long long)b.kheap_used, (long long)b.kheap_used - (long long)a.kheap_used);
        cos_puts("IMGDEV DONE\n"); return 0;
    }
    for (int i = 0; i < n; i++) decode_one(nm[i], 0);
    cos_puts("IMGDEV DONE\n");
    return 0;
}
