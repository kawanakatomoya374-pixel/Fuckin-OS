/* calc.c - plugin of the Terminal: calc EXPRESSION (+ - * / % ^ parentheses, hex 0x.., decimals), e.g.  calc (2+3)*4.5 */
#include "cos_plugin.h"
#include "cos_plugin_terminal.h"
#include "cos_plugin_util.h"
static const term_api_t *A;
static const char *p; static int err;
static double expr(void);
static void ws(void) { while (*p == ' ') p++; }
static double number(void) {
    ws(); double v = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; while ((*p >= '0' && *p <= '9') || (pu_lower(*p) >= 'a' && pu_lower(*p) <= 'f')) { v = v * 16 + (*p <= '9' ? *p - '0' : pu_lower(*p) - 'a' + 10); p++; } return v; }
    if (!((*p >= '0' && *p <= '9') || *p == '.')) { err = 1; return 0; }
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p == '.') { p++; double f = 0.1; while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * f; f /= 10; } }
    return v;
}
static double unary(void) {
    ws();
    if (*p == '-') { p++; return -unary(); }
    if (*p == '+') { p++; return unary(); }
    if (*p == '(') { p++; double v = expr(); ws(); if (*p == ')') p++; else err = 1; return v; }
    return number();
}
static double power(void) { double b = unary(); ws(); if (*p == '^') { p++; double e = power(); long n = (long)e; double r = 1; if (n < 0) { n = -n; for (long i = 0; i < n && i < 1000; i++) r *= b; return 1.0 / r; } for (long i = 0; i < n && i < 1000; i++) r *= b; return r; } return b; }
static double term(void) {
    double v = power();
    for (;;) { ws();
        if (*p == '*') { p++; v *= power(); }
        else if (*p == '/') { p++; double d = power(); if (d == 0) { err = 2; return 0; } v /= d; }
        else if (*p == '%') { p++; double d = power(); if ((long)d == 0) { err = 2; return 0; } v = (double)((long)v % (long)d); }
        else return v; }
}
static double expr(void) { double v = term(); for (;;) { ws(); if (*p == '+') { p++; v += term(); } else if (*p == '-') { p++; v -= term(); } else return v; } }
static int cmd_calc(int argc, char **argv, const term_io_t *io) {
    char e[200] = ""; for (int i = 1; i < argc; i++) { if (i > 1) pu_cat(e, sizeof e, " "); pu_cat(e, sizeof e, argv[i]); }
    if (!e[0]) { io->print("usage: calc EXPRESSION   (+ - * / % ^ and parentheses)"); return 1; }
    p = e; err = 0; double v = expr(); ws();
    if (err == 2) { io->print("calc: division by zero"); return 1; }
    if (err || *p) { io->print("calc: cannot read the expression"); return 1; }
    char m[120]; long iv = (long)v;
    if ((double)iv == v) A->base.snprintf(m, sizeof m, "= %ld   (0x%lX)", iv, iv);
    else { long whole = (long)v; double fr = v - (double)whole; if (fr < 0) fr = -fr; long f6 = (long)(fr * 1000000 + 0.5); A->base.snprintf(m, sizeof m, "= %s%ld.%06ld", (v < 0 && whole == 0) ? "-" : "", whole, f6); }
    io->print(m); return 0;
}
static int init(const cos_plugin_host_t *h) {
    A = (const term_api_t *)h->api;
    if (!A || A->abi != TERMP_ABI) return -1;
    return A->add_command("calc", "calc EXPRESSION   arithmetic: + - * / % ^ ( ), 0x.. hex", cmd_calc);
}
static void fini(void) { A = 0; }
COS_PLUGIN("terminal", "calc", "1.0", init, fini);
