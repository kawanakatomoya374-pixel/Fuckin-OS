/* edview.c - the editor widget: gutter, text, selection, squiggles, caret,
 * scrollbar, find bar, completion popup and diagnostic tooltip.
 *
 * Metrics follow the mockup: the code font is a monospace face whose cell is
 * 0.604 em wide and whose lines are 1.467 em tall - 7.25 x 17.6 px at 12 px -
 * so a line's top is fractional and is rounded when drawn. Glyphs are placed
 * one by one on that grid, because cui_text() would round every advance to a
 * whole pixel and the columns would drift.
 */
#include "studio.h"

float ed_line_h(void) { return (float)G.code_px * 1.466667f; }
float ed_cell_w(void) { return (float)G.code_px * 0.604167f; }

#define TEXT_LEFT    62         /* text origin, relative to the editor's left edge */
#define GUTTER_PX    50
#define TOP_PAD_F    3.8f

static int line_top(int i) { return (int)((float)L.ed_y + TOP_PAD_F + (float)i * ed_line_h() + 0.5f); }

Rect ed_rect(void) { Rect r = { L.main_x, L.ed_y, L.main_w, L.ed_h }; return r; }

int ed_max_scroll(Doc *d) {
    layout_compute();
    int content = (int)((float)buf_lines(&d->b) * ed_line_h() + TOP_PAD_F);
    int m = content - L.ed_h + (int)(ed_line_h() * 4);        /* allow scrolling a few lines past the end */
    return m > 0 ? m : 0;
}

void ed_scroll_by(int dy, int dx) {
    Doc *d = app_doc();
    if (!d) return;
    d->scroll_y += dy; d->scroll_x += dx;
    int m = ed_max_scroll(d);
    if (d->scroll_y > m) d->scroll_y = m;
    if (d->scroll_y < 0) d->scroll_y = 0;
    if (d->scroll_x < 0) d->scroll_x = 0;
    G.dirty_frame = true;
}

/* keep the caret on screen; `center` puts it mid-view (used by jumps) */
void ed_ensure_caret_visible(Doc *d) {
    layout_compute();                                   /* may run before the first frame */
    size_t l = buf_line_of(&d->b, d->caret);
    float lh = ed_line_h();
    int y = (int)((float)l * lh);
    int top = d->scroll_y, bot = d->scroll_y + L.ed_h - (int)(lh * 1.5f) - 4;
    if (y < top) d->scroll_y = y - (int)lh;
    else if (y > bot) d->scroll_y = y - (L.ed_h - (int)(lh * 2.5f) - 4);
    int m = ed_max_scroll(d);
    if (d->scroll_y > m) d->scroll_y = m;
    if (d->scroll_y < 0) d->scroll_y = 0;
    int col = doc_visual_col(d, l, d->caret);
    int cx = (int)((float)col * ed_cell_w());
    int vis = L.main_w - TEXT_LEFT - 24;
    if (cx < d->scroll_x) d->scroll_x = cx > 40 ? cx - 40 : 0;
    else if (cx > d->scroll_x + vis - 20) d->scroll_x = cx - vis + 60;
    if (d->scroll_x < 0) d->scroll_x = 0;
}
void ed_center_caret(Doc *d) {
    layout_compute();
    size_t l = buf_line_of(&d->b, d->caret);
    int y = (int)((float)l * ed_line_h()) - L.ed_h / 2 + (int)ed_line_h();
    int m = ed_max_scroll(d);
    d->scroll_y = y < 0 ? 0 : y > m ? m : y;
    ed_ensure_caret_visible(d);
}

int ed_diag_on_line(const Doc *d, int line1) {
    for (int i = 0; i < G.ndiag; ++i)
        if (G.diags[i].line == line1 && (!strcmp(G.diags[i].path, d->path) || !strcmp(path_base(G.diags[i].path), path_base(d->path)))) return i;
    return -1;
}

/* pixel -> byte offset */
size_t ed_pos_at(int x, int y) {
    Doc *d = app_doc();
    if (!d) return 0;
    float lh = ed_line_h();
    int line = (int)(((float)(y - L.ed_y) - TOP_PAD_F + (float)d->scroll_y) / lh);
    if (y - L.ed_y < 0) line = 0;
    int col = (int)(((float)(x - L.main_x - TEXT_LEFT + d->scroll_x)) / ed_cell_w() + 0.5f);
    if (col < 0) col = 0;
    return doc_pos_from_line_col(d, line, col);
}

/* ---------------------------------------------------------------- */
/* drawing                                                           */
/* ---------------------------------------------------------------- */
static void put_cp(cui_font *f, int x, int y, const char *cp, uint32_t col) { cui_text(&G.cv, f, x, y, cp, col); }

