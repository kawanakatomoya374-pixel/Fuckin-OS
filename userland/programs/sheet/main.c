/* sheet.c-os - the Spreadsheet as a ring-3 program (it used to be drawn by the kernel).
 *
 * 26 columns x 100 rows of text cells. A cell that starts with '=' is a formula: + - * / ^ %, parentheses, cell
 * references (A1), ranges (A1:B3) and SUM / AVERAGE / AVG / MIN / MAX / COUNT / ABS / SQRT / ROUND. Cycles and bad
 * references show an error instead of hanging. Files are CSV (the raw cell text, so formulas survive a round trip).
 * Each launch is its own process; argv[1], when given, is the CSV to open.
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
#include "cosime_ui.h"

#define COLS 26
#define ROWS 100
#define CELLCAP 96
#define BG      0x151A24
#define PANEL   0x1C2333
#define LINE    0x2B3550
#define HEAD    0x222B40
#define TEXT    0xE4E9F2
#define MUTED   0x7C8AA5
#define ACCENT  0x4C9AFF
#define ERRC    0xFF6B6B
#define WARN    0xF2B84B

#define TOOLBAR_H 40
#define FORMULA_H 30
#define COLHEAD_H 26
#define ROWHEAD_W 48
#define CELL_W    104
#define CELL_H    26
#define STATUS_H  26

static cui_canvas c;
static cui_font *f_t, *f_s, *f_b;
static int win_w, win_h;
static char cells[ROWS][COLS][CELLCAP];
static char path[256];
static bool dirty;
static int selr, selc, scr_r, scr_c;
static bool editing; static char ebuf[CELLCAP]; static int elen;
static int64_t g_win; static int ime_x = -1, ime_y;    /* the caret in the cell being edited, for the Japanese input window */
static char status[160];
enum { PR_NONE, PR_SAVEAS, PR_OPEN };
static int prompt; static char pin[256]; static int pin_len;
static bool confirm_discard;

/* ---- evaluation ----------------------------------------------------------------------------------------------- */
enum { E_OK, E_SYNTAX, E_DIV0, E_CYCLE, E_REF, E_VALUE };
static const char *const ERRNAME[] = { "", "#ERR", "#DIV/0", "#CYCLE", "#REF", "#VALUE" };
typedef struct { int err; bool text; double n; } val_t;
static unsigned char vstate[ROWS][COLS];
static val_t vcache[ROWS][COLS];
static const char *ep;       /* parser cursor */
static int perr;

static val_t eval_cell(int r, int cc);
static val_t parse_expr(void);

static bool cell_empty(int r, int cc) { return cells[r][cc][0] == 0; }

static bool parse_number(const char *s, double *out) {
    while (*s == ' ') s++;
    if (!*s) return false;
    char *end; double v = strtod(s, &end);
    if (end == s) return false;
    while (*end == ' ') end++;
    if (*end) return false;
    *out = v; return true;
}

static void skip_ws(void) { while (*ep == ' ') ep++; }

static bool parse_ref(const char **pp, int *r, int *cc) {
    const char *p = *pp;
    if (!isalpha((unsigned char)*p)) return false;
    int col = toupper((unsigned char)*p) - 'A';
    p++;
    if (!isdigit((unsigned char)*p)) return false;
    int row = 0;
    while (isdigit((unsigned char)*p)) { row = row * 10 + (*p - '0'); if (row > 9999) break; p++; }
    if (col < 0 || col >= COLS || row < 1 || row > ROWS) { perr = perr ? perr : E_REF; return false; }
    *r = row - 1; *cc = col; *pp = p;
    return true;
}

/* 1 when the cell holds a number to aggregate; empty and text cells are skipped, an error is remembered in perr */
static int cell_num(int r, int cc, double *out) {
    if (cell_empty(r, cc)) return 0;
    val_t v = eval_cell(r, cc);
    if (v.err) { if (!perr) perr = v.err; return 0; }
    if (v.text) return 0;
    *out = v.n;
    return 1;
}

