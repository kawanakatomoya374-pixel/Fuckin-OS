/* cosime_ui.c - see cosime_ui.h. One IME engine per process. */
#include <string.h>
#include <stdio.h>
#include "cosime_ui.h"

#define DIC_PATH "/system/ime/cosime.dic"
static cosime *g_ime;
static bool g_on, g_tried, g_loading;
static void (*g_redraw)(void);
static int g_rx, g_ry, g_rw, g_rh;                               /* where the last imeui_draw() put its popup (w = 0: nothing) */
bool imeui_last_rect(int *x, int *y, int *w, int *h) { *x = g_rx; *y = g_ry; *w = g_rw; *h = g_rh; return g_rw > 0; }

/* Ctrl+Shift+J switches Japanese input on and off in every program (Ctrl+Space and Ctrl+\ still work too). The key may arrive as 'j', 'J' or the control code 10. */
static bool ime_toggle_key(const cos_win_event_t *ev)
{
    if (!(ev->mods & COS_MOD_CTRL)) return false;
    if (ev->ascii == ' ' || ev->ascii == '\\') return true;
    return (ev->mods & COS_MOD_SHIFT) && (ev->ascii == 'j' || ev->ascii == 'J' || ev->ascii == 10);
}

static cosime *eng(void) { if (!g_ime) g_ime = cosime_new(); return g_ime; }
void imeui_set_redraw(void (*redraw)(void)) { g_redraw = redraw; }
bool imeui_on(void) { return g_on; }
void imeui_set(bool on) {
    cosime *m = eng(); if (!m) return;
    g_on = on; cosime_set_enabled(m, on);
}
void imeui_toggle(void) { imeui_set(!g_on); }
bool imeui_composing(void) { return g_on && g_ime && cosime_state(g_ime) != COSIME_IDLE; }
void imeui_cancel(void) { if (g_ime && cosime_state(g_ime) != COSIME_IDLE) { cosime_set_enabled(g_ime, false); cosime_set_enabled(g_ime, g_on); } }

static void load_dictionary(void)
{
    if (g_tried || !g_ime || cosime_loaded(g_ime)) return;
    g_tried = true; g_loading = true;
    if (g_redraw) g_redraw();                                      /* shows "loading the dictionary" */
    uint64_t t0 = cos_time_ms();
    int r = cosime_load(g_ime, DIC_PATH);
    char lb[96]; snprintf(lb, sizeof lb, "[IME] dictionary load=%d in %d ms\n", r, (int)(cos_time_ms() - t0)); cos_puts(lb);
    g_loading = false;
    if (g_redraw) g_redraw();
}

int imeui_key(const cos_win_event_t *ev, char *commit, size_t cap)
{
    if (commit && cap) commit[0] = 0;
    if (ime_toggle_key(ev)) { imeui_toggle(); return COSIME_CONSUMED; }
    if (!g_on || !g_ime) return COSIME_PASS;
    if (ev->ascii >= 'a' && ev->ascii <= 'z' && !(ev->mods & COS_MOD_CTRL)) load_dictionary();
    return cosime_key(g_ime, ev, commit, cap);
}

void imeui_indicator(cui_canvas *c, cui_font *f, int x, int y)
{
    if (!g_on) return;
    int w = cui_text_width(f, "あ") + 14, h = cui_font_height(f) + 6;
    cui_round_rect(c, x, y, w, h, 6, 0x2F5C9E);
    cui_text(c, f, x + 7, y + 3, "あ", 0xFFFFFF);
}

#define POP_BG   0x0E1220
#define POP_LINE 0x2B3550
#define POP_SEL  0x2F5C9E
#define POP_TXT  0xE4E9F2
#define POP_DIM  0x7C8AA5