static void draw_squiggle(int x0, int x1, int y, uint32_t col) {
    /* a 2-px-amplitude zig-zag, anti-aliased */
    float prev_x = (float)x0, prev_y = (float)y;
    for (int x = x0 + 2; x <= x1 + 1; x += 2) {
        float ny = (((x - x0) / 2) & 1) ? (float)y - 2.0f : (float)y;
        cui_line(&G.cv, prev_x, prev_y + 0.5f, (float)x, ny + 0.5f, 1.0f, col);
        prev_x = (float)x; prev_y = ny;
    }
}

static void draw_find_bar(void);
static void draw_completion(Doc *d);
static void draw_sig_help(Doc *d);

static int line_text_x(Doc *d, int col) { return L.main_x + TEXT_LEFT + (int)((float)col * ed_cell_w() + 0.5f) - d->scroll_x; }

void ed_draw(void) {
    Doc *d = app_doc();
    cui_canvas *c = &G.cv;
    Rect r = ed_rect();
    cui_unclip(c);
    cui_fill(c, r.x, r.y, r.w, r.h, T->bg);
    if (!d) {
        cui_text_center(c, font_ui(13), r.x, r.y, r.w, r.h - 40, "Open a file or press Ctrl+O", T->dim);
        cui_text_center(c, font_ui(11), r.x, r.y + 24, r.w, r.h - 40, "Ctrl+N new file   -   Ctrl+P go to file   -   F5 run", T->faint);
        return;
    }
    cui_fill(c, r.x, r.y, GUTTER_PX, r.h, T->gutter);
    cui_clip(c, r.x, r.y, r.w, r.h);

    cui_font *fc = font_code(), *fcb = font_code_bold();
    float lh = ed_line_h(), cw = ed_cell_w();
    size_t nl = buf_lines(&d->b);
    const char *txt = buf_text(&d->b);
    int first = (int)((float)d->scroll_y / lh);
    if (first < 0) first = 0;
    int last = first + (int)((float)r.h / lh) + 2;
    if ((size_t)last >= nl) last = (int)nl - 1;

    size_t cline = buf_line_of(&d->b, d->caret);
    size_t sa, sb; doc_sel_range(d, &sa, &sb);
    int band_h = (int)(lh + 0.99f);
    int tx0 = r.x + TEXT_LEFT - d->scroll_x;

    /* ---- pass 1: line bands (current line, error lines) and selection ---- */
    for (int l = first; l <= last; ++l) {
        int y = line_top(l) - d->scroll_y;
        int di = ed_diag_on_line(d, l + 1);
        if (di >= 0 && G.diags[di].sev == 0) cui_fill(c, r.x + GUTTER_PX, y, r.w - GUTTER_PX, band_h, T->errline);
        else if ((size_t)l == cline && !doc_has_sel(d)) cui_fill(c, r.x + GUTTER_PX, y, r.w - GUTTER_PX, band_h, T->curline);
        if (di >= 0 && G.diags[di].sev == 0) cui_fill(c, r.x, y, GUTTER_PX, band_h, T->errline);
        if (doc_has_sel(d)) {
            size_t ls = buf_line_start(&d->b, (size_t)l), le = buf_line_end(&d->b, (size_t)l);
            if (sb > ls && sa <= le) {
                size_t a = sa > ls ? sa : ls, b = sb < le ? sb : le;
                int c0 = doc_visual_col(d, (size_t)l, a), c1 = doc_visual_col(d, (size_t)l, b);
                int x0 = tx0 + (int)((float)c0 * cw + 0.5f), x1 = tx0 + (int)((float)c1 * cw + 0.5f);
                if (sb > le) x1 += 6;                                  /* selection includes the line break: show a nub */
                if (x1 > x0) cui_fill(c, x0, y + 1, x1 - x0, band_h - 1, T->sel);
            }
        }
    }
    /* mockup fixture only: a decoration standing in for the highlighted "canvas" word */
    if (G.demo && G.demo_extra) {
        int y = line_top(G.demo_line) - d->scroll_y;
        cui_fill(c, tx0 + (int)((float)G.demo_c0 * cw + 0.5f), y + 1, (int)((float)(G.demo_c1 - G.demo_c0) * cw + 0.5f), band_h - 1, T->sel);
    }

    /* ---- pass 2: text ---- */
    doc_hs_sync(d, (size_t)last);
    for (int l = first; l <= last; ++l) {
        size_t a = buf_line_start(&d->b, (size_t)l), e = buf_line_end(&d->b, (size_t)l);
        int y = line_top(l) - d->scroll_y;
        bool ic = d->hs && d->hs[l] != 0;
        Tok toks[256];
        int nt = hl_line(txt + a, (int)(e - a), &ic, toks, 256);
        int ti = 0, col = 0, tw = G.tab_width > 0 ? G.tab_width : 4;
        for (size_t i = a; i < e;) {
            unsigned char ch = (unsigned char)txt[i];
            size_t nx = doc_next_cp(d, i);
            int off = (int)(i - a);
            while (ti < nt && toks[ti].start + toks[ti].len <= off) ++ti;
            int kind = (ti < nt && off >= toks[ti].start) ? toks[ti].kind : TK_TEXT;
            if (ch == '\t') { col += tw - (col % tw); i = nx; continue; }
            int x = tx0 + (int)((float)col * cw + 0.5f);
            if (ch != ' ' && x + 12 > r.x + GUTTER_PX && x < r.x + r.w) {
                uint32_t colr = kind == TK_KW ? T->kw : kind == TK_TYPE ? T->type : kind == TK_FN ? T->fn : kind == TK_STR ? T->str :
                                kind == TK_NUM ? T->num : kind == TK_CMT ? T->cmt : kind == TK_PP ? T->pp : T->text;
                char cp[5]; size_t n = nx - i; if (n > 4) n = 4;
                memcpy(cp, txt + i, n); cp[n] = 0;
                put_cp(fc, x, y, cp, colr);
            }
            ++col; i = nx;
        }
        /* diagnostics: squiggle */
        int di = ed_diag_on_line(d, l + 1);
        if (di >= 0) {
            Diag *g = &G.diags[di];
            int x0 = tx0 + (int)((float)g->col * cw + 0.5f), x1 = tx0 + (int)((float)g->col_end * cw + 0.5f);
            draw_squiggle(x0, x1, y + (int)lh - 2, g->sev == 0 ? T->err : T->warn);
        }
    }

    /* ---- bracket match ---- */
    { size_t ba, bb;
      if (!doc_has_sel(d) && doc_match_bracket(d, d->caret, &ba, &bb)) {
          size_t pos[2] = { ba, bb };
          for (int k = 0; k < 2; ++k) {
              size_t l = buf_line_of(&d->b, pos[k]);
              if ((int)l < first || (int)l > last) continue;
              int x = tx0 + (int)((float)doc_visual_col(d, l, pos[k]) * cw + 0.5f), y = line_top((int)l) - d->scroll_y;
              cui_round_rect_outline(c, x - 1, y + 1, (int)(cw + 2.5f), band_h - 1, 2, 1.0f, T->accent);
          }
      } }

    /* ---- caret ---- */
    { uint64_t now = cos_time_ms();
      bool on = ((now - G.caret_t) / 530) % 2 == 0;
      if ((on || G.demo) && !G.dlg) {
          int x = tx0 + (int)((float)doc_visual_col(d, cline, d->caret) * cw + 0.5f), y = line_top((int)cline) - d->scroll_y;
          cui_fill(c, x, y + 1, 2, band_h - 2, T->accent);
      } }

    /* ---- gutter ---- */
    cui_clip(c, r.x, r.y, GUTTER_PX, r.h);
    cui_font *gf = font_code_px(G.code_px <= 12 ? 10 : G.code_px - 2, false), *gfb = font_code_px(G.code_px <= 12 ? 10 : G.code_px - 2, true);
    (void)fcb;
    for (int l = first; l <= last; ++l) {
        int y = line_top(l) - d->scroll_y;
        char num[16]; snprintf(num, sizeof num, "%d", l + 1);
        cui_font *f = (size_t)l == cline ? gfb : gf;
        int w = cui_text_width(f, num);
        cui_text(c, f, r.x + 257 - 236 - w + 1, y + 1, num, (size_t)l == cline ? 0xFFFFFF : T->dim);
        int di = ed_diag_on_line(d, l + 1);
        if (di >= 0) cui_circle(c, (float)(r.x + 10), (float)(y + band_h / 2), 4.6f, G.diags[di].sev == 0 ? 0xFF6975 : T->warn);
    }
    cui_clip(c, r.x, r.y, r.w, r.h);

    /* ---- vertical scrollbar thumb ---- */
    { int m = ed_max_scroll(d);
      if (m > 0) {
          int track = r.h - 16, content = (int)((float)nl * lh + TOP_PAD_F) + (int)(lh * 4);
          int th = (int)((float)track * (float)r.h / (float)content);
          if (th < 24) th = 24;
          if (th > track) th = track;
          int ty = r.y + 8 + (int)((long)(track - th) * d->scroll_y / m);
          cui_round_rect(c, r.x + r.w - 8, ty, 5, th, 2, T->thumb);
      } }

    if (G.find_open) draw_find_bar();
    if (G.comp_open) draw_completion(d);
    else if (G.sig_open) draw_sig_help(d);
    cui_unclip(c);
}

