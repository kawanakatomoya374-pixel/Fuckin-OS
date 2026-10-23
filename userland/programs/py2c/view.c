/* view.c - the text view: drawing, scrolling, caret, selection, mouse and keyboard editing for the editor and the read-only panes;
 * plus the one-line text field used by the find bar and the dialogs. */
#include "p2.h"

extern const unsigned char studio_font_mono[];            /* userland/programs/studio/studio_fonts.c: DejaVu Sans Mono */
cui_font *g_mono, *g_ui, *g_ui_bold, *g_ui_small;
int g_code_px = 13, g_cw = 8, g_lh = 18;

void fonts_set_code_px(int px) {
    static cui_font *cache[40];
    if (px < 9) px = 9;
    if (px > 28) px = 28;
    g_code_px = px;
    if (!cache[px]) {
        cache[px] = cui_font_load(studio_font_mono, px);
        if (cache[px]) cui_font_set_fallback(cache[px], cui_font_default(px));     /* Japanese comments and strings */
    }
    g_mono = cache[px] ? cache[px] : cui_font_default(px);
    g_cw = cui_text_width(g_mono, "M"); if (g_cw < 4) g_cw = 7;
    g_lh = cui_font_height(g_mono) + 4;
}

/* ---------------------------------------------------------------- measuring */
static bool all_ascii(const char *s, size_t n) { for (size_t i = 0; i < n; ++i) if ((unsigned char)s[i] >= 0x80) return false; return true; }
/* width in pixels of a tab-free run */
static int run_w(const char *s, size_t n) {
    if (all_ascii(s, n)) return (int)n * g_cw;
    int w = 0; char buf[260];
    for (size_t i = 0; i < n; ) {
        size_t k = n - i; if (k > 250) k = 250;
        while (k < n - i && k > 0 && ((unsigned char)s[i + k] & 0xC0) == 0x80) k--;
        memcpy(buf, s + i, k); buf[k] = 0; w += cui_text_width(g_mono, buf); i += k;
        if (k == 0) break;
    }
    return w;
}
static int next_tab(int x) { int ts = 4 * g_cw; return (x / ts + 1) * ts; }
/* x of byte offset `to` inside the line that starts at s (tabs expand to 4 columns) */
static int x_in_line(const char *t, size_t s, size_t to) {
    int x = 0; size_t i = s;
    while (i < to) {
        if (t[i] == '\t') { x = next_tab(x); i++; continue; }
        size_t j = i; while (j < to && t[j] != '\t') j++;
        x += run_w(t + i, j - i); i = j;
    }
    return x;
}
int view_x_of(Doc *d, size_t pos) { size_t l = doc_line_of(d, pos); return x_in_line(d->t, doc_line_start(d, l), pos); }

static void gutter_metrics(View *v) {
    int digits = 3; size_t n = doc_lines(v->d); while (n >= 1000) { digits++; n /= 10; }
    int gw = v->gutter ? digits * g_cw + 22 : 0;
    v->text_x = v->x + gw + (v->gutter ? 6 : 8);
    v->vis_lines = (v->h - 2) / g_lh; if (v->vis_lines < 1) v->vis_lines = 1;
}
void view_layout(View *v) { gutter_metrics(v); }

static inline bool ch_eq(char a, char b, bool icase) {
    if (a == b) return true;
    if (!icase) return false;
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    return a == b;
}
/* ---------------------------------------------------------------- drawing */
static uint32_t kind_color(int k) {
    switch (k) { case K_KW: return T->kw; case K_STR: return T->str; case K_NUM: return T->num; case K_CMT: return T->cmt; case K_FN: return T->fn;
                 case K_DECO: return T->deco; case K_TYPE: return T->type; case K_BUILTIN: return T->builtin; default: return T->text; }
}
/* draws a run (no newline) at *x, expanding tabs; returns after advancing *x */
static void draw_run(cui_canvas *c, int *x, int y, const char *s, size_t n, uint32_t col, int xmin, int xmax, int origin) {
    size_t i = 0;
    while (i < n) {
        if (s[i] == '\t') { *x = origin + next_tab(*x - origin); i++; continue; }
        size_t j = i; while (j < n && s[j] != '\t') j++;
        size_t k = i;
        while (k < j) {
            size_t m = j - k; if (m > 200) { m = 200; while (m > 1 && ((unsigned char)s[k + m] & 0xC0) == 0x80) m--; }
            char buf[260]; memcpy(buf, s + k, m); buf[m] = 0;
            int w = run_w(buf, m);
            if (*x + w >= xmin && *x <= xmax) cui_text(c, g_mono, *x, y, buf, col);
            *x += w; k += m;
            if (*x > xmax) return;
        }
        i = j;
    }
}

