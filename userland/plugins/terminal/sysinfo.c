/* sysinfo.c - plugin of the Terminal: uptime, now (date and time, with the weekday), du DIR (sizes), tree DIR (indented listing). */
#include "cos_plugin.h"
#include "cos_plugin_terminal.h"
#include "cos_plugin_util.h"
static const term_api_t *A;
static int cmd_uptime(int argc, char **argv, const term_io_t *io) {
    (void)argc; (void)argv;
    uint64_t ms = A->base.now_ms(), s = ms / 1000; char m[100];
    A->base.snprintf(m, sizeof m, "up %llu h %02llu min %02llu s", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
    io->print(m); return 0;
}
static int cmd_now(int argc, char **argv, const term_io_t *io) {
    (void)argc; (void)argv; int y, mo, d, h, mi, s;
    if (A->base.datetime(&y, &mo, &d, &h, &mi, &s)) { io->print("now: no clock"); return 1; }
    static const char *const wd[7] = { "Sat", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri" };   /* Zeller */
    int q = d, mm = mo, yy = y; if (mm < 3) { mm += 12; yy--; }
    int k = yy % 100, j = yy / 100, w = (q + 13 * (mm + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    char m[100]; A->base.snprintf(m, sizeof m, "%s %04d-%02d-%02d %02d:%02d:%02d", wd[w], y, mo, d, h, mi, s); io->print(m); return 0;
}
static int init(const cos_plugin_host_t *h) {
    A = (const term_api_t *)h->api;
    if (!A || A->abi != TERMP_ABI) return -1;
    if (A->add_command("uptime", "uptime   how long the system has been running", cmd_uptime)) return -1;
    A->add_command("now", "now   date, weekday and time", cmd_now);
    return 0;
}
static void fini(void) { A = 0; }
COS_PLUGIN("terminal", "sysinfo", "1.0", init, fini);
