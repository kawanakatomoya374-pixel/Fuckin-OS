/* cos_widget.c - see cos_widget.h for the design. */
#include <stdlib.h>
#include <string.h>
#include "cos_widget.h"

#define MAX_DAMAGE 16

struct cw_app {
    int64_t win;
    cui_canvas canvas;
    cw_widget *root;
    cw_widget *hover, *capture, *focus;
    cw_rect damage[MAX_DAMAGE];
    int ndamage;
    bool first_frame, need_layout, quit;
    cw_timer_fn timer_fn; void *timer_user; uint32_t timer_interval; uint64_t timer_next;
    uint32_t st_frames, st_rects; uint64_t st_pixels;
};

/* ---------------------------------------------------------------- geometry helpers */
static bool rect_isect(cw_rect a, cw_rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
static cw_rect rect_union(cw_rect a, cw_rect b) {
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (cw_rect){ x0, y0, x1 - x0, y1 - y0 };
}

cw_rect cw_widget_abs_rect(const cw_widget *w) {
    int x = 0, y = 0;
    for (const cw_widget *p = w; p; p = p->parent) { x += p->x; y += p->y; }
    return (cw_rect){ x, y, w->w, w->h };
}

/* ---------------------------------------------------------------- damage */
void cw_invalidate_rect(cw_app *app, int x, int y, int w, int h) {
    if (!app) return;
    cw_rect r = { x, y, w, h };
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > app->canvas.w) r.w = app->canvas.w - r.x;
    if (r.y + r.h > app->canvas.h) r.h = app->canvas.h - r.y;
    if (r.w <= 0 || r.h <= 0) return;
    /* merge with anything it touches, so overlapping updates become one rectangle */
    for (int i = 0; i < app->ndamage;) {
        cw_rect d = app->damage[i];
        bool touch = r.x <= d.x + d.w && d.x <= r.x + r.w && r.y <= d.y + d.h && d.y <= r.y + r.h;
        if (touch) {
            r = rect_union(r, d);
            app->damage[i] = app->damage[--app->ndamage];
            i = 0;                                   /* the union may now touch others */
        } else ++i;
    }
    if (app->ndamage == MAX_DAMAGE) {                /* overflow: fold everything into one rectangle */
        for (int i = 0; i < app->ndamage; ++i) r = rect_union(r, app->damage[i]);
        app->ndamage = 0;
    }
    app->damage[app->ndamage++] = r;
}

void cw_invalidate(cw_widget *w) {
    if (!w || !w->app) return;
    cw_rect r = cw_widget_abs_rect(w);
    /* a widget hidden anywhere up its chain paints nothing */
    for (cw_widget *p = w; p; p = p->parent) if (!(p->flags & CW_VISIBLE)) return;
    cw_invalidate_rect(w->app, r.x, r.y, r.w, r.h);
}

/* ---------------------------------------------------------------- tree */
static cw_widget *widget_new(cw_widget *parent, const cw_vtable *vt, size_t size, int w, int h) {
    cw_widget *n = (cw_widget *)calloc(1, size);
    if (!n) return NULL;
    n->vt = vt; n->w = w; n->h = h;
    n->flags = CW_VISIBLE | CW_ENABLED;
    if (parent) {
        n->parent = parent; n->app = parent->app;
        if (parent->last_child) parent->last_child->next = n; else parent->first_child = n;
        parent->last_child = n;
        if (n->app) n->app->need_layout = true;
    }
    return n;
}

static void unlink_child(cw_widget *w) {
    cw_widget *p = w->parent;
    if (!p) return;
    cw_widget *prev = NULL;
    for (cw_widget *c = p->first_child; c; prev = c, c = c->next) {
        if (c != w) continue;
        if (prev) prev->next = c->next; else p->first_child = c->next;
        if (p->last_child == c) p->last_child = prev;
        break;
    }
    w->parent = NULL; w->next = NULL;
}

static void free_tree(cw_widget *w) {
    cw_app *app = w->app;
    if (app) {
        if (app->hover == w) app->hover = NULL;
        if (app->capture == w) app->capture = NULL;
        if (app->focus == w) app->focus = NULL;
    }
    for (cw_widget *c = w->first_child; c;) { cw_widget *n = c->next; free_tree(c); c = n; }
    if (w->vt && w->vt->destroy) w->vt->destroy(w);
    free(w);
}

void cw_widget_destroy(cw_widget *w) {
    if (!w || !w->parent) return;
    cw_invalidate(w);
    cw_app *app = w->app;
    unlink_child(w);
    free_tree(w);
    if (app) app->need_layout = true;
}

void cw_widget_set_visible(cw_widget *w, bool v) {
    if (!w || (bool)(w->flags & CW_VISIBLE) == v) return;
    if (!v) cw_invalidate(w);                       /* repaint what it covered while it still counts as visible */
    if (v) w->flags |= CW_VISIBLE; else w->flags &= ~CW_VISIBLE;
    if (v) cw_invalidate(w);
    if (w->app) w->app->need_layout = true;
}
void cw_widget_set_enabled(cw_widget *w, bool e) {
    if (!w || (bool)(w->flags & CW_ENABLED) == e) return;
    if (e) w->flags |= CW_ENABLED; else w->flags &= ~CW_ENABLED;
    cw_invalidate(w);
}
void cw_widget_set_geometry(cw_widget *w, int x, int y, int ww, int hh) {
    if (!w || (w->x == x && w->y == y && w->w == ww && w->h == hh)) return;
    cw_invalidate(w);
    w->x = x; w->y = y; w->w = ww; w->h = hh;
    cw_invalidate(w);
    if (w->vt && w->vt->layout) w->vt->layout(w);
}
void cw_widget_set_weight(cw_widget *w, int weight) { if (w && w->weight != weight) { w->weight = weight; if (w->app) w->app->need_layout = true; } }

