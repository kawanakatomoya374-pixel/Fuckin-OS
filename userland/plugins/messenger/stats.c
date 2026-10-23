/* stats.c - a plugin of C-OS Message that keeps state: counts what you send and receive per chat while it is loaded (an outgoing filter that never changes
 * anything, and an incoming hook), and /stats shows the counts. Unloading it forgets them. */
#include "cos_plugin.h"
#include "cos_plugin_messenger.h"
static const msgr_api_t *A;
static struct { char chat[24]; unsigned in, out, words; } S[8];
static unsigned out_total;

static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static unsigned count_words(const char *t) { unsigned n = 0; int in = 0; for (; *t; t++) { if (*t == ' ') in = 0; else if (!in) { in = 1; n++; } } return n; }
static int slot(const char *chat) {
    int free_i = -1;
    for (int i = 0; i < 8; i++) { if (S[i].chat[0] && same(S[i].chat, chat)) return i; if (!S[i].chat[0] && free_i < 0) free_i = i; }
    if (free_i < 0) return -1;
    int k = 0; for (; chat[k] && k < 23; k++) S[free_i].chat[k] = chat[k];
    S[free_i].chat[k] = 0; return free_i;
}
static int outgoing(char *text, size_t cap) {
    (void)cap;
    const char *c = A->chat_name();
    out_total++;
    if (c) { int i = slot(c); if (i >= 0) { S[i].out++; S[i].words += count_words(text); } }
    return 0;                                                          /* never swallows */
}
static void incoming(const char *chat, const char *text) { int i = slot(chat); if (i >= 0) { S[i].in++; S[i].words += count_words(text); } }
static void cmd_stats(const char *arg) {
    (void)arg;
    char body[600]; int o = 0;
    for (int i = 0; i < 8; i++) if (S[i].chat[0]) o += A->snprintf(body + o, sizeof body - (size_t)o, "%s\tsent %u, received %u, %u words\n", S[i].chat, S[i].out, S[i].in, S[i].words);
    if (!o) A->snprintf(body, sizeof body, "(nothing counted yet)\n");
    A->note("Message statistics", body);
}
static int init(const cos_plugin_host_t *h) {
    A = (const msgr_api_t *)h->api;
    if (!A || A->abi != MSGRP_ABI) return -1;
    if (A->add_outgoing_filter(outgoing) || A->add_incoming_hook(incoming)) return -1;
    return A->add_command("/stats", "/stats\tshow how many messages were sent and received per chat", cmd_stats);
}
static void fini(void) { A = 0; }
COS_PLUGIN("messenger", "stats", "1.0", init, fini);