static void scrollbar(cui_canvas *c, View *v, size_t nlines) {
    if ((int)nlines <= v->vis_lines) return;
    int tx = v->x + v->w - 9, th = v->h - 4;
    cui_fill(c, tx, v->y + 2, 7, th, cui_mix(T->bg, T->border, 90));
    int ph = th * v->vis_lines / (int)nlines; if (ph < 24) ph = 24;
    int py = v->y + 2 + (th - ph) * v->d->top / ((int)nlines - v->vis_lines);
    cui_round_rect(c, tx, py, 7, ph, 3, T->faint);
}

void view_draw(cui_canvas *c, View *v) {
    Doc *d = v->d;
    gutter_metrics(v);
    cui_clip(c, v->x, v->y, v->w, v->h);
    cui_fill(c, v->x, v->y, v->w, v->h, T->bg);
    if (!d) { cui_unclip(c); return; }
    size_t nl = doc_lines(d);
    if (d->top < 0) d->top = 0;
    if ((int)nl - d->top < 1) d->top = (int)nl - 1;
    hl_prepare(d);
    int gw = v->text_x - v->x - (v->gutter ? 6 : 8);
    if (v->gutter) cui_fill(c, v->x, v->y, gw, v->h, T->gutter);
    size_t cl = doc_line_of(d, d->caret);
    size_t sa, sb; doc_sel(d, &sa, &sb);
    bool sel = sa != sb;
    int tx0 = v->text_x - d->left;
    int right = v->x + v->w - 10;
    for (int row = 0; row < v->vis_lines + 1; ++row) {
        size_t l = (size_t)d->top + (size_t)row;
        if (l >= nl) break;
        int y = v->y + 1 + row * g_lh;
        if (y > v->y + v->h) break;
        size_t ls = doc_line_start(d, l), le = doc_line_end(d, l);
        if (v->focus && v->editable && l == cl && !sel) cui_fill(c, v->text_x - 4, y, right - v->text_x + 4, g_lh, T->curline);
        if (v->mark_err && d->err_line == (int)l + 1) cui_fill_alpha(c, v->text_x - 4, y, right - v->text_x + 4, g_lh, T->err, 46);
        /* selection */
        if (sel && sb > ls && sa <= le) {
            size_t a = sa > ls ? sa : ls, b = sb < le ? sb : le;
            int x1 = x_in_line(d->t, ls, a), x2 = x_in_line(d->t, ls, b);
            if (sb > le) x2 += g_cw / 2;                                  /* the selected newline */
            if (x2 <= x1) x2 = x1 + (sb > le ? g_cw / 2 : 0);
            cui_fill(c, tx0 + x1, y, x2 - x1, g_lh, T->sel);
        }
        /* find matches */
        if (v->find && v->find_len) {
            for (size_t i = ls; i + v->find_len <= le; ) {
                size_t j = 0;
                while (j < v->find_len && ch_eq(d->t[i + j], v->find[j], v->find_icase)) j++;
                if (j == v->find_len) {
                    int x1 = x_in_line(d->t, ls, i), x2 = x_in_line(d->t, ls, i + v->find_len);
                    cui_fill_alpha(c, tx0 + x1, y, x2 - x1, g_lh, T->warn, 90);
                    i += v->find_len;
                } else i++;
            }
        }
        /* text */
        int x = tx0; int ty = y + 2;
        const char *s = d->t + ls; int n = (int)(le - ls);
        if (d->lang == L_TEXT || !d->lst) draw_run(c, &x, ty, s, (size_t)n, T->text, v->text_x - 20, right, tx0);
        else {
            int st = d->lst[l]; Tok toks[256]; int nt = hl_line(d->lang, s, n, &st, toks, 256);
            int pos = 0;
            for (int k = 0; k < nt; ++k) {
                if (toks[k].start > pos) draw_run(c, &x, ty, s + pos, (size_t)(toks[k].start - pos), T->text, v->text_x - 20, right, tx0);
                draw_run(c, &x, ty, s + toks[k].start, (size_t)toks[k].len, kind_color(toks[k].kind), v->text_x - 20, right, tx0);
                pos = toks[k].start + toks[k].len;
                if (x > right) break;
            }
            if (pos < n && x <= right) draw_run(c, &x, ty, s + pos, (size_t)(n - pos), T->text, v->text_x - 20, right, tx0);
        }
        if (v->mark_err && d->err_line == (int)l + 1) {                   /* a red wave under the line's text */
            int ex = tx0 + x_in_line(d->t, ls, le); if (ex < tx0 + 8) ex = tx0 + 8;
            for (int sx = tx0; sx < ex; sx += 4) cui_fill(c, sx, y + g_lh - 2, 2, 1, T->err);
        }
        /* gutter number */
        if (v->gutter) {
            char nb[16]; snprintf(nb, sizeof nb, "%d", (int)l + 1);
            int w = cui_text_width(g_ui_small, nb);
            cui_text(c, g_ui_small, v->x + gw - 10 - w, y + (g_lh - cui_font_height(g_ui_small)) / 2, nb, (v->mark_err && d->err_line == (int)l + 1) ? T->err : l == cl ? T->text : T->faint);
        }
    }
    /* caret */
    if (v->focus) {
        uint64_t ph = (cos_time_ms() - A.caret_t) / 530;
        if ((ph & 1) == 0) {
            size_t l = cl;
            if ((int)l >= d->top && (int)l < d->top + v->vis_lines + 1) {
                int cx = tx0 + view_x_of(d, d->caret), cy = v->y + 1 + ((int)l - d->top) * g_lh;
                if (cx >= v->text_x - 4 && cx < right) cui_fill(c, cx, cy + 1, 2, g_lh - 2, T->text);
            }
        }
    }
    scrollbar(c, v, nl);
    cui_unclip(c);
}

