/* calc.c-os - the Calculator as a ring-3 program (Basic, Scientific, Study, Hissan and Graph in one window).
 *
 * Study / Hissan / CAS expressions (solve2(), quad(), stat(), hissan(), diff() ...) go to the school-math engine
 * (src/gui/apps/tools/calc_school_math.c, linked into this program); anything it does not handle is evaluated by the
 * numeric parser below, which also drives the graph (y = f(x) with pan, zoom and a trace readout).
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include "cos.h"
#include "cos_ui.h"
#include "calc_school_math.h"

#define BG      0x141826
#define PANEL   0x1C2333
#define KEY     0x262F47
#define KEYOP   0x34466E
#define KEYEQ   0x3D7BE0
#define LINE    0x2B3550
#define TEXT    0xE8ECF5
#define MUTED   0x8891A5
#define ACCENT  0x4C9AFF
#define ERRC    0xFF6B6B
#define GOOD    0x3DD68C

enum { M_BASIC, M_SCI, M_STUDY, M_HISSAN, M_GRAPH, M_COUNT };
static const char *const MODE_NAME[M_COUNT] = { "Basic", "Scientific", "Study", "Hissan", "Graph" };

typedef struct { const char *token, *title, *formula, *notes, *template_expr, *example_expr; } topic_t;
#include "topics.inc"
#define NTOPICS ((int)(sizeof calc_study_topics / sizeof calc_study_topics[0]))

static cui_canvas c;
static cui_font *f_s, *f_m, *f_b, *f_big;
static int win_w, win_h;
static int mode = M_BASIC;
static char expr[512];
static char result[160] = "0";
static char status[128] = "Ready";
static char steps[4096];
static int steps_scroll;
static bool deg = true, clear_next;
static double last_ans;
static int topic;

/* ---- numeric parser ------------------------------------------------------------------------------------------- */
static const char *cp;
static bool c_ok;
static const char *c_err;
static double xval;

static double c_expr(void);
static double c_unary(void);
static void c_fail(const char *why) { if (c_ok) { c_ok = false; c_err = why; } }
static void skipws(void) { while (*cp == ' ') cp++; }

static double to_rad(double a) { return deg ? a * (M_PI / 180.0) : a; }
static double from_rad(double a) { return deg ? a * (180.0 / M_PI) : a; }

static double fact(double n) {
    if (n < 0 || n != floor(n) || n > 170) { c_fail("Factorial needs an integer 0..170"); return 0; }
    double r = 1;
    for (int i = 2; i <= (int)n; i++) r *= i;
    return r;
}
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static double call_fn(const char *id, double *a, int n) {
    #define F1(name, expr_) if (!strcmp(id, name)) { if (n != 1) { c_fail("Wrong argument count"); return 0; } double v = a[0]; (void)v; return expr_; }
    F1("sin", sin(to_rad(v))) F1("cos", cos(to_rad(v))) F1("tan", tan(to_rad(v)))
    F1("asin", from_rad(asin(v))) F1("acos", from_rad(acos(v))) F1("atan", from_rad(atan(v)))
    F1("sinh", sinh(v)) F1("cosh", cosh(v)) F1("tanh", tanh(v))
    F1("sqrt", v < 0 ? (c_fail("sqrt of a negative number"), 0.0) : sqrt(v))
    F1("cbrt", cbrt(v)) F1("exp", exp(v)) F1("abs", fabs(v)) F1("floor", floor(v)) F1("ceil", ceil(v)) F1("round", round(v))
    F1("ln", v <= 0 ? (c_fail("ln needs a positive number"), 0.0) : log(v))
    F1("log", v <= 0 ? (c_fail("log needs a positive number"), 0.0) : log10(v))
    F1("log2", v <= 0 ? (c_fail("log needs a positive number"), 0.0) : log2(v))
    F1("fact", fact(v))
    if (n == 2) {
        if (!strcmp(id, "pow")) return pow(a[0], a[1]);
        if (!strcmp(id, "mod")) { if (a[1] == 0) { c_fail("Division by zero"); return 0; } return fmod(a[0], a[1]); }
        if (!strcmp(id, "atan2")) return from_rad(atan2(a[0], a[1]));
        if (!strcmp(id, "max")) return a[0] > a[1] ? a[0] : a[1];
        if (!strcmp(id, "min")) return a[0] < a[1] ? a[0] : a[1];
        if (!strcmp(id, "ncr") || !strcmp(id, "npr")) {
            double nn = a[0], k = a[1];
            if (nn < 0 || k < 0 || k > nn || nn != floor(nn) || k != floor(k) || nn > 170) { c_fail("nCr/nPr need 0 <= r <= n"); return 0; }
            double r = 1;
            for (int i = 0; i < (int)k; i++) r *= (nn - i);
            return id[1] == 'c' ? r / fact(k) : r;
        }
    }
    if (n >= 1 && (!strcmp(id, "sum") || !strcmp(id, "avg") || !strcmp(id, "prod") || !strcmp(id, "median") || !strcmp(id, "std"))) {
        double s = 0, p = 1;
        for (int i = 0; i < n; i++) { s += a[i]; p *= a[i]; }
        if (!strcmp(id, "sum")) return s;
        if (!strcmp(id, "prod")) return p;
        if (!strcmp(id, "avg")) return s / n;
        if (!strcmp(id, "std")) { double m = s / n, q = 0; for (int i = 0; i < n; i++) q += (a[i] - m) * (a[i] - m); return sqrt(q / n); }
        qsort(a, (size_t)n, sizeof *a, cmp_d);
        return n & 1 ? a[n / 2] : (a[n / 2 - 1] + a[n / 2]) / 2;
    }
    c_fail("Unknown function or wrong arguments");
    return 0;
}

