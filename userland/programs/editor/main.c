/* editor.c-os - the Text Editor as a ring-3 program (it used to be drawn by the kernel).
 *
 * One growable UTF-8 byte buffer, caret + selection, snapshot undo/redo, find, Open/Save/Save As through a one-line
 * prompt, and the shared system clipboard. Every launch is its own process, so opening a second file never discards
 * unsaved edits in the first. argv[1], when given, is the file to open.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cos.h"
#include "cos_ui.h"
#include "cosime_ui.h"

#define BG       0x151A24
#define PANEL    0x1C2333
#define LINE     0x2B3550
#define TEXT     0xE4E9F2
#define MUTED    0x7C8AA5
#define ACCENT   0x4C9AFF
#define SELECT   0x2F5C9E
#define HITBG    0x6B5A1E
#define WARN     0xF2B84B

#define TOOLBAR_H 40
#define STATUS_H  26
#define GUTTER    54
#define MAX_FILE  (4u << 20)
#define MAX_UNDO  48

static cui_canvas c;
static cui_font *f_t, *f_s, *f_b;
static int lh, win_w, win_h;
static int64_t g_win;
static int ime_x, ime_y;                 /* where the Japanese input window goes: just under the caret */

static char *buf; static size_t len, cap;
static size_t cur, anc;                 /* caret and selection anchor (byte offsets) */
static char path[256];
static bool dirty;
static int scroll_y, scroll_x, want_col = -1;
static size_t *lines; static int nlines, lines_cap; static bool lines_valid;
static char status[160];

typedef struct { char *data; size_t len, cur; } snap_t;
static snap_t undo[MAX_UNDO], redo[MAX_UNDO]; static int nundo, nredo;
static int last_edit_kind;              /* 1 typing, 2 deleting, 0 other: consecutive same-kind edits share one undo step */

enum { PR_NONE, PR_SAVEAS, PR_OPEN, PR_FIND };
static int prompt; static char pin[256]; static int pin_len;
static char find_str[128]; static bool confirm_discard;

static void set_status(const char *s) { snprintf(status, sizeof status, "%s", s); }

static bool buf_reserve(size_t n) {
    if (n + 1 <= cap) return true;
    size_t nc = cap ? cap : 4096;
    while (nc < n + 1) nc *= 2;
    char *nb = realloc(buf, nc);
    if (!nb) return false;
    buf = nb; cap = nc;
    return true;
}

static void snap_free(snap_t *s) { free(s->data); s->data = NULL; }
static void snap_take(snap_t *s) {
    s->data = malloc(len + 1);
    if (s->data) { memcpy(s->data, buf, len); s->len = len; s->cur = cur; }
}
static void push_undo(int kind) {
    if (kind && kind == last_edit_kind) return;
    last_edit_kind = kind;
    for (int i = 0; i < nredo; i++) snap_free(&redo[i]);
    nredo = 0;
    if (nundo == MAX_UNDO) { snap_free(&undo[0]); memmove(&undo[0], &undo[1], sizeof(snap_t) * (MAX_UNDO - 1)); nundo--; }
    snap_take(&undo[nundo]);
    if (undo[nundo].data) nundo++;
}
static void restore(snap_t *s) {
    if (!s->data || !buf_reserve(s->len)) return;
    memcpy(buf, s->data, s->len); len = s->len; buf[len] = 0;
    cur = anc = s->cur > len ? len : s->cur;
    lines_valid = false; dirty = true;
}
static void do_undo(void) {
    if (!nundo || nredo == MAX_UNDO) { set_status("Nothing to undo"); return; }
    snap_take(&redo[nredo]); if (redo[nredo].data) nredo++;
    snap_t s = undo[--nundo]; restore(&s); snap_free(&s); last_edit_kind = 0;
}
static void do_redo(void) {
    if (!nredo || nundo == MAX_UNDO) { set_status("Nothing to redo"); return; }
    snap_take(&undo[nundo]); if (undo[nundo].data) nundo++;
    snap_t s = redo[--nredo]; restore(&s); snap_free(&s); last_edit_kind = 0;
}