void view_caret_xy(View *v, int *x, int *y) {
    Doc *d = v->d; size_t l = doc_line_of(d, d->caret);
    *x = v->text_x - d->left + view_x_of(d, d->caret);
    *y = v->y + 1 + ((int)l - d->top) * g_lh;
}

/* ---------------------------------------------------------------- scrolling */
void view_ensure_caret(View *v) {
    Doc *d = v->d; if (!d) return;
    gutter_metrics(v);
    size_t l = doc_line_of(d, d->caret);
    if ((int)l < d->top) d->top = (int)l;
    else if ((int)l >= d->top + v->vis_lines) d->top = (int)l - v->vis_lines + 1;
    int cx = view_x_of(d, d->caret), tw = v->w - (v->text_x - v->x) - 24;
    if (cx < d->left) d->left = cx > 40 ? cx - 40 : 0;
    else if (cx + g_cw > d->left + tw) d->left = cx + g_cw - tw + 40;
    if (d->left < 0) d->left = 0;
}
void view_center_caret(View *v) {
    Doc *d = v->d; if (!d) return;
    gutter_metrics(v);
    size_t l = doc_line_of(d, d->caret);
    d->top = (int)l - v->vis_lines / 2; if (d->top < 0) d->top = 0;
    view_ensure_caret(v);
}
void view_goto_line(View *v, int line) {
    Doc *d = v->d; if (!d) return;
    size_t nl = doc_lines(d); if (line < 1) line = 1; if ((size_t)line > nl) line = (int)nl;
    d->caret = d->anchor = doc_line_start(d, (size_t)line - 1);
    d->want_col = 0; view_center_caret(v);
}
void view_wheel(View *v, int dy, bool shift) {
    Doc *d = v->d; if (!d) return;
    if (shift) { d->left -= dy > 0 ? 3 * g_cw : -3 * g_cw; if (d->left < 0) d->left = 0; return; }
    d->top -= dy > 0 ? 3 : -3;
    int max = (int)doc_lines(d) - 1; if (d->top > max) d->top = max; if (d->top < 0) d->top = 0;
}