/* ---------------------------------------------------------------- */
/* find bar                                                          */
/* ---------------------------------------------------------------- */
static Rect find_rect(void) {
    Rect r = ed_rect();
    int w = G.find_replace ? 388 : 330, h = G.find_replace ? 60 : 32;
    Rect f = { r.x + r.w - w - 20, r.y + 6, w, h };
    return f;
}
static void field(int x, int y, int w, int h, const char *text, bool focus, const char *hint) {
    cui_canvas *c = &G.cv;
    cui_round_rect(c, x, y, w, h, 4, T->field);
    cui_round_rect_outline(c, x, y, w, h, 4, 1.0f, focus ? T->accent : T->border);
    cui_font *f = font_ui(12);
    if (text[0]) cui_text_fit(c, f, x + 8, y + (h - cui_font_height(f)) / 2, w - 16, text, T->text);
    else if (hint) cui_text(c, f, x + 8, y + (h - cui_font_height(f)) / 2, hint, T->faint);
    if (focus && ((cos_time_ms() - G.caret_t) / 530) % 2 == 0) cui_fill(c, x + 8 + cui_text_width(f, text), y + 5, 1, h - 10, T->text);
}
static void toggle_btn(int x, int y, int w, int h, const char *label, bool on) {
    cui_canvas *c = &G.cv;
    cui_round_rect(c, x, y, w, h, 4, on ? T->accent : T->btn);
    cui_text_center(c, font_ui(11), x, y, w, h, label, on ? T->on_accent : T->text);
}
static void draw_find_bar(void) {
    cui_canvas *c = &G.cv;
    Rect f = find_rect();
    cui_round_rect(c, f.x - 1, f.y - 1, f.w + 2, f.h + 2, 7, T->border);
    cui_round_rect(c, f.x, f.y, f.w, f.h, 6, T->tip_bg);
    field(f.x + 8, f.y + 5, 150, 22, G.find.what, G.find_focus == 0, "Find");
    toggle_btn(f.x + 164, f.y + 5, 24, 22, "Aa", !G.find.icase ? true : false);
    toggle_btn(f.x + 192, f.y + 5, 24, 22, "ab", G.find.word);
    char cnt[32];
    if (G.find.what[0]) snprintf(cnt, sizeof cnt, "%d found", G.find.count); else cnt[0] = 0;
    cui_text(c, font_ui(11), f.x + 222, f.y + 10, cnt, G.find.count ? T->dim : (G.find.what[0] ? T->err : T->dim));
    int bx = f.x + f.w - 8 - 24 * 3 - 4 * 2;
    cui_round_rect(c, bx, f.y + 5, 24, 22, 4, T->btn);        cui_triangle(c, (float)(bx + 12), (float)(f.y + 12), (float)(bx + 7), (float)(f.y + 19), (float)(bx + 17), (float)(f.y + 19), T->text);
    cui_round_rect(c, bx + 28, f.y + 5, 24, 22, 4, T->btn);   cui_triangle(c, (float)(bx + 40), (float)(f.y + 20), (float)(bx + 35), (float)(f.y + 13), (float)(bx + 45), (float)(f.y + 13), T->text);
    cui_round_rect(c, bx + 56, f.y + 5, 24, 22, 4, T->btn);   cui_line(c, (float)(bx + 63), (float)(f.y + 12), (float)(bx + 73), (float)(f.y + 20), 1.6f, T->text); cui_line(c, (float)(bx + 73), (float)(f.y + 12), (float)(bx + 63), (float)(f.y + 20), 1.6f, T->text);
    if (G.find_replace) {
        field(f.x + 8, f.y + 32, 150, 22, G.find.with, G.find_focus == 1, "Replace");
        cui_round_rect(c, f.x + 164, f.y + 32, 70, 22, 4, T->btn);  cui_text_center(c, font_ui(11), f.x + 164, f.y + 32, 70, 22, "Replace", T->text);
        cui_round_rect(c, f.x + 238, f.y + 32, 84, 22, 4, T->btn);  cui_text_center(c, font_ui(11), f.x + 238, f.y + 32, 84, 22, "Replace All", T->text);
    }
}
/* returns true if the click was consumed */
bool ed_find_click(int x, int y) {
    if (!G.find_open) return false;
    Rect f = find_rect();
    if (!pt_in(x, y, f.x, f.y, f.w, f.h)) return false;
    Doc *d = app_doc();
    int lx = x - f.x, ly = y - f.y;
    if (ly >= 5 && ly < 27) {
        if (lx >= 8 && lx < 158) G.find_focus = 0;
        else if (lx >= 164 && lx < 188) { G.find.icase = !G.find.icase; }
        else if (lx >= 192 && lx < 216) { G.find.word = !G.find.word; }
        else {
            int bx = f.w - 8 - 24 * 3 - 4 * 2;
            if (d && lx >= bx && lx < bx + 24) { long p = find_next(d, &G.find, d->anchor < d->caret ? d->anchor : d->caret, true); if (p >= 0) { d->anchor = (size_t)p; d->caret = (size_t)p + strlen(G.find.what); ed_ensure_caret_visible(d); } }
            else if (d && lx >= bx + 28 && lx < bx + 52) { long p = find_next(d, &G.find, d->caret, false); if (p >= 0) { d->anchor = (size_t)p; d->caret = (size_t)p + strlen(G.find.what); ed_ensure_caret_visible(d); } }
            else if (lx >= bx + 56 && lx < bx + 80) { G.find_open = false; }
        }
        if (d) G.find.count = find_count(d, &G.find);
    } else if (G.find_replace && ly >= 32 && ly < 54) {
        if (lx >= 8 && lx < 158) G.find_focus = 1;
        else if (d && lx >= 164 && lx < 234) {
            size_t a, b; doc_sel_range(d, &a, &b);
            if (b - a == strlen(G.find.what) && b > a) { doc_group_begin(d); doc_replace(d, a, b - a, G.find.with, strlen(G.find.with), false); doc_group_end(d); d->caret = d->anchor = a + strlen(G.find.with); }
            long p = find_next(d, &G.find, d->caret, false); if (p >= 0) { d->anchor = (size_t)p; d->caret = (size_t)p + strlen(G.find.what); ed_ensure_caret_visible(d); }
            G.find.count = find_count(d, &G.find);
        } else if (d && lx >= 238 && lx < 322) {
            int n = find_replace_all(d, &G.find);
            app_status("Replaced %d occurrence%s", n, n == 1 ? "" : "s");
            G.find.count = find_count(d, &G.find);
        }
    }
    G.dirty_frame = true;
    return true;
}
/* keyboard input while the find bar has focus. Returns true if consumed. */
bool ed_find_key(const cos_win_event_t *ev) {
    if (!G.find_open) return false;
    Doc *d = app_doc();
    char *buf = G.find_focus == 1 ? G.find.with : G.find.what;
    size_t cap = 128, n = strlen(buf);
    if (ev->special == COS_KEY_ESC) { G.find_open = false; G.dirty_frame = true; return true; }
    if (ev->special == COS_KEY_TAB && G.find_replace) { G.find_focus ^= 1; G.dirty_frame = true; return true; }
    if (ev->special == COS_KEY_ENTER) {
        if (d && G.find.what[0]) {
            if (G.find_focus == 1) {
                size_t a, b; doc_sel_range(d, &a, &b);
                if (b > a && b - a == strlen(G.find.what)) { doc_group_begin(d); doc_replace(d, a, b - a, G.find.with, strlen(G.find.with), false); doc_group_end(d); d->caret = d->anchor = a + strlen(G.find.with); }
            }
            bool back = (ev->mods & COS_MOD_SHIFT) != 0;
            size_t from = back ? (d->anchor < d->caret ? d->anchor : d->caret) : (d->anchor > d->caret ? d->anchor : d->caret);
            long p = find_next(d, &G.find, from, back);
            if (p >= 0) { d->anchor = (size_t)p; d->caret = (size_t)p + strlen(G.find.what); ed_ensure_caret_visible(d); }
            G.find.count = find_count(d, &G.find);
        }
        G.dirty_frame = true; return true;
    }
    if (ev->special == COS_KEY_BACKSPACE) { if (n) { while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) --n; buf[n > 0 ? n - 1 : 0] = 0; if (n == 0) buf[0] = 0; else buf[n - 1] = 0; } }
    else if (ev->special == COS_KEY_NONE && ev->ascii >= 32 && !(ev->mods & COS_MOD_CTRL)) { if (n + 1 < cap) { buf[n] = ev->ascii; buf[n + 1] = 0; } }
    else return false;
    if (d && G.find_focus == 0) {
        G.find.count = find_count(d, &G.find);
        long p = find_next(d, &G.find, d->anchor < d->caret ? d->anchor : d->caret, false);          /* incremental: jump as you type */
        if (p >= 0) { d->anchor = (size_t)p; d->caret = (size_t)p + strlen(G.find.what); ed_ensure_caret_visible(d); }
    }
    G.caret_t = cos_time_ms();
    G.dirty_frame = true;
    return true;
}