static void replace_range(size_t a, size_t b, const char *s, size_t n) {
    if (a > b) { size_t t = a; a = b; b = t; }
    if (len - (b - a) + n > MAX_FILE || !buf_reserve(len - (b - a) + n)) { set_status("Document too large"); return; }
    memmove(buf + a + n, buf + b, len - b + 1);
    if (n) memcpy(buf + a, s, n);
    len = len - (b - a) + n; buf[len] = 0;
    cur = anc = a + n; lines_valid = false; dirty = true; confirm_discard = false;
}

static bool has_sel(void) { return cur != anc; }
static size_t sel_lo(void) { return cur < anc ? cur : anc; }
static size_t sel_hi(void) { return cur < anc ? anc : cur; }

static void insert_text(const char *s, size_t n, int kind) {
    push_undo(kind);
    replace_range(has_sel() ? sel_lo() : cur, has_sel() ? sel_hi() : cur, s, n);
    want_col = -1;
}

static void build_lines(void) {
    if (lines_valid) return;
    nlines = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == 0 || buf[i - 1] == '\n') {
            if (nlines == lines_cap) { lines_cap = lines_cap ? lines_cap * 2 : 1024; lines = realloc(lines, lines_cap * sizeof *lines); }
            lines[nlines++] = i;
        }
    }
    lines_valid = true;
}
static size_t line_end(int l) { return l + 1 < nlines ? lines[l + 1] - 1 : len; }
static int line_of(size_t pos) {
    build_lines();
    int lo = 0, hi = nlines - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (lines[mid] <= pos) lo = mid; else hi = mid - 1; }
    return lo;
}
static size_t prev_cp(size_t p) { if (p) p--; while (p && ((unsigned char)buf[p] & 0xC0) == 0x80) p--; return p; }
static size_t next_cp(size_t p) { if (p < len) p++; while (p < len && ((unsigned char)buf[p] & 0xC0) == 0x80) p++; return p; }
static int col_of(size_t pos) {
    int l = line_of(pos), n = 0;
    for (size_t p = lines[l]; p < pos; p = next_cp(p)) n++;
    return n;
}
static size_t pos_at_col(int l, int col) {
    size_t p = lines[l], e = line_end(l);
    while (col-- > 0 && p < e) p = next_cp(p);
    return p;
}

/* a line's text with tabs widened, so measuring and drawing agree */
static int expand(size_t from, size_t to, char *out, int outcap) {
    int o = 0;
    for (size_t i = from; i < to && o < outcap - 5; i++) {
        if (buf[i] == '\t') { for (int k = 0; k < 4; k++) out[o++] = ' '; }
        else if (buf[i] != '\r') out[o++] = buf[i];
    }
    out[o] = 0;
    return o;
}
static int x_of(size_t pos) {
    int l = line_of(pos);
    char tmp[1024];
    expand(lines[l], pos, tmp, sizeof tmp);
    return cui_text_width(f_t, tmp);
}
static size_t pos_at_x(int l, int x) {
    size_t p = lines[l], e = line_end(l);
    while (p < e) {
        size_t n = next_cp(p);
        if (x_of(n) - scroll_x > x) {
            int a = x_of(p) - scroll_x, b = x_of(n) - scroll_x;
            return (x - a < b - x) ? p : n;
        }
        p = n;
    }
    return e;
}

static int visible_lines(void) { int n = (win_h - TOOLBAR_H - STATUS_H - (prompt ? 34 : 0)) / lh; return n < 1 ? 1 : n; }
static void ensure_visible(void) {
    build_lines();
    int l = line_of(cur), vis = visible_lines();
    if (l < scroll_y) scroll_y = l;
    if (l >= scroll_y + vis) scroll_y = l - vis + 1;
    int text_w = win_w - GUTTER - 16, x = x_of(cur);
    if (x - scroll_x < 0) scroll_x = x > 40 ? x - 40 : 0;
    if (x - scroll_x > text_w - 8) scroll_x = x - text_w + 60;
}

static void move_to(size_t p, bool extend) {
    cur = p; if (!extend) anc = p;
    ensure_visible();
}