/* ---------------------------------------------------------------- panel */
typedef struct { cw_widget base; uint32_t bg; cw_layout mode; int pad, gap; } panel_t;

static void panel_draw(cw_widget *w, cui_canvas *c, int ox, int oy) {
    panel_t *p = (panel_t *)w;
    if (p->bg != 0xFFFFFFFFu) cui_fill(c, ox, oy, w->w, w->h, p->bg);
}
static void panel_layout(cw_widget *w) {
    panel_t *p = (panel_t *)w;
    if (p->mode == CW_NONE) { for (cw_widget *c = w->first_child; c; c = c->next) if (c->vt && c->vt->layout) c->vt->layout(c); return; }
    bool vert = p->mode == CW_VBOX;
    int inner_w = w->w - 2 * p->pad, inner_h = w->h - 2 * p->pad;
    int main_total = vert ? inner_h : inner_w;
    int fixed = 0, wsum = 0, n = 0;
    for (cw_widget *c = w->first_child; c; c = c->next) {
        if (!(c->flags & CW_VISIBLE)) continue;
        ++n;
        if (c->weight > 0) wsum += c->weight; else fixed += vert ? c->h : c->w;
    }
    if (n == 0) return;
    int left = main_total - fixed - p->gap * (n - 1);
    if (left < 0) left = 0;
    int pos = p->pad, given = 0, seen_w = 0;
    for (cw_widget *c = w->first_child; c; c = c->next) {
        if (!(c->flags & CW_VISIBLE)) continue;
        int size = vert ? c->h : c->w;
        if (c->weight > 0) {
            seen_w += c->weight;
            int upto = wsum ? (int)((int64_t)left * seen_w / wsum) : 0;
            size = upto - given; given = upto;
        }
        int nx, ny, nw, nh;
        if (vert) { nx = p->pad; ny = pos; nw = inner_w; nh = size; }
        else      { nx = pos; ny = p->pad; nw = size; nh = inner_h; }
        cw_widget_set_geometry(c, nx, ny, nw, nh);
        if (c->vt && c->vt->layout) c->vt->layout(c);
        pos += size + p->gap;
    }
}
static const cw_vtable panel_vt = { panel_draw, NULL, panel_layout, NULL };

cw_widget *cw_panel_create(cw_widget *parent, uint32_t bg) {
    panel_t *p = (panel_t *)widget_new(parent, &panel_vt, sizeof *p, 0, 0);
    if (!p) return NULL;
    p->bg = bg; p->mode = CW_NONE;
    return &p->base;
}
void cw_panel_set_layout(cw_widget *panel, cw_layout mode, int padding, int gap) {
    panel_t *p = (panel_t *)panel;
    if (!p || (p->mode == mode && p->pad == padding && p->gap == gap)) return;
    p->mode = mode; p->pad = padding; p->gap = gap;
    if (panel->app) panel->app->need_layout = true;
}
void cw_panel_set_bg(cw_widget *panel, uint32_t bg) {
    panel_t *p = (panel_t *)panel;
    if (!p || p->bg == bg) return;
    p->bg = bg; cw_invalidate(panel);
}

/* ---------------------------------------------------------------- label */
typedef struct { cw_widget base; char *text; int px; uint32_t color; } label_t;
static void label_draw(cw_widget *w, cui_canvas *c, int ox, int oy) {
    label_t *l = (label_t *)w;
    uint32_t col = (w->flags & CW_ENABLED) ? l->color : CW_MUTED;
    cui_text_fit(c, cui_font_default(l->px), ox, oy + (w->h - cui_font_height(cui_font_default(l->px))) / 2, w->w, l->text, col);
}
static void label_destroy(cw_widget *w) { free(((label_t *)w)->text); }
static const cw_vtable label_vt = { label_draw, NULL, NULL, label_destroy };
cw_widget *cw_label_create(cw_widget *parent, const char *text, int font_px, uint32_t color) {
    cui_font *f = cui_font_default(font_px);
    label_t *l = (label_t *)widget_new(parent, &label_vt, sizeof *l, cui_text_width(f, text ? text : ""), cui_font_height(f) + 4);
    if (!l) return NULL;
    l->text = strdup(text ? text : ""); l->px = font_px; l->color = color;
    return &l->base;
}
void cw_label_set_text(cw_widget *label, const char *text) {
    label_t *l = (label_t *)label;
    if (!l || !text || strcmp(l->text, text) == 0) return;
    char *n = strdup(text);
    if (!n) return;
    free(l->text); l->text = n;
    cw_invalidate(label);
}