/* ---------------------------------------------------------------- */
/* completion popup                                                  */
/* ---------------------------------------------------------------- */
static const char *comp_badge(uint8_t k) {
    switch (k) {
        case DK_FUNC: return "ƒ"; case DK_VAR: return "v"; case DK_TYPEDEF: return "T";
        case DK_STRUCT: return "S"; case DK_UNION: return "U"; case DK_ENUM: return "E";
        case DK_MACRO: return "#"; case DK_ENUMCONST: return "c"; case DK_MEMBER: return "m";
        default: return "k";
    }
}
static uint32_t comp_badge_color(uint8_t k, const Theme *t) {
    switch (k) {
        case DK_FUNC: return 0x7C9CFF; case DK_VAR: case DK_MEMBER: return 0x6FCF97;
        case DK_TYPEDEF: case DK_STRUCT: case DK_UNION: case DK_ENUM: return 0xE0A75E;
        case DK_MACRO: return 0xD98CD9; case DK_ENUMCONST: return 0x6FCF97;
        default: return t->dim;
    }
}
static void draw_completion(Doc *d) {
    cui_canvas *c = &G.cv;
    cui_unclip(c);   /* the gutter/text drawing just before this leaves a narrow clip active; a popup must ignore it */
    size_t l = buf_line_of(&d->b, G.comp_from);
    int x = line_text_x(d, doc_visual_col(d, l, G.comp_from)) - 4;
    int y = line_top((int)buf_line_of(&d->b, d->caret)) - d->scroll_y + (int)ed_line_h() + 2;
    int w = 300, rows = G.comp_n, h = rows * 22 + 6;
    Rect r = ed_rect();
    if (x + w > r.x + r.w - 10) x = r.x + r.w - w - 10;
    if (y + h > r.y + r.h) y = line_top((int)buf_line_of(&d->b, d->caret)) - d->scroll_y - h - 2;
    cui_round_rect(c, x - 1, y - 1, w + 2, h + 2, 6, T->border);
    cui_round_rect(c, x, y, w, h, 5, T->tip_bg);
    cui_font *f = font_code_px(12, false), *fb = font_code_px(11, true), *fd = font_code_px(11, false);
    for (int i = 0; i < rows; ++i) {
        int ry = y + 3 + i * 22;
        bool sel = i == G.comp_sel;
        if (sel) cui_round_rect(c, x + 3, ry, w - 6, 21, 3, T->row_sel);
        uint32_t tc = sel ? 0xFFFFFF : T->text;
        if (G.comp_kind[i] != 255) {
            uint32_t bc = comp_badge_color(G.comp_kind[i], T);
            cui_round_rect(c, x + 6, ry + 3, 16, 16, 4, sel ? cui_mix(bc, 0xFFFFFF, 40) : bc);
            cui_text_center(c, fb, x + 6, ry + 2, 16, 16, comp_badge(G.comp_kind[i]), 0x101010);
        }
        cui_text(c, f, x + 30, ry + 3, G.comp_items[i], tc);
        if (G.comp_detail[i][0]) {
            int nw = cui_text_width(f, G.comp_items[i]);
            cui_text_fit(c, fd, x + 34 + nw, ry + 5, w - 40 - nw, G.comp_detail[i], sel ? 0xE8ECFF : T->dim);
        }
    }
}