static double c_primary(void) {
    skipws();
    if (*cp == '(') {
        cp++; double v = c_expr(); skipws();
        if (*cp == ')') cp++; else c_fail("Missing )");
        return v;
    }
    if (isdigit((unsigned char)*cp) || *cp == '.') {
        char *end; double v = strtod(cp, &end);
        if (end == cp) { c_fail("Bad number"); return 0; }
        cp = end; return v;
    }
    if (isalpha((unsigned char)*cp)) {
        char id[16]; int n = 0;
        while (isalnum((unsigned char)*cp) && n < 15) id[n++] = (char)tolower((unsigned char)*cp++);
        id[n] = 0;
        skipws();
        if (*cp == '(') {
            cp++;
            double args[64]; int na = 0;
            skipws();
            if (*cp != ')') {
                for (;;) {
                    double v = c_expr();
                    if (na < 64) args[na++] = v;
                    skipws();
                    if (*cp == ',') { cp++; continue; }
                    break;
                }
            }
            skipws();
            if (*cp == ')') cp++; else c_fail("Missing )");
            if (!c_ok) return 0;
            return call_fn(id, args, na);
        }
        if (!strcmp(id, "pi")) return M_PI;
        if (!strcmp(id, "e")) return M_E;
        if (!strcmp(id, "x")) return xval;
        if (!strcmp(id, "ans")) return last_ans;
        c_fail("Unknown name");
        return 0;
    }
    c_fail("Syntax error");
    return 0;
}

static double c_postfix(void) {
    double v = c_primary();
    for (;;) {
        skipws();
        if (*cp == '!') { cp++; v = fact(v); }
        else if (*cp == '%') { cp++; v /= 100.0; }
        else return v;
    }
}
static double c_pow(void) {
    double b = c_postfix();
    skipws();
    if (*cp == '^') { cp++; double e = c_unary(); return pow(b, e); }
    return b;
}
static double c_unary(void) {
    skipws();
    if (*cp == '-') { cp++; return -c_unary(); }
    if (*cp == '+') { cp++; return c_unary(); }
    return c_pow();
}
static double c_term(void) {
    double v = c_unary();
    for (;;) {
        skipws();
        if (*cp == '*') { cp++; v *= c_unary(); }
        else if (*cp == '/') { cp++; double d = c_unary(); if (d == 0) c_fail("Division by zero"); else v /= d; }
        else if (isalpha((unsigned char)*cp) || *cp == '(') v *= c_pow();   /* 2x, 3(4+1), 2pi */
        else return v;
    }
}
static double c_expr(void) {
    double v = c_term();
    for (;;) {
        skipws();
        if (*cp == '+') { cp++; v += c_term(); }
        else if (*cp == '-') { cp++; v -= c_term(); }
        else return v;
    }
}

static bool eval_numeric(const char *s, double x, double *out) {
    cp = s; c_ok = true; c_err = NULL; xval = x;
    double v = c_expr();
    skipws();
    if (c_ok && *cp) c_fail("Unexpected input");
    if (c_ok && (isnan(v) || isinf(v))) c_fail("Result is not a real number");
    *out = v;
    return c_ok;
}