void imeui_draw(cui_canvas *c, cui_font *f, int cx, int cy, int lh)
{
    g_rw = 0;
    if (!g_on || !g_ime) return;
    int fh = cui_font_height(f);
    if (g_loading) {
        const char *t = "辞書を読み込み中...";
        int w = cui_text_width(f, t) + 20, h = fh + 10, y = cy + lh + 2;
        if (cx + w > c->w) cx = c->w - w; if (cx < 0) cx = 0;
        if (y + h > c->h) y = cy - h - 2;
        g_rx = cx; g_ry = y; g_rw = w; g_rh = h;
        cui_round_rect(c, cx, y, w, h, 7, POP_BG); cui_round_rect_outline(c, cx, y, w, h, 7, 1.0f, POP_LINE);
        cui_text(c, f, cx + 10, y + 5, t, POP_TXT);
        return;
    }
    int st = cosime_state(g_ime);
    if (st == COSIME_IDLE) return;
    int ncand = st == COSIME_CONVERTING ? cosime_ncand(g_ime) : 0, sel = st == COSIME_CONVERTING ? cosime_cand_sel(g_ime) : 0;
    int page = sel / 9, first = page * 9, rows = ncand - first < 9 ? ncand - first : 9;
    if (rows < 0) rows = 0;
    const char *pre = cosime_text(g_ime);
    int pw = cui_text_width(f, pre) + 20, w = pw;
    for (int i = 0; i < rows; i++) { int rw = cui_text_width(f, cosime_cand(g_ime, first + i)) + 56; if (rw > w) w = rw; }
    if (w < 140) w = 140;
    int ph = fh + 12, rowh = fh + 8, h = ph + (rows ? rows * rowh + 8 : 0);
    int x = cx, y = cy + lh + 2;
    if (x + w > c->w - 4) x = c->w - 4 - w; if (x < 2) x = 2;
    if (y + h > c->h - 2) y = cy - h - 2; if (y < 0) y = 0;
    g_rx = x; g_ry = y; g_rw = w; g_rh = h;
    cui_round_rect(c, x, y, w, h, 8, POP_BG); cui_round_rect_outline(c, x, y, w, h, 8, 1.0f, POP_LINE);
    int tx = x + 10, ty = y + 6;
    if (st == COSIME_CONVERTING) {                                 /* the chosen candidate of each bunsetsu; the focused one is highlighted */
        for (int i = 0; i < cosime_nseg(g_ime); i++) {
            const char *t = cosime_seg_text(g_ime, i); int tw = cui_text_width(f, t);
            if (i == cosime_focus(g_ime)) cui_fill(c, tx, ty - 1, tw, fh + 2, POP_SEL); else cui_fill(c, tx, ty + fh, tw, 1, POP_DIM);
            cui_text(c, f, tx, ty, t, POP_TXT); tx += tw;
        }
    } else { cui_text(c, f, tx, ty, pre, POP_TXT); cui_fill(c, tx, ty + fh, cui_text_width(f, pre), 1, 0x4C9AFF); }
    for (int i = 0; i < rows; i++) {
        int ry = y + ph + 4 + i * rowh;
        if (first + i == sel) cui_round_rect(c, x + 4, ry, w - 8, rowh - 2, 6, POP_SEL);
        char nb[8]; snprintf(nb, sizeof nb, "%d", i + 1);
        cui_text(c, f, x + 12, ry + 3, nb, POP_DIM);
        cui_text(c, f, x + 34, ry + 3, cosime_cand(g_ime, first + i), POP_TXT);
    }
}

size_t imeui_utf8_prev(const char *s, size_t pos)
{
    if (!pos) return 0;
    do pos--; while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80);
    return pos;
}

int imeui_key_gated(const cos_win_event_t *ev, bool active, char *commit, size_t cap)
{
    if (active) return imeui_key(ev, commit, cap);
    if (commit && cap) commit[0] = 0;
    if (ime_toggle_key(ev)) { imeui_toggle(); return COSIME_CONSUMED; }
    return COSIME_PASS;
}