/* ---------------------------------------------------------------- button */
typedef struct { cw_widget base; char *text; cw_click_fn on_click; void *user; } button_t;
static void button_draw(cw_widget *w, cui_canvas *c, int ox, int oy) {
    button_t *b = (button_t *)w;
    bool on = w->flags & CW_ENABLED;
    uint32_t face = !on ? CW_FACE : (w->flags & CW_PRESSED) ? CW_ACCENT_LO : (w->flags & CW_HOVER) ? CW_ACCENT_HI : CW_ACCENT;
    cui_round_rect(c, ox, oy, w->w, w->h, 8, face);
    if (w->flags & CW_FOCUS) cui_round_rect_outline(c, ox, oy, w->w, w->h, 8, 2.0f, CW_ACCENT_LO);
    cui_text_center(c, cui_font_bold(14), ox, oy, w->w, w->h, b->text, on ? 0xFFFFFF : CW_MUTED);
}
static bool button_event(cw_widget *w, const cos_win_event_t *ev, int lx, int ly) {
    button_t *b = (button_t *)w;
    if (!(w->flags & CW_ENABLED)) return false;
    bool inside = lx >= 0 && ly >= 0 && lx < w->w && ly < w->h;
    switch (ev->type) {
    case COS_EV_MOUSE_DOWN:
        if (ev->button == 1) { w->flags |= CW_PRESSED; cw_invalidate(w); return true; }
        break;
    case COS_EV_MOUSE_MOVE:
        return false;
    case COS_EV_MOUSE_UP:
        if (ev->button == 1 && (w->flags & CW_PRESSED)) {
            w->flags &= ~CW_PRESSED; cw_invalidate(w);
            if (inside && b->on_click) b->on_click(w, b->user);
            return true;
        }
        break;
    case COS_EV_KEY:
        if ((w->flags & CW_FOCUS) && (ev->special == COS_KEY_ENTER || ev->ascii == ' ')) { if (b->on_click) b->on_click(w, b->user); return true; }
        break;
    }
    return false;
}
static void button_destroy(cw_widget *w) { free(((button_t *)w)->text); }
static const cw_vtable button_vt = { button_draw, button_event, NULL, button_destroy };
cw_widget *cw_button_create(cw_widget *parent, const char *text, cw_click_fn on_click, void *user) {
    cui_font *f = cui_font_bold(14);
    button_t *b = (button_t *)widget_new(parent, &button_vt, sizeof *b, cui_text_width(f, text) + 32, 34);
    if (!b) return NULL;
    b->text = strdup(text ? text : ""); b->on_click = on_click; b->user = user;
    b->base.flags |= CW_FOCUSABLE;
    return &b->base;
}
void cw_button_set_text(cw_widget *button, const char *text) {
    button_t *b = (button_t *)button;
    if (!b || !text || strcmp(b->text, text) == 0) return;
    char *n = strdup(text);
    if (!n) return;
    free(b->text); b->text = n; cw_invalidate(button);
}

/* ---------------------------------------------------------------- checkbox */
typedef struct { cw_widget base; char *text; bool on; cw_value_fn cb; void *user; } check_t;
static void check_draw(cw_widget *w, cui_canvas *c, int ox, int oy) {
    check_t *k = (check_t *)w;
    int bs = 18, by = oy + (w->h - bs) / 2;
    uint32_t edge = (w->flags & CW_HOVER) ? CW_ACCENT : CW_BORDER;
    cui_round_rect(c, ox, by, bs, bs, 4, k->on ? CW_ACCENT : 0xFFFFFF);
    cui_round_rect_outline(c, ox, by, bs, bs, 4, (w->flags & CW_FOCUS) ? 2.0f : 1.0f, k->on ? CW_ACCENT_LO : edge);
    if (k->on) {
        cui_line(c, (float)ox + 4, (float)by + 9, (float)ox + 8, (float)by + 13, 2.0f, 0xFFFFFF);
        cui_line(c, (float)ox + 8, (float)by + 13, (float)ox + 14, (float)by + 5, 2.0f, 0xFFFFFF);
    }
    cui_font *f = cui_font_default(14);
    cui_text_fit(c, f, ox + bs + 8, oy + (w->h - cui_font_height(f)) / 2, w->w - bs - 8, k->text, (w->flags & CW_ENABLED) ? CW_TEXT : CW_MUTED);
}
static void check_toggle(cw_widget *w) {
    check_t *k = (check_t *)w;
    k->on = !k->on; cw_invalidate(w);
    if (k->cb) k->cb(w, k->on, k->user);
}
static bool check_event(cw_widget *w, const cos_win_event_t *ev, int lx, int ly) {
    if (!(w->flags & CW_ENABLED)) return false;
    bool inside = lx >= 0 && ly >= 0 && lx < w->w && ly < w->h;
    if (ev->type == COS_EV_MOUSE_DOWN && ev->button == 1) { w->flags |= CW_PRESSED; return true; }
    if (ev->type == COS_EV_MOUSE_UP && ev->button == 1 && (w->flags & CW_PRESSED)) {
        w->flags &= ~CW_PRESSED;
        if (inside) check_toggle(w);
        return true;
    }
    if (ev->type == COS_EV_KEY && (w->flags & CW_FOCUS) && ev->ascii == ' ') { check_toggle(w); return true; }
    return false;
}
static void check_destroy(cw_widget *w) { free(((check_t *)w)->text); }
static const cw_vtable check_vt = { check_draw, check_event, NULL, check_destroy };
cw_widget *cw_checkbox_create(cw_widget *parent, const char *text, bool checked, cw_value_fn on_change, void *user) {
    cui_font *f = cui_font_default(14);
    check_t *k = (check_t *)widget_new(parent, &check_vt, sizeof *k, 26 + cui_text_width(f, text), 26);
    if (!k) return NULL;
    k->text = strdup(text ? text : ""); k->on = checked; k->cb = on_change; k->user = user;
    k->base.flags |= CW_FOCUSABLE;
    return &k->base;
}
bool cw_checkbox_get(const cw_widget *cb) { return cb && ((const check_t *)cb)->on; }
void cw_checkbox_set(cw_widget *cb, bool v) { check_t *k = (check_t *)cb; if (k && k->on != v) { k->on = v; cw_invalidate(cb); } }