static val_t num(double n) { val_t v = { E_OK, false, n }; return v; }
static val_t fail(int e) { val_t v = { e, false, 0 }; return v; }

static val_t parse_call(const char *name_start, int name_len) {
    char name[16]; if (name_len > 15) name_len = 15;
    for (int i = 0; i < name_len; i++) name[i] = (char)toupper((unsigned char)name_start[i]);
    name[name_len] = 0;
    static double stack_vals[16][ROWS * COLS > 4096 ? 4096 : ROWS * COLS];
    static int depth;
    if (depth >= 16) return fail(E_SYNTAX);
    double *mine = stack_vals[depth++];
    int n = 0;
    skip_ws();
    if (*ep != ')') {
        for (;;) {
            skip_ws();
            const char *save = ep; int r1, c1, r2, c2;
            if (parse_ref(&save, &r1, &c1) && *save == ':') {
                save++;
                if (parse_ref(&save, &r2, &c2)) {
                    ep = save;
                    int ra = r1 < r2 ? r1 : r2, rb = r1 < r2 ? r2 : r1, ca = c1 < c2 ? c1 : c2, cb = c1 < c2 ? c2 : c1;
                    for (int r = ra; r <= rb; r++) for (int cc = ca; cc <= cb; cc++) {
                        double cv;
                        if (cell_num(r, cc, &cv) && n < 4096) mine[n++] = cv;
                    }
                } else { perr = perr ? perr : E_REF; depth--; return fail(perr); }
            } else {
                val_t v = parse_expr();
                if (v.err) { depth--; return v; }
                if (n < 4096) mine[n++] = v.n;
            }
            skip_ws();
            if (*ep == ',' || *ep == ';') { ep++; continue; }
            break;
        }
    }
    skip_ws();
    if (*ep != ')') { depth--; return fail(E_SYNTAX); }
    ep++;
    double acc = 0; val_t out = fail(E_SYNTAX);
    if (!strcmp(name, "SUM")) { for (int i = 0; i < n; i++) acc += mine[i]; out = num(acc); }
    else if (!strcmp(name, "AVERAGE") || !strcmp(name, "AVG")) {
        if (!n) out = fail(E_DIV0); else { for (int i = 0; i < n; i++) acc += mine[i]; out = num(acc / n); }
    } else if (!strcmp(name, "MIN") || !strcmp(name, "MAX")) {
        if (!n) out = num(0);
        else { acc = mine[0]; bool mx = name[1] == 'A'; for (int i = 1; i < n; i++) if (mx ? mine[i] > acc : mine[i] < acc) acc = mine[i]; out = num(acc); }
    } else if (!strcmp(name, "COUNT")) out = num(n);
    else if (!strcmp(name, "ABS") && n == 1) out = num(fabs(mine[0]));
    else if (!strcmp(name, "SQRT") && n == 1) out = mine[0] < 0 ? fail(E_VALUE) : num(sqrt(mine[0]));
    else if (!strcmp(name, "ROUND") && (n == 1 || n == 2)) {
        double p = n == 2 ? pow(10, mine[1]) : 1;
        out = num(round(mine[0] * p) / p);
    }
    depth--;
    return out;
}