/* ---------------------------------------------------------------- */
/* signature help: a one-line tip above/below the caret showing the    */
/* enclosing call's parameters, with the active one picked out         */
/* ---------------------------------------------------------------- */
static void draw_sig_help(Doc *d) {
    cui_canvas *c = &G.cv;
    cui_unclip(c);   /* same reason as draw_completion: don't inherit the editor's own text clip */
    size_t l = buf_line_of(&d->b, G.sig_paren_pos);
    int x = line_text_x(d, doc_visual_col(d, l, G.sig_paren_pos)) - 4;
    int y = line_top((int)l) - d->scroll_y - 26;
    Rect r = ed_rect();
    if (y < r.y) y = line_top((int)buf_line_of(&d->b, d->caret)) - d->scroll_y + (int)ed_line_h() + 2;
    cui_font *f = font_code_px(12, false), *fb = font_code_px(12, true);
    int w = cui_text_width(fb, G.sig_name) + 14;
    for (int i = 0; i < G.sig_nparams; ++i) w += cui_text_width(f, G.sig_params[i]) + (i ? 12 : 6);
    if (w < 60) w = 60;
    if (w > 480) w = 480;
    if (x + w > r.x + r.w - 10) x = r.x + r.w - w - 10;
    if (x < r.x) x = r.x + 4;
    cui_round_rect(c, x - 1, y - 1, w + 2, 24, 6, T->border);
    cui_round_rect(c, x, y, w, 22, 5, T->tip_bg);
    cui_clip(c, x + 4, y, w - 8, 22);
    int px = x + 8;
    px += cui_text(c, fb, px, y + 4, G.sig_name, T->accent);
    px += cui_text(c, f, px, y + 4, "(", T->dim);
    for (int i = 0; i < G.sig_nparams; ++i) {
        if (i) px += cui_text(c, f, px, y + 4, ", ", T->dim);
        px += cui_text(c, f, px, y + 4, G.sig_params[i], i == G.sig_active ? T->accent : T->dim);
    }
    cui_text(c, f, px, y + 4, ")", T->dim);
    cui_unclip(c);
}