static bool load_file(const char *p) {
    cos_stat_t st;
    if (cos_stat(p, &st) != 0 || st.is_dir) { set_status("Cannot open that file"); return false; }
    if (st.size > MAX_FILE) { set_status("File is larger than 4 MB"); return false; }
    if (!buf_reserve((size_t)st.size)) return false;
    ssize_t n = st.size ? cos_read_file(p, buf, (size_t)st.size) : 0;
    if (n < 0) { set_status("Read failed"); return false; }
    len = (size_t)n; buf[len] = 0;
    for (size_t i = 0; i < len; i++) if (buf[i] == 0) { set_status("Binary file preview unavailable"); len = 0; buf[0] = 0; return false; }
    snprintf(path, sizeof path, "%s", p);
    cur = anc = 0; scroll_y = scroll_x = 0; dirty = false; lines_valid = false;
    for (int i = 0; i < nundo; i++) snap_free(&undo[i]);
    for (int i = 0; i < nredo; i++) snap_free(&redo[i]);
    nundo = nredo = 0; last_edit_kind = 0;
    set_status("Opened");
    return true;
}
static bool save_to(const char *p) {
    if (cos_write_file(p, buf, len) < 0) { set_status("Save failed"); return false; }
    snprintf(path, sizeof path, "%s", p);
    dirty = false; set_status("Saved");
    return true;
}
static void do_save(void) {
    if (path[0]) save_to(path);
    else { prompt = PR_SAVEAS; snprintf(pin, sizeof pin, "/desktop/untitled.txt"); pin_len = (int)strlen(pin); }
}

static void clip_copy(void) { if (has_sel()) cos_clip_set(buf + sel_lo(), sel_hi() - sel_lo()); }
static void clip_paste(void) {
    static char tmp[COS_CLIP_MAX + 1];
    int64_t n = cos_clip_get(tmp, COS_CLIP_MAX);
    if (n > 0) insert_text(tmp, (size_t)n, 0);
}

static bool find_next(bool from_cursor) {
    if (!find_str[0]) return false;
    size_t fl = strlen(find_str), start = from_cursor ? (has_sel() ? sel_hi() : cur) : 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = pass ? 0 : start; i + fl <= len; i++) {
            if (buf[i] == find_str[0] && memcmp(buf + i, find_str, fl) == 0) {
                anc = i; cur = i + fl; ensure_visible(); set_status(pass ? "Search wrapped" : "Found");
                return true;
            }
        }
    }
    set_status("Not found");
    return false;
}

static void doc_new(void) {
    len = 0; if (buf) buf[0] = 0;
    cur = anc = 0; path[0] = 0; dirty = false; lines_valid = false; scroll_y = scroll_x = 0;
    for (int i = 0; i < nundo; i++) snap_free(&undo[i]);
    for (int i = 0; i < nredo; i++) snap_free(&redo[i]);
    nundo = nredo = 0; set_status("New document");
}

static void btn(int x, int w, const char *s, bool on) {
    cui_round_rect(&c, x, 7, w, 26, 6, on ? ACCENT : 0x2A3550);
    cui_text_center(&c, f_s, x, 7, w, 26, s, TEXT);
}
static const struct { const char *label; int w; } TB[] = { {"New", 46}, {"Open", 52}, {"Save", 52}, {"Save As", 70}, {"Find", 52}, {"Undo", 52}, {"Redo", 52} };
static int tb_x(int i) { int x = 10; for (int k = 0; k < i; k++) x += TB[k].w + 6; return x; }