static val_t parse_primary(void) {
    skip_ws();
    if (*ep == '(') {
        ep++; val_t v = parse_expr(); skip_ws();
        if (*ep != ')') return fail(E_SYNTAX);
        ep++; return v;
    }
    if (*ep == '-') { ep++; val_t v = parse_primary(); if (!v.err) v.n = -v.n; return v; }
    if (*ep == '+') { ep++; return parse_primary(); }
    if (isdigit((unsigned char)*ep) || *ep == '.') {
        char *end; double v = strtod(ep, &end);
        if (end == ep) return fail(E_SYNTAX);
        ep = end; return num(v);
    }
    if (isalpha((unsigned char)*ep)) {
        const char *s = ep; int r, cc;
        const char *q = ep;
        if (parse_ref(&q, &r, &cc) && !isalpha((unsigned char)*q) && *q != '(') {
            ep = q;
            if (cell_empty(r, cc)) return num(0);
            val_t v = eval_cell(r, cc);
            if (!v.err && v.text) return fail(E_VALUE);
            return v;
        }
        if (perr == E_REF && !isalpha((unsigned char)*q)) return fail(E_REF);
        perr = 0;
        const char *e = s; while (isalpha((unsigned char)*e)) e++;
        if (*e == '(') { ep = e + 1; return parse_call(s, (int)(e - s)); }
        return fail(E_SYNTAX);
    }
    return fail(E_SYNTAX);
}

static val_t parse_pow(void) {
    val_t a = parse_primary(); skip_ws();
    if (!a.err && *ep == '^') { ep++; val_t b = parse_pow(); if (b.err) return b; return num(pow(a.n, b.n)); }
    if (!a.err && *ep == '%') { ep++; a.n /= 100.0; }
    return a;
}
static val_t parse_term(void) {
    val_t a = parse_pow();
    for (;;) {
        skip_ws();
        if (a.err || (*ep != '*' && *ep != '/')) return a;
        char op = *ep++; val_t b = parse_pow();
        if (b.err) return b;
        if (op == '*') a.n *= b.n; else { if (b.n == 0) return fail(E_DIV0); a.n /= b.n; }
    }
}
static val_t parse_expr(void) {
    val_t a = parse_term();
    for (;;) {
        skip_ws();
        if (a.err || (*ep != '+' && *ep != '-')) return a;
        char op = *ep++; val_t b = parse_term();
        if (b.err) return b;
        a.n = op == '+' ? a.n + b.n : a.n - b.n;
    }
}

static val_t eval_cell(int r, int cc) {
    if (vstate[r][cc] == 2) return vcache[r][cc];
    if (vstate[r][cc] == 1) return fail(E_CYCLE);
    vstate[r][cc] = 1;
    val_t v;
    const char *raw = cells[r][cc];
    if (raw[0] == '=') {
        const char *save_ep = ep; int save_perr = perr;
        ep = raw + 1; perr = 0;
        v = parse_expr(); skip_ws();
        if (!v.err && *ep) v = fail(E_SYNTAX);
        if (!v.err && perr) v = fail(perr);
        ep = save_ep; perr = save_perr;
    } else {
        double n;
        if (!raw[0]) v = num(0);
        else if (parse_number(raw, &n)) v = num(n);
        else { v.err = E_OK; v.text = true; v.n = 0; }
    }
    vstate[r][cc] = 2; vcache[r][cc] = v;
    return v;
}

static void recalc(void) { memset(vstate, 0, sizeof vstate); }

static void fmt_val(val_t v, char *out, size_t n, const char *raw) {
    if (v.err) { snprintf(out, n, "%s", ERRNAME[v.err]); return; }
    if (v.text) { snprintf(out, n, "%s", raw); return; }
    snprintf(out, n, "%.10g", v.n);
}

/* ---- files ---------------------------------------------------------------------------------------------------- */
static void set_status(const char *s) { snprintf(status, sizeof status, "%s", s); }

static void clear_all(void) { memset(cells, 0, sizeof cells); recalc(); selr = selc = scr_r = scr_c = 0; editing = false; }