/* ---------------------------------------------------------------- */
/* mouse                                                             */
/* ---------------------------------------------------------------- */
bool ed_find_click(int x, int y);

bool ed_mouse_down(int x, int y, uint8_t button, uint8_t mods) {
    Doc *d = app_doc();
    Rect r = ed_rect();
    if (!pt_in(x, y, r.x, r.y, r.w, r.h)) return false;
    if (ed_find_click(x, y)) return true;
    if (!d) return true;
    G.comp_open = false; G.sig_open = false;
    if (button == COS_MOUSE_BTN_RIGHT) return true;
    /* scrollbar */
    if (x >= r.x + r.w - 12 && ed_max_scroll(d) > 0) {
        G.drag_kind = 4; G.drag_off = 0;
        int m = ed_max_scroll(d), track = r.h - 16;
        int rel = y - r.y - 8; if (rel < 0) rel = 0; if (rel > track) rel = track;
        d->scroll_y = (int)((long)m * rel / track);
        G.dirty_frame = true;
        return true;
    }
    /* gutter click on an error mark: show the message */
    if (x < r.x + GUTTER_PX) {
        float lh = ed_line_h();
        int line = (int)(((float)(y - r.y) - TOP_PAD_F + (float)d->scroll_y) / lh) + 1;
        int di = ed_diag_on_line(d, line);
        if (di >= 0) { G.tip_visible = true; G.tip_diag = di; G.tip_t = cos_time_ms(); }
        else { size_t p = doc_pos_from_line_col(d, line - 1, 0); d->anchor = p; d->caret = doc_pos_from_line_col(d, line, 0); if (line >= (int)buf_lines(&d->b)) d->caret = buf_len(&d->b); G.drag_kind = 1; }
        G.dirty_frame = true;
        return true;
    }
    size_t p = ed_pos_at(x, y);
    uint64_t now = cos_time_ms();
    bool same = G.click_count > 0 && now - G.last_click_t < 450 && abs(x - G.last_click_x) < 5 && abs(y - G.last_click_y) < 5;
    G.click_count = same ? (G.click_count % 3) + 1 : 1;
    G.last_click_t = now; G.last_click_x = x; G.last_click_y = y;
    if (G.click_count == 2) doc_select_word_at(d, p);
    else if (G.click_count == 3) doc_select_line_at(d, p);
    else { d->caret = p; if (!(mods & COS_MOD_SHIFT)) d->anchor = p; }
    d->want_col = doc_visual_col(d, buf_line_of(&d->b, d->caret), d->caret);
    G.drag_kind = 1;
    G.caret_t = now;
    G.tip_visible = false;
    G.dirty_frame = true;
    return true;
}