static void fmt_num(double v, char *out, size_t n) {
    if (v == 0) v = 0;
    if (fabs(v) >= 1e15 || (v != 0 && fabs(v) < 1e-9)) snprintf(out, n, "%.10g", v);
    else snprintf(out, n, "%.12g", v);
}

/* ---- evaluation ----------------------------------------------------------------------------------------------- */
static void evaluate(void) {
    if (!expr[0]) return;
    if (mode == M_GRAPH) {
        /* the expression stays as typed: it is f(x), re-plotted every frame */
        double v;
        if (eval_numeric(expr, 0, &v) || (c_err && !strcmp(c_err, "Division by zero"))) { snprintf(result, sizeof result, "y = f(x)"); snprintf(status, sizeof status, "Plotting"); }
        else { snprintf(result, sizeof result, "Error"); snprintf(status, sizeof status, "%s", c_err ? c_err : "Error"); }
        return;
    }
    steps[0] = 0; steps_scroll = 0;
    static calc_school_result_t sr;
    char buf[560];
    const char *in = expr;
    if (mode == M_HISSAN && strncmp(expr, "hissan(", 7)) { snprintf(buf, sizeof buf, "hissan(%s)", expr); in = buf; }
    memset(&sr, 0, sizeof sr);
    if (mode != M_BASIC && mode != M_GRAPH && calc_school_math_evaluate(in, deg, &sr) && sr.handled) {
        snprintf(result, sizeof result, "%s", sr.display);
        snprintf(status, sizeof status, "%s", sr.status);
        snprintf(steps, sizeof steps, "%s", sr.steps);
        double v;
        if (eval_numeric(sr.display, 0, &v)) last_ans = v;
        snprintf(expr, sizeof expr, "%s", sr.display);
        clear_next = true;
        return;
    }
    double v;
    if (eval_numeric(expr, 0, &v)) {
        fmt_num(v, result, sizeof result);
        snprintf(status, sizeof status, "%s", mode == M_SCI ? (deg ? "Deg" : "Rad") : "Ready");
        last_ans = v;
        snprintf(expr, sizeof expr, "%s", result);
        clear_next = true;
    } else {
        snprintf(result, sizeof result, "Error");
        snprintf(status, sizeof status, "%s", c_err ? c_err : "Error");
        clear_next = true;
    }
}

static void expr_append(const char *s) {
    if (clear_next) {
        /* a new number or function starts a fresh expression; an operator continues from the last answer */
        bool op = (s[0] == '+' || s[0] == '-' || s[0] == '*' || s[0] == '/' || s[0] == '^' || s[0] == '%' || s[0] == '!');
        if (!op) expr[0] = 0;
        clear_next = false;
    }
    size_t l = strlen(expr);
    if (l + strlen(s) < sizeof expr) strcat(expr, s);
}
static void expr_backspace(void) { size_t l = strlen(expr); if (l) expr[l - 1] = 0; clear_next = false; }
static void clear_all(void) { expr[0] = 0; snprintf(result, sizeof result, "0"); snprintf(status, sizeof status, "%s", MODE_NAME[mode]); steps[0] = 0; clear_next = false; }

/* ---- study topics --------------------------------------------------------------------------------------------- */
static void apply_topic(int load) {
    const topic_t *t = &calc_study_topics[topic];
    snprintf(result, sizeof result, "%s", t->title);
    snprintf(status, sizeof status, "%s", t->formula);
    snprintf(steps, sizeof steps, "%s", t->notes);
    steps_scroll = 0;
    if (!strcmp(t->token, "CLR")) { clear_all(); return; }
    if (!strcmp(t->token, "GRAPH") && load) { snprintf(expr, sizeof expr, "%s", t->template_expr); mode = M_GRAPH; return; }
    if (load == 1) { snprintf(expr, sizeof expr, "%s", t->template_expr); clear_next = true; }
    else if (load == 2) { snprintf(expr, sizeof expr, "%s", t->example_expr); clear_next = true; }
}