static bool load_csv(const char *p) {
    cos_stat_t st;
    if (cos_stat(p, &st) != 0 || st.is_dir || st.size > (2u << 20)) { set_status("Cannot open that file"); return false; }
    char *d = malloc((size_t)st.size + 1);
    if (!d) return false;
    ssize_t n = st.size ? cos_read_file(p, d, (size_t)st.size) : 0;
    if (n < 0) { free(d); set_status("Read failed"); return false; }
    d[n] = 0;
    clear_all();
    int r = 0, cc = 0, len = 0; bool q = false;
    for (ssize_t i = 0; i <= n && r < ROWS; i++) {
        char ch = i < n ? d[i] : '\n';
        if (q) {
            if (ch == '"') { if (i + 1 < n && d[i + 1] == '"') { if (len < CELLCAP - 1 && cc < COLS) cells[r][cc][len++] = '"'; i++; } else q = false; }
            else if (len < CELLCAP - 1 && cc < COLS) cells[r][cc][len++] = ch;
        } else if (ch == '"' && len == 0) q = true;
        else if (ch == ',') { cc++; len = 0; }
        else if (ch == '\n') { if (!(i == n && cc == 0 && len == 0)) { r++; } cc = 0; len = 0; }
        else if (ch != '\r' && len < CELLCAP - 1 && cc < COLS) cells[r][cc][len++] = ch;
    }
    free(d);
    snprintf(path, sizeof path, "%s", p); dirty = false; set_status("Opened");
    return true;
}

static bool save_csv(const char *p) {
    int lr = -1, lc = -1;
    for (int r = 0; r < ROWS; r++) for (int cc = 0; cc < COLS; cc++) if (cells[r][cc][0]) { if (r > lr) lr = r; if (cc > lc) lc = cc; }
    size_t cap = (size_t)(lr + 2) * (size_t)(lc + 2) * (CELLCAP * 2 + 2) + 16, o = 0;
    char *d = malloc(cap);
    if (!d) return false;
    for (int r = 0; r <= lr; r++) {
        for (int cc = 0; cc <= lc; cc++) {
            const char *s = cells[r][cc];
            bool quote = strpbrk(s, ",\"\n") != NULL;
            if (quote) d[o++] = '"';
            for (; *s; s++) { if (*s == '"') d[o++] = '"'; d[o++] = *s; }
            if (quote) d[o++] = '"';
            if (cc < lc) d[o++] = ',';
        }
        d[o++] = '\n';
    }
    ssize_t w = cos_write_file(p, d, o);
    free(d);
    if (w < 0) { set_status("Save failed"); return false; }
    snprintf(path, sizeof path, "%s", p); dirty = false; set_status("Saved");
    return true;
}

static void do_save(void) {
    if (path[0]) save_csv(path);
    else { prompt = PR_SAVEAS; snprintf(pin, sizeof pin, "/desktop/sheet.csv"); pin_len = (int)strlen(pin); }
}

/* ---- editing -------------------------------------------------------------------------------------------------- */
static void commit_edit(void) {
    if (!editing) return;
    if (strcmp(cells[selr][selc], ebuf)) { snprintf(cells[selr][selc], CELLCAP, "%s", ebuf); dirty = true; recalc(); confirm_discard = false; }
    editing = false;
}
static void begin_edit(bool keep) {
    editing = true;
    if (keep) snprintf(ebuf, sizeof ebuf, "%s", cells[selr][selc]); else ebuf[0] = 0;
    elen = (int)strlen(ebuf);
}
static void ensure_visible(void) {
    int vr = (win_h - TOOLBAR_H - FORMULA_H - COLHEAD_H - STATUS_H - (prompt ? 34 : 0)) / CELL_H;
    int vc = (win_w - ROWHEAD_W) / CELL_W;
    if (vr < 1) vr = 1;
    if (vc < 1) vc = 1;
    if (selr < scr_r) scr_r = selr;
    if (selr >= scr_r + vr) scr_r = selr - vr + 1;
    if (selc < scr_c) scr_c = selc;
    if (selc >= scr_c + vc) scr_c = selc - vc + 1;
}
static void move_sel(int dr, int dc) {
    commit_edit();
    selr += dr; selc += dc;
    if (selr < 0) selr = 0;
    if (selr >= ROWS) selr = ROWS - 1;
    if (selc < 0) selc = 0;
    if (selc >= COLS) selc = COLS - 1;
    ensure_visible();
}