static void draw(void) {
    build_lines();
    cui_fill(&c, 0, 0, win_w, win_h, BG);
    cui_fill(&c, 0, 0, win_w, TOOLBAR_H, PANEL);
    for (int i = 0; i < 7; i++) btn(tb_x(i), TB[i].w, TB[i].label, false);
    {
        char t[300]; snprintf(t, sizeof t, "%s%s", path[0] ? path : "Untitled", dirty ? "  *" : "");
        int x = tb_x(7) + 10;
        cui_text_fit(&c, f_s, x, 12, win_w - x - 10, t, MUTED);
    }
    int vis = visible_lines(), top = TOOLBAR_H, text_x = GUTTER + 8 - scroll_x;
    cui_fill(&c, 0, top, GUTTER, win_h - top - STATUS_H, 0x121723);
    cui_clip(&c, GUTTER, top, win_w - GUTTER, vis * lh);
    size_t lo = sel_lo(), hi = sel_hi();
    for (int r = 0; r < vis && scroll_y + r < nlines; r++) {
        int l = scroll_y + r, y = top + r * lh;
        size_t ls = lines[l], le = line_end(l);
        if (l == line_of(cur) && !has_sel()) cui_fill(&c, GUTTER, y, win_w - GUTTER, lh, 0x1B2233);
        if (has_sel() && hi > ls && lo <= le) {
            size_t a = lo > ls ? lo : ls, b = hi < le ? hi : le;
            int xa = x_of(a), xb = x_of(b) + (hi > le ? 6 : 0);
            cui_fill(&c, GUTTER + 8 + xa - scroll_x, y, xb - xa, lh, SELECT);
        }
        char tmp[1024]; expand(ls, le, tmp, sizeof tmp);
        cui_text(&c, f_t, text_x, y + 2, tmp, TEXT);
    }
    cui_unclip(&c);
    for (int r = 0; r < vis && scroll_y + r < nlines; r++) {
        char n[16]; snprintf(n, sizeof n, "%d", scroll_y + r + 1);
        cui_text(&c, f_s, GUTTER - 10 - cui_text_width(f_s, n), top + r * lh + 3, n, scroll_y + r == line_of(cur) ? TEXT : MUTED);
    }
    int cl = line_of(cur);
    ime_x = GUTTER + 8 + x_of(cur) - scroll_x; ime_y = top + (cl - scroll_y) * lh;
    if (cl >= scroll_y && cl < scroll_y + vis && ((cos_time_ms() / 500) & 1) == 0 && !prompt)
        cui_fill(&c, ime_x, ime_y + 1, 2, lh - 2, ACCENT);

    if (prompt) {
        int py = win_h - STATUS_H - 34;
        cui_fill(&c, 0, py, win_w, 34, PANEL); cui_fill(&c, 0, py, win_w, 1, LINE);
        const char *lab = prompt == PR_SAVEAS ? "Save as:" : prompt == PR_OPEN ? "Open path:" : "Find:";
        cui_text(&c, f_b, 12, py + 9, lab, ACCENT);
        int ix = 12 + cui_text_width(f_b, lab) + 10;
        cui_round_rect(&c, ix, py + 4, win_w - ix - 12, 26, 5, 0x0F131C);
        cui_text_fit(&c, f_t, ix + 8, py + 8, win_w - ix - 30, pin, TEXT);
        if (((cos_time_ms() / 500) & 1) == 0) cui_fill(&c, ix + 8 + cui_text_width(f_t, pin), py + 8, 2, 18, ACCENT);
        ime_x = ix + 8 + cui_text_width(f_t, pin); ime_y = py + 4;
    }
    cui_fill(&c, 0, win_h - STATUS_H, win_w, STATUS_H, PANEL);
    char st[200];
    snprintf(st, sizeof st, "Ln %d, Col %d    %d lines    %zu bytes%s", cl + 1, col_of(cur) + 1, nlines, len, has_sel() ? "    (selection)" : "");
    cui_text(&c, f_s, 12, win_h - STATUS_H + 5, st, MUTED);
    if (status[0]) cui_text(&c, f_s, win_w - cui_text_width(f_s, status) - 12, win_h - STATUS_H + 5, status, confirm_discard ? WARN : ACCENT);
    imeui_indicator(&c, f_s, 12 + cui_text_width(f_s, st) + 24, win_h - STATUS_H + 2);
    imeui_draw(&c, f_t, ime_x, ime_y, prompt ? 26 : lh);
}

static void run_prompt(void) {
    int pr = prompt; prompt = PR_NONE;
    if (pr == PR_FIND) { snprintf(find_str, sizeof find_str, "%s", pin); find_next(true); return; }
    if (!pin[0]) return;
    if (pr == PR_SAVEAS) save_to(pin);
    else if (pr == PR_OPEN) { if (dirty && !confirm_discard) { confirm_discard = true; set_status("Unsaved changes - Enter again to discard"); prompt = pr; return; } load_file(pin); }
}