/* ---- buttons -------------------------------------------------------------------------------------------------- */
static const char *const B_BASIC[] = { "C","CE","<-","(",")", "7","8","9","/","%", "4","5","6","*","^", "1","2","3","-","sqrt", "0",".","+","=","+/-", NULL };
static const char *const B_SCI[] = { "sin","cos","tan","log","ln", "sqrt","x^2","x^y","abs","pi", "e","!","1/x","(",")", "7","8","9","/","%", "4","5","6","*","DEG", "1","2","3","-","RAD", "0",".","+","=","C", "nCr","nPr","cbrt","floor","ANS", NULL };
static const char *const B_STUDY[] = { "C","CE","<-","(",")", "7","8","9","/","^", "4","5","6","*","x", "1","2","3","-",",", "0",".","+","=","ANS", NULL };
static const char *const B_GRAPH[] = { "x","^","(",")","<-", "sin","cos","tan","ln","C", "sqrt","abs","pi","e","=", NULL };

static const char *const *buttons(void) {
    switch (mode) { case M_SCI: return B_SCI; case M_STUDY: case M_HISSAN: return B_STUDY; case M_GRAPH: return B_GRAPH; default: return B_BASIC; }
}
static int nbuttons(const char *const *b) { int n = 0; while (b[n]) n++; return n; }

#define TAB_H 36
#define DISP_H 124
static int steps_h(void) { return (mode == M_STUDY || mode == M_HISSAN) ? 150 : 0; }
static int topic_h(void) { return mode == M_STUDY ? 34 : 0; }
static int grid_top(void) { return TAB_H + DISP_H + steps_h() + topic_h() + 8; }
static void key_rect(int i, int n, int *x, int *y, int *w, int *h) {
    int cols = 5, rows = (n + cols - 1) / cols, top = grid_top();
    if (mode == M_GRAPH) top = win_h - 3 * 46 - 8;
    int gh = win_h - top - 8; if (mode == M_GRAPH) gh = 3 * 46;
    int cw = (win_w - 16) / cols, ch = gh / rows;
    *x = 8 + (i % cols) * cw; *y = top + (i / cols) * ch; *w = cw - 6; *h = ch - 6;
}

static void press(const char *t) {
    if (!strcmp(t, "C")) { clear_all(); return; }
    if (!strcmp(t, "CE") || !strcmp(t, "<-")) { expr_backspace(); return; }
    if (!strcmp(t, "=")) { evaluate(); return; }
    if (!strcmp(t, "DEG")) { deg = true; snprintf(status, sizeof status, "Deg"); return; }
    if (!strcmp(t, "RAD")) { deg = false; snprintf(status, sizeof status, "Rad"); return; }
    if (!strcmp(t, "ANS")) { char b[40]; fmt_num(last_ans, b, sizeof b); expr_append(b); return; }
    if (!strcmp(t, "x^2")) { expr_append("^2"); return; }
    if (!strcmp(t, "x^y")) { expr_append("^"); return; }
    if (!strcmp(t, "1/x")) { char b[560]; snprintf(b, sizeof b, "1/(%s)", expr); snprintf(expr, sizeof expr, "%s", b); clear_next = false; return; }
    if (!strcmp(t, "+/-")) {
        if (expr[0] == '-') memmove(expr, expr + 1, strlen(expr));
        else { char b[560]; snprintf(b, sizeof b, "-%s", expr); snprintf(expr, sizeof expr, "%s", b); }
        return;
    }
    static const char *const FN[] = { "sin","cos","tan","log","ln","sqrt","abs","cbrt","floor","nCr","nPr", NULL };
    for (int i = 0; FN[i]; i++) if (!strcmp(t, FN[i])) { char b[16]; snprintf(b, sizeof b, "%s(", t); expr_append(b); return; }
    expr_append(t);
}

/* ---- graph ---------------------------------------------------------------------------------------------------- */
static double gx0 = -10, gx1 = 10, gy0 = -7, gy1 = 7;
static int hover_x = -1, hover_y = -1;
static void graph_rect(int *x, int *y, int *w, int *h) { *x = 8; *y = TAB_H + DISP_H; *w = win_w - 16; *h = win_h - *y - 3 * 46 - 16; if (*h < 80) *h = 80; }

static double nice_step(double range) {
    double raw = range / 8, mag = pow(10, floor(log10(raw))), n = raw / mag;
    return (n < 1.5 ? 1 : n < 3.5 ? 2 : n < 7.5 ? 5 : 10) * mag;
}

