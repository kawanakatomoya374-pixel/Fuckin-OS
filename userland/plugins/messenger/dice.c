/* dice.c - a plugin of C-OS Message: /dice [sides], /coin, /pick a b c ... send a random result to the open chat. */
#include "cos_plugin.h"
#include "cos_plugin_messenger.h"
static const msgr_api_t *A;

static unsigned number(const char *s, unsigned def) {
    unsigned v = 0; int any = 0;
    while (s && *s == ' ') s++;
    for (; s && *s >= '0' && *s <= '9'; s++) { v = v * 10 + (unsigned)(*s - '0'); any = 1; if (v > 100000) break; }
    return any && v >= 2 ? v : def;
}
static void cmd_dice(const char *arg) {
    unsigned sides = number(arg, 6), r = A->random32() % sides + 1;
    char b[96]; A->snprintf(b, sizeof b, "🎲 d%u: %u", sides, r);
    if (A->send(b) != 0) A->status("Open a chat first");
}
static void cmd_coin(const char *arg) {
    (void)arg;
    if (A->send(A->random32() & 1 ? "🪙 heads" : "🪙 tails") != 0) A->status("Open a chat first");
}
static void cmd_pick(const char *arg) {
    char words[16][48]; int n = 0;
    while (arg && *arg && n < 16) {
        while (*arg == ' ') arg++;
        int k = 0; while (*arg && *arg != ' ' && k < 47) words[n][k++] = *arg++;
        words[n][k] = 0; if (k) n++;
    }
    if (n < 2) { A->status("Usage: /pick a b c ..."); return; }
    char b[96]; A->snprintf(b, sizeof b, "🎯 %s", words[A->random32() % (unsigned)n]);
    if (A->send(b) != 0) A->status("Open a chat first");
}
static int init(const cos_plugin_host_t *h) {
    A = (const msgr_api_t *)h->api;
    if (!A || A->abi != MSGRP_ABI) return -1;
    if (A->add_command("/dice", "/dice [sides]\tsend a dice roll (default d6)", cmd_dice)) return -1;
    A->add_command("/coin", "/coin\tflip a coin", cmd_coin);
    A->add_command("/pick", "/pick a b c ...\tpick one of the words at random", cmd_pick);
    return 0;
}
static void fini(void) { A = 0; }
COS_PLUGIN("messenger", "dice", "1.0", init, fini);