static void toolbar_click(int i) {
    switch (i) {
    case 0: if (dirty && !confirm_discard) { confirm_discard = true; set_status("Unsaved changes - click New again to discard"); } else doc_new(); break;
    case 1: prompt = PR_OPEN; snprintf(pin, sizeof pin, "%s", path[0] ? path : "/desktop/"); pin_len = (int)strlen(pin); break;
    case 2: do_save(); break;
    case 3: prompt = PR_SAVEAS; snprintf(pin, sizeof pin, "%s", path[0] ? path : "/desktop/untitled.txt"); pin_len = (int)strlen(pin); break;
    case 4: prompt = PR_FIND; snprintf(pin, sizeof pin, "%s", find_str); pin_len = (int)strlen(pin); break;
    case 5: do_undo(); break;
    case 6: do_redo(); break;
    }
}

static void pin_insert(const char *s) {
    size_t n = strlen(s);
    if (pin_len + (int)n >= (int)sizeof pin - 1) return;
    memcpy(pin + pin_len, s, n); pin_len += (int)n; pin[pin_len] = 0;
}

static void key_prompt(const cos_win_event_t *ev) {
    if (ev->special == COS_KEY_ESC) { prompt = PR_NONE; confirm_discard = false; return; }
    if (ev->special == COS_KEY_ENTER || ev->ascii == '\n' || ev->ascii == '\r') { run_prompt(); return; }
    if (ev->ascii == 8 || ev->ascii == 127 || ev->special == COS_KEY_BACKSPACE) {
        if (pin_len) { do pin_len--; while (pin_len > 0 && ((unsigned char)pin[pin_len] & 0xC0) == 0x80); pin[pin_len] = 0; }
        return;
    }
    if ((ev->mods & COS_MOD_CTRL) && (ev->ascii == 'v' || ev->ascii == 'V')) {
        char tmp[200]; int64_t n = cos_clip_get(tmp, sizeof tmp - 1);
        for (int64_t i = 0; i < n && pin_len < (int)sizeof pin - 1; i++) if (tmp[i] >= 32) pin[pin_len++] = tmp[i];
        pin[pin_len] = 0; return;
    }
    if (ev->ascii >= 32 && ev->ascii < 127 && !(ev->mods & COS_MOD_CTRL) && pin_len < (int)sizeof pin - 1) { pin[pin_len++] = ev->ascii; pin[pin_len] = 0; }
}

static void key_edit(const cos_win_event_t *ev) {
    bool ctrl = ev->mods & COS_MOD_CTRL, shift = ev->mods & COS_MOD_SHIFT;
    build_lines();
    if (ctrl) {
        char k = ev->ascii >= 'A' && ev->ascii <= 'Z' ? ev->ascii + 32 : ev->ascii;
        switch (k) {
        case 's': do_save(); return;
        case 'z': do_undo(); ensure_visible(); return;
        case 'y': do_redo(); ensure_visible(); return;
        case 'a': anc = 0; cur = len; return;
        case 'c': clip_copy(); return;
        case 'x': if (has_sel()) { clip_copy(); insert_text("", 0, 0); } return;
        case 'v': clip_paste(); ensure_visible(); return;
        case 'f': toolbar_click(4); return;
        case 'o': toolbar_click(1); return;
        case 'g': find_next(true); return;
        default: break;
        }
    }
    int l = line_of(cur);
    switch (ev->special) {
    case COS_KEY_LEFT:  move_to(has_sel() && !shift ? sel_lo() : prev_cp(cur), shift); want_col = -1; return;
    case COS_KEY_RIGHT: move_to(has_sel() && !shift ? sel_hi() : next_cp(cur), shift); want_col = -1; return;
    case COS_KEY_UP: case COS_KEY_DOWN: case COS_KEY_PGUP: case COS_KEY_PGDN: {
        int step = (ev->special == COS_KEY_PGUP || ev->special == COS_KEY_PGDN) ? visible_lines() - 1 : 1;
        int nl = (ev->special == COS_KEY_UP || ev->special == COS_KEY_PGUP) ? l - step : l + step;
        if (nl < 0) { move_to(0, shift); return; }
        if (nl >= nlines) { move_to(len, shift); return; }
        if (want_col < 0) want_col = col_of(cur);
        move_to(pos_at_col(nl, want_col), shift);
        return;
    }
    case COS_KEY_HOME: move_to(ctrl ? 0 : lines[l], shift); want_col = -1; return;
    case COS_KEY_END:  move_to(ctrl ? len : line_end(l), shift); want_col = -1; return;
    case COS_KEY_DELETE:
        if (has_sel()) insert_text("", 0, 0);
        else if (cur < len) { push_undo(2); replace_range(cur, next_cp(cur), "", 0); }
        ensure_visible(); return;
    default: break;
    }
    if (ev->ascii == 8 || ev->ascii == 127 || ev->special == COS_KEY_BACKSPACE) {
        if (has_sel()) insert_text("", 0, 0);
        else if (cur > 0) { push_undo(2); replace_range(prev_cp(cur), cur, "", 0); }
        want_col = -1; ensure_visible(); return;
    }
    if (ev->ascii == '\n' || ev->ascii == '\r' || ev->special == COS_KEY_ENTER) {
        /* keep the current line's indentation */
        size_t ls = lines[l], i = ls; char ind[64]; int n = 0; ind[n++] = '\n';
        while (i < cur && (buf[i] == ' ' || buf[i] == '\t') && n < 60) ind[n++] = buf[i++];
        insert_text(ind, n, 0); ensure_visible(); return;
    }
    if (ev->ascii == '\t') { insert_text("    ", 4, 0); ensure_visible(); return; }
    if ((unsigned char)ev->ascii >= 32 && ev->ascii != 127 && !ctrl) { insert_text(&ev->ascii, 1, 1); ensure_visible(); }
}