/* ---------------------------------------------------------------- mouse */
static size_t pos_at(View *v, int mx, int my) {
    Doc *d = v->d; size_t nl = doc_lines(d);
    int row = (my - v->y - 1) / g_lh; if (my < v->y) row = -1;
    long l = (long)d->top + row; if (l < 0) l = 0; if ((size_t)l >= nl) l = (long)nl - 1;
    size_t ls = doc_line_start(d, (size_t)l), le = doc_line_end(d, (size_t)l);
    int x = mx - (v->text_x - d->left); if (x < 0) return ls;
    int cx = 0; size_t i = ls;
    while (i < le) {
        size_t nx = u8_next(d->t, d->len, i); int w;
        if (d->t[i] == '\t') w = next_tab(cx) - cx;
        else if ((unsigned char)d->t[i] < 0x80) w = g_cw;
        else { char b[8]; size_t k = nx - i; if (k > 6) k = 6; memcpy(b, d->t + i, k); b[k] = 0; w = cui_text_width(g_mono, b); }
        if (x < cx + w / 2 + (w == 1 ? 0 : 0)) return i;
        if (x < cx + w) return nx;
        cx += w; i = nx;
    }
    return le;
}
static bool is_word(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80; }
bool view_mouse(View *v, int type, int x, int y, int buttons, int mods) {
    Doc *d = v->d; if (!d) return false;
    gutter_metrics(v);
    if (type == 0) {                                                   /* down */
        if (x < v->x || x >= v->x + v->w || y < v->y || y >= v->y + v->h) return false;
        if (x >= v->x + v->w - 12 && (int)doc_lines(d) > v->vis_lines) {  /* scrollbar */
            size_t nl = doc_lines(d); int frac = (y - v->y) * ((int)nl - v->vis_lines) / (v->h > 0 ? v->h : 1);
            d->top = frac < 0 ? 0 : frac; v->drag = false; return true;
        }
        uint64_t now = cos_time_ms();
        bool again = now - v->click_t < 450 && abs(x - v->click_x) < 5 && abs(y - v->click_y) < 5;
        v->click_n = again ? (v->click_n % 3) + 1 : 1; v->click_t = now; v->click_x = x; v->click_y = y;
        size_t p = pos_at(v, x, y);
        if (x < v->text_x - 6 && v->gutter) {                          /* the gutter selects whole lines */
            size_t l = doc_line_of(d, p), s = doc_line_start(d, l), e = doc_line_end(d, l); e = e < d->len ? e + 1 : e;
            d->anchor = s; d->caret = e; v->drag = true; return true;
        }
        if (v->click_n == 2) {
            size_t a = p, b = p;
            while (a > 0 && is_word(d->t[a - 1])) a--;
            while (b < d->len && is_word(d->t[b])) b++;
            if (a == b && b < d->len) b = u8_next(d->t, d->len, b);
            d->anchor = a; d->caret = b;
        } else if (v->click_n == 3) {
            size_t l = doc_line_of(d, p); d->anchor = doc_line_start(d, l); size_t e = doc_line_end(d, l); d->caret = e < d->len ? e + 1 : e;
        } else {
            if (mods & COS_MOD_SHIFT) d->caret = p; else { d->caret = d->anchor = p; }
        }
        d->want_col = doc_col_of(d, d->caret);
        v->drag = true; A.caret_t = cos_time_ms();
        return true;
    }
    if (type == 1) {                                                   /* move */
        if (!v->drag || !(buttons & COS_MOUSE_BTN_LEFT)) return false;
        if (y < v->y && d->top > 0) d->top--; else if (y > v->y + v->h && d->top + 1 < (int)doc_lines(d)) d->top++;
        if (x < v->text_x && d->left > 0) d->left -= g_cw; else if (x > v->x + v->w - 12) d->left += g_cw;
        d->caret = pos_at(v, x, y);
        if (v->click_n == 2) { while (d->caret < d->len && is_word(d->t[d->caret])) d->caret++; }
        d->want_col = doc_col_of(d, d->caret);
        return true;
    }
    if (type == 2) { bool was = v->drag; v->drag = false; return was; }
    return false;
}

/* ---------------------------------------------------------------- clipboard and edits */
void view_copy(View *v) { size_t n; char *s = doc_sel_text(v->d, &n); if (s) { cos_clip_set(s, n); free(s); } }
static void delete_sel(Doc *d) { size_t a, b; doc_sel(d, &a, &b); if (a != b) doc_replace(d, a, b - a, "", 0, false); }
void view_cut(View *v) { if (!v->editable) { view_copy(v); return; } view_copy(v); delete_sel(v->d); }
static void insert_text(Doc *d, const char *s, size_t n, bool merge) {
    size_t a, b; doc_sel(d, &a, &b);
    if (a != b) { doc_group_begin(d); doc_replace(d, a, b - a, s, n, false); doc_group_end(d); }
    else doc_replace(d, a, 0, s, n, merge);
    d->anchor = d->caret;
}
void view_paste(View *v) {
    if (!v->editable) return;
    static char buf[1 << 16];
    int64_t n = cos_clip_get(buf, sizeof buf - 1);
    if (n <= 0) return;
    size_t o = 0;
    for (int64_t i = 0; i < n; ++i) { if (buf[i] == '\r') { if (i + 1 < n && buf[i + 1] == '\n') continue; buf[o++] = '\n'; } else buf[o++] = buf[i]; }
    insert_text(v->d, buf, o, false);
}
void view_select_all(View *v) { v->d->anchor = 0; v->d->caret = v->d->len; }