static void draw_graph(void) {
    int gx, gy, gw, gh; graph_rect(&gx, &gy, &gw, &gh);
    cui_round_rect(&c, gx, gy, gw, gh, 8, 0x0E1220);
    cui_clip(&c, gx, gy, gw, gh);
    double sx = gw / (gx1 - gx0), sy = gh / (gy1 - gy0);
    #define PX(X) (gx + (int)(((X) - gx0) * sx))
    #define PY(Y) (gy + gh - (int)(((Y) - gy0) * sy))
    double st = nice_step(gx1 - gx0), sty = nice_step(gy1 - gy0);
    char lb[32];
    for (double X = ceil(gx0 / st) * st; X <= gx1; X += st) { cui_fill(&c, PX(X), gy, 1, gh, fabs(X) < st / 2 ? 0x5A6A90 : 0x1E2640); if (fabs(X) > st / 2) { snprintf(lb, sizeof lb, "%g", X); cui_text(&c, f_s, PX(X) + 3, gy + gh - 16, lb, MUTED); } }
    for (double Y = ceil(gy0 / sty) * sty; Y <= gy1; Y += sty) { cui_fill(&c, gx, PY(Y), gw, 1, fabs(Y) < sty / 2 ? 0x5A6A90 : 0x1E2640); if (fabs(Y) > sty / 2) { snprintf(lb, sizeof lb, "%g", Y); cui_text(&c, f_s, gx + 4, PY(Y) - 16, lb, MUTED); } }
    bool have = false; double py = 0; bool pv = false;
    for (int i = 0; i < gw; i++) {
        double x = gx0 + (i + 0.5) / sx, y;
        bool ok = expr[0] && eval_numeric(expr, x, &y);
        if (ok) have = true;
        if (ok && pv && fabs(y - py) < (gy1 - gy0) * 3) cui_line(&c, (float)(gx + i - 1), (float)PY(py), (float)(gx + i), (float)PY(y), 2.0f, ACCENT);
        py = y; pv = ok;
    }
    if (hover_x >= gx && hover_x < gx + gw && hover_y >= gy && hover_y < gy + gh) {
        double x = gx0 + (hover_x - gx) / sx, y;
        if (expr[0] && eval_numeric(expr, x, &y)) {
            cui_circle(&c, (float)hover_x, (float)PY(y), 4.0f, GOOD);
            char t[64]; snprintf(t, sizeof t, "x=%.4g  y=%.4g", x, y);
            cui_text(&c, f_s, gx + gw - cui_text_width(f_s, t) - 8, gy + 6, t, GOOD);
        }
    }
    (void)have;
    cui_unclip(&c);
    cui_text(&c, f_s, gx + 8, gy + 6, "drag: pan   wheel: zoom   double-click: reset", MUTED);
    #undef PX
    #undef PY
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */
static int tab_x(int i) { return 8 + i * ((win_w - 16) / M_COUNT); }

static void draw_lines(int x, int y, int w, int h, const char *text, int skip) {
    cui_clip(&c, x, y, w, h);
    int ly = y - skip * 17;
    const char *p = text;
    char line[256];
    while (*p) {
        int n = 0;
        while (p[n] && p[n] != '\n' && n < 255) n++;
        memcpy(line, p, (size_t)n); line[n] = 0;
        if (ly + 17 > y && ly < y + h) cui_text(&c, f_s, x, ly, line, TEXT);
        ly += 17;
        p += n; if (*p == '\n') p++; else if (n == 255) continue;
    }
    cui_unclip(&c);
}
static int count_lines(const char *t) { int n = 1; for (; *t; t++) if (*t == '\n') n++; return n; }

static void draw(void) {
    cui_fill(&c, 0, 0, win_w, win_h, BG);
    for (int i = 0; i < M_COUNT; i++) {
        int w = (win_w - 16) / M_COUNT - 4;
        if (i == mode) { cui_round_rect(&c, tab_x(i), 4, w, TAB_H - 8, 7, PANEL); cui_fill(&c, tab_x(i) + 10, TAB_H - 7, w - 20, 3, ACCENT); }
        cui_text_center(&c, f_s, tab_x(i), 4, w, TAB_H - 8, MODE_NAME[i], i == mode ? TEXT : MUTED);
    }
    cui_round_rect(&c, 8, TAB_H, win_w - 16, DISP_H - 6, 10, PANEL);
    cui_text_fit(&c, f_m, 22, TAB_H + 12, win_w - 44, expr, MUTED);
    int rw = cui_text_width(f_big, result);
    int rx = win_w - 22 - rw; if (rx < 22) rx = 22;
    cui_text_fit(&c, f_big, rx, TAB_H + 40, win_w - 44, result, !strcmp(result, "Error") ? ERRC : TEXT);
    cui_text_fit(&c, f_s, 22, TAB_H + DISP_H - 28, win_w - 44, status, !strcmp(result, "Error") ? ERRC : ACCENT);

    int y = TAB_H + DISP_H;
    if (mode == M_STUDY) {
        const topic_t *t = &calc_study_topics[topic];
        cui_round_rect(&c, 8, y, 34, 28, 6, KEY); cui_text_center(&c, f_b, 8, y, 34, 28, "<", TEXT);
        cui_round_rect(&c, win_w - 42, y, 34, 28, 6, KEY); cui_text_center(&c, f_b, win_w - 42, y, 34, 28, ">", TEXT);
        char hd[160]; snprintf(hd, sizeof hd, "%d/%d  %s", topic + 1, NTOPICS, t->title);
        cui_text_center(&c, f_b, 46, y, win_w - 92 - 140, 28, hd, TEXT);
        int bx = win_w - 46 - 132;
        cui_round_rect(&c, bx, y, 62, 28, 6, KEYOP); cui_text_center(&c, f_s, bx, y, 62, 28, "Template", TEXT);
        cui_round_rect(&c, bx + 68, y, 62, 28, 6, KEYOP); cui_text_center(&c, f_s, bx + 68, y, 62, 28, "Example", TEXT);
        y += 34;
    }
    if (mode == M_STUDY || mode == M_HISSAN) {
        cui_round_rect(&c, 8, y, win_w - 16, 144, 8, 0x0F1320);
        draw_lines(18, y + 6, win_w - 36, 132, steps[0] ? steps : (mode == M_HISSAN ? "Enter e.g. 12345/67 or 478*36 and press =" : "Pick a topic, load its Template or Example, then press =   (Ctrl+1..5 switches mode)"), steps_scroll);
        y += 150;
    }
    if (mode == M_GRAPH) draw_graph();
    const char *const *b = buttons();
    int n = nbuttons(b);
    for (int i = 0; i < n; i++) {
        int x, ky, w, h; key_rect(i, n, &x, &ky, &w, &h);
        bool eq = !strcmp(b[i], "="), op = strlen(b[i]) > 1 || strchr("/*-+^%()!", b[i][0]);
        cui_round_rect(&c, x, ky, w, h, 8, eq ? KEYEQ : op ? KEYOP : KEY);
        cui_text_center(&c, f_m, x, ky, w, h, b[i], TEXT);
    }
}

/* ---- main ----------------------------------------------------------------------------------------------------- */
static void on_key(const cos_win_event_t *ev) {
    if (ev->mods & COS_MOD_CTRL) {
        char k = ev->ascii;
        if (k >= '1' && k <= '5') { mode = k - '1'; steps_scroll = 0; if (mode != M_STUDY && mode != M_HISSAN) steps[0] = 0; snprintf(status, sizeof status, "%s", MODE_NAME[mode]); return; }
        if (k == 'c' || k == 'C') cos_clip_set(result, strlen(result));
        else if (k == 'v' || k == 'V') { char t[200]; int64_t n = cos_clip_get(t, sizeof t - 1); if (n > 0) { t[n] = 0; for (char *p = t; *p; p++) if (*p == '\n') *p = ' '; expr_append(t); } }
        return;
    }
    if (ev->special == COS_KEY_ENTER || ev->ascii == '\n' || ev->ascii == '\r' || ev->ascii == '=') { evaluate(); return; }
    if (ev->special == COS_KEY_ESC) { clear_all(); return; }
    if (ev->special == COS_KEY_BACKSPACE || ev->ascii == 8 || ev->ascii == 127) { expr_backspace(); return; }
    if (ev->special == COS_KEY_UP && steps_scroll > 0) { steps_scroll--; return; }
    if (ev->special == COS_KEY_DOWN) { steps_scroll++; return; }
    if (ev->ascii >= 32 && ev->ascii < 127) { char s[2] = { ev->ascii, 0 }; expr_append(s); }
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1]) { if (!strcmp(argv[1], "hissan")) mode = M_HISSAN; else if (!strcmp(argv[1], "graph")) mode = M_GRAPH; else if (!strcmp(argv[1], "sci")) mode = M_SCI; else if (!strcmp(argv[1], "study")) mode = M_STUDY; }
    cos_win_info_t wi;
    int64_t h = cos_win2_create("Calculator", 460, 720, &wi);
    if (h <= 0) return 1;
    win_w = wi.width; win_h = wi.height;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    f_s = cui_font_default(13); f_m = cui_font_default(16); f_b = cui_font_bold(15); f_big = cui_font_bold(34);
    clear_all();
    bool drag = false; int lx = 0, ly = 0; uint64_t last_click = 0;

    for (;;) {
        draw();
        cos_win2_present(h);
        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, 500);
        while (r > 0) {
            if (ev.type == COS_EV_CLOSE) goto done;
            if (ev.type == COS_EV_KEY) on_key(&ev);
            else if (ev.type == COS_EV_WHEEL) {
                int gx, gy, gw, gh; graph_rect(&gx, &gy, &gw, &gh);
                if (mode == M_GRAPH && ev.x >= gx && ev.x < gx + gw && ev.y >= gy && ev.y < gy + gh) {
                    double f = ev.wheel > 0 ? 0.8 : 1.25;
                    double cx = gx0 + (ev.x - gx) * (gx1 - gx0) / gw, cy = gy1 - (ev.y - gy) * (gy1 - gy0) / gh;
                    gx0 = cx + (gx0 - cx) * f; gx1 = cx + (gx1 - cx) * f; gy0 = cy + (gy0 - cy) * f; gy1 = cy + (gy1 - cy) * f;
                } else { steps_scroll -= ev.wheel; if (steps_scroll < 0) steps_scroll = 0; int mx = count_lines(steps) - 1; if (steps_scroll > mx) steps_scroll = mx; }
            } else if (ev.type == COS_EV_MOUSE_MOVE) {
                hover_x = ev.x; hover_y = ev.y;
                if (drag && mode == M_GRAPH && (ev.buttons & COS_MOUSE_BTN_LEFT)) {
                    int gx, gy, gw, gh; graph_rect(&gx, &gy, &gw, &gh);
                    double dx = (ev.x - lx) * (gx1 - gx0) / gw, dy = (ev.y - ly) * (gy1 - gy0) / gh;
                    gx0 -= dx; gx1 -= dx; gy0 += dy; gy1 += dy; lx = ev.x; ly = ev.y;
                }
            } else if (ev.type == COS_EV_MOUSE_UP) drag = false;
            else if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) {
                int mx = ev.x, my = ev.y;
                if (my < TAB_H) { for (int i = 0; i < M_COUNT; i++) if (mx >= tab_x(i) && mx < tab_x(i) + (win_w - 16) / M_COUNT) { mode = i; steps_scroll = 0; if (i != M_STUDY && i != M_HISSAN) steps[0] = 0; snprintf(status, sizeof status, "%s", MODE_NAME[i]); } }
                else {
                    int y0 = TAB_H + DISP_H;
                    if (mode == M_STUDY && my >= y0 && my < y0 + 28) {
                        int bx = win_w - 46 - 132;
                        if (mx < 42) { topic = (topic + NTOPICS - 1) % NTOPICS; apply_topic(0); }
                        else if (mx >= win_w - 42) { topic = (topic + 1) % NTOPICS; apply_topic(0); }
                        else if (mx >= bx && mx < bx + 62) apply_topic(1);
                        else if (mx >= bx + 68 && mx < bx + 130) apply_topic(2);
                    }
                    int gx, gy, gw, gh; graph_rect(&gx, &gy, &gw, &gh);
                    if (mode == M_GRAPH && mx >= gx && mx < gx + gw && my >= gy && my < gy + gh) {
                        uint64_t now = cos_time_ms();
                        if (now - last_click < 400) { gx0 = -10; gx1 = 10; gy0 = -7; gy1 = 7; }
                        last_click = now; drag = true; lx = mx; ly = my;
                    }
                    const char *const *b = buttons(); int n = nbuttons(b);
                    for (int i = 0; i < n; i++) {
                        int x, ky, w, hh; key_rect(i, n, &x, &ky, &w, &hh);
                        if (mx >= x && mx < x + w && my >= ky && my < ky + hh) press(b[i]);
                    }
                }
            }
            r = cos_win2_poll(h, &ev);
        }
    }
done:
    cos_win2_close(h);
    return 0;
}