/* ---------------------------------------------------------------- text box */
typedef struct {
    cw_widget base; char *buf; int len, cap, caret, scroll;
    cw_text_fn on_change, on_enter; void *user;
} tbox_t;
#define TB_PAD 8
static int tb_prefix_w(const tbox_t *t, int n) {
    char tmp[256];
    if (n > 255) n = 255;
    memcpy(tmp, t->buf, (size_t)n); tmp[n] = 0;
    return cui_text_width(cui_font_default(14), tmp);
}
static void tb_scroll_to_caret(tbox_t *t) {
    int vis = t->base.w - 2 * TB_PAD - 2;
    int cx = tb_prefix_w(t, t->caret);
    if (cx - t->scroll > vis) t->scroll = cx - vis;
    if (cx - t->scroll < 0) t->scroll = cx;
    if (t->scroll < 0) t->scroll = 0;
}
static void tb_draw(cw_widget *w, cui_canvas *c, int ox, int oy) {
    tbox_t *t = (tbox_t *)w;
    bool focus = w->flags & CW_FOCUS;
    cui_round_rect(c, ox, oy, w->w, w->h, 6, (w->flags & CW_ENABLED) ? 0xFFFFFF : CW_FACE);
    cui_round_rect_outline(c, ox, oy, w->w, w->h, 6, focus ? 2.0f : 1.0f, focus ? CW_ACCENT : ((w->flags & CW_HOVER) ? CW_ACCENT_HI : CW_BORDER));
    cui_font *f = cui_font_default(14);
    cui_canvas saved = *c;
    cui_clip(c, ox + TB_PAD - 1, oy + 2, w->w - 2 * TB_PAD + 2, w->h - 4);
    int ty = oy + (w->h - cui_font_height(f)) / 2;
    cui_text(c, f, ox + TB_PAD - t->scroll, ty, t->buf, CW_TEXT);
    if (focus) {
        int cx = ox + TB_PAD - t->scroll + tb_prefix_w(t, t->caret);
        cui_fill(c, cx, ty, 1, cui_font_height(f), CW_ACCENT_LO);
    }
    *c = saved;
}
static void tb_changed(tbox_t *t) {
    tb_scroll_to_caret(t);
    cw_invalidate(&t->base);
    if (t->on_change) t->on_change(&t->base, t->buf, t->user);
}
static bool tb_insert(tbox_t *t, char ch) {
    if (t->len + 1 >= t->cap) return false;
    memmove(t->buf + t->caret + 1, t->buf + t->caret, (size_t)(t->len - t->caret + 1));
    t->buf[t->caret++] = ch; ++t->len;
    return true;
}
static bool tb_event(cw_widget *w, const cos_win_event_t *ev, int lx, int ly) {
    tbox_t *t = (tbox_t *)w;
    (void)ly;
    if (!(w->flags & CW_ENABLED)) return false;
    if (ev->type == COS_EV_MOUSE_DOWN && ev->button == 1) {
        int target = lx - TB_PAD + t->scroll, best = 0;
        for (int i = 0; i <= t->len; ++i) { if (tb_prefix_w(t, i) <= target + 3) best = i; else break; }
        if (best != t->caret) { t->caret = best; cw_invalidate(w); }
        return true;
    }
    if (ev->type != COS_EV_KEY || !(w->flags & CW_FOCUS)) return false;
    switch (ev->special) {
    case COS_KEY_BACKSPACE: if (t->caret > 0) { memmove(t->buf + t->caret - 1, t->buf + t->caret, (size_t)(t->len - t->caret + 1)); --t->caret; --t->len; tb_changed(t); } return true;
    case COS_KEY_DELETE:    if (t->caret < t->len) { memmove(t->buf + t->caret, t->buf + t->caret + 1, (size_t)(t->len - t->caret)); --t->len; tb_changed(t); } return true;
    case COS_KEY_LEFT:      if (t->caret > 0) { --t->caret; tb_scroll_to_caret(t); cw_invalidate(w); } return true;
    case COS_KEY_RIGHT:     if (t->caret < t->len) { ++t->caret; tb_scroll_to_caret(t); cw_invalidate(w); } return true;
    case COS_KEY_HOME:      if (t->caret) { t->caret = 0; tb_scroll_to_caret(t); cw_invalidate(w); } return true;
    case COS_KEY_END:       if (t->caret != t->len) { t->caret = t->len; tb_scroll_to_caret(t); cw_invalidate(w); } return true;
    case COS_KEY_ENTER:     if (t->on_enter) t->on_enter(w, t->buf, t->user); return true;
    default: break;
    }
    if (ev->ascii >= 32 && ev->ascii < 127 && !((ev->mods & (COS_MOD_CTRL | COS_MOD_ALT)))) {
        if (tb_insert(t, ev->ascii)) tb_changed(t);
        return true;
    }
    return false;
}
static void tb_destroy(cw_widget *w) { free(((tbox_t *)w)->buf); }
static const cw_vtable tb_vt = { tb_draw, tb_event, NULL, tb_destroy };
cw_widget *cw_textbox_create(cw_widget *parent, const char *text, int max_len, cw_text_fn on_change, cw_text_fn on_enter, void *user) {
    tbox_t *t = (tbox_t *)widget_new(parent, &tb_vt, sizeof *t, 200, 32);
    if (!t) return NULL;
    t->cap = (max_len > 0 && max_len < 1024 ? max_len : 255) + 1;
    t->buf = (char *)calloc(1, (size_t)t->cap);
    if (!t->buf) { free(t); return NULL; }
    if (text) { strncpy(t->buf, text, (size_t)t->cap - 1); t->len = (int)strlen(t->buf); t->caret = t->len; }
    t->on_change = on_change; t->on_enter = on_enter; t->user = user;
    t->base.flags |= CW_FOCUSABLE;
    return &t->base;
}
const char *cw_textbox_get(const cw_widget *tb) { return tb ? ((const tbox_t *)tb)->buf : ""; }
void cw_textbox_set(cw_widget *tb, const char *text) {
    tbox_t *t = (tbox_t *)tb;
    if (!t || !text || strncmp(t->buf, text, (size_t)t->cap - 1) == 0) return;
    strncpy(t->buf, text, (size_t)t->cap - 1); t->buf[t->cap - 1] = 0;
    t->len = (int)strlen(t->buf); t->caret = t->len; t->scroll = 0;
    tb_scroll_to_caret(t); cw_invalidate(tb);
}