static void sel_lines(Doc *d, size_t *l0, size_t *l1) {
    size_t a, b; doc_sel(d, &a, &b);
    *l0 = doc_line_of(d, a); *l1 = doc_line_of(d, b);
    if (b > a && b == doc_line_start(d, *l1) && *l1 > *l0) (*l1)--;                 /* a selection that ends at a line start does not include that line */
}
void view_indent(View *v, bool out) {
    Doc *d = v->d; if (d->readonly) return;
    size_t l0, l1; sel_lines(d, &l0, &l1);
    size_t ca = d->caret, an = d->anchor;
    doc_group_begin(d);
    for (size_t l = l1 + 1; l-- > l0; ) {
        size_t s = doc_line_start(d, l), e = doc_line_end(d, l);
        if (!out) { if (e > s) { doc_replace(d, s, 0, "    ", 4, false); if (ca >= s) ca += 4; if (an >= s) an += 4; } }
        else {
            size_t k = 0; while (k < 4 && s + k < e && d->t[s + k] == ' ') k++;
            if (!k && s < e && d->t[s] == '\t') k = 1;
            if (k) { doc_replace(d, s, k, "", 0, false); if (ca > s) ca -= (ca - s < k ? ca - s : k); if (an > s) an -= (an - s < k ? an - s : k); }
        }
    }
    doc_group_end(d);
    d->caret = ca > d->len ? d->len : ca; d->anchor = an > d->len ? d->len : an;
}
void view_comment_toggle(View *v) {
    Doc *d = v->d; if (d->readonly) return;
    const char *mark = d->lang == L_C ? "// " : "# "; size_t ml = strlen(mark);
    size_t l0, l1; sel_lines(d, &l0, &l1);
    bool all = true;
    for (size_t l = l0; l <= l1; ++l) {
        size_t s = doc_line_start(d, l), e = doc_line_end(d, l); while (s < e && (d->t[s] == ' ' || d->t[s] == '\t')) s++;
        if (s < e && strncmp(d->t + s, mark, ml - 1) != 0) { all = false; break; }
    }
    size_t ca = d->caret, an = d->anchor;
    doc_group_begin(d);
    for (size_t l = l1 + 1; l-- > l0; ) {
        size_t s = doc_line_start(d, l), e = doc_line_end(d, l), w = s; while (w < e && (d->t[w] == ' ' || d->t[w] == '\t')) w++;
        if (w == e) continue;
        if (all) { size_t k = ml - 1; if (w + ml <= e && d->t[w + ml - 1] == ' ') k = ml; doc_replace(d, w, k, "", 0, false); if (ca > w) ca -= (ca - w < k ? ca - w : k); if (an > w) an -= (an - w < k ? an - w : k); }
        else { doc_replace(d, w, 0, mark, ml, false); if (ca >= w) ca += ml; if (an >= w) an += ml; }
    }
    doc_group_end(d);
    d->caret = ca > d->len ? d->len : ca; d->anchor = an > d->len ? d->len : an;
}
static void dup_line(Doc *d) {
    size_t l = doc_line_of(d, d->caret), s = doc_line_start(d, l), e = doc_line_end(d, l);
    char *tmp = (char *)malloc(e - s + 2); if (!tmp) return;
    tmp[0] = '\n'; memcpy(tmp + 1, d->t + s, e - s);
    size_t off = d->caret - s;
    doc_replace(d, e, 0, tmp, e - s + 1, false); free(tmp);
    d->caret = d->anchor = e + 1 + off;
}
static void move_line(Doc *d, int dir) {
    size_t l = doc_line_of(d, d->caret), nl = doc_lines(d);
    if ((dir < 0 && l == 0) || (dir > 0 && l + 1 >= nl)) return;
    size_t a = dir < 0 ? l - 1 : l, b = a + 1;                    /* swap lines a and b */
    size_t as = doc_line_start(d, a), ae = doc_line_end(d, a), bs = doc_line_start(d, b), be = doc_line_end(d, b);
    size_t col = d->caret - doc_line_start(d, l);
    char *ta = (char *)malloc(ae - as + 1), *tb = (char *)malloc(be - bs + 1);
    if (ta && tb) {
        memcpy(ta, d->t + as, ae - as); memcpy(tb, d->t + bs, be - bs);
        char *both = (char *)malloc((be - as) + 1);
        if (both) {
            size_t o = 0; memcpy(both, tb, be - bs); o = be - bs; both[o++] = '\n'; memcpy(both + o, ta, ae - as); o += ae - as;
            doc_replace(d, as, be - as, both, o, false); free(both);
            size_t ns = dir < 0 ? as : as + (be - bs) + 1;
            d->caret = d->anchor = ns + col;
        }
    }
    free(ta); free(tb);
}

