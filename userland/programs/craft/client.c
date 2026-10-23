/* client.c - the multiplayer client of the C-OS port of Craft (protocol of the original Craft server, one line per message).
 *
 * The original reads the socket in a second thread; here the socket is non-blocking and polled once per frame by
 * client_recv(), which returns every complete line received so far. Sending is queued and flushed without ever waiting. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cos.h"
#include "client.h"

#define QMAX (1 << 16)
static int s_enabled, s_sd = -1;
static char *s_in; static int s_inlen, s_incap;
static char s_out[QMAX]; static int s_outlen;

void client_enable() { s_enabled = 1; }
void client_disable() { s_enabled = 0; }
int get_client_enabled() { return s_enabled; }
int client_is_connected(void) { return s_sd >= 0; }

void client_connect(char *hostname, int port) {
    if (!s_enabled) return;
    uint32_t ip;
    if (cos_resolve_wait(hostname, &ip, 5000) < 0) { cos_puts("[CRAFT] cannot resolve the server name\n"); return; }
    int sd = cos_tcp_connect(ip, (uint16_t)port, 5000);
    if (sd < 0) { cos_puts("[CRAFT] cannot connect to the server\n"); return; }
    s_sd = sd; s_inlen = 0; s_outlen = 0;
}
void client_start() {}
void client_stop() {
    if (s_sd >= 0) { client_recv(); cos_sock_close(s_sd); s_sd = -1; }
    free(s_in); s_in = NULL; s_inlen = s_incap = s_outlen = 0;
}

static void flush_out(void) {
    while (s_sd >= 0 && s_outlen > 0) {
        ssize_t n = cos_send(s_sd, s_out, (size_t)s_outlen);
        if (n == -COS_EAGAIN) return;
        if (n <= 0) { cos_sock_close(s_sd); s_sd = -1; return; }
        memmove(s_out, s_out + n, (size_t)(s_outlen - n)); s_outlen -= (int)n;
    }
}
void client_send(char *data) {
    if (!s_enabled || s_sd < 0) return;
    int n = (int)strlen(data);
    if (s_outlen + n > QMAX) { flush_out(); if (s_outlen + n > QMAX) return; }
    memcpy(s_out + s_outlen, data, (size_t)n); s_outlen += n;
    flush_out();
}

char *client_recv() {
    if (!s_enabled || s_sd < 0) return NULL;
    flush_out();
    for (int round = 0; round < 16 && s_sd >= 0; ++round) {
        if (s_incap - s_inlen < 4096) {
            int nc = s_incap ? s_incap * 2 : 16384;
            char *p = (char *)realloc(s_in, (size_t)nc);
            if (!p) break;
            s_in = p; s_incap = nc;
        }
        ssize_t n = cos_recv(s_sd, s_in + s_inlen, (size_t)(s_incap - s_inlen));
        if (n == -COS_EAGAIN) break;
        if (n <= 0) { cos_sock_close(s_sd); s_sd = -1; break; }
        s_inlen += (int)n;
    }
    int end = s_inlen;
    while (end > 0 && s_in[end - 1] != '\n') --end;
    if (end == 0) return NULL;
    char *out = (char *)malloc((size_t)end + 1);
    if (!out) return NULL;
    memcpy(out, s_in, (size_t)end); out[end] = 0;
    memmove(s_in, s_in + end, (size_t)(s_inlen - end)); s_inlen -= end;
    return out;
}

void client_version(int version) { char b[32]; snprintf(b, sizeof b, "V,%d\n", version); client_send(b); }
void client_login(const char *username, const char *identity_token) { char b[320]; snprintf(b, sizeof b, "A,%s,%s\n", username, identity_token); client_send(b); }
void client_position(float x, float y, float z, float rx, float ry) {
    static float px, py, pz, prx, pry;
    if (!s_enabled) return;
    float d = (px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z) + (prx - rx) * (prx - rx) + (pry - ry) * (pry - ry);
    if (d < 0.0001f) return;
    px = x; py = y; pz = z; prx = rx; pry = ry;
    char b[128]; snprintf(b, sizeof b, "P,%.2f,%.2f,%.2f,%.2f,%.2f\n", x, y, z, rx, ry); client_send(b);
}
void client_chunk(int p, int q, int key) { char b[64]; snprintf(b, sizeof b, "C,%d,%d,%d\n", p, q, key); client_send(b); }
void client_block(int x, int y, int z, int w) { char b[64]; snprintf(b, sizeof b, "B,%d,%d,%d,%d\n", x, y, z, w); client_send(b); }
void client_light(int x, int y, int z, int w) { char b[64]; snprintf(b, sizeof b, "L,%d,%d,%d,%d\n", x, y, z, w); client_send(b); }
void client_sign(int x, int y, int z, int face, const char *text) { char b[320]; snprintf(b, sizeof b, "S,%d,%d,%d,%d,%s\n", x, y, z, face, text); client_send(b); }
void client_talk(const char *text) {
    if (!s_enabled || !text[0]) return;
    char b[320]; snprintf(b, sizeof b, "T,%s\n", text); client_send(b);
}