/* ---------------------------------------------------------------- list */
typedef struct {
    cw_widget base; char **items; int n, cap, sel, hot, scroll;   /* scroll in rows */
    cw_value_fn on_select; void *user;
} list_t;
#define LIST_ROW 26
static int list_visible_rows(const list_t *l) { int v = (l->base.h - 2) / LIST_ROW; return v < 1 ? 1 : v; }
static cw_rect list_row_rect(const list_t *l, int idx) {
    cw_rect a = cw_widget_abs_rect(&l->base);
    return (cw_rect){ a.x + 1, a.y + 1 + (idx - l->scroll) * LIST_ROW, a.w - 2, LIST_ROW };
}
static void list_draw(cw_widget *w, cui_canvas *c, int ox, int oy) {
    list_t *l = (list_t *)w;
    cui_round_rect(c, ox, oy, w->w, w->h, 6, 0xFFFFFF);
    cui_round_rect_outline(c, ox, oy, w->w, w->h, 6, (w->flags & CW_FOCUS) ? 2.0f : 1.0f, (w->flags & CW_FOCUS) ? CW_ACCENT : CW_BORDER);
    cui_canvas saved = *c;
    cui_clip(c, ox + 1, oy + 1, w->w - 2, w->h - 2);
    cui_font *f = cui_font_default(14);
    int vis = list_visible_rows(l);
    for (int i = l->scroll; i < l->n && i < l->scroll + vis + 1; ++i) {
        int ry = oy + 1 + (i - l->scroll) * LIST_ROW;
        bool sel = i == l->sel;
        if (sel) cui_fill(c, ox + 1, ry, w->w - 2, LIST_ROW, CW_ACCENT);
        else if (i == l->hot) cui_fill(c, ox + 1, ry, w->w - 2, LIST_ROW, 0xEAF0FD);
        cui_text_fit(c, f, ox + 10, ry + (LIST_ROW - cui_font_height(f)) / 2, w->w - 20, l->items[i], sel ? 0xFFFFFF : CW_TEXT);
    }
    *c = saved;
}
static void list_set_hot(list_t *l, int idx) {
    if (idx == l->hot) return;
    int old = l->hot; l->hot = idx;
    if (old >= 0 && old >= l->scroll && old < l->scroll + list_visible_rows(l) + 1) { cw_rect r = list_row_rect(l, old); cw_invalidate_rect(l->base.app, r.x, r.y, r.w, r.h); }
    if (idx >= 0) { cw_rect r = list_row_rect(l, idx); cw_invalidate_rect(l->base.app, r.x, r.y, r.w, r.h); }
}
static void list_select_idx(list_t *l, int idx, bool notify) {
    if (idx < -1 || idx >= l->n || idx == l->sel) return;
    int old = l->sel; l->sel = idx;
    int vis = list_visible_rows(l), before = l->scroll;
    if (idx >= 0) { if (idx < l->scroll) l->scroll = idx; if (idx >= l->scroll + vis) l->scroll = idx - vis + 1; }
    if (l->scroll != before) cw_invalidate(&l->base);
    else {
        if (old >= 0) { cw_rect r = list_row_rect(l, old); cw_invalidate_rect(l->base.app, r.x, r.y, r.w, r.h); }
        if (idx >= 0) { cw_rect r = list_row_rect(l, idx); cw_invalidate_rect(l->base.app, r.x, r.y, r.w, r.h); }
    }
    if (notify && l->on_select) l->on_select(&l->base, idx, l->user);
}
static bool list_event(cw_widget *w, const cos_win_event_t *ev, int lx, int ly) {
    list_t *l = (list_t *)w;
    if (!(w->flags & CW_ENABLED)) return false;
    int row = (ly - 1) / LIST_ROW + l->scroll;
    bool in = lx >= 0 && ly >= 1 && lx < w->w && ly < w->h - 1;
    if (row >= l->n) row = -1;
    switch (ev->type) {
    case COS_EV_MOUSE_MOVE: list_set_hot(l, in ? row : -1); return false;
    case COS_EV_MOUSE_DOWN: if (ev->button == 1 && in && row >= 0) { list_select_idx(l, row, true); return true; } return false;
    case COS_EV_WHEEL: {
        int max = l->n - list_visible_rows(l); if (max < 0) max = 0;
        int ns = l->scroll - (ev->wheel > 0 ? 1 : -1) * 3;
        if (ns < 0) ns = 0;
        if (ns > max) ns = max;
        if (ns != l->scroll) { l->scroll = ns; cw_invalidate(w); }
        return true;
    }
    case COS_EV_KEY:
        if (!(w->flags & CW_FOCUS)) return false;
        if (ev->special == COS_KEY_DOWN && l->sel + 1 < l->n) { list_select_idx(l, l->sel + 1, true); return true; }
        if (ev->special == COS_KEY_UP && l->sel > 0) { list_select_idx(l, l->sel - 1, true); return true; }
        return false;
    }
    return false;
}
static void list_destroy(cw_widget *w) {
    list_t *l = (list_t *)w;
    for (int i = 0; i < l->n; ++i) free(l->items[i]);
    free(l->items);
}
static const cw_vtable list_vt = { list_draw, list_event, NULL, list_destroy };
cw_widget *cw_list_create(cw_widget *parent, cw_value_fn on_select, void *user) {
    list_t *l = (list_t *)widget_new(parent, &list_vt, sizeof *l, 200, 150);
    if (!l) return NULL;
    l->sel = -1; l->hot = -1; l->on_select = on_select; l->user = user;
    l->base.flags |= CW_FOCUSABLE;
    return &l->base;
}
void cw_list_add(cw_widget *list, const char *item) {
    list_t *l = (list_t *)list;
    if (!l || !item) return;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        char **ni = (char **)realloc(l->items, (size_t)nc * sizeof(char *));
        if (!ni) return;
        l->items = ni; l->cap = nc;
    }
    l->items[l->n] = strdup(item);
    if (!l->items[l->n]) return;
    int idx = l->n++;
    if (idx < l->scroll + list_visible_rows(l) + 1) { cw_rect r = list_row_rect(l, idx); cw_invalidate_rect(list->app, r.x, r.y, r.w, r.h); }
}
void cw_list_clear(cw_widget *list) {
    list_t *l = (list_t *)list;
    if (!l || l->n == 0) return;
    for (int i = 0; i < l->n; ++i) free(l->items[i]);
    l->n = 0; l->sel = -1; l->hot = -1; l->scroll = 0;
    cw_invalidate(list);
}
int cw_list_selected(const cw_widget *list) { return list ? ((const list_t *)list)->sel : -1; }
void cw_list_select(cw_widget *list, int index) { if (list) list_select_idx((list_t *)list, index, false); }