static void cell_name(int r, int cc, char *out, size_t n) { snprintf(out, n, "%c%d", 'A' + cc, r + 1); }

/* ---- drawing -------------------------------------------------------------------------------------------------- */
static const struct { const char *label; int w; } TB[] = { {"New", 46}, {"Open", 52}, {"Save", 52}, {"Save As", 70} };
static int tb_x(int i) { int x = 10; for (int k = 0; k < i; k++) x += TB[k].w + 6; return x; }

static void draw(void) {
    cui_fill(&c, 0, 0, win_w, win_h, BG);
    cui_fill(&c, 0, 0, win_w, TOOLBAR_H, PANEL);
    for (int i = 0; i < 4; i++) {
        cui_round_rect(&c, tb_x(i), 7, TB[i].w, 26, 6, 0x2A3550);
        cui_text_center(&c, f_s, tb_x(i), 7, TB[i].w, 26, TB[i].label, TEXT);
    }
    { char t[300]; snprintf(t, sizeof t, "%s%s", path[0] ? path : "Untitled", dirty ? "  *" : ""); int x = tb_x(4) + 10; cui_text_fit(&c, f_s, x, 12, win_w - x - 10, t, MUTED); }

    int fy = TOOLBAR_H;
    cui_fill(&c, 0, fy, win_w, FORMULA_H, 0x181E2E);
    char nm[8]; cell_name(selr, selc, nm, sizeof nm);
    cui_text(&c, f_b, 12, fy + 7, nm, ACCENT);
    cui_fill(&c, ROWHEAD_W, fy + 4, 1, FORMULA_H - 8, LINE);
    cui_text_fit(&c, f_t, ROWHEAD_W + 10, fy + 6, win_w - ROWHEAD_W - 20, editing ? ebuf : cells[selr][selc], TEXT);

    int gy = fy + FORMULA_H, vr = (win_h - gy - COLHEAD_H - STATUS_H - (prompt ? 34 : 0)) / CELL_H, vc = (win_w - ROWHEAD_W) / CELL_W + 1;
    if (vr < 1) vr = 1;
    cui_fill(&c, 0, gy, win_w, COLHEAD_H, HEAD);
    for (int k = 0; k < vc && scr_c + k < COLS; k++) {
        char h[4] = { (char)('A' + scr_c + k), 0 };
        cui_text_center(&c, f_b, ROWHEAD_W + k * CELL_W, gy, CELL_W, COLHEAD_H, h, scr_c + k == selc ? ACCENT : MUTED);
    }
    cui_clip(&c, ROWHEAD_W, gy + COLHEAD_H, win_w - ROWHEAD_W, vr * CELL_H);
    for (int i = 0; i < vr && scr_r + i < ROWS; i++) {
        int r = scr_r + i, y = gy + COLHEAD_H + i * CELL_H;
        for (int k = 0; k < vc && scr_c + k < COLS; k++) {
            int cc = scr_c + k, x = ROWHEAD_W + k * CELL_W;
            bool sel = r == selr && cc == selc;
            cui_fill(&c, x, y, CELL_W, CELL_H, sel ? 0x1F2B45 : (r & 1 ? 0x171C2A : BG));
            cui_fill(&c, x + CELL_W - 1, y, 1, CELL_H, LINE);
            cui_fill(&c, x, y + CELL_H - 1, CELL_W, 1, LINE);
            if (sel && editing) { cui_text_fit(&c, f_t, x + 6, y + 4, CELL_W - 12, ebuf, TEXT); continue; }
            if (cell_empty(r, cc)) continue;
            val_t v = eval_cell(r, cc);
            char t[96]; fmt_val(v, t, sizeof t, cells[r][cc]);
            if (!v.err && !v.text) cui_text_fit(&c, f_t, x + CELL_W - 6 - cui_text_width(f_t, t) > x + 4 ? x + CELL_W - 6 - cui_text_width(f_t, t) : x + 4, y + 4, CELL_W - 8, t, TEXT);
            else cui_text_fit(&c, f_t, x + 6, y + 4, CELL_W - 12, t, v.err ? ERRC : TEXT);
        }
    }
    cui_unclip(&c);
    cui_fill(&c, 0, gy + COLHEAD_H, ROWHEAD_W, vr * CELL_H, HEAD);
    for (int i = 0; i < vr && scr_r + i < ROWS; i++) {
        char h[8]; snprintf(h, sizeof h, "%d", scr_r + i + 1);
        cui_text_center(&c, f_s, 0, gy + COLHEAD_H + i * CELL_H, ROWHEAD_W, CELL_H, h, scr_r + i == selr ? ACCENT : MUTED);
    }
    if (selr >= scr_r && selr < scr_r + vr && selc >= scr_c) {
        int x = ROWHEAD_W + (selc - scr_c) * CELL_W, y = gy + COLHEAD_H + (selr - scr_r) * CELL_H;
        cui_round_rect_outline(&c, x, y, CELL_W, CELL_H, 2, 2.0f, ACCENT);
        if (editing) { ime_x = x + 6 + cui_text_width(f_t, ebuf); ime_y = y + 2; }
        if (editing && ((cos_time_ms() / 500) & 1) == 0) {
            int cx = x + 6 + cui_text_width(f_t, ebuf);
            if (cx < x + CELL_W - 6) cui_fill(&c, cx, y + 4, 2, CELL_H - 8, ACCENT);
        }
    }
    if (prompt) {
        int py = win_h - STATUS_H - 34;
        cui_fill(&c, 0, py, win_w, 34, PANEL); cui_fill(&c, 0, py, win_w, 1, LINE);
        const char *lab = prompt == PR_SAVEAS ? "Save as:" : "Open path:";
        cui_text(&c, f_b, 12, py + 9, lab, ACCENT);
        int ix = 12 + cui_text_width(f_b, lab) + 10;
        cui_round_rect(&c, ix, py + 4, win_w - ix - 12, 26, 5, 0x0F131C);
        cui_text_fit(&c, f_t, ix + 8, py + 8, win_w - ix - 30, pin, TEXT);
        ime_x = ix + 8 + cui_text_width(f_t, pin); ime_y = py + 4;
    }
    cui_fill(&c, 0, win_h - STATUS_H, win_w, STATUS_H, PANEL);
    const char *hint = "Enter edit/commit   Tab next   Del clear   Ctrl+C/V copy/paste   Ctrl+S save   Ctrl+Space Japanese input";
    cui_text(&c, f_s, 12, win_h - STATUS_H + 5, hint, MUTED);
    if (status[0]) cui_text(&c, f_s, win_w - cui_text_width(f_s, status) - 12, win_h - STATUS_H + 5, status, confirm_discard ? WARN : ACCENT);
    imeui_indicator(&c, f_s, 12 + cui_text_width(f_s, hint) + 14, win_h - STATUS_H + 2);
    if (editing || prompt) imeui_draw(&c, f_t, ime_x, ime_y, prompt ? 26 : CELL_H - 4);
}