static size_t word_left(Doc *d, size_t p) {
    while (p > 0 && !is_word(d->t[p - 1])) p = u8_prev(d->t, p);
    while (p > 0 && is_word(d->t[p - 1])) p = u8_prev(d->t, p);
    return p;
}
static size_t word_right(Doc *d, size_t p) {
    while (p < d->len && !is_word(d->t[p])) p = u8_next(d->t, d->len, p);
    while (p < d->len && is_word(d->t[p])) p = u8_next(d->t, d->len, p);
    return p;
}
static void move_to(Doc *d, size_t p, bool shift, bool keepcol) {
    d->caret = p; if (!shift) d->anchor = p;
    if (!keepcol) d->want_col = doc_col_of(d, p);
}

bool view_key(View *v, const cos_win_event_t *ev, const char *commit) {
    Doc *d = v->d; if (!d) return false;
    bool ctrl = (ev->mods & COS_MOD_CTRL) != 0, shift = (ev->mods & COS_MOD_SHIFT) != 0, alt = (ev->mods & COS_MOD_ALT) != 0;
    bool ed = v->editable && !d->readonly;
    A.caret_t = cos_time_ms();
    if (commit && commit[0]) { if (ed) { insert_text(d, commit, strlen(commit), true); view_ensure_caret(v); } return true; }
    gutter_metrics(v);
    int sp = ev->special;
    if (ctrl && sp == COS_KEY_NONE) {
        char c = ev->ascii; if (c >= 'A' && c <= 'Z') c += 32;
        switch (c) {
            case 'a': view_select_all(v); return true;
            case 'c': view_copy(v); return true;
            case 'x': if (ed) { view_cut(v); view_ensure_caret(v); } else view_copy(v); return true;
            case 'v': if (ed) { view_paste(v); view_ensure_caret(v); } return true;
            case 'z': if (ed) { if (shift) doc_redo(d); else doc_undo(d); view_ensure_caret(v); } return true;
            case 'y': if (ed) { doc_redo(d); view_ensure_caret(v); } return true;
            case 'd': if (ed) { dup_line(d); view_ensure_caret(v); } return true;
            case '/': if (ed) { view_comment_toggle(v); } return true;
            default: return false;
        }
    }
    if (alt && ed && (sp == COS_KEY_UP || sp == COS_KEY_DOWN)) { move_line(d, sp == COS_KEY_UP ? -1 : 1); view_ensure_caret(v); return true; }
    size_t l = doc_line_of(d, d->caret);
    switch (sp) {
        case COS_KEY_LEFT:
            if (!shift && doc_has_sel(d)) { size_t a, b; doc_sel(d, &a, &b); move_to(d, a, false, false); }
            else move_to(d, ctrl ? word_left(d, d->caret) : u8_prev(d->t, d->caret), shift, false);
            view_ensure_caret(v); return true;
        case COS_KEY_RIGHT:
            if (!shift && doc_has_sel(d)) { size_t a, b; doc_sel(d, &a, &b); move_to(d, b, false, false); }
            else move_to(d, ctrl ? word_right(d, d->caret) : u8_next(d->t, d->len, d->caret), shift, false);
            view_ensure_caret(v); return true;
        case COS_KEY_UP: case COS_KEY_DOWN: case COS_KEY_PGUP: case COS_KEY_PGDN: {
            int step = (sp == COS_KEY_UP) ? -1 : (sp == COS_KEY_DOWN) ? 1 : (sp == COS_KEY_PGUP) ? -(v->vis_lines - 1) : (v->vis_lines - 1);
            long nl = (long)l + step; long last = (long)doc_lines(d) - 1;
            if (nl < 0) { move_to(d, 0, shift, false); }
            else if (nl > last) { move_to(d, d->len, shift, false); }
            else move_to(d, doc_pos_at_col(d, (size_t)nl, d->want_col), shift, true);
            if (sp == COS_KEY_PGUP || sp == COS_KEY_PGDN) { d->top += step; if (d->top < 0) d->top = 0; int mx = (int)doc_lines(d) - 1; if (d->top > mx) d->top = mx; }
            view_ensure_caret(v); return true; }
        case COS_KEY_HOME: {
            if (ctrl) { move_to(d, 0, shift, false); }
            else { size_t s = doc_line_start(d, l), f = s; while (f < d->len && (d->t[f] == ' ' || d->t[f] == '\t')) f++;
                   move_to(d, d->caret == f ? s : f, shift, false); }
            view_ensure_caret(v); return true; }
        case COS_KEY_END:
            move_to(d, ctrl ? d->len : doc_line_end(d, l), shift, false); view_ensure_caret(v); return true;
        default: break;
    }
    if (!ed) return sp != COS_KEY_NONE ? false : false;
    if (sp == COS_KEY_ENTER || (sp == COS_KEY_NONE && (ev->ascii == '\n' || ev->ascii == '\r'))) {
        size_t s = doc_line_start(d, l), a, b; doc_sel(d, &a, &b);
        size_t w = s; while (w < d->len && (d->t[w] == ' ' || d->t[w] == '\t') && w < a) w++;
        char ins[256]; size_t n = 0; ins[n++] = '\n';
        for (size_t k = s; k < w && n < 200; ++k) ins[n++] = d->t[k];
        size_t e = a; while (e > s && (d->t[e - 1] == ' ' || d->t[e - 1] == '\t')) e--;
        if (e > s && ((d->lang == L_PY && d->t[e - 1] == ':') || (d->lang == L_C && d->t[e - 1] == '{')) && n < 240) { memcpy(ins + n, "    ", 4); n += 4; }
        insert_text(d, ins, n, false); view_ensure_caret(v); return true;
    }
    if (sp == COS_KEY_BACKSPACE || (sp == COS_KEY_NONE && (ev->ascii == 8 || ev->ascii == 127))) {
        if (doc_has_sel(d)) { delete_sel(d); }
        else if (d->caret > 0) {
            size_t s = doc_line_start(d, l); bool spaces = d->caret > s;
            for (size_t k = s; k < d->caret; ++k) if (d->t[k] != ' ') { spaces = false; break; }
            size_t n = u8_prev(d->t, d->caret);
            if (spaces) { size_t col = d->caret - s; size_t del = col % 4 ? col % 4 : 4; n = d->caret - del; }
            doc_replace(d, n, d->caret - n, "", 0, false); d->anchor = d->caret;
        }
        d->want_col = doc_col_of(d, d->caret); view_ensure_caret(v); return true;
    }
    if (sp == COS_KEY_DELETE) {
        if (doc_has_sel(d)) delete_sel(d);
        else if (d->caret < d->len) { size_t n = u8_next(d->t, d->len, d->caret); doc_replace(d, d->caret, n - d->caret, "", 0, false); d->anchor = d->caret; }
        view_ensure_caret(v); return true;
    }
    if (sp == COS_KEY_TAB) {
        if (shift) { view_indent(v, true); }
        else {
            size_t a, b; doc_sel(d, &a, &b);
            if (a != b && doc_line_of(d, a) != doc_line_of(d, b)) view_indent(v, false);
            else { int col = doc_col_of(d, a); int n = 4 - col % 4; insert_text(d, "    ", (size_t)n, false); }
        }
        view_ensure_caret(v); return true;
    }
    if (sp == COS_KEY_NONE && ev->ascii >= 32 && ev->ascii < 127 && !ctrl && !alt) {
        char ch = ev->ascii; insert_text(d, &ch, 1, true); d->want_col = doc_col_of(d, d->caret); view_ensure_caret(v); return true;
    }
    return false;
}