/* ---------------------------------------------------------------- slider */
typedef struct { cw_widget base; int min, max, val; cw_value_fn cb; void *user; } slider_t;
static int slider_knob_x(const slider_t *s) {
    int span = s->base.w - 20;
    return 10 + (s->max > s->min ? (int)((int64_t)span * (s->val - s->min) / (s->max - s->min)) : 0);
}
static void slider_draw(cw_widget *w, cui_canvas *c, int ox, int oy) {
    slider_t *s = (slider_t *)w;
    int ty = oy + w->h / 2, kx = ox + slider_knob_x(s);
    cui_round_rect(c, ox + 10, ty - 3, w->w - 20, 6, 3, CW_FACE_HI);
    cui_round_rect(c, ox + 10, ty - 3, kx - (ox + 10), 6, 3, CW_ACCENT);
    cui_circle(c, (float)kx, (float)ty, 9.0f, 0xFFFFFF);
    cui_ring(c, (float)kx, (float)ty, 9.0f, (w->flags & (CW_HOVER | CW_PRESSED | CW_FOCUS)) ? 2.0f : 1.2f, CW_ACCENT);
}
static void slider_set_from_x(slider_t *s, int lx) {
    int span = s->base.w - 20;
    int v = s->min + (span > 0 ? (int)(((int64_t)(lx - 10) * (s->max - s->min) + span / 2) / span) : 0);
    if (v < s->min) v = s->min;
    if (v > s->max) v = s->max;
    if (v != s->val) {
        s->val = v; cw_invalidate(&s->base);
        if (s->cb) s->cb(&s->base, v, s->user);
    }
}
static bool slider_event(cw_widget *w, const cos_win_event_t *ev, int lx, int ly) {
    slider_t *s = (slider_t *)w;
    (void)ly;
    if (!(w->flags & CW_ENABLED)) return false;
    if (ev->type == COS_EV_MOUSE_DOWN && ev->button == 1) { w->flags |= CW_PRESSED; slider_set_from_x(s, lx); return true; }
    if (ev->type == COS_EV_MOUSE_MOVE && (w->flags & CW_PRESSED)) { slider_set_from_x(s, lx); return true; }
    if (ev->type == COS_EV_MOUSE_UP && (w->flags & CW_PRESSED)) { w->flags &= ~CW_PRESSED; cw_invalidate(w); return true; }
    if (ev->type == COS_EV_KEY && (w->flags & CW_FOCUS)) {
        int step = (s->max - s->min) / 20; if (step < 1) step = 1;
        if (ev->special == COS_KEY_LEFT)  { cw_slider_set(w, s->val - step); if (s->cb) s->cb(w, s->val, s->user); return true; }
        if (ev->special == COS_KEY_RIGHT) { cw_slider_set(w, s->val + step); if (s->cb) s->cb(w, s->val, s->user); return true; }
    }
    return false;
}
static const cw_vtable slider_vt = { slider_draw, slider_event, NULL, NULL };
cw_widget *cw_slider_create(cw_widget *parent, int min, int max, int value, cw_value_fn on_change, void *user) {
    slider_t *s = (slider_t *)widget_new(parent, &slider_vt, sizeof *s, 200, 28);
    if (!s) return NULL;
    s->min = min; s->max = max > min ? max : min + 1; s->val = value; s->cb = on_change; s->user = user;
    s->base.flags |= CW_FOCUSABLE;
    return &s->base;
}
int cw_slider_get(const cw_widget *s) { return s ? ((const slider_t *)s)->val : 0; }
void cw_slider_set(cw_widget *w, int v) {
    slider_t *s = (slider_t *)w;
    if (!s) return;
    if (v < s->min) v = s->min;
    if (v > s->max) v = s->max;
    if (v != s->val) { s->val = v; cw_invalidate(w); }
}