/* ---- input ---------------------------------------------------------------------------------------------------- */
static void run_prompt(void) {
    int pr = prompt; prompt = PR_NONE;
    if (!pin[0]) return;
    if (pr == PR_SAVEAS) save_csv(pin);
    else if (pr == PR_OPEN) {
        if (dirty && !confirm_discard) { confirm_discard = true; set_status("Unsaved changes - Enter again to discard"); prompt = pr; return; }
        load_csv(pin);
    }
}

static void toolbar_click(int i) {
    switch (i) {
    case 0: if (dirty && !confirm_discard) { confirm_discard = true; set_status("Unsaved changes - click New again to discard"); }
            else { clear_all(); path[0] = 0; dirty = false; set_status("New sheet"); } break;
    case 1: prompt = PR_OPEN; snprintf(pin, sizeof pin, "%s", path[0] ? path : "/desktop/"); pin_len = (int)strlen(pin); break;
    case 2: do_save(); break;
    case 3: prompt = PR_SAVEAS; snprintf(pin, sizeof pin, "%s", path[0] ? path : "/desktop/sheet.csv"); pin_len = (int)strlen(pin); break;
    }
}

static void key_prompt(const cos_win_event_t *ev) {
    if (ev->special == COS_KEY_ESC) { prompt = PR_NONE; confirm_discard = false; return; }
    if (ev->special == COS_KEY_ENTER || ev->ascii == '\n' || ev->ascii == '\r') { run_prompt(); return; }
    if (ev->ascii == 8 || ev->ascii == 127 || ev->special == COS_KEY_BACKSPACE) { if (pin_len) { pin_len = (int)imeui_utf8_prev(pin, (size_t)pin_len); pin[pin_len] = 0; } return; }
    if ((ev->mods & COS_MOD_CTRL) && (ev->ascii == 'v' || ev->ascii == 'V')) {
        char tmp[200]; int64_t n = cos_clip_get(tmp, sizeof tmp - 1);
        for (int64_t i = 0; i < n && pin_len < (int)sizeof pin - 1; i++) if (tmp[i] >= 32) pin[pin_len++] = tmp[i];
        pin[pin_len] = 0; return;
    }
    if (ev->ascii >= 32 && ev->ascii < 127 && !(ev->mods & COS_MOD_CTRL) && pin_len < (int)sizeof pin - 1) { pin[pin_len++] = ev->ascii; pin[pin_len] = 0; }
}

