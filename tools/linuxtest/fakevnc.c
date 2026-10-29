/* a fake X server for testing linuxgui: RFB 3.8, no security, a 1000x640 gradient; logs the key events it receives to /tmp/keys.log, answers a non-incremental request with the gradient again */
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
static void logf_(const char *fmt, ...) { FILE *f = fopen("/tmp/keys.log", "a"); if (!f) return; va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fclose(f); }
#define W 1000
#define H 640
static int rd(int s, void *b, int n) { char *p = b; int g = 0; while (g < n) { int r = read(s, p + g, n - g); if (r <= 0) return 0; g += r; } return 1; }
static void put16(uint8_t *p, int v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t px(int x, int y) { return ((x * 255 / W) << 16) | ((y * 255 / H) << 8) | 0x60; }
int main(void) {
    FILE *lg = (FILE *)1; { FILE *f = fopen("/tmp/keys.log", "w"); if (f) fclose(f); }
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons(5901); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int br = bind(ls, (void *)&a, sizeof a); int e1 = errno; if (br || listen(ls, 1)) { logf_("bind/listen failed errno %d\n", errno); return 1; }
    int s = accept(ls, 0, 0); logf_("accepted\n");
    write(s, "RFB 003.008\n", 12); char v[12]; rd(s, v, 12);
    uint8_t sec[2] = { 1, 1 }; write(s, sec, 2); uint8_t ch; rd(s, &ch, 1);
    uint8_t ok[4] = { 0 }; write(s, ok, 4); rd(s, &ch, 1);
    uint8_t si[24 + 4] = { 0 }; put16(si, W); put16(si + 2, H); si[4] = 32; si[5] = 24; si[7] = 1; put16(si + 8, 255); put16(si + 10, 255); put16(si + 12, 255); si[14] = 16; si[15] = 8; put32(si + 20, 4); memcpy(si + 24, "fake", 4);
    write(s, si, 28);
    for (;;) {
        uint8_t t; if (!rd(s, &t, 1)) break;
        if (t == 0) { uint8_t b[19]; rd(s, b, 19); }
        else if (t == 2) { uint8_t b[3]; rd(s, b, 3); int n = (b[1] << 8) | b[2]; uint8_t *e = malloc(n * 4 + 1); rd(s, e, n * 4); free(e); }
        else if (t == 3) {
            uint8_t b[9]; rd(s, b, 9); int inc = b[0], x = (b[1] << 8) | b[2], y = (b[3] << 8) | b[4], w = (b[5] << 8) | b[6], h = (b[7] << 8) | b[8];
            if (!inc) {
                logf_("refresh %d,%d %dx%d\n", x, y, w, h);
                uint8_t hd[16] = { 0, 0, 0, 1 }; put16(hd + 4, x); put16(hd + 6, y); put16(hd + 8, w); put16(hd + 10, h); put32(hd + 12, 0);
                write(s, hd, 16);
                uint32_t *row = malloc(w * 4);
                for (int j = 0; j < h; ++j) { for (int i = 0; i < w; ++i) row[i] = px(x + i, y + j); write(s, row, w * 4); }
                free(row);
            }
        }
        else if (t == 4) { uint8_t b[7]; rd(s, b, 7); uint32_t k = (uint32_t)b[3] << 24 | b[4] << 16 | b[5] << 8 | b[6]; logf_("key %s %08x\n", b[0] ? "down" : "up", k); }
        else if (t == 5) { uint8_t b[5]; rd(s, b, 5); }
        else if (t == 6) { uint8_t b[7]; rd(s, b, 7); uint32_t n = (uint32_t)b[3] << 24 | b[4] << 16 | b[5] << 8 | b[6]; char *d = malloc(n + 1); rd(s, d, n); free(d); }
        else { logf_("unknown %d\n", t); break; }
    }
    logf_("closed\n");
    for (;;) sleep(100);
}