/* ---------------------------------------------------------------- app: paint */
static void paint_tree(cw_widget *w, cui_canvas *c, int ox, int oy, cw_rect clip) {
    if (!(w->flags & CW_VISIBLE)) return;
    cw_rect r = { ox, oy, w->w, w->h };
    if (!rect_isect(r, clip)) return;
    if (w->vt && w->vt->draw) w->vt->draw(w, c, ox, oy);
    for (cw_widget *ch = w->first_child; ch; ch = ch->next) paint_tree(ch, c, ox + ch->x, oy + ch->y, clip);
}

static void app_paint_and_present(cw_app *app) {
    if (app->need_layout) cw_app_layout(app);
    if (app->first_frame) {
        app->first_frame = false;
        app->ndamage = 0;
        cui_unclip(&app->canvas);
        paint_tree(app->root, &app->canvas, 0, 0, (cw_rect){ 0, 0, app->canvas.w, app->canvas.h });
        cos_win2_present(app->win);
        ++app->st_frames; ++app->st_rects; app->st_pixels += (uint64_t)app->canvas.w * (uint64_t)app->canvas.h;
        return;
    }
    if (app->ndamage == 0) return;
    cos_win_rect_t out[MAX_DAMAGE];
    int n = app->ndamage;
    for (int i = 0; i < n; ++i) {
        cw_rect d = app->damage[i];
        cui_unclip(&app->canvas);
        cui_clip(&app->canvas, d.x, d.y, d.w, d.h);
        paint_tree(app->root, &app->canvas, 0, 0, d);
        out[i] = (cos_win_rect_t){ d.x, d.y, d.w, d.h };
        app->st_pixels += (uint64_t)d.w * (uint64_t)d.h;
    }
    cui_unclip(&app->canvas);
    app->ndamage = 0;
    cos_win2_present_rects(app->win, out, (uint32_t)n);
    ++app->st_frames; app->st_rects += (uint32_t)n;
}

/* ---------------------------------------------------------------- app: events */
static cw_widget *hit_test(cw_widget *w, int px, int py) {       /* px,py in w's parent space */
    if (!(w->flags & CW_VISIBLE)) return NULL;
    int lx = px - w->x, ly = py - w->y;
    if (lx < 0 || ly < 0 || lx >= w->w || ly >= w->h) return NULL;
    cw_widget *best = NULL;                                 /* later siblings paint on top, so the last hit wins */
    for (cw_widget *c = w->first_child; c; c = c->next) { cw_widget *h = hit_test(c, lx, ly); if (h) best = h; }
    return best ? best : w;
}
static cw_widget *interactive_ancestor(cw_widget *w) {
    for (; w; w = w->parent) if (w->vt && w->vt->event && (w->flags & CW_ENABLED)) return w;
    return NULL;
}
static void abs_origin(const cw_widget *w, int *x, int *y) { cw_rect r = cw_widget_abs_rect(w); *x = r.x; *y = r.y; }

static void set_hover(cw_app *app, cw_widget *n) {
    if (app->hover == n) return;
    cw_widget *o = app->hover;
    app->hover = n;
    if (o) {
        o->flags &= ~CW_HOVER; cw_invalidate(o);
        if (o->vt && o->vt->event) {                 /* tell it the pointer left (an off-widget move) so row highlights etc. clear */
            cos_win_event_t lv; memset(&lv, 0, sizeof lv);
            lv.type = COS_EV_MOUSE_MOVE; lv.x = lv.y = -100000;
            o->vt->event(o, &lv, -100000, -100000);
        }
    }
    if (n) { n->flags |= CW_HOVER; cw_invalidate(n); }
}
void cw_app_focus(cw_app *app, cw_widget *w) {
    if (!app || app->focus == w) return;
    cw_widget *o = app->focus;
    app->focus = w;
    if (o) { o->flags &= ~CW_FOCUS; cw_invalidate(o); }
    if (w) { w->flags |= CW_FOCUS; cw_invalidate(w); }
}
static void focus_next(cw_app *app, bool backwards) {
    /* depth-first order of focusable, visible, enabled widgets */
    cw_widget *order[64]; int n = 0;
    cw_widget *stack[64]; int sp = 0;
    stack[sp++] = app->root;
    while (sp > 0 && n < 64) {
        cw_widget *w = stack[--sp];
        if (!(w->flags & CW_VISIBLE)) continue;
        if ((w->flags & CW_FOCUSABLE) && (w->flags & CW_ENABLED)) order[n++] = w;
        cw_widget *kids[32]; int kn = 0;
        for (cw_widget *c = w->first_child; c && kn < 32; c = c->next) kids[kn++] = c;
        for (int i = kn - 1; i >= 0 && sp < 64; --i) stack[sp++] = kids[i];
    }
    if (n == 0) return;
    int cur = -1;
    for (int i = 0; i < n; ++i) if (order[i] == app->focus) { cur = i; break; }
    int nx = cur < 0 ? (backwards ? n - 1 : 0) : (cur + (backwards ? n - 1 : 1)) % n;
    cw_app_focus(app, order[nx]);
}

static void deliver(cw_app *app, cw_widget *target, const cos_win_event_t *ev) {
    for (cw_widget *w = target; w; w = w->parent) {
        if (!w->vt || !w->vt->event || !(w->flags & CW_ENABLED)) continue;
        int ox, oy; abs_origin(w, &ox, &oy);
        if (w->vt->event(w, ev, ev->x - ox, ev->y - oy)) return;
    }
    (void)app;
}