static void redraw_now(void) { draw(); cos_win2_present(g_win); }

int main(int argc, char **argv) {
    cos_win_info_t wi;
    int64_t h = cos_win2_create("Text Editor", 900, 650, &wi);
    if (h <= 0) return 1;
    g_win = h;
    win_w = wi.width; win_h = wi.height;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    f_t = cui_font_default(15); f_s = cui_font_default(13); f_b = cui_font_bold(13);
    lh = cui_font_height(f_t) + 6;
    buf_reserve(0); buf[0] = 0;
    if (argc > 1 && argv[1] && argv[1][0]) load_file(argv[1]);
    bool drag = false;
    imeui_set_redraw(redraw_now);

    for (;;) {
        draw();
        cos_win2_present(h);
        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, 250);
        while (r > 0) {
            if (ev.type == COS_EV_CLOSE) goto done;
            if (ev.type == COS_EV_OPEN) { char p[256]; if (cos_win2_get_path(h, p, sizeof p) && !dirty) load_file(p); }
            else if (ev.type == COS_EV_KEY) {
                char commit[1024]; int ir = imeui_key(&ev, commit, sizeof commit);          /* Japanese input first */
                if (ir & COSIME_COMMIT) {
                    if (prompt) pin_insert(commit); else { insert_text(commit, strlen(commit), 1); ensure_visible(); }
                }
                if (!(ir & COSIME_CONSUMED)) { if (prompt) key_prompt(&ev); else key_edit(&ev); }
            }
            else if (ev.type == COS_EV_WHEEL) {
                scroll_y -= ev.wheel * 3;
                if (scroll_y < 0) scroll_y = 0;
                build_lines(); if (scroll_y > nlines - 1) scroll_y = nlines - 1;
            } else if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) {
                imeui_cancel();
                if (ev.y < TOOLBAR_H) {
                    for (int i = 0; i < 7; i++) if (ev.x >= tb_x(i) && ev.x < tb_x(i) + TB[i].w) toolbar_click(i);
                } else if (ev.x >= GUTTER && ev.y < win_h - STATUS_H - (prompt ? 34 : 0)) {
                    build_lines();
                    int l = scroll_y + (ev.y - TOOLBAR_H) / lh;
                    if (l >= nlines) l = nlines - 1;
                    cur = pos_at_x(l, ev.x - GUTTER - 8); if (!(ev.mods & COS_MOD_SHIFT)) anc = cur;
                    want_col = -1; drag = true;
                }
            } else if (ev.type == COS_EV_MOUSE_MOVE && drag && (ev.buttons & COS_MOUSE_BTN_LEFT)) {
                build_lines();
                int l = scroll_y + (ev.y - TOOLBAR_H) / lh;
                if (l < 0) l = 0;
                if (l >= nlines) l = nlines - 1;
                cur = pos_at_x(l, ev.x - GUTTER - 8);
            } else if (ev.type == COS_EV_MOUSE_UP) drag = false;
            r = cos_win2_poll(h, &ev);
        }
    }
done:
    cos_win2_close(h);
    return 0;
}