/* ---------------------------------------------------------------- the one-line text field */
void field_set(Field *f, const char *s) { snprintf(f->text, sizeof f->text, "%s", s ? s : ""); f->len = (int)strlen(f->text); f->pos = f->sel = f->len; }
static void f_del(Field *f, int a, int b) { if (a > b) { int t = a; a = b; b = t; } if (a == b) return; memmove(f->text + a, f->text + b, (size_t)(f->len - b + 1)); f->len -= b - a; f->pos = f->sel = a; }
static void f_ins(Field *f, const char *s, size_t n) {
    if (f->pos != f->sel) f_del(f, f->pos, f->sel);
    if ((size_t)f->len + n >= sizeof f->text) n = sizeof f->text - 1 - (size_t)f->len;
    memmove(f->text + f->pos + n, f->text + f->pos, (size_t)(f->len - f->pos + 1)); memcpy(f->text + f->pos, s, n);
    f->len += (int)n; f->pos += (int)n; f->sel = f->pos;
}
bool field_key(Field *f, const cos_win_event_t *ev, const char *commit) {
    bool ctrl = (ev->mods & COS_MOD_CTRL) != 0, shift = (ev->mods & COS_MOD_SHIFT) != 0;
    A.caret_t = cos_time_ms();
    if (commit && commit[0]) { char b[260]; size_t n = 0; for (const char *p = commit; *p && n < sizeof b - 1; ++p) if (*p != '\n' && *p != '\r') b[n++] = *p; f_ins(f, b, n); return true; }
    int sp = ev->special;
    if (ctrl && sp == COS_KEY_NONE) {
        char c = ev->ascii | 32;
        if (c == 'a') { f->sel = 0; f->pos = f->len; return true; }
        if (c == 'c' || c == 'x') { if (f->pos != f->sel) { int a = f->pos < f->sel ? f->pos : f->sel, b = f->pos < f->sel ? f->sel : f->pos; cos_clip_set(f->text + a, (size_t)(b - a)); if (c == 'x') f_del(f, a, b); } return true; }
        if (c == 'v') { char b[260]; int64_t n = cos_clip_get(b, sizeof b - 1); if (n > 0) { size_t m = 0; for (int64_t i = 0; i < n; ++i) if (b[i] != '\n' && b[i] != '\r') b[m++] = b[i]; f_ins(f, b, m); } return true; }
        return false;
    }
    switch (sp) {
        case COS_KEY_LEFT:  if (f->pos != f->sel && !shift) f->pos = f->sel = f->pos < f->sel ? f->pos : f->sel; else { f->pos = (int)u8_prev(f->text, (size_t)f->pos); if (!shift) f->sel = f->pos; } return true;
        case COS_KEY_RIGHT: if (f->pos != f->sel && !shift) f->pos = f->sel = f->pos > f->sel ? f->pos : f->sel; else { f->pos = (int)u8_next(f->text, (size_t)f->len, (size_t)f->pos); if (!shift) f->sel = f->pos; } return true;
        case COS_KEY_HOME: f->pos = 0; if (!shift) f->sel = 0; return true;
        case COS_KEY_END: f->pos = f->len; if (!shift) f->sel = f->pos; return true;
        case COS_KEY_BACKSPACE: if (f->pos != f->sel) f_del(f, f->pos, f->sel); else if (f->pos > 0) f_del(f, (int)u8_prev(f->text, (size_t)f->pos), f->pos); return true;
        case COS_KEY_DELETE: if (f->pos != f->sel) f_del(f, f->pos, f->sel); else if (f->pos < f->len) f_del(f, f->pos, (int)u8_next(f->text, (size_t)f->len, (size_t)f->pos)); return true;
        default: break;
    }
    if (sp == COS_KEY_NONE && (ev->ascii == 8 || ev->ascii == 127)) { if (f->pos != f->sel) f_del(f, f->pos, f->sel); else if (f->pos > 0) f_del(f, (int)u8_prev(f->text, (size_t)f->pos), f->pos); return true; }
    if (sp == COS_KEY_NONE && ev->ascii >= 32 && ev->ascii < 127) { char ch = ev->ascii; f_ins(f, &ch, 1); return true; }
    return false;
}
void field_draw(cui_canvas *c, Field *f, int x, int y, int w, int h, bool numeric_hint) {
    (void)numeric_hint;
    cui_round_rect(c, x, y, w, h, 5, T->field);
    cui_round_rect_outline(c, x, y, w, h, 5, 1.0f, f->focus ? T->accent : T->border);
    cui_clip(c, x + 6, y + 1, w - 12, h - 2);
    int ty = y + (h - cui_font_height(g_ui)) / 2;
    int off = 0;
    char pre[270]; snprintf(pre, sizeof pre, "%.*s", f->pos, f->text);
    int cx = cui_text_width(g_ui, pre);
    if (cx > w - 20) off = cx - (w - 20);
    if (f->focus && f->pos != f->sel) {
        int a = f->pos < f->sel ? f->pos : f->sel, b = f->pos < f->sel ? f->sel : f->pos;
        char t1[270], t2[270]; snprintf(t1, sizeof t1, "%.*s", a, f->text); snprintf(t2, sizeof t2, "%.*s", b, f->text);
        int x1 = cui_text_width(g_ui, t1), x2 = cui_text_width(g_ui, t2);
        cui_fill(c, x + 8 + x1 - off, y + 3, x2 - x1, h - 6, T->sel);
    }
    cui_text(c, g_ui, x + 8 - off, ty, f->text, T->text);
    if (f->focus && ((cos_time_ms() - A.caret_t) / 530 & 1) == 0) cui_fill(c, x + 8 + cx - off, y + 4, 1, h - 8, T->text);
    cui_unclip(c);
}
int field_hit(Field *f, int fx, int mx) {
    int rel = mx - (fx + 8), best = f->len, w = 0;
    char b[270];
    int off = 0; { char pre[270]; snprintf(pre, sizeof pre, "%.*s", f->pos, f->text); int cx = cui_text_width(g_ui, pre); (void)cx; }
    for (int i = 0; i <= f->len; i = (int)u8_next(f->text, (size_t)f->len, (size_t)i)) {
        snprintf(b, sizeof b, "%.*s", i, f->text); w = cui_text_width(g_ui, b);
        if (w - off >= rel) { best = i; break; }
        if (i >= f->len) break;
    }
    return best;
}