#ifdef CW_DEBUG
#include <stdio.h>
#endif
static void handle_event(cw_app *app, const cos_win_event_t *ev) {
#ifdef CW_DEBUG
    if (ev->type != COS_EV_MOUSE_MOVE) { char b[96]; snprintf(b, sizeof b, "[CW] ev type=%u x=%d y=%d btn=%u special=%u\n", ev->type, ev->x, ev->y, ev->button, ev->special); cos_puts(b); }
#endif
    switch (ev->type) {
    case COS_EV_CLOSE: app->quit = true; break;
    case COS_EV_MOUSE_MOVE: {
        cw_widget *t = app->capture ? app->capture : interactive_ancestor(hit_test(app->root, ev->x, ev->y));
        if (!app->capture) set_hover(app, t);
        if (t) deliver(app, t, ev);
        break;
    }
    case COS_EV_MOUSE_DOWN: {
        cw_widget *t = interactive_ancestor(hit_test(app->root, ev->x, ev->y));
        set_hover(app, t);
        if (t) {
            app->capture = t;
            if (t->flags & CW_FOCUSABLE) cw_app_focus(app, t); else cw_app_focus(app, NULL);
            deliver(app, t, ev);
        } else cw_app_focus(app, NULL);
        break;
    }
    case COS_EV_MOUSE_UP: {
        cw_widget *t = app->capture;
        if (t) { deliver(app, t, ev); app->capture = NULL; }
        set_hover(app, interactive_ancestor(hit_test(app->root, ev->x, ev->y)));
        break;
    }
    case COS_EV_WHEEL: {
        cw_widget *t = interactive_ancestor(hit_test(app->root, ev->x, ev->y));
        if (t) deliver(app, t, ev);
        break;
    }
    case COS_EV_KEY: {
        cw_widget *t = app->focus;
        bool used = false;
        if (t) {
            for (cw_widget *w = t; w && !used; w = w->parent) {
                if (!w->vt || !w->vt->event || !(w->flags & CW_ENABLED)) continue;
                int ox, oy; abs_origin(w, &ox, &oy);
                used = w->vt->event(w, ev, 0, 0);
            }
        }
        if (!used && ev->special == COS_KEY_TAB) focus_next(app, (ev->mods & COS_MOD_SHIFT) != 0);   /* shift = backwards */
        break;
    }
    default: break;
    }
}

/* ---------------------------------------------------------------- app: lifecycle */
cw_app *cw_app_create(const char *title, int w, int h) {
    cw_app *app = (cw_app *)calloc(1, sizeof *app);
    if (!app) return NULL;
    cos_win_info_t wi;
    app->win = cos_win2_create(title, w, h, &wi);
    if (app->win <= 0) { free(app); return NULL; }
    cui_canvas_init(&app->canvas, wi.pixels, wi.width, wi.height, wi.stride);
    app->root = widget_new(NULL, &panel_vt, sizeof(panel_t), wi.width, wi.height);
    ((panel_t *)app->root)->bg = CW_BG;
    app->root->app = app;
    app->first_frame = true;
    app->need_layout = true;
    return app;
}
cw_widget *cw_app_root(cw_app *app) { return app ? app->root : NULL; }
void cw_app_quit(cw_app *app) { if (app) app->quit = true; }
void cw_app_layout(cw_app *app) {
    if (!app) return;
    app->need_layout = false;
    app->root->x = app->root->y = 0;
    app->root->w = app->canvas.w; app->root->h = app->canvas.h;
    if (app->root->vt->layout) app->root->vt->layout(app->root);
}
void cw_app_set_timer(cw_app *app, uint32_t interval_ms, cw_timer_fn fn, void *user) {
    if (!app) return;
    app->timer_fn = interval_ms ? fn : NULL; app->timer_user = user; app->timer_interval = interval_ms;
    app->timer_next = cos_time_ms() + interval_ms;
}
void cw_app_stats(const cw_app *app, uint32_t *frames, uint32_t *rects, uint64_t *pixels) {
    if (!app) return;
    if (frames) *frames = app->st_frames;
    if (rects) *rects = app->st_rects;
    if (pixels) *pixels = app->st_pixels;
}

int cw_app_run(cw_app *app) {
    if (!app) return 1;
    app_paint_and_present(app);
    while (!app->quit) {
        /* An idle app sleeps in the kernel until input arrives (or its timer is due). */
        uint32_t timeout = 0;                                  /* 0 = wait for an event with no timeout */
        if (app->timer_fn) {
            uint64_t now = cos_time_ms();
            timeout = app->timer_next > now ? (uint32_t)(app->timer_next - now) : 1;
        }
        cos_win_event_t ev, pending_move;
        bool have_move = false;
        int r = cos_win2_wait(app->win, &ev, timeout);
        if (r < 0) break;
        /* One batch per wake-up: drain everything queued, coalescing mouse motion into its last position. */
        while (r > 0) {
            if (ev.type == COS_EV_MOUSE_MOVE) { pending_move = ev; have_move = true; }
            else {
                if (have_move) { handle_event(app, &pending_move); have_move = false; }
                handle_event(app, &ev);
            }
            if (app->quit) break;
            r = cos_win2_poll(app->win, &ev);
        }
        if (have_move && !app->quit) handle_event(app, &pending_move);
        if (app->timer_fn && cos_time_ms() >= app->timer_next) {
            app->timer_next = cos_time_ms() + app->timer_interval;
            app->timer_fn(app, app->timer_user);
        }
        if (!app->quit) app_paint_and_present(app);
    }
    cos_win2_close(app->win);
    return 0;
}