static void key_grid(const cos_win_event_t *ev) {
    bool ctrl = ev->mods & COS_MOD_CTRL;
    if (ctrl) {
        char k = ev->ascii >= 'A' && ev->ascii <= 'Z' ? ev->ascii + 32 : ev->ascii;
        if (k == 's') { commit_edit(); do_save(); return; }
        if (k == 'o') { toolbar_click(1); return; }
        if (k == 'c' || k == 'x') {
            const char *s = editing ? ebuf : cells[selr][selc];
            cos_clip_set(s, strlen(s));
            if (k == 'x' && !editing) { cells[selr][selc][0] = 0; dirty = true; recalc(); }
            return;
        }
        if (k == 'v') {
            char tmp[CELLCAP]; int64_t n = cos_clip_get(tmp, sizeof tmp - 1);
            if (n < 0) return;
            tmp[n] = 0;
            for (char *p = tmp; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
            if (editing) { for (char *p = tmp; *p && elen < CELLCAP - 1; p++) ebuf[elen++] = *p; ebuf[elen] = 0; }
            else { snprintf(cells[selr][selc], CELLCAP, "%s", tmp); dirty = true; recalc(); }
            return;
        }
        return;
    }
    switch (ev->special) {
    case COS_KEY_UP: move_sel(-1, 0); return;
    case COS_KEY_DOWN: move_sel(1, 0); return;
    case COS_KEY_LEFT: if (!editing) move_sel(0, -1); return;
    case COS_KEY_RIGHT: if (!editing) move_sel(0, 1); return;
    case COS_KEY_TAB: move_sel(0, 1); return;
    case COS_KEY_PGUP: move_sel(-10, 0); return;
    case COS_KEY_PGDN: move_sel(10, 0); return;
    case COS_KEY_HOME: move_sel(0, -selc); return;
    case COS_KEY_ESC: editing = false; return;
    case COS_KEY_DELETE:
        if (!editing && cells[selr][selc][0]) { cells[selr][selc][0] = 0; dirty = true; recalc(); }
        return;
    default: break;
    }
    if (ev->special == COS_KEY_F1 + 1) { begin_edit(true); return; }   /* F2 */
    if (ev->ascii == '\n' || ev->ascii == '\r' || ev->special == COS_KEY_ENTER) {
        if (editing) move_sel(1, 0); else begin_edit(true);
        return;
    }
    if (ev->ascii == 8 || ev->ascii == 127 || ev->special == COS_KEY_BACKSPACE) {
        if (!editing) begin_edit(true);
        if (elen) { elen = (int)imeui_utf8_prev(ebuf, (size_t)elen); ebuf[elen] = 0; }
        return;
    }
    if ((unsigned char)ev->ascii >= 32 && ev->ascii != 127) {
        if (!editing) begin_edit(false);
        if (elen < CELLCAP - 1) { ebuf[elen++] = ev->ascii; ebuf[elen] = 0; }
    }
}

static void redraw_now(void) { draw(); cos_win2_present(g_win); }

int main(int argc, char **argv) {
    cos_win_info_t wi;
    int64_t h = cos_win2_create("Spreadsheet", 960, 640, &wi);
    if (h <= 0) return 1;
    g_win = h; imeui_set_redraw(redraw_now);
    win_w = wi.width; win_h = wi.height;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    f_t = cui_font_default(14); f_s = cui_font_default(13); f_b = cui_font_bold(13);
    if (argc > 1 && argv[1] && argv[1][0]) load_csv(argv[1]);
    uint64_t last_click = 0; int last_r = -1, last_c = -1;

    for (;;) {
        draw();
        cos_win2_present(h);
        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, 250);
        while (r > 0) {
            if (ev.type == COS_EV_CLOSE) goto done;
            if (ev.type == COS_EV_OPEN) { char p[256]; if (cos_win2_get_path(h, p, sizeof p) && !dirty) load_csv(p); }
            else if (ev.type == COS_EV_KEY) {
                char commit[1024]; int ir = imeui_key(&ev, commit, sizeof commit);          /* Japanese input first */
                if (ir & COSIME_COMMIT) {
                    if (prompt) { size_t n = strlen(commit); if (pin_len + (int)n < (int)sizeof pin - 1) { memcpy(pin + pin_len, commit, n); pin_len += (int)n; pin[pin_len] = 0; } }
                    else { if (!editing) begin_edit(false); size_t n = strlen(commit); if (elen + (int)n < CELLCAP - 1) { memcpy(ebuf + elen, commit, n); elen += (int)n; ebuf[elen] = 0; } }
                }
                if (!(ir & COSIME_CONSUMED)) { if (prompt) key_prompt(&ev); else key_grid(&ev); }
            }
            else if (ev.type == COS_EV_WHEEL) { commit_edit(); scr_r -= ev.wheel * 3; if (scr_r < 0) scr_r = 0; if (scr_r > ROWS - 1) scr_r = ROWS - 1; }
            else if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) {
                int gy = TOOLBAR_H + FORMULA_H;
                if (ev.y < TOOLBAR_H) { for (int i = 0; i < 4; i++) if (ev.x >= tb_x(i) && ev.x < tb_x(i) + TB[i].w) toolbar_click(i); }
                else if (ev.y >= gy + COLHEAD_H && ev.x >= ROWHEAD_W && ev.y < win_h - STATUS_H - (prompt ? 34 : 0)) {
                    int rr = scr_r + (ev.y - gy - COLHEAD_H) / CELL_H, cc = scr_c + (ev.x - ROWHEAD_W) / CELL_W;
                    if (rr < ROWS && cc < COLS) {
                        uint64_t now = cos_time_ms();
                        commit_edit();
                        selr = rr; selc = cc;
                        if (rr == last_r && cc == last_c && now - last_click < 400) begin_edit(true);
                        last_click = now; last_r = rr; last_c = cc;
                        ensure_visible();
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
