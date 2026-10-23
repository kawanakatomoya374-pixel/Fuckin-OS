/* textfx.c - a plugin of C-OS Message: an outgoing filter that expands :shrug: :lenny: :tableflip: and <3 in every message you send, and the commands
 * /shout, /rot13 and /mock that send a transformed copy of their argument. */
#include "cos_plugin.h"
#include "cos_plugin_messenger.h"
static const msgr_api_t *A;
static unsigned long replaced;

static size_t len(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int starts(const char *s, const char *p) { while (*p) { if (*s++ != *p++) return 0; } return 1; }
static const struct { const char *from, *to; } MAP[] = {
    { ":shrug:", "¯\\_(ツ)_/¯" }, { ":lenny:", "( ͡° ͜ʖ ͡°)" }, { ":tableflip:", "(╯°□°)╯︵ ┻━┻" }, { "<3", "❤" },
};
/* rewrites text in place (cap bytes at most); returns 0 so the message is still sent */
static int filter(char *text, size_t cap) {
    char out[1024]; size_t o = 0;
    for (size_t i = 0; text[i] && o + 1 < sizeof out; ) {
        int hit = 0;
        for (unsigned k = 0; k < sizeof MAP / sizeof MAP[0]; k++) {
            if (starts(text + i, MAP[k].from)) {
                size_t l = len(MAP[k].to);
                if (o + l + 1 < sizeof out) { for (size_t j = 0; j < l; j++) out[o++] = MAP[k].to[j]; i += len(MAP[k].from); hit = 1; replaced++; }
                break;
            }
        }
        if (!hit) out[o++] = text[i++];
    }
    out[o] = 0;
    if (o < cap) for (size_t i = 0; i <= o; i++) text[i] = out[i];
    return 0;
}
static void shout(const char *arg) { char b[512]; size_t i = 0; for (; arg[i] && i < 500; i++) b[i] = arg[i] >= 'a' && arg[i] <= 'z' ? (char)(arg[i] - 32) : arg[i]; b[i] = 0; if (i && A->send(b)) A->status("Open a chat first"); }
static void rot13(const char *arg) {
    char b[512]; size_t i = 0;
    for (; arg[i] && i < 500; i++) { char c = arg[i]; b[i] = (c >= 'a' && c <= 'z') ? (char)('a' + (c - 'a' + 13) % 26) : (c >= 'A' && c <= 'Z') ? (char)('A' + (c - 'A' + 13) % 26) : c; }
    b[i] = 0; if (i && A->send(b)) A->status("Open a chat first");
}
static void mock(const char *arg) { char b[512]; size_t i = 0; int up = 0; for (; arg[i] && i < 500; i++) { char c = arg[i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c >= 'a' && c <= 'z') { if (up) c = (char)(c - 32); up = !up; } b[i] = c; } b[i] = 0; if (i && A->send(b)) A->status("Open a chat first"); }
static int init(const cos_plugin_host_t *h) {
    A = (const msgr_api_t *)h->api;
    if (!A || A->abi != MSGRP_ABI) return -1;
    if (A->add_outgoing_filter(filter)) return -1;
    A->add_command("/shout", "/shout text\tsend it in capital letters", shout);
    A->add_command("/rot13", "/rot13 text\tsend it ROT13-encoded (and again to decode)", rot13);
    A->add_command("/mock", "/mock text\tsend it aLtErNaTiNg", mock);
    return 0;
}
static void fini(void) { A = 0; }
COS_PLUGIN("messenger", "textfx", "1.0", init, fini);