void ed_mouse_drag(int x, int y) {
    Doc *d = app_doc();
    if (!d) return;
    Rect r = ed_rect();
    if (G.drag_kind == 4) {
        int m = ed_max_scroll(d), track = r.h - 16;
        int rel = y - r.y - 8; if (rel < 0) rel = 0; if (rel > track) rel = track;
        d->scroll_y = (int)((long)m * rel / track);
    } else if (G.drag_kind == 1) {
        d->caret = ed_pos_at(x, y);
        if (y < r.y + 8) ed_scroll_by(-8, 0); else if (y > r.y + r.h - 8) ed_scroll_by(8, 0);
        d->want_col = doc_visual_col(d, buf_line_of(&d->b, d->caret), d->caret);
    }
    G.dirty_frame = true;
}

/* hover: show the tooltip for a diagnostic when the pointer rests on it */
void ed_mouse_move(int x, int y) {
    Doc *d = app_doc();
    Rect r = ed_rect();
    int hit = -1;
    if (d && pt_in(x, y, r.x, r.y, r.w, r.h) && !G.dlg) {
        float lh = ed_line_h();
        int line = (int)(((float)(y - r.y) - TOP_PAD_F + (float)d->scroll_y) / lh) + 1;
        int di = ed_diag_on_line(d, line);
        if (di >= 0) {
            Diag *g = &G.diags[di];
            int x0 = line_text_x(d, g->col) - 2, x1 = line_text_x(d, g->col_end) + 2;
            if (x < r.x + GUTTER_PX || (x >= x0 && x <= x1)) hit = di;
        }
    }
    if (hit != (G.tip_visible ? G.tip_diag : -1)) { G.tip_visible = hit >= 0; G.tip_diag = hit; G.tip_t = cos_time_ms(); G.dirty_frame = true; }
}

/* tooltip: drawn last, over everything in the editor and panel */
void ed_draw_tooltip(void) {
    if (!G.tip_visible || G.tip_diag < 0 || G.tip_diag >= G.ndiag) return;
    Doc *d = app_doc();
    if (!d) return;
    Diag *g = &G.diags[G.tip_diag];
    cui_canvas *c = &G.cv;
    cui_unclip(c);
    char l1[200], l2[220];
    snprintf(l1, sizeof l1, "%s: %s", g->sev == 0 ? "error" : "warning", g->msg);
    if (g->fix_ch) snprintf(l2, sizeof l2, "%s:%d   Quick fix: insert '%c'   (Alt+Enter)", path_base(g->file), g->rep_line, g->fix_ch);
    else snprintf(l2, sizeof l2, "%s:%d", path_base(g->file), g->rep_line);
    cui_font *fb = font_ui_bold(12), *fr = font_ui(11);
    int w1 = cui_text_width(fb, l1), w2 = cui_text_width(fr, l2);
    int w = (w1 > w2 ? w1 : w2) + 20, h = 45;
    Rect r = ed_rect();
    int ly = line_top((int)g->line - 1) - d->scroll_y;
    int x = G.demo ? G.mx : G.mx - 8;
    int y = ly + (int)ed_line_h() + 7;
    if (x + w > r.x + r.w - 6) x = r.x + r.w - w - 6;
    if (x < r.x + GUTTER_PX) x = r.x + GUTTER_PX;
    if (y + h > r.y + r.h) y = ly - h - 4;
    uint32_t bc = g->sev == 0 ? T->err : T->warn;
    cui_round_rect(c, x, y, w, h, 7, cui_mix(T->tip_bg, bc, 205));
    cui_round_rect(c, x + 1, y + 1, w - 2, h - 2, 6, T->tip_bg);
    cui_text(c, fb, x + 10, y + 7, l1, bc);
    cui_text(c, fr, x + 11, y + 26, l2, T->dim);
}

/* ---------------------------------------------------------------- */
/* keys (editor focus)                                               */
/* ---------------------------------------------------------------- */
bool ed_find_key(const cos_win_event_t *ev);

bool ed_key(const cos_win_event_t *ev) {
    Doc *d = app_doc();
    if (!d) return false;
    bool sh = (ev->mods & COS_MOD_SHIFT) != 0, ctrl = (ev->mods & COS_MOD_CTRL) != 0, alt = (ev->mods & COS_MOD_ALT) != 0;
    int lines_page = (int)((float)L.ed_h / ed_line_h()) - 1;
    bool typed = false;
    if (G.comp_open) {
        if (ev->special == COS_KEY_DOWN) { G.comp_sel = (G.comp_sel + 1) % G.comp_n; G.dirty_frame = true; return true; }
        if (ev->special == COS_KEY_UP) { G.comp_sel = (G.comp_sel + G.comp_n - 1) % G.comp_n; G.dirty_frame = true; return true; }
        if (ev->special == COS_KEY_TAB || ev->special == COS_KEY_ENTER) { comp_accept(d); ed_ensure_caret_visible(d); G.dirty_frame = true; return true; }
        if (ev->special == COS_KEY_ESC) { G.comp_open = false; G.dirty_frame = true; return true; }
    }
    if (ctrl && ev->special == COS_KEY_NONE && ev->ascii == ' ') {   /* explicit trigger: works even on a 0- or 1-letter prefix */
        comp_update_auto(d, true); G.dirty_frame = true; return true;
    }
    switch (ev->special) {
        case COS_KEY_LEFT:  doc_move_left(d, sh, ctrl); break;
        case COS_KEY_RIGHT: doc_move_right(d, sh, ctrl); break;
        case COS_KEY_UP:    if (alt) doc_move_line(d, -1); else doc_move_vert(d, -1, sh); break;
        case COS_KEY_DOWN:  if (alt) doc_move_line(d, 1); else doc_move_vert(d, 1, sh); break;
        case COS_KEY_HOME:  if (ctrl) doc_move_doc_edge(d, false, sh); else doc_move_home(d, sh); break;
        case COS_KEY_END:   if (ctrl) doc_move_doc_edge(d, true, sh); else doc_move_end(d, sh); break;
        case COS_KEY_PGUP:  doc_move_vert(d, -lines_page, sh); break;
        case COS_KEY_PGDN:  doc_move_vert(d, lines_page, sh); break;
        case COS_KEY_ENTER: doc_newline(d); typed = true; break;
        case COS_KEY_BACKSPACE: doc_backspace(d, ctrl); typed = true; break;
        case COS_KEY_DELETE: doc_delete(d, ctrl); break;
        case COS_KEY_TAB:   doc_indent(d, sh); break;
        case COS_KEY_ESC:   d->anchor = d->caret; G.comp_open = false; G.sig_open = false; break;
        case COS_KEY_NONE:
            if (ev->ascii >= 32 && ev->ascii < 127 && !ctrl && !alt) { doc_type_char(d, ev->ascii); typed = true; }
            else return false;
            break;
        default: return false;
    }
    ed_ensure_caret_visible(d);
    G.caret_t = cos_time_ms();
    G.dirty_frame = true;
    G.tip_visible = false;
    /* completion: any identifier char keeps the popup (or opens it past 2 letters); '.', the second
     * char of "->", '"', '<' and '#' open the other three modes (see comp_update_auto); anything
     * else typed closes it. Backspace re-derives from the (now shorter) context instead of closing. */
    bool idch = ev->special == COS_KEY_NONE && ((ev->ascii >= 'a' && ev->ascii <= 'z') || (ev->ascii >= 'A' && ev->ascii <= 'Z') || ev->ascii == '_' || (ev->ascii >= '0' && ev->ascii <= '9'));
    bool trigger_ch = ev->special == COS_KEY_NONE && (ev->ascii == '.' || ev->ascii == '>' || ev->ascii == '"' || ev->ascii == '<' || ev->ascii == '#' || ev->ascii == '/');
    if (typed && (idch || trigger_ch)) comp_update_auto(d, false);
    else if (typed && ev->special == COS_KEY_BACKSPACE) comp_update_auto(d, false);
    else if (typed) G.comp_open = false;
    /* signature help tracks the enclosing call independently of the completion popup, and is
     * suppressed while the popup is open so the two never overlap on screen */
    if (typed) { if (!G.comp_open) sig_update(d); else G.sig_open = false; }
    return true;
}
