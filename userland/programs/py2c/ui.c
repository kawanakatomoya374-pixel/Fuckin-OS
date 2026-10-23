/* ui.c - Py2C Studio's chrome: menus, toolbar, tabs, sidebar, find bar, panels, status bar, dialogs, and all mouse / keyboard routing.
 * Drawing registers hit rectangles (H); the mouse handlers use the list of the last frame. */
#include "p2.h"
#include "cos_popup.h"

typedef struct { int x, y, w, h, id, arg; } Hit;
enum { H_NONE, H_MENU, H_MITEM, H_TOOL, H_TAB, H_TABX, H_RAIL, H_SIDE, H_SIDE_EX, H_SIDE_UP, H_PTAB, H_PBTN, H_PROB, H_FIND_NEXT, H_FIND_PREV, H_FIND_CASE,
       H_FIND_CLOSE, H_FIND_REPL, H_FIND_ALL, H_FIELD_FIND, H_FIELD_REPL, H_DLG_OK, H_DLG_CANCEL, H_DLG_SKIP, H_DLG_UP, H_DLG_ENTRY, H_DLG_F1, H_DLG_F2,
       H_DLG_OPT, H_WELCOME, H_CPANE_BTN, H_PLUG, H_MENUBAR_BG };
static Hit hits[640]; static int nhits, modal_from;
static void H(int x, int y, int w, int h, int id, int arg) { if (nhits < 640) hits[nhits++] = (Hit){ x, y, w, h, id, arg }; }
static bool in(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }
static Hit *hit_at(int x, int y, int from) { for (int i = nhits - 1; i >= from; --i) if (in(x, y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) return &hits[i]; return NULL; }
static bool hov(int x, int y, int w, int h) { return in(A.mx, A.my, x, y, w, h); }

/* ---------------------------------------------------------------- themes */
const Theme THEME_DARK = {
    .bg = 0x1B1E26, .panel = 0x171B22, .side = 0x161920, .bar = 0x20242E, .border = 0x2A2F3B, .text = 0xD7DAE0, .dim = 0x9BA8BB, .faint = 0x6B7488,
    .accent = 0x5B8CFF, .on_accent = 0xFFFFFF, .sel = 0x34518A, .curline = 0x222733, .gutter = 0x181B22, .field = 0x12151B, .btn = 0x2A3040, .btn_hi = 0x38405A,
    .tab_on = 0x1B1E26, .err = 0xF0616D, .warn = 0xE2B96F, .ok = 0x77D6A3,
    .kw = 0xC586C0, .str = 0xCE9178, .num = 0xB5CEA8, .cmt = 0x6A9955, .fn = 0xDCDCAA, .deco = 0x4EC9B0, .type = 0x4EC9B0, .builtin = 0x9CDCFE };
const Theme THEME_LIGHT = {
    .bg = 0xFFFFFF, .panel = 0xF3F4F6, .side = 0xEEF0F4, .bar = 0xE6E9EF, .border = 0xCBD0DA, .text = 0x24292F, .dim = 0x57606A, .faint = 0x8C959F,
    .accent = 0x2F6FEB, .on_accent = 0xFFFFFF, .sel = 0xBBD3FF, .curline = 0xF1F5FF, .gutter = 0xF3F4F6, .field = 0xFFFFFF, .btn = 0xE2E6EE, .btn_hi = 0xD0D7E4,
    .tab_on = 0xFFFFFF, .err = 0xCF222E, .warn = 0x9A6700, .ok = 0x1A7F37,
    .kw = 0x8250DF, .str = 0x0A3069, .num = 0x0550AE, .cmt = 0x6E7781, .fn = 0x953800, .deco = 0x116329, .type = 0x116329, .builtin = 0x0550AE };
const Theme *T = &THEME_DARK;
void app_apply_theme(bool dark) { A.opt.dark = dark; T = dark ? &THEME_DARK : &THEME_LIGHT; A.dirty = true; }

/* ---------------------------------------------------------------- layout */
#define MENU_H 26
#define TOOL_H 40
#define TAB_H 30
#define STATUS_H 24
#define RAIL_W 40
#define PANEL_HDR 28
#define CPANE_HDR 26
static struct { int rail_w, side_w, main_x, main_w, tabs_y, body_y, panel_y, panel_h, ed_x, ed_y, ed_w, ed_h, c_x, c_w, find_h, bottom; } L;

static Doc *cdoc_shown(void) { return (A.opt.split_c && A.cdoc && A.cdoc->len) ? A.cdoc : NULL; }
void app_relayout(void) {
    A.W = A.wi.width; A.H = A.wi.height;
    L.rail_w = A.opt.ui_mode ? RAIL_W : 0;
    L.side_w = (A.opt.ui_mode && A.opt.show_sidebar) ? A.side_w : 0;
    if (L.side_w > A.W / 2) L.side_w = A.W / 2;
    L.main_x = L.rail_w + L.side_w; L.main_w = A.W - L.main_x;
    L.tabs_y = MENU_H + TOOL_H; L.body_y = L.tabs_y + TAB_H; L.bottom = A.H - STATUS_H;
    int avail = L.bottom - L.body_y;
    L.panel_h = A.opt.show_panel ? A.panel_h : 0;
    if (L.panel_h > avail - 120) L.panel_h = avail - 120;
    if (L.panel_h && L.panel_h < 80) L.panel_h = 80;
    L.panel_y = L.bottom - L.panel_h;
    L.find_h = A.find_open ? (A.find_replace ? 70 : 36) : 0;
    L.c_w = cdoc_shown() ? A.cpane_w : 0;
    if (L.c_w > L.main_w - 220) L.c_w = L.main_w - 220;
    if (L.c_w && L.c_w < 200) L.c_w = 0;
    L.ed_x = L.main_x; L.ed_w = L.main_w - L.c_w; L.c_x = L.main_x + L.ed_w;
    L.ed_y = L.body_y + L.find_h; L.ed_h = L.panel_y - L.ed_y;
    A.ed = (View){ .d = app_doc(), .x = L.ed_x, .y = L.ed_y, .w = L.ed_w, .h = L.ed_h, .gutter = true, .editable = true, .focus = A.focus == 0 && !A.dlg, .mark_err = true,
                   .click_n = A.ed.click_n, .click_t = A.ed.click_t, .click_x = A.ed.click_x, .click_y = A.ed.click_y, .drag = A.ed.drag };
    if (A.find_open && A.find_f.len) { A.ed.find = A.find_f.text; A.ed.find_len = (size_t)A.find_f.len; A.ed.find_icase = A.find_icase; }
    A.cv_view = (View){ .d = A.cdoc, .x = L.c_x, .y = L.body_y + CPANE_HDR, .w = L.c_w, .h = L.panel_y - L.body_y - CPANE_HDR, .gutter = true, .editable = false, .focus = A.focus == 1 && !A.dlg,
                        .click_n = A.cv_view.click_n, .click_t = A.cv_view.click_t, .click_x = A.cv_view.click_x, .click_y = A.cv_view.click_y, .drag = A.cv_view.drag };
    Doc *pd = A.panel_tab == PANEL_PROBLEMS ? NULL : A.panel_docs[A.panel_tab];
    A.pv = (View){ .d = pd, .x = L.main_x, .y = L.panel_y + PANEL_HDR, .w = L.main_w, .h = L.panel_h - PANEL_HDR, .gutter = false, .editable = A.panel_tab == PANEL_INPUT, .focus = A.focus == 2 && !A.dlg,
                   .click_n = A.pv.click_n, .click_t = A.pv.click_t, .click_x = A.pv.click_x, .click_y = A.pv.click_y, .drag = A.pv.drag };
    if (pd) pd->readonly = A.panel_tab != PANEL_INPUT;
    if (A.cdoc) A.cdoc->readonly = true;
    A.dirty = true;
}
void app_redraw(void) { A.dirty = true; }

static cpop_menu ctxm;                       /* the right-click menu (see "right-click menus" below) */
static const cpop_theme *ctx_theme(void);
static void ctx_run(int cmd);
/* ---------------------------------------------------------------- menus */
typedef struct { const char *label; int cmd; const char *sc; } MI;
static const MI M_FILE[] = { {"New", CMD_NEW, "Ctrl+N"}, {"Open...", CMD_OPEN, "Ctrl+O"}, {"Save", CMD_SAVE, "Ctrl+S"}, {"Save As...", CMD_SAVE_AS, "Ctrl+Shift+S"}, {"Save All", CMD_SAVE_ALL, ""},
                             {"Close Tab", CMD_CLOSE_TAB, "Ctrl+W"}, {NULL, -1, 0}, {"@recent", 0, 0}, {NULL, -1, 0}, {"Exit", CMD_EXIT, ""}, {0} };
static const MI M_EDIT[] = { {"Undo", CMD_UNDO, "Ctrl+Z"}, {"Redo", CMD_REDO, "Ctrl+Y"}, {NULL, -1, 0}, {"Cut", CMD_CUT, "Ctrl+X"}, {"Copy", CMD_COPY, "Ctrl+C"}, {"Paste", CMD_PASTE, "Ctrl+V"},
                             {"Select All", CMD_SELECT_ALL, "Ctrl+A"}, {NULL, -1, 0}, {"Find...", CMD_FIND, "Ctrl+F"}, {"Replace...", CMD_REPLACE, "Ctrl+H"}, {"Find Next", CMD_FIND_NEXT, "F3"},
                             {"Find Previous", CMD_FIND_PREV, "Shift+F3"}, {"Go to Line...", CMD_GOTO_LINE, "Ctrl+G"}, {NULL, -1, 0}, {"Toggle Comment", CMD_COMMENT, "Ctrl+/"},
                             {"Indent", CMD_INDENT, "Tab"}, {"Outdent", CMD_OUTDENT, "Shift+Tab"}, {"Duplicate Line", CMD_DUP_LINE, "Ctrl+D"}, {0} };
static const MI M_VIEW[] = { {"Sidebar", CMD_VIEW_SIDEBAR, "Ctrl+B"}, {"Bottom Panel", CMD_VIEW_PANEL, "Ctrl+J"}, {"Converted Pane", CMD_VIEW_CPANE, "Ctrl+K"}, {NULL, -1, 0},
                             {"Zoom In", CMD_ZOOM_IN, "Ctrl++"}, {"Zoom Out", CMD_ZOOM_OUT, "Ctrl+-"}, {NULL, -1, 0}, {"Dark Theme", CMD_THEME_DARK, ""}, {"Light Theme", CMD_THEME_LIGHT, ""}, {NULL, -1, 0},
                             {"Studio Layout", CMD_UI_STUDIO, ""}, {"Classic Layout", CMD_UI_CLASSIC, ""}, {NULL, -1, 0},
                             {"Output", CMD_PANEL_OUTPUT, ""}, {"Problems", CMD_PANEL_PROBLEMS, ""}, {"Program Input", CMD_PANEL_INPUT, ""}, {"Syntax Tree", CMD_PANEL_AST, ""}, {"Language Reference", CMD_PANEL_LANG, "F1"}, {0} };
static const MI M_RUN[] = { {"Convert", CMD_CONVERT, "F5"}, {"Run", CMD_RUN, "F6"}, {"Stop", CMD_STOP, "Shift+F5"}, {"Build Executable (.c-os)", CMD_BUILD, "F7"}, {NULL, -1, 0},
                            {"Show Syntax Tree", CMD_AST, ""}, {"Save Converted As...", CMD_SAVE_C, ""}, {"Open Converted in C-OS Studio", CMD_OPEN_IN_STUDIO, ""}, {NULL, -1, 0},
                            {"Plugins...", CMD_PLUGINS, ""}, {"Options...", CMD_OPTIONS, "Ctrl+,"}, {0} };
static const MI M_HELP[] = { {"Language Reference", CMD_PANEL_LANG, "F1"}, {"Open Py2C Source in C-OS Studio", CMD_OPEN_SRC, ""}, {"Build Py2C with tcc (C-OS Studio)", CMD_BUILD_SRC, ""}, {"About Py2C Studio", CMD_ABOUT, ""}, {0} };
static const char *const MENU_NAMES[] = { "File", "Edit", "View", "Run", "Examples", "Help" };
#define NMENU 6
static int menu_x[NMENU], menu_w[NMENU];

static MI dyn[48]; static char dyn_lab[48][300];
static const MI *menu_items(int m, int *n) {
    int k = 0;
    if (m == 0) {
        for (int i = 0; M_FILE[i].label || M_FILE[i].cmd; ++i) {
            if (M_FILE[i].label && !strcmp(M_FILE[i].label, "@recent")) {
                if (!A.nrecent) { dyn[k++] = (MI){ "(no recent files)", -2, "" }; }
                for (int r = 0; r < A.nrecent && k < 44; ++r) { int ix = k++; snprintf(dyn_lab[ix], sizeof dyn_lab[ix], "%s", A.recent[r]); dyn[ix] = (MI){ dyn_lab[ix], CMD_RECENT + r, "" }; }
            } else dyn[k++] = M_FILE[i];
        }
        *n = k; return dyn;
    }
    if (m == 4) {
        if (!A.nex) dyn[k++] = (MI){ "(no examples installed)", -2, "" };
        for (int i = 0; i < A.nex && k < 44; ++i) { int ix = k++; snprintf(dyn_lab[ix], sizeof dyn_lab[ix], "%s", A.ex_names[i]); dyn[ix] = (MI){ dyn_lab[ix], CMD_EXAMPLE + i, "" }; }
        *n = k; return dyn;
    }
    const MI *src = m == 1 ? M_EDIT : m == 2 ? M_VIEW : m == 3 ? M_RUN : m == 5 ? M_HELP : M_FILE;
    int c = 0; while (src[c].label || src[c].cmd) ++c;
    *n = c; return src;
}
static bool cmd_enabled(int cmd) {
    Doc *d = app_doc();
    switch (cmd) {
        case -2: return false;
        case CMD_SAVE: return d && (d->dirty || d->untitled);
        case CMD_SAVE_AS: case CMD_CLOSE_TAB: case CMD_FIND: case CMD_REPLACE: case CMD_GOTO_LINE: case CMD_SELECT_ALL: case CMD_PASTE: case CMD_COMMENT:
        case CMD_INDENT: case CMD_OUTDENT: case CMD_DUP_LINE: case CMD_CONVERT: case CMD_AST: case CMD_FIND_NEXT: case CMD_FIND_PREV: return d != NULL;
        case CMD_RUN: case CMD_BUILD: return d && d->lang != L_C;
        case CMD_UNDO: return d && d->head > 0;
        case CMD_REDO: return d && d->head < d->nops;
        case CMD_CUT: case CMD_COPY: return d && (A.focus != 0 || doc_has_sel(d));
        case CMD_STOP: return run_busy();
        case CMD_SAVE_C: case CMD_OPEN_IN_STUDIO: return d != NULL;
        default: return true;
    }
}
static bool cmd_checked(int cmd) {
    switch (cmd) {
        case CMD_VIEW_SIDEBAR: return A.opt.show_sidebar; case CMD_VIEW_PANEL: return A.opt.show_panel; case CMD_VIEW_CPANE: return A.opt.split_c;
        case CMD_THEME_DARK: return A.opt.dark; case CMD_THEME_LIGHT: return !A.opt.dark; case CMD_UI_STUDIO: return A.opt.ui_mode == 1; case CMD_UI_CLASSIC: return A.opt.ui_mode == 0;
        default: return false;
    }
}
static void menu_box(int m, int *bx, int *by, int *bw, int *bh) {
    int n; const MI *mi = menu_items(m, &n);
    int w = 170;
    for (int i = 0; i < n; ++i) if (mi[i].label) { int iw = cui_text_width(g_ui, mi[i].label) + (mi[i].sc && mi[i].sc[0] ? cui_text_width(g_ui_small, mi[i].sc) + 40 : 20) + 36; if (iw > w) w = iw; }
    if (w > 460) w = 460;
    int h = 10; for (int i = 0; i < n; ++i) h += mi[i].label ? 26 : 9;
    *bx = menu_x[m] - 6; *by = MENU_H; *bw = w; *bh = h;
    if (*bx + w > A.W - 4) *bx = A.W - w - 4;
}

/* ---------------------------------------------------------------- small drawing helpers */
static int ty(cui_font *f, int y, int h) { return y + (h - cui_font_height(f)) / 2; }
static void txt(int x, int y, int h, cui_font *f, uint32_t col, const char *s) { cui_text(&A.cv, f, x, ty(f, y, h), s, col); }
static void txtfit(int x, int y, int h, int w, cui_font *f, uint32_t col, const char *s) { cui_text_fit(&A.cv, f, x, ty(f, y, h), w, s, col); }
static bool btn(int x, int y, int w, int h, const char *label, int id, int arg, bool primary, bool enabled) {
    bool h_ = enabled && hov(x, y, w, h);
    cui_round_rect(&A.cv, x, y, w, h, 5, primary ? (h_ ? cui_mix(T->accent, 0xFFFFFF, 40) : T->accent) : (h_ ? T->btn_hi : T->btn));
    cui_text_center(&A.cv, g_ui, x, y, w, h, label, !enabled ? T->faint : primary ? T->on_accent : T->text);
    if (enabled) H(x, y, w, h, id, arg);
    return h_;
}
static void check(int x, int y, const char *label, bool on, int id, int arg) {
    cui_canvas *c = &A.cv;
    cui_round_rect(c, x, y, 16, 16, 4, on ? T->accent : T->field);
    if (!on) cui_round_rect_outline(c, x, y, 16, 16, 4, 1.0f, T->border);
    else { cui_line(c, (float)x + 4, (float)y + 8.5f, (float)x + 7, (float)y + 11.5f, 1.8f, 0xFFFFFF); cui_line(c, (float)x + 7, (float)y + 11.5f, (float)x + 12, (float)y + 4.5f, 1.8f, 0xFFFFFF); }
    cui_text(c, g_ui, x + 24, y + (16 - cui_font_height(g_ui)) / 2, label, T->text);
    H(x, y - 2, cui_text_width(g_ui, label) + 30, 20, id, arg);
}

/* ---------------------------------------------------------------- explorer data */
static int ent_cmp(const void *a, const void *b) {
    const cos_dirent_ex_t *x = (const cos_dirent_ex_t *)a, *y = (const cos_dirent_ex_t *)b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static int dir_load(const char *dir, cos_dirent_ex_t **out, int *cap_io) {
    if (!*out) { *out = (cos_dirent_ex_t *)malloc(sizeof(cos_dirent_ex_t) * 512); *cap_io = 512; }
    if (!*out) return 0;
    int n = cos_listdir_ex(dir, *out, 512); if (n < 0) n = 0;
    int k = 0;
    for (int i = 0; i < n; ++i) { if ((*out)[i].name[0] == '.' ) continue; if (k != i) (*out)[k] = (*out)[i]; k++; }
    qsort(*out, (size_t)k, sizeof(cos_dirent_ex_t), ent_cmp);
    return k;
}
void explorer_refresh(void) { static int cap; A.nents = dir_load(A.dir, &A.ents, &cap); A.dirty = true; }
static void explorer_cd(const char *p) { snprintf(A.dir, sizeof A.dir, "%s", p[0] ? p : "/"); A.side_scroll = 0; explorer_refresh(); }
static void path_parent(char *p) { char *s = strrchr(p, '/'); if (!s || s == p) { p[1] = 0; p[0] = '/'; } else *s = 0; }
static void path_join(char *out, size_t cap, const char *a, const char *b) { if (!strcmp(a, "/")) snprintf(out, cap, "/%s", b); else snprintf(out, cap, "%s/%s", a, b); }

/* ---------------------------------------------------------------- tabs */
static int tab_x[MAX_DOCS], tab_w[MAX_DOCS];
void ui_tab_rects(void) {
    int x = L.main_x ? L.main_x : 0, total = 0;
    int n = A.ndocs; if (!n) return;
    int avail = A.W - x - 8;
    int w[MAX_DOCS];
    for (int i = 0; i < n; ++i) { w[i] = cui_text_width(g_ui, A.docs[i]->name) + 54; total += w[i]; }
    if (total > avail) for (int i = 0; i < n; ++i) w[i] = w[i] * avail / total;
    for (int i = 0; i < n; ++i) { tab_x[i] = x; tab_w[i] = w[i]; x += w[i]; }
}
void tab_switch(int i) {
    if (i < 0 || i >= A.ndocs) return;
    A.cur = i; app_relayout(); A.dirty = true;
}

/* ---------------------------------------------------------------- plugins.inc interface */
int plug_btn_slots(void); bool plug_btn_live(int i); const char *plug_btn_label(int i); void plug_btn_run(int i);
void plug_refresh(void); int plug_avail_count(void); const char *plug_avail_name(int i); bool plug_avail_on(int i); void plug_toggle(int i);

/* ---------------------------------------------------------------- menu bar, toolbar, tabs */
static void draw_menubar(void) {
    cui_canvas *c = &A.cv;
    cui_fill(c, 0, 0, A.W, MENU_H, T->bar); cui_fill(c, 0, MENU_H - 1, A.W, 1, T->border);
    H(0, 0, A.W, MENU_H, H_MENUBAR_BG, 0);
    int x = 8;
    for (int m = 0; m < NMENU; ++m) {
        int w = cui_text_width(g_ui, MENU_NAMES[m]) + 20; menu_x[m] = x; menu_w[m] = w;
        bool open = A.menu_open == m;
        if (open || (hov(x, 0, w, MENU_H - 1) && A.menu_open < 0)) cui_round_rect(c, x, 2, w, MENU_H - 5, 4, open ? T->btn_hi : T->btn);
        cui_text(c, g_ui, x + 10, ty(g_ui, 0, MENU_H - 1), MENU_NAMES[m], T->text);
        H(x, 0, w, MENU_H - 1, H_MENU, m); x += w;
    }
    const char *t = "Py2C Studio  -  Python to C";
    cui_text(c, g_ui_small, A.W - cui_text_width(g_ui_small, t) - 12, ty(g_ui_small, 0, MENU_H - 1), t, T->faint);
}
typedef struct { const char *label; int cmd; int style; } TB;      /* style: 0 plain, 1 primary, 2 gap */
static const TB TBS[] = { {"New", CMD_NEW, 0}, {"Open", CMD_OPEN, 0}, {"Save", CMD_SAVE, 0}, {"", 0, 2},
                          {"Convert", CMD_CONVERT, 1}, {"Run", CMD_RUN, 1}, {"Stop", CMD_STOP, 0}, {"Build", CMD_BUILD, 0}, {"", 0, 2},
                          {"Save C", CMD_SAVE_C, 0}, {"Studio", CMD_OPEN_IN_STUDIO, 0}, {"", 0, 2}, {"Find", CMD_FIND, 0}, {"Options", CMD_OPTIONS, 0}, {"Plugins", CMD_PLUGINS, 0} };
static int s_tool_end;
static void draw_toolbar(void) {
    cui_canvas *c = &A.cv;
    cui_fill(c, 0, MENU_H, A.W, TOOL_H, T->side); cui_fill(c, 0, MENU_H + TOOL_H - 1, A.W, 1, T->border);
    int x = 8, y = MENU_H + 6, h = TOOL_H - 12;
    for (unsigned i = 0; i < sizeof TBS / sizeof TBS[0]; ++i) {
        if (TBS[i].style == 2) { x += 10; continue; }
        const char *lab = TBS[i].label; int cmd = TBS[i].cmd;
        if (cmd == CMD_RUN && A.bs == B_RUNNING) { }
        int w = cui_text_width(g_ui, lab) + 26;
        btn(x, y, w, h, lab, H_TOOL, cmd, TBS[i].style == 1 && cmd_enabled(cmd), cmd_enabled(cmd));
        x += w + 5;
    }
    int nb = plug_btn_slots(); bool first = true;
    for (int i = 0; i < nb; ++i) if (plug_btn_live(i)) {
        if (first) { x += 10; first = false; }
        const char *lab = plug_btn_label(i); int w = cui_text_width(g_ui, lab) + 22;
        btn(x, y, w, h, lab, H_PLUG, i, false, true); x += w + 5;
    }
    s_tool_end = x;
    Doc *d = app_doc();
    if (d && x + 120 < A.W) {
        const char *p = d->path[0] ? d->path : d->name;
        int w = A.W - x - 14;
        int pw = cui_text_width(g_ui_small, p);
        if (pw <= w) cui_text(c, g_ui_small, A.W - pw - 12, ty(g_ui_small, MENU_H, TOOL_H - 1), p, T->faint);
        else cui_text_fit(c, g_ui_small, x + 10, ty(g_ui_small, MENU_H, TOOL_H - 1), w, p, T->faint);
    }
}
static void draw_tabs(void) {
    cui_canvas *c = &A.cv;
    cui_fill(c, 0, L.tabs_y, A.W, TAB_H, T->bar); cui_fill(c, 0, L.tabs_y + TAB_H - 1, A.W, 1, T->border);
    ui_tab_rects();
    for (int i = 0; i < A.ndocs; ++i) {
        Doc *d = A.docs[i]; bool on = i == A.cur; int x = tab_x[i], w = tab_w[i];
        if (on) { cui_fill(c, x, L.tabs_y, w, TAB_H, T->tab_on); cui_fill(c, x, L.tabs_y, w, 2, T->accent); }
        else if (hov(x, L.tabs_y, w, TAB_H)) cui_fill(c, x, L.tabs_y, w, TAB_H, T->btn);
        if (d->lang == L_PY) cui_fill(c, x + 10, L.tabs_y + 11, 8, 8, 0x4B8BBE); else if (d->lang == L_C) cui_fill(c, x + 10, L.tabs_y + 11, 8, 8, 0x6A9955); else cui_fill(c, x + 10, L.tabs_y + 11, 8, 8, T->faint);
        cui_text_fit(c, g_ui, x + 24, ty(g_ui, L.tabs_y, TAB_H), w - 24 - 24, d->name, on ? T->text : T->dim);
        bool cx = hov(x + w - 22, L.tabs_y, 22, TAB_H);
        if (d->dirty && !cx) cui_circle(c, (float)x + w - 11, (float)L.tabs_y + TAB_H / 2, 4.0f, T->warn);
        else { if (cx) cui_round_rect(c, x + w - 21, L.tabs_y + 7, 16, 16, 4, T->btn_hi);
               cui_line(c, (float)x + w - 17, (float)L.tabs_y + 11, (float)x + w - 9, (float)L.tabs_y + 19, 1.4f, cx ? T->text : T->faint);
               cui_line(c, (float)x + w - 9, (float)L.tabs_y + 11, (float)x + w - 17, (float)L.tabs_y + 19, 1.4f, cx ? T->text : T->faint); }
        H(x, L.tabs_y, w, TAB_H, H_TAB, i); H(x + w - 22, L.tabs_y, 22, TAB_H, H_TABX, i);
    }
    if (A.ndocs < MAX_DOCS) {
        int x = (A.ndocs ? tab_x[A.ndocs - 1] + tab_w[A.ndocs - 1] : L.main_x) + 6;
        if (hov(x, L.tabs_y + 3, 24, TAB_H - 6)) cui_round_rect(c, x, L.tabs_y + 3, 24, TAB_H - 6, 4, T->btn_hi);
        cui_text_center(c, g_ui_bold, x, L.tabs_y + 3, 24, TAB_H - 6, "+", T->dim);
        H(x, L.tabs_y + 3, 24, TAB_H - 6, H_TOOL, CMD_NEW);
    }
}

/* ---------------------------------------------------------------- rail and sidebar */
static void rail_icon(int x, int y, int n, bool on) {
    cui_canvas *c = &A.cv;
    bool h = hov(x, y, RAIL_W, 40);
    if (on) cui_fill(c, x, y, 2, 40, T->accent);
    if (h) cui_fill(c, x, y, RAIL_W, 40, cui_mix(T->side, T->btn, 120));
    uint32_t col = on || h ? T->text : T->faint; float cx = (float)x + 20, cy = (float)y + 20;
    switch (n) {
        case 0: cui_round_rect_outline(c, x + 11, y + 12, 18, 16, 3, 1.5f, col); cui_fill(c, x + 11, y + 12, 8, 3, col); break;                  /* explorer: a folder */
        case 1: cui_triangle(c, cx - 5, cy - 8, cx - 5, cy + 8, cx + 8, cy, col); break;                                                          /* run: a triangle */
        case 2: cui_circle(c, cx, cy, 9.0f, col); cui_fill(c, x + 19, y + 14, 2, 8, T->side); cui_fill(c, x + 19, y + 24, 2, 2, T->side); break; /* problems: ! in a disc */
        case 3: { static const float dx[8] = { 1, 0.7071f, 0, -0.7071f, -1, -0.7071f, 0, 0.7071f }, dy[8] = { 0, 0.7071f, 1, 0.7071f, 0, -0.7071f, -1, -0.7071f };   /* options: a gear */
                  cui_ring(c, cx, cy, 6.0f, 2.2f, col);
                  for (int k = 0; k < 8; ++k) cui_line(c, cx + 8.0f * dx[k], cy + 8.0f * dy[k], cx + 11.0f * dx[k], cy + 11.0f * dy[k], 2.4f, col);
                  break; }
        default: break;
    }
    H(x, y, RAIL_W, 40, H_RAIL, n);
}
static void draw_rail(void) {
    if (!L.rail_w) return;
    cui_fill(&A.cv, 0, L.tabs_y, RAIL_W, A.H - L.tabs_y - STATUS_H, cui_mix(T->side, T->bg, 50));
    int y = L.tabs_y + 4;
    rail_icon(0, y, 0, A.opt.show_sidebar); rail_icon(0, y + 42, 1, run_busy()); rail_icon(0, y + 84, 2, A.opt.show_panel && A.panel_tab == PANEL_PROBLEMS);
    rail_icon(0, A.H - STATUS_H - 44, 3, false);
}
static void pipe_row(int x, int y, int w, const char *name, int state) {      /* 0 todo, 1 working, 2 done, 3 failed */
    uint32_t col = state == 2 ? T->ok : state == 1 ? T->accent : state == 3 ? T->err : T->faint;
    cui_circle(&A.cv, (float)x + 6, (float)y + 9, 4.5f, col);
    cui_text_fit(&A.cv, g_ui_small, x + 18, ty(g_ui_small, y, 18), w - 18, name, state ? T->text : T->faint);
}
static void draw_sidebar(void) {
    if (!L.side_w) return;
    cui_canvas *c = &A.cv;
    int x = L.rail_w, y0 = L.tabs_y, w = L.side_w, h = A.H - y0 - STATUS_H;
    cui_fill(c, x, y0, w, h, T->side); cui_fill(c, x + w - 1, y0, 1, h, T->border);
    cui_text(c, g_ui_bold, x + 14, y0 + 8, "EXPLORER", T->dim);
    /* up and refresh */
    bool upo = hov(x + w - 54, y0 + 5, 22, 22), rfo = hov(x + w - 30, y0 + 5, 22, 22);
    if (upo) cui_round_rect(c, x + w - 54, y0 + 5, 22, 22, 4, T->btn_hi);
    if (rfo) cui_round_rect(c, x + w - 30, y0 + 5, 22, 22, 4, T->btn_hi);
    cui_text_center(c, g_ui_bold, x + w - 54, y0 + 5, 22, 22, "^", T->dim); cui_text_center(c, g_ui_bold, x + w - 30, y0 + 5, 22, 22, "o", T->dim);
    H(x + w - 54, y0 + 5, 22, 22, H_SIDE_UP, 0); H(x + w - 30, y0 + 5, 22, 22, H_SIDE_UP, 1);
    { const char *b = strrchr(A.dir, '/'); const char *nm = (b && b[1]) ? b + 1 : A.dir; cui_text_fit(c, g_ui_small, x + 14, y0 + 30, w - 24, nm, T->text); }
    int pipe_h = 102, ex_h = A.nex ? 26 + (A.nex < 6 ? A.nex : 6) * 22 : 0;
    int list_y = y0 + 52, list_h = h - 52 - pipe_h - ex_h; if (list_h < 60) { ex_h = 0; list_h = h - 52 - pipe_h; }
    cui_clip(c, x, list_y, w - 1, list_h);
    int rows = list_h / 22, maxs = A.nents - rows; if (maxs < 0) maxs = 0; if (A.side_scroll > maxs) A.side_scroll = maxs; if (A.side_scroll < 0) A.side_scroll = 0;
    Doc *cur = app_doc();
    for (int r = 0; r < rows + 1; ++r) {
        int i = A.side_scroll + r; if (i >= A.nents) break;
        int ry = list_y + r * 22; cos_dirent_ex_t *e = &A.ents[i];
        char full[300]; path_join(full, sizeof full, A.dir, e->name);
        bool active = cur && !strcmp(cur->path, full);
        bool hv = hov(x, ry, w - 1, 22) && hov(x, list_y, w, list_h);
        if (active) cui_fill(c, x, ry, w - 1, 22, T->btn); else if (hv) cui_fill(c, x, ry, w - 1, 22, cui_mix(T->side, T->btn, 130));
        if (e->is_dir) { cui_round_rect(c, x + 14, ry + 6, 14, 10, 2, 0xE2B96F); }
        else { cui_round_rect_outline(c, x + 16, ry + 4, 10, 14, 2, 1.2f, lang_of_name(e->name) == L_PY ? 0x4B8BBE : lang_of_name(e->name) == L_C ? 0x6A9955 : T->faint); }
        cui_text_fit(c, g_ui, x + 36, ty(g_ui, ry, 22), w - 44, e->name, e->is_dir ? 0xE2B96F : T->text);
        H(x, ry, w - 1, 22, H_SIDE, i);
    }
    cui_unclip(c);
    if (!A.nents) cui_text(c, g_ui_small, x + 14, list_y + 8, "(empty folder)", T->faint);
    if (ex_h) {
        int ey = y0 + h - pipe_h - ex_h;
        cui_fill(c, x + 10, ey, w - 21, 1, T->border);
        cui_text(c, g_ui_bold, x + 14, ey + 7, "EXAMPLES", T->dim);
        for (int i = 0; i < A.nex && i < 6; ++i) {
            int ry = ey + 26 + i * 22; bool hv = hov(x, ry, w - 1, 22);
            if (hv) cui_fill(c, x, ry, w - 1, 22, cui_mix(T->side, T->btn, 130));
            cui_round_rect_outline(c, x + 16, ry + 4, 10, 14, 2, 1.2f, 0x4B8BBE);
            cui_text_fit(c, g_ui, x + 36, ty(g_ui, ry, 22), w - 44, A.ex_names[i], T->text);
            H(x, ry, w - 1, 22, H_SIDE_EX, i);
        }
    }
    int py = y0 + h - pipe_h;
    cui_fill(c, x + 10, py, w - 21, 1, T->border);
    cui_text(c, g_ui_bold, x + 14, py + 7, "PIPELINE", T->dim);
    int st_parse = A.conv_ok < 0 ? 0 : A.conv_ok ? 2 : 3, st_gen = A.conv_ok == 1 ? 2 : A.conv_ok == 0 ? 3 : 0;
    int st_cc = A.bs == B_COMPILING ? 1 : (A.bs == B_RUNNING || A.bs == B_EXITED) ? 2 : A.bs == B_FAILED ? 3 : 0, st_run = A.bs == B_RUNNING ? 1 : A.bs == B_EXITED ? (A.exit_code ? 3 : 2) : 0;
    if (A.bs == B_CONVERTING) st_parse = 1;
    pipe_row(x + 14, py + 26, w - 24, "01  Parse Python", st_parse); pipe_row(x + 14, py + 44, w - 24, "02  Generate C", st_gen);
    pipe_row(x + 14, py + 62, w - 24, "03  Compile (tcc)", st_cc); pipe_row(x + 14, py + 80 - 0, w - 24, "04  Run", st_run);
    (void)pipe_h;
}

/* ---------------------------------------------------------------- find bar, editor area, panel, status */
static void draw_find_bar(void) {
    if (!L.find_h) return;
    cui_canvas *c = &A.cv;
    int x = L.ed_x, y = L.body_y, w = L.ed_w;
    cui_fill(c, x, y, w, L.find_h, T->bar); cui_fill(c, x, y + L.find_h - 1, w, 1, T->border);
    int fw = w - 250; if (fw > 320) fw = 320; if (fw < 120) fw = 120;
    A.find_f.focus = A.focus == 3 && A.find_focus == 0; A.repl_f.focus = A.focus == 3 && A.find_focus == 1;
    txt(x + 10, y + 4, 28, g_ui_small, T->dim, "Find");
    field_draw(c, &A.find_f, x + 52, y + 5, fw, 26, false); H(x + 52, y + 5, fw, 26, H_FIELD_FIND, 0);
    int bx = x + 52 + fw + 8;
    btn(bx, y + 5, 26, 26, "<", H_FIND_PREV, 0, false, true); btn(bx + 30, y + 5, 26, 26, ">", H_FIND_NEXT, 0, false, true);
    bool cs = hov(bx + 62, y + 5, 34, 26);
    cui_round_rect(c, bx + 62, y + 5, 34, 26, 5, !A.find_icase ? T->accent : cs ? T->btn_hi : T->btn);
    cui_text_center(c, g_ui, bx + 62, y + 5, 34, 26, "Aa", !A.find_icase ? T->on_accent : T->text); H(bx + 62, y + 5, 34, 26, H_FIND_CASE, 0);
    char cnt[40]; if (!A.find_f.len) cnt[0] = 0; else if (A.find_count < 0) snprintf(cnt, sizeof cnt, "no match"); else snprintf(cnt, sizeof cnt, "%d / %d", A.find_idx, A.find_count);
    cui_text(c, g_ui_small, bx + 104, ty(g_ui_small, y + 5, 26), cnt, A.find_count < 0 ? T->err : T->dim);
    btn(x + w - 34, y + 5, 26, 26, "x", H_FIND_CLOSE, 0, false, true);
    if (A.find_replace) {
        txt(x + 10, y + 38, 28, g_ui_small, T->dim, "With");
        field_draw(c, &A.repl_f, x + 52, y + 39, fw, 26, false); H(x + 52, y + 39, fw, 26, H_FIELD_REPL, 0);
        btn(bx, y + 39, 70, 26, "Replace", H_FIND_REPL, 0, false, true); btn(bx + 76, y + 39, 90, 26, "Replace All", H_FIND_ALL, 0, false, true);
    }
}

static void draw_welcome(void) {
    cui_canvas *c = &A.cv;
    int x = L.main_x, y = L.body_y, w = L.main_w, h = L.panel_y - L.body_y;
    cui_fill(c, x, y, w, h, T->bg);
    int cx = x + w / 2, cy = y + h / 2 - 80;
    cui_text_center(c, cui_font_bold(26), cx - 200, cy, 400, 36, "Py2C Studio", T->text);
    cui_text_center(c, g_ui, cx - 250, cy + 40, 500, 22, "Write Python, convert it to C, build it with tcc and run it - all on C-OS.", T->dim);
    btn(cx - 215, cy + 90, 130, 34, "New File", H_TOOL, CMD_NEW, true, true);
    btn(cx - 65, cy + 90, 130, 34, "Open...", H_TOOL, CMD_OPEN, false, true);
    btn(cx + 85, cy + 90, 130, 34, "Options...", H_TOOL, CMD_OPTIONS, false, true);
    if (A.nex) {
        cui_text_center(c, g_ui_bold, cx - 200, cy + 148, 400, 20, "Examples", T->dim);
        for (int i = 0; i < A.nex && i < 8; ++i) {
            int rx = cx - 230 + (i % 4) * 118, ry = cy + 176 + (i / 4) * 38;
            btn(rx, ry, 110, 30, A.ex_names[i], H_SIDE_EX, i, false, true);
        }
    }
}

static void draw_editor_area(void) {
    cui_canvas *c = &A.cv;
    Doc *d = app_doc();
    if (!d) { draw_welcome(); return; }
    A.ed.d = d;
    view_draw(c, &A.ed);
    draw_find_bar();
    if (L.c_w && A.cdoc) {                                    /* the converted pane */
        cui_fill(c, L.c_x - 1, L.body_y, 1, L.panel_y - L.body_y, T->border);
        cui_fill(c, L.c_x, L.body_y, L.c_w, CPANE_HDR, T->bar); cui_fill(c, L.c_x, L.body_y + CPANE_HDR - 1, L.c_w, 1, T->border);
        char title[160]; snprintf(title, sizeof title, "%s%s", A.cdoc->name, A.conv_ok == 1 ? "" : "  (error)");
        cui_text_fit(c, g_ui_bold, L.c_x + 12, ty(g_ui_bold, L.body_y, CPANE_HDR), L.c_w - 120, title, A.conv_ok == 1 ? T->text : T->err);
        int bx = L.c_x + L.c_w - 20;
        { bool h = hov(bx, L.body_y + 3, 18, 20); if (h) cui_round_rect(c, bx, L.body_y + 3, 18, 20, 4, T->btn_hi); cui_text_center(c, g_ui, bx, L.body_y + 3, 18, 20, "x", T->dim); H(bx, L.body_y + 3, 18, 20, H_CPANE_BTN, 2); }
        { int w2 = cui_text_width(g_ui_small, "Copy") + 14; bx -= w2 + 4; bool h = hov(bx, L.body_y + 3, w2, 20); cui_round_rect(c, bx, L.body_y + 3, w2, 20, 4, h ? T->btn_hi : T->btn); cui_text_center(c, g_ui_small, bx, L.body_y + 3, w2, 20, "Copy", T->text); H(bx, L.body_y + 3, w2, 20, H_CPANE_BTN, 0); }
        { int w2 = cui_text_width(g_ui_small, "Save") + 14; bx -= w2 + 4; bool h = hov(bx, L.body_y + 3, w2, 20); cui_round_rect(c, bx, L.body_y + 3, w2, 20, 4, h ? T->btn_hi : T->btn); cui_text_center(c, g_ui_small, bx, L.body_y + 3, w2, 20, "Save", T->text); H(bx, L.body_y + 3, w2, 20, H_CPANE_BTN, 1); }
        A.cv_view.d = A.cdoc; view_draw(c, &A.cv_view);
    }
}

static const char *const PANEL_NAMES[PANEL_N] = { "OUTPUT", "PROBLEMS", "INPUT", "SYNTAX TREE", "LANGUAGE" };
static void draw_problems(void) {
    cui_canvas *c = &A.cv;
    int x = L.main_x, y = A.pv.y, w = L.main_w, h = A.pv.h;
    cui_fill(c, x, y, w, h, T->bg);
    if (!A.nprobs) { cui_text(c, g_ui, x + 16, y + 12, A.conv_ok == 1 ? "No problems: the last conversion succeeded." : "No problems reported. Press Convert (F5) to check the file.", T->dim); return; }
    cui_clip(c, x, y, w, h);
    for (int i = 0; i < A.nprobs; ++i) {
        int ry = y + 2 + i * 26; Problem *p = &A.probs[i];
        bool sel = i == A.prob_sel, hv = hov(x, ry, w, 26);
        if (sel) cui_fill(c, x, ry, w, 26, T->sel); else if (hv) cui_fill(c, x, ry, w, 26, T->btn);
        cui_circle(c, (float)x + 18, (float)ry + 13, 5.0f, p->sev ? T->warn : T->err);
        char loc[48]; if (p->line) snprintf(loc, sizeof loc, "Line %d, col %d", p->line, p->col); else snprintf(loc, sizeof loc, "(no position)");
        cui_text(c, g_ui, x + 34, ty(g_ui, ry, 26), loc, T->dim);
        cui_text_fit(c, g_ui, x + 170, ty(g_ui, ry, 26), w - 190, p->msg, T->text);
        H(x, ry, w, 26, H_PROB, i);
    }
    cui_unclip(c);
}
static void draw_panel(void) {
    if (!L.panel_h) return;
    cui_canvas *c = &A.cv;
    int x = L.main_x, y = L.panel_y, w = L.main_w;
    cui_fill(c, x, y, w, L.panel_h, T->panel); cui_fill(c, x, y, w, 1, T->border);
    cui_fill(c, x, y + 1, w, PANEL_HDR - 1, T->bar); cui_fill(c, x, y + PANEL_HDR - 1, w, 1, T->border);
    int tx = x + 10;
    for (int t = 0; t < PANEL_N; ++t) {
        char lab[40]; if (t == PANEL_PROBLEMS && A.nprobs) snprintf(lab, sizeof lab, "%s (%d)", PANEL_NAMES[t], A.nprobs); else snprintf(lab, sizeof lab, "%s", PANEL_NAMES[t]);
        int tw = cui_text_width(g_ui_small, lab) + 20; bool on = t == A.panel_tab;
        if (on) cui_fill(c, tx, y + PANEL_HDR - 3, tw, 2, T->accent);
        else if (hov(tx, y + 1, tw, PANEL_HDR - 1)) cui_fill(c, tx, y + 1, tw, PANEL_HDR - 2, T->btn);
        cui_text(c, g_ui_small, tx + 10, ty(g_ui_small, y + 1, PANEL_HDR - 2), lab, on ? T->text : T->dim);
        H(tx, y + 1, tw, PANEL_HDR - 1, H_PTAB, t); tx += tw;
    }
    int bx = x + w - 10;
    static const char *const PB[3] = { "x", "Copy", "Clear" };
    for (int i = 0; i < 3; ++i) {
        int bw = cui_text_width(g_ui_small, PB[i]) + 16; bx -= bw + 4; bool hv = hov(bx, y + 3, bw, PANEL_HDR - 6);
        cui_round_rect(c, bx, y + 3, bw, PANEL_HDR - 6, 4, hv ? T->btn_hi : T->btn);
        cui_text_center(c, g_ui_small, bx, y + 3, bw, PANEL_HDR - 6, PB[i], T->text); H(bx, y + 3, bw, PANEL_HDR - 6, H_PBTN, i);
    }
    if (A.panel_tab == PANEL_PROBLEMS) draw_problems();
    else { A.pv.d = A.panel_docs[A.panel_tab]; if (A.panel_tab == PANEL_LANG) panel_fill_lang(); view_draw(c, &A.pv);
           if (A.panel_tab == PANEL_INPUT && !A.pv.d->len) cui_text(c, g_ui_small, x + 12, A.pv.y + 6, "Type the text the program reads with input() here (one line per call).", T->faint); }
}
static void draw_status(void) {
    cui_canvas *c = &A.cv;
    int y = A.H - STATUS_H;
    uint32_t bg = A.bs == B_RUNNING ? 0x2E7D4F : A.bs == B_COMPILING ? 0xA06A1C : A.bs == B_FAILED ? 0x9A2F3A : T->accent;
    cui_fill(c, 0, y, A.W, STATUS_H, bg);
    uint32_t fg = 0xFFFFFF;
    const char *state = A.bs == B_RUNNING ? "Running" : A.bs == B_COMPILING ? "Compiling" : A.bs == B_FAILED ? "Build failed" : A.bs == B_CONVERTING ? "Converting" : "Ready";
    cui_text(c, g_ui_small, 12, ty(g_ui_small, y, STATUS_H), state, fg);
    bool st = A.status[0] && cos_time_ms() - A.status_t < 6000;
    if (st) cui_text_fit(c, g_ui_small, 100, ty(g_ui_small, y, STATUS_H), A.W - 520, A.status, cui_mix(fg, bg, 30));
    Doc *d = app_doc();
    if (d) {
        char b[120]; int line = (int)doc_line_of(d, d->caret) + 1, col = doc_col_of(d, d->caret) + 1;
        size_t sa, sb; doc_sel(d, &sa, &sb);
        if (sb > sa) snprintf(b, sizeof b, "Ln %d, Col %d  (%d selected)", line, col, (int)(sb - sa)); else snprintf(b, sizeof b, "Ln %d, Col %d", line, col);
        int x = A.W - 12;
        const char *lang = d->lang == L_PY ? "Python" : d->lang == L_C ? "C" : "Text";
        char r[120]; snprintf(r, sizeof r, "%s   UTF-8   %s", lang, d->crlf ? "CRLF" : "LF");
        int rw = cui_text_width(g_ui_small, r); x -= rw; cui_text(c, g_ui_small, x, ty(g_ui_small, y, STATUS_H), r, fg);
        int bw = cui_text_width(g_ui_small, b); x -= bw + 24; cui_text(c, g_ui_small, x, ty(g_ui_small, y, STATUS_H), b, fg);
    }
    if (A.opt.ui_mode == 0) imeui_indicator(c, g_ui_small, A.W - 400, y + 4);
}

/* ---------------------------------------------------------------- menu dropdown */
static void draw_menu(void) {
    if (A.menu_open < 0) return;
    cui_canvas *c = &A.cv;
    int bx, by, bw, bh; menu_box(A.menu_open, &bx, &by, &bw, &bh);
    int n; const MI *mi = menu_items(A.menu_open, &n);
    cui_round_rect_alpha(c, bx - 1, by + 3, bw + 2, bh + 2, 8, 0x000000, 70);
    cui_round_rect(c, bx - 1, by - 1, bw + 2, bh + 2, 7, T->border);
    cui_round_rect(c, bx, by, bw, bh, 6, T->bar);
    int y = by + 5;
    for (int i = 0; i < n; ++i) {
        if (!mi[i].label) { cui_fill(c, bx + 10, y + 4, bw - 20, 1, T->border); y += 9; continue; }
        bool en = cmd_enabled(mi[i].cmd), hv = en && hov(bx, y, bw, 26);
        if (hv) cui_round_rect(c, bx + 4, y, bw - 8, 26, 4, T->accent);
        uint32_t col = !en ? T->faint : hv ? T->on_accent : T->text;
        if (cmd_checked(mi[i].cmd)) { cui_line(c, (float)bx + 12, (float)y + 13, (float)bx + 16, (float)y + 17, 1.8f, col); cui_line(c, (float)bx + 16, (float)y + 17, (float)bx + 23, (float)y + 9, 1.8f, col); }
        cui_text_fit(c, g_ui, bx + 30, ty(g_ui, y, 26), bw - 40 - (mi[i].sc && mi[i].sc[0] ? cui_text_width(g_ui_small, mi[i].sc) + 20 : 0), mi[i].label, col);
        if (mi[i].sc && mi[i].sc[0]) cui_text(c, g_ui_small, bx + bw - 14 - cui_text_width(g_ui_small, mi[i].sc), ty(g_ui_small, y, 26), mi[i].sc, hv ? cui_mix(T->on_accent, T->accent, 60) : T->faint);
        if (en) H(bx, y, bw, 26, H_MITEM, mi[i].cmd);
        y += 26;
    }
}

/* ---------------------------------------------------------------- find */
static void find_update_count(void) {
    Doc *d = app_doc(); A.find_count = 0; A.find_idx = 0;
    if (!d || !A.find_f.len) return;
    size_t wl = (size_t)A.find_f.len, a, b; doc_sel(d, &a, &b);
    for (size_t i = 0; i + wl <= d->len; ) {
        long p = find_next(d, A.find_f.text, wl, i, A.find_icase, false);
        if (p < 0 || (size_t)p < i) break;
        A.find_count++; if ((size_t)p == a && b - a == wl) A.find_idx = A.find_count;
        i = (size_t)p + wl; if (A.find_count > 9999) break;
    }
    if (!A.find_count) A.find_count = -1;
}
void find_step(bool back) {
    Doc *d = app_doc(); if (!d || !A.find_f.len) return;
    size_t wl = (size_t)A.find_f.len, a, b; doc_sel(d, &a, &b);
    long p = find_next(d, A.find_f.text, wl, back ? (a > 0 ? a - 1 : d->len) : b, A.find_icase, back);
    if (p < 0) { app_status("\"%s\" not found", A.find_f.text); find_update_count(); return; }
    d->anchor = (size_t)p; d->caret = (size_t)p + wl; d->want_col = doc_col_of(d, d->caret);
    view_center_caret(&A.ed); find_update_count();
}
void find_open(bool replace) {
    Doc *d = app_doc(); if (!d) return;
    A.find_open = true; A.find_replace = replace; A.focus = 3; A.find_focus = 0;
    if (doc_has_sel(d)) { size_t n; char *t = doc_sel_text(d, &n); if (t && n < 200 && !strchr(t, '\n')) { field_set(&A.find_f, t); A.find_f.sel = 0; } free(t); }
    A.find_f.sel = 0; A.find_f.pos = A.find_f.len;
    find_update_count(); app_relayout();
}
static void find_replace_one(void) {
    Doc *d = app_doc(); if (!d || d->readonly || !A.find_f.len) return;
    size_t wl = (size_t)A.find_f.len, a, b; doc_sel(d, &a, &b);
    bool match = b - a == wl && find_next(d, A.find_f.text, wl, a, A.find_icase, false) == (long)a;
    if (match) { doc_replace(d, a, wl, A.repl_f.text, (size_t)A.repl_f.len, false); d->anchor = d->caret; }
    find_step(false);
}
static void find_replace_all(void) {
    Doc *d = app_doc(); if (!d || d->readonly || !A.find_f.len) return;
    size_t wl = (size_t)A.find_f.len; int n = 0; size_t pos = 0;
    doc_group_begin(d);
    for (;;) {
        long p = find_next(d, A.find_f.text, wl, pos, A.find_icase, false);
        if (p < 0 || (size_t)p < pos) break;
        doc_replace(d, (size_t)p, wl, A.repl_f.text, (size_t)A.repl_f.len, false); n++;
        pos = (size_t)p + (size_t)A.repl_f.len; if (n > 100000) break;
    }
    doc_group_end(d); d->anchor = d->caret;
    app_status("Replaced %d occurrence%s", n, n == 1 ? "" : "s"); find_update_count();
}

/* ---------------------------------------------------------------- dialogs */
typedef struct { int x, y, w, h; } Box;
static Box dlg_frame(int w, int h, const char *title) {
    cui_canvas *c = &A.cv;
    cui_fill_alpha(c, 0, 0, A.W, A.H, 0x000000, 120);
    Box b = { (A.W - w) / 2, (A.H - h) / 2 - 10, w, h };
    cui_round_rect_alpha(c, b.x - 2, b.y + 4, b.w + 4, b.h + 4, 12, 0x000000, 80);
    cui_round_rect(c, b.x - 1, b.y - 1, b.w + 2, b.h + 2, 10, T->border);
    cui_round_rect(c, b.x, b.y, b.w, b.h, 9, T->bar);
    cui_text(c, g_ui_bold, b.x + 20, b.y + 11, title, T->text);
    cui_fill(c, b.x + 1, b.y + 38, b.w - 2, 1, T->border);
    H(b.x, b.y, b.w, b.h, H_NONE, 0);
    return b;
}
static void browser_load(void) { static int cap; A.nbents = dir_load(A.bdir, &A.bents, &cap); A.bscroll = 0; A.bsel = -1; }
void dlg_open(int kind) {
    Doc *d = app_doc();
    A.dlg = kind; A.dlg_msg[0] = 0; A.overwrite_ok = false; A.dlg_focus = 0; A.focus = 3;
    field_set(&A.dlg_f1, ""); field_set(&A.dlg_f2, "");
    if (kind == DLG_OPEN || kind == DLG_SAVE_AS || kind == DLG_SAVE_C) {
        if (d && d->path[0]) { snprintf(A.bdir, sizeof A.bdir, "%s", d->path); path_parent(A.bdir); } else snprintf(A.bdir, sizeof A.bdir, "%s", A.dir[0] ? A.dir : "/desktop");
        { cos_stat_ex_t st; if (cos_stat_ex(A.bdir, &st) != 0 || !st.is_dir) snprintf(A.bdir, sizeof A.bdir, "/"); }
        browser_load();
        if (kind == DLG_SAVE_AS && d) { field_set(&A.dlg_f1, d->name); A.dlg_f1.sel = 0; }      /* selected: typing replaces the suggested name */
        if (kind == DLG_SAVE_C && A.cdoc) { field_set(&A.dlg_f1, A.cdoc->name); A.dlg_f1.sel = 0; }
        snprintf(A.dlg_title, sizeof A.dlg_title, "%s", kind == DLG_OPEN ? "Open File" : kind == DLG_SAVE_AS ? "Save As" : "Save Converted Text As");
    } else if (kind == DLG_GOTO) {
        snprintf(A.dlg_title, sizeof A.dlg_title, "Go to Line");
    } else if (kind == DLG_OPTIONS) {
        snprintf(A.dlg_title, sizeof A.dlg_title, "Options"); field_set(&A.dlg_f1, A.opt.embed);
    } else if (kind == DLG_PLUGINS) { snprintf(A.dlg_title, sizeof A.dlg_title, "Plugins"); plug_refresh(); }
    else if (kind == DLG_ABOUT) snprintf(A.dlg_title, sizeof A.dlg_title, "About Py2C Studio");
    else if (kind == DLG_CONFIRM) snprintf(A.dlg_title, sizeof A.dlg_title, "Unsaved Changes");
    A.dlg_f1.focus = true; A.dlg_f2.focus = false;
    imeui_cancel(); A.dirty = true;
}
void dlg_close(void) {
    if (A.dlg == DLG_OPTIONS) { snprintf(A.opt.embed, sizeof A.opt.embed, "%s", A.dlg_f1.text); app_config_save(); }
    A.dlg = DLG_NONE; A.focus = 0; A.dirty = true;
}
static void finish_pending(void) {
    int act = A.pending_action; A.pending_action = 0;
    if (act == 1) app_close_tab(A.pending_doc, true);
    else if (act == 2) { bool more = false; for (int i = 0; i < A.ndocs; ++i) if (A.docs[i]->dirty) more = true; if (!more) A.quit = true; else app_do(CMD_EXIT); }
}
static void dlg_cancel(void) { A.pending_action = 0; dlg_close(); }

static bool path_is_dir(const char *p) { cos_stat_ex_t st; return cos_stat_ex(p, &st) == 0 && st.is_dir; }
static bool path_exists(const char *p) { cos_stat_ex_t st; return cos_stat_ex(p, &st) == 0; }
static void browse_to(const char *p) { snprintf(A.bdir, sizeof A.bdir, "%s", p); browser_load(); }
static void dlg_accept(void) {
    switch (A.dlg) {
        case DLG_OPEN: {
            char path[300];
            if (A.dlg_f1.len) { if (A.dlg_f1.text[0] == '/') snprintf(path, sizeof path, "%s", A.dlg_f1.text); else path_join(path, sizeof path, A.bdir, A.dlg_f1.text); }
            else if (A.bsel >= 0 && A.bsel < A.nbents) path_join(path, sizeof path, A.bdir, A.bents[A.bsel].name);
            else { snprintf(A.dlg_msg, sizeof A.dlg_msg, "Choose a file or type its name"); return; }
            if (path_is_dir(path)) { browse_to(path); field_set(&A.dlg_f1, ""); return; }
            if (!path_exists(path)) { snprintf(A.dlg_msg, sizeof A.dlg_msg, "No such file: %s", path); return; }
            dlg_close(); app_open(path); return; }
        case DLG_SAVE_AS: case DLG_SAVE_C: {
            if (!A.dlg_f1.len) { snprintf(A.dlg_msg, sizeof A.dlg_msg, "Type a file name"); return; }
            char path[300], nm[260]; snprintf(nm, sizeof nm, "%s", A.dlg_f1.text);
            if (!strchr(nm, '.')) strncat(nm, A.dlg == DLG_SAVE_C ? (A.cdoc && A.cdoc->lang == L_PY ? ".py" : ".c") : ".py", sizeof nm - strlen(nm) - 1);
            if (nm[0] == '/') snprintf(path, sizeof path, "%s", nm); else path_join(path, sizeof path, A.bdir, nm);
            if (path_is_dir(path)) { browse_to(path); field_set(&A.dlg_f1, ""); return; }
            if (path_exists(path) && !A.overwrite_ok) { snprintf(A.dlg_msg, sizeof A.dlg_msg, "%s exists - press Save again to replace it", nm); A.overwrite_ok = true; return; }
            if (A.dlg == DLG_SAVE_C) {
                bool ok = A.cdoc && p2_write_file(path, A.cdoc->t, A.cdoc->len) == (ssize_t)A.cdoc->len;
                app_status(ok ? "Saved %s" : "Could not save %s", path); if (ok) slog("saved %s", path); dlg_close(); explorer_refresh(); return;
            }
            Doc *d = app_doc();
            if (d && doc_save(d, path)) { app_status("Saved %s", path); slog("saved %s", path); snprintf(A.dir, sizeof A.dir, "%s", path); path_parent(A.dir); explorer_refresh(); dlg_close(); finish_pending(); }
            else snprintf(A.dlg_msg, sizeof A.dlg_msg, "Could not write %s", path);
            return; }
        case DLG_GOTO: {
            int line = atoi(A.dlg_f1.text); dlg_close();
            if (line > 0) { view_goto_line(&A.ed, line); app_status("Line %d", line); }
            return; }
        case DLG_CONFIRM: {                                     /* Save */
            Doc *d = (A.pending_doc >= 0 && A.pending_doc < A.ndocs) ? A.docs[A.pending_doc] : NULL;
            if (!d) { dlg_close(); return; }
            if (d->untitled || !d->path[0]) { A.cur = A.pending_doc; dlg_close(); A.dirty = true; dlg_open(DLG_SAVE_AS); return; }
            if (!doc_save(d, NULL)) { snprintf(A.dlg_msg, sizeof A.dlg_msg, "Could not save %s", d->name); return; }
            dlg_close(); finish_pending(); return; }
        default: dlg_close(); return;
    }
}
static void dlg_draw(void) {
    if (!A.dlg) return;
    cui_canvas *c = &A.cv;
    modal_from = nhits;
    switch (A.dlg) {
    case DLG_OPEN: case DLG_SAVE_AS: case DLG_SAVE_C: {
        Box b = dlg_frame(580, 460, A.dlg_title);
        btn(b.x + 16, b.y + 48, 30, 28, "^", H_DLG_UP, 0, false, true);
        cui_round_rect(c, b.x + 54, b.y + 48, b.w - 70, 28, 5, T->field);
        cui_text_fit(c, g_ui, b.x + 64, ty(g_ui, b.y + 48, 28), b.w - 90, A.bdir, T->text);
        int ly = b.y + 86, lh = b.h - 86 - 106, rows = lh / 24;
        cui_round_rect(c, b.x + 16, ly, b.w - 32, lh, 5, T->field);
        int maxs = A.nbents - rows; if (maxs < 0) maxs = 0; if (A.bscroll > maxs) A.bscroll = maxs; if (A.bscroll < 0) A.bscroll = 0;
        cui_clip(c, b.x + 16, ly, b.w - 32, lh);
        for (int r = 0; r < rows; ++r) {
            int i = A.bscroll + r; if (i >= A.nbents) break;
            int ry = ly + r * 24; cos_dirent_ex_t *e = &A.bents[i];
            bool sel = i == A.bsel, hv = hov(b.x + 16, ry, b.w - 32, 24);
            if (sel) cui_fill(c, b.x + 16, ry, b.w - 32, 24, T->sel); else if (hv) cui_fill(c, b.x + 16, ry, b.w - 32, 24, T->btn);
            if (e->is_dir) cui_round_rect(c, b.x + 28, ry + 7, 14, 10, 2, 0xE2B96F);
            else cui_round_rect_outline(c, b.x + 30, ry + 5, 10, 14, 2, 1.2f, lang_of_name(e->name) == L_PY ? 0x4B8BBE : T->faint);
            cui_text_fit(c, g_ui, b.x + 52, ty(g_ui, ry, 24), b.w - 190, e->name, e->is_dir ? 0xE2B96F : T->text);
            if (!e->is_dir) { char sz[32]; snprintf(sz, sizeof sz, "%d KB", (int)((e->size + 1023) / 1024)); cui_text(c, g_ui_small, b.x + b.w - 32 - cui_text_width(g_ui_small, sz) - 8, ty(g_ui_small, ry, 24), sz, T->faint); }
            H(b.x + 16, ry, b.w - 32, 24, H_DLG_ENTRY, i);
        }
        cui_unclip(c);
        int fy = b.y + b.h - 92;
        txt(b.x + 16, fy, 28, g_ui, T->dim, "Name");
        A.dlg_f1.focus = true; field_draw(c, &A.dlg_f1, b.x + 72, fy, b.w - 88, 28, false); H(b.x + 72, fy, b.w - 88, 28, H_DLG_F1, 0);
        if (A.dlg_msg[0]) cui_text_fit(c, g_ui_small, b.x + 16, fy + 34, b.w - 32, A.dlg_msg, A.dlg_msg[0] ? T->warn : T->dim);
        btn(b.x + b.w - 16 - 90, b.y + b.h - 44, 90, 30, A.dlg == DLG_OPEN ? "Open" : "Save", H_DLG_OK, 0, true, true);
        btn(b.x + b.w - 16 - 90 - 10 - 90, b.y + b.h - 44, 90, 30, "Cancel", H_DLG_CANCEL, 0, false, true);
        break; }
    case DLG_GOTO: {
        Box b = dlg_frame(360, 160, A.dlg_title);
        Doc *d = app_doc(); char l[80]; snprintf(l, sizeof l, "Line number (1 - %d)", d ? (int)doc_lines(d) : 1);
        txt(b.x + 20, b.y + 52, 20, g_ui, T->dim, l);
        field_draw(c, &A.dlg_f1, b.x + 20, b.y + 76, b.w - 40, 28, true); H(b.x + 20, b.y + 76, b.w - 40, 28, H_DLG_F1, 0);
        btn(b.x + b.w - 16 - 84, b.y + b.h - 42, 84, 28, "Go", H_DLG_OK, 0, true, true); btn(b.x + b.w - 16 - 84 - 10 - 84, b.y + b.h - 42, 84, 28, "Cancel", H_DLG_CANCEL, 0, false, true);
        break; }
    case DLG_OPTIONS: {
        Box b = dlg_frame(600, 500, A.dlg_title);
        int y = b.y + 52;
        txt(b.x + 20, y, 20, g_ui_bold, T->dim, "Conversion"); y += 26;
        check(b.x + 24, y, "Put the Python source lines in the C as comments   (--comments)", A.opt.comments, H_DLG_OPT, 0); y += 26;
        check(b.x + 24, y, "Strict ISO C11: refuse syntax that needs GNU extensions   (--c11)", A.opt.strict_c11, H_DLG_OPT, 1); y += 26;
        check(b.x + 24, y, "Unsupported syntax becomes a runtime NotImplementedError   (--fallback)", A.opt.fallback, H_DLG_OPT, 2); y += 32;
        txt(b.x + 24, y, 28, g_ui, T->text, "Embed entry function   (--embed-entry NAME, empty = int main)"); y += 28;
        A.dlg_f1.focus = A.dlg_focus == 0; field_draw(c, &A.dlg_f1, b.x + 24, y, b.w - 48, 28, false); H(b.x + 24, y, b.w - 48, 28, H_DLG_F1, 0); y += 42;
        txt(b.x + 20, y, 20, g_ui_bold, T->dim, "Editor"); y += 26;
        check(b.x + 24, y, "Save the file before Run / Build", A.opt.autosave_run, H_DLG_OPT, 3); y += 26;
        check(b.x + 24, y, "Show the converted pane next to the editor", A.opt.split_c, H_DLG_OPT, 4); y += 32;
        txt(b.x + 24, y, 28, g_ui, T->text, "Code size"); btn(b.x + 140, y, 30, 28, "-", H_DLG_OPT, 10, false, true);
        char sz[16]; snprintf(sz, sizeof sz, "%d px", A.opt.code_px); cui_text_center(c, g_ui, b.x + 174, y, 60, 28, sz, T->text); btn(b.x + 238, y, 30, 28, "+", H_DLG_OPT, 11, false, true); y += 40;
        txt(b.x + 24, y, 28, g_ui, T->text, "Theme"); btn(b.x + 140, y, 80, 28, "Dark", H_DLG_OPT, 12, A.opt.dark, true); btn(b.x + 226, y, 80, 28, "Light", H_DLG_OPT, 13, !A.opt.dark, true); y += 40;
        txt(b.x + 24, y, 28, g_ui, T->text, "Layout"); btn(b.x + 140, y, 100, 28, "Studio", H_DLG_OPT, 14, A.opt.ui_mode == 1, true); btn(b.x + 246, y, 100, 28, "Classic", H_DLG_OPT, 15, A.opt.ui_mode == 0, true);
        btn(b.x + b.w - 16 - 90, b.y + b.h - 44, 90, 30, "Close", H_DLG_OK, 0, true, true);
        break; }
    case DLG_CONFIRM: {
        Box b = dlg_frame(480, 170, A.dlg_title);
        cui_text_fit(c, g_ui, b.x + 24, b.y + 56, b.w - 48, A.dlg_msg[0] ? A.dlg_msg : "Save your changes?", T->text);
        txt(b.x + 24, b.y + 84, 20, g_ui_small, T->dim, "Your changes will be lost if you do not save them.");
        btn(b.x + b.w - 16 - 90, b.y + b.h - 44, 90, 30, "Save", H_DLG_OK, 0, true, true);
        btn(b.x + b.w - 16 - 90 - 10 - 120, b.y + b.h - 44, 120, 30, "Don't Save", H_DLG_SKIP, 0, false, true);
        btn(b.x + b.w - 16 - 90 - 10 - 120 - 10 - 80, b.y + b.h - 44, 80, 30, "Cancel", H_DLG_CANCEL, 0, false, true);
        break; }
    case DLG_ABOUT: {
        Box b = dlg_frame(460, 270, A.dlg_title);
        cui_round_rect(c, b.x + 24, b.y + 56, 52, 52, 12, T->accent); cui_text_center(c, cui_font_bold(24), b.x + 24, b.y + 56, 52, 52, "Py", T->on_accent);
        cui_text(c, cui_font_bold(18), b.x + 92, b.y + 58, "Py2C Studio", T->text);
        cui_text(c, g_ui, b.x + 92, b.y + 84, "Python Code to C - Alpha0.6 transpiler", T->dim);
        cui_fill(c, b.x + 20, b.y + 122, b.w - 40, 1, T->border);
        txt(b.x + 24, b.y + 132, 22, g_ui, T->dim, "Convert Python 3 to C11, or a C file to a Python sketch,");
        txt(b.x + 24, b.y + 154, 22, g_ui, T->dim, "build it with the on-device compiler (tcc) and run it.");
        txt(b.x + 24, b.y + 184, 22, g_ui_small, T->faint, "F5 Convert    F6 Run    F7 Build    Shift+F5 Stop    Ctrl+, Options");
        btn(b.x + b.w - 16 - 90, b.y + b.h - 44, 90, 30, "Close", H_DLG_OK, 0, true, true);
        break; }
    case DLG_PLUGINS: {
        int n = plug_avail_count();
        Box b = dlg_frame(500, 140 + 40 * (n ? n : 1), A.dlg_title);
        txt(b.x + 24, b.y + 48, 22, g_ui_small, T->dim, "Click a plugin to switch it on or off while Py2C Studio is running.");
        int y = b.y + 78;
        if (!n) txt(b.x + 24, y, 28, g_ui, T->faint, "(no plugins installed)");
        for (int i = 0; i < n; ++i, y += 40) {
            bool on = plug_avail_on(i), hv = hov(b.x + 24, y, b.w - 48, 34);
            cui_round_rect(c, b.x + 24, y, b.w - 48, 34, 6, on ? T->accent : hv ? T->btn_hi : T->btn);
            cui_text(c, g_ui, b.x + 40, ty(g_ui, y, 34), plug_avail_name(i), on ? T->on_accent : T->text);
            cui_text(c, g_ui_small, b.x + b.w - 24 - 40 - cui_text_width(g_ui_small, on ? "ON" : "OFF"), ty(g_ui_small, y, 34), on ? "ON" : "OFF", on ? T->on_accent : T->faint);
            H(b.x + 24, y, b.w - 48, 34, H_DLG_OPT, 100 + i);
        }
        btn(b.x + b.w - 16 - 90, b.y + b.h - 44, 90, 30, "Close", H_DLG_OK, 0, true, true);
        break; }
    default: break;
    }
}

/* ---------------------------------------------------------------- the whole frame */
void ui_init(void) {
    g_ui = cui_font_default(13); g_ui_bold = cui_font_bold(13); g_ui_small = cui_font_default(11);
    fonts_set_code_px(A.opt.code_px);
}
void ui_draw(void) {
    cui_canvas *c = &A.cv;
    nhits = 0; modal_from = 0;
    cui_unclip(c);
    cui_fill(c, 0, 0, A.W, A.H, T->bg);
    draw_editor_area();
    draw_panel();
    draw_sidebar();
    draw_rail();
    draw_tabs();
    draw_toolbar();
    draw_status();
    draw_menubar();
    draw_menu();
    dlg_draw();
    /* the IME window floats next to the caret of whatever is being typed into */
    A.ime_have = false;
    if (!A.dlg && A.menu_open < 0) {
        if (A.focus == 0 && app_doc()) { view_caret_xy(&A.ed, &A.ime_x, &A.ime_y); A.ime_have = true; }
        else if (A.focus == 2 && A.pv.editable && A.pv.d) { view_caret_xy(&A.pv, &A.ime_x, &A.ime_y); A.ime_have = true; }
        else if (A.focus == 3) { Field *f = A.find_focus ? &A.repl_f : &A.find_f; A.ime_x = L.ed_x + 60; A.ime_y = L.body_y + (A.find_focus ? 39 : 5); (void)f; A.ime_have = true; }
    } else if (A.dlg == DLG_OPEN || A.dlg == DLG_SAVE_AS || A.dlg == DLG_SAVE_C || A.dlg == DLG_GOTO || A.dlg == DLG_OPTIONS) { A.ime_x = A.W / 2 - 100; A.ime_y = A.H / 2 + 120; A.ime_have = true; }
    if (A.ime_have) imeui_draw(c, g_mono, A.ime_x, A.ime_y, g_lh);
    imeui_indicator(c, g_ui_small, A.W - 36, MENU_H + 8);
    cpop_draw(&ctxm, c, ctx_theme());
    A.dirty = false;
}

/* ---------------------------------------------------------------- keyboard */
static bool dlg_key(const cos_win_event_t *ev, const char *commit) {
    bool ctrl = (ev->mods & COS_MOD_CTRL) != 0;
    if (ev->special == COS_KEY_ESC) { dlg_cancel(); return true; }
    if (ev->special == COS_KEY_ENTER || (ev->special == COS_KEY_NONE && (ev->ascii == '\n' || ev->ascii == '\r'))) { dlg_accept(); return true; }
    if (A.dlg == DLG_CONFIRM && ev->special == COS_KEY_NONE && !ctrl) {
        char ch = ev->ascii | 32;
        if (ch == 'd' || ch == 'n') { int act = A.pending_action; (void)act; A.dirty = true; Doc *d = (A.pending_doc >= 0 && A.pending_doc < A.ndocs) ? A.docs[A.pending_doc] : NULL; if (d) d->dirty = false; dlg_close(); finish_pending(); return true; }
    }
    if (A.dlg == DLG_OPTIONS) { if (field_key(&A.dlg_f1, ev, commit)) return true; return false; }
    if (A.dlg == DLG_OPEN || A.dlg == DLG_SAVE_AS || A.dlg == DLG_SAVE_C) {
        if (ev->special == COS_KEY_UP || ev->special == COS_KEY_DOWN) {
            int d = ev->special == COS_KEY_UP ? -1 : 1; A.bsel += d; if (A.bsel < 0) A.bsel = 0; if (A.bsel >= A.nbents) A.bsel = A.nbents - 1;
            if (A.bsel >= 0 && A.bsel < A.nbents && !A.bents[A.bsel].is_dir) field_set(&A.dlg_f1, A.bents[A.bsel].name);
            int rows = (460 - 86 - 106) / 24; if (A.bsel < A.bscroll) A.bscroll = A.bsel; if (A.bsel >= A.bscroll + rows) A.bscroll = A.bsel - rows + 1;
            return true;
        }
        if (ev->special == COS_KEY_BACKSPACE && !A.dlg_f1.len) { char p[256]; snprintf(p, sizeof p, "%s", A.bdir); path_parent(p); browse_to(p); return true; }
        A.overwrite_ok = false;
    }
    if (A.dlg == DLG_GOTO || A.dlg == DLG_OPEN || A.dlg == DLG_SAVE_AS || A.dlg == DLG_SAVE_C) { if (field_key(&A.dlg_f1, ev, commit)) { A.dlg_msg[0] = 0; return true; } }
    return true;
}

static void sc_cmd(int cmd) { app_do(cmd); }
bool ui_key(const cos_win_event_t *ev, const char *commit) {
    bool ctrl = (ev->mods & COS_MOD_CTRL) != 0, shift = (ev->mods & COS_MOD_SHIFT) != 0, alt = (ev->mods & COS_MOD_ALT) != 0;
    int sp = ev->special;
    A.caret_t = cos_time_ms();
    if (A.dlg) return dlg_key(ev, commit);
    if (ctxm.open) { int r = cpop_key(&ctxm, sp); if (r >= 0) ctx_run(r); A.dirty = true; return true; }
    if (A.menu_open >= 0) { if (sp == COS_KEY_ESC) { A.menu_open = -1; A.dirty = true; return true; } A.menu_open = -1; }
    if (sp >= COS_KEY_F1 && sp <= COS_KEY_F12) {
        switch (sp - COS_KEY_F1 + 1) {
            case 1: sc_cmd(CMD_PANEL_LANG); return true;
            case 3: sc_cmd(shift ? CMD_FIND_PREV : CMD_FIND_NEXT); return true;
            case 5: sc_cmd(shift ? CMD_STOP : CMD_CONVERT); return true;
            case 6: sc_cmd(CMD_RUN); return true;
            case 7: sc_cmd(CMD_BUILD); return true;
            default: return false;
        }
    }
    if (sp == COS_KEY_ESC) {
        if (A.find_open && (A.focus == 3 || A.focus == 0)) { A.find_open = false; A.focus = 0; app_relayout(); return true; }
        return false;
    }
    if (ctrl && (sp == COS_KEY_PGUP)) { sc_cmd(CMD_PREV_TAB); return true; }
    if (ctrl && (sp == COS_KEY_PGDN)) { sc_cmd(CMD_NEXT_TAB); return true; }
    if (ctrl && sp == COS_KEY_TAB) { sc_cmd(shift ? CMD_PREV_TAB : CMD_NEXT_TAB); return true; }
    if (ctrl && (sp == COS_KEY_ENTER || (sp == COS_KEY_NONE && (ev->ascii == '\n' || ev->ascii == '\r')))) { sc_cmd(CMD_CONVERT); return true; }
    if (ctrl && sp == COS_KEY_NONE) {
        char ch = ev->ascii; if (ch >= 'A' && ch <= 'Z') ch += 32;
        switch (ch) {
            case 'n': sc_cmd(CMD_NEW); return true;
            case 'o': sc_cmd(CMD_OPEN); return true;
            case 's': sc_cmd(shift ? CMD_SAVE_AS : CMD_SAVE); return true;
            case 'w': sc_cmd(CMD_CLOSE_TAB); return true;
            case 'f': sc_cmd(CMD_FIND); return true;
            case 'h': sc_cmd(CMD_REPLACE); return true;
            case 'g': sc_cmd(CMD_GOTO_LINE); return true;
            case 'b': sc_cmd(CMD_VIEW_SIDEBAR); return true;
            case 'j': sc_cmd(CMD_VIEW_PANEL); return true;
            case 'k': sc_cmd(CMD_VIEW_CPANE); return true;
            case 'r': sc_cmd(CMD_RUN); return true;
            case ',': sc_cmd(CMD_OPTIONS); return true;
            case '+': case '=': sc_cmd(CMD_ZOOM_IN); return true;
            case '-': case '_': sc_cmd(CMD_ZOOM_OUT); return true;
            default: break;
        }
    }
    if (alt && ctrl) return false;
    /* the focused widget */
    if (A.focus == 3) {
        Field *f = A.find_focus ? &A.repl_f : &A.find_f;
        if (sp == COS_KEY_ENTER || (sp == COS_KEY_NONE && (ev->ascii == '\n' || ev->ascii == '\r'))) { if (A.find_focus) find_replace_one(); else find_step(shift); return true; }
        if (sp == COS_KEY_TAB && A.find_replace) { A.find_focus = !A.find_focus; return true; }
        bool had = f->len; bool used = field_key(f, ev, commit); (void)had;
        if (used && !A.find_focus) { Doc *d = app_doc(); if (d && f->len) { size_t a, b; doc_sel(d, &a, &b); long p = find_next(d, f->text, (size_t)f->len, a, A.find_icase, false); if (p >= 0) { d->anchor = (size_t)p; d->caret = (size_t)p + (size_t)f->len; view_center_caret(&A.ed); } } find_update_count(); }
        return used;
    }
    View *v = A.focus == 1 ? &A.cv_view : A.focus == 2 ? &A.pv : &A.ed;
    if (!v->d) return false;
    bool used = view_key(v, ev, commit);
    if (used && v == &A.ed) { A.ed.d->err_line = A.ed.d->err_line; }
    return used;
}

/* ---------------------------------------------------------------- mouse */
void open_example(int i) {
    if (i < 0 || i >= A.nex) return;
    char path[300], txtbuf[1 << 16]; snprintf(path, sizeof path, "/system/sdk/examples/py2c/%s", A.ex_names[i]);
    ssize_t n = cos_read_file(path, txtbuf, sizeof txtbuf - 1);
    if (n < 0) { app_status("Could not read %s", path); return; }
    txtbuf[n] = 0; app_new(txtbuf, A.ex_names[i]);
}
static void hit_action(Hit *h) {
    switch (h->id) {
        case H_MENU: A.menu_open = A.menu_open == h->arg ? -1 : h->arg; return;
        case H_MITEM: { int cmd = h->arg; A.menu_open = -1; app_do(cmd); return; }
        case H_TOOL: app_do(h->arg); return;
        case H_PLUG: plug_btn_run(h->arg); return;
        case H_TAB: tab_switch(h->arg); A.focus = 0; return;
        case H_TABX: app_close_tab(h->arg, false); return;
        case H_RAIL:
            if (h->arg == 0) app_do(CMD_VIEW_SIDEBAR); else if (h->arg == 1) app_do(CMD_RUN); else if (h->arg == 2) app_do(CMD_PANEL_PROBLEMS); else app_do(CMD_OPTIONS);
            return;
        case H_SIDE: {
            cos_dirent_ex_t *e = &A.ents[h->arg]; char p[300]; path_join(p, sizeof p, A.dir, e->name);
            if (e->is_dir) explorer_cd(p); else app_open(p);
            return; }
        case H_SIDE_EX: open_example(h->arg); return;
        case H_SIDE_UP: if (h->arg == 0) { path_parent(A.dir); explorer_cd(A.dir); } else explorer_refresh(); return;
        case H_PTAB: app_set_panel(h->arg); return;
        case H_PBTN: app_do(h->arg == 0 ? CMD_VIEW_PANEL : h->arg == 1 ? CMD_PANEL_COPY : CMD_PANEL_CLEAR); return;
        case H_PROB: {
            A.prob_sel = h->arg; Problem *p = &A.probs[h->arg];
            if (p->line > 0) { view_goto_line(&A.ed, p->line); A.focus = 0; }
            return; }
        case H_FIND_NEXT: find_step(false); return;
        case H_FIND_PREV: find_step(true); return;
        case H_FIND_CASE: A.find_icase = !A.find_icase; find_update_count(); return;
        case H_FIND_CLOSE: A.find_open = false; A.focus = 0; app_relayout(); return;
        case H_FIND_REPL: find_replace_one(); return;
        case H_FIND_ALL: find_replace_all(); return;
        case H_FIELD_FIND: A.focus = 3; A.find_focus = 0; A.find_f.pos = A.find_f.sel = field_hit(&A.find_f, L.ed_x + 52, A.mx); return;
        case H_FIELD_REPL: A.focus = 3; A.find_focus = 1; A.repl_f.pos = A.repl_f.sel = field_hit(&A.repl_f, L.ed_x + 52, A.mx); return;
        case H_CPANE_BTN:
            if (h->arg == 0) { if (A.cdoc) { cos_clip_set(A.cdoc->t, A.cdoc->len); app_status("Copied %d bytes", (int)A.cdoc->len); } }
            else if (h->arg == 1) app_do(CMD_SAVE_C); else { A.opt.split_c = false; app_relayout(); }
            return;
        default: return;
    }
}
static void dlg_hit(Hit *h) {
    switch (h->id) {
        case H_DLG_OK: if (A.dlg == DLG_OPTIONS || A.dlg == DLG_ABOUT || A.dlg == DLG_PLUGINS) dlg_close(); else dlg_accept(); return;
        case H_DLG_CANCEL: dlg_cancel(); return;
        case H_DLG_SKIP: { Doc *d = (A.pending_doc >= 0 && A.pending_doc < A.ndocs) ? A.docs[A.pending_doc] : NULL; if (d) d->dirty = false; dlg_close(); finish_pending(); return; }
        case H_DLG_UP: { char p[256]; snprintf(p, sizeof p, "%s", A.bdir); path_parent(p); browse_to(p); return; }
        case H_DLG_ENTRY: {
            uint64_t now = cos_time_ms(); static uint64_t last; static int last_i = -1;
            bool dbl = now - last < 450 && last_i == h->arg; last = now; last_i = h->arg;
            A.bsel = h->arg; cos_dirent_ex_t *e = &A.bents[h->arg];
            if (!e->is_dir) field_set(&A.dlg_f1, e->name);
            A.overwrite_ok = false; A.dlg_msg[0] = 0;
            if (dbl) { if (e->is_dir) { char p[300]; path_join(p, sizeof p, A.bdir, e->name); browse_to(p); field_set(&A.dlg_f1, ""); } else dlg_accept(); }
            return; }
        case H_DLG_F1: A.dlg_focus = 0; A.dlg_f1.pos = A.dlg_f1.sel = field_hit(&A.dlg_f1, h->x, A.mx); return;
        case H_DLG_OPT:
            switch (h->arg) {
                case 0: A.opt.comments = !A.opt.comments; break; case 1: A.opt.strict_c11 = !A.opt.strict_c11; break; case 2: A.opt.fallback = !A.opt.fallback; break;
                case 3: A.opt.autosave_run = !A.opt.autosave_run; break; case 4: A.opt.split_c = !A.opt.split_c; app_relayout(); break;
                case 10: if (A.opt.code_px > 9) { A.opt.code_px--; fonts_set_code_px(A.opt.code_px); app_relayout(); } break;
                case 11: if (A.opt.code_px < 28) { A.opt.code_px++; fonts_set_code_px(A.opt.code_px); app_relayout(); } break;
                case 12: app_apply_theme(true); break; case 13: app_apply_theme(false); break;
                case 14: A.opt.ui_mode = 1; app_relayout(); break; case 15: A.opt.ui_mode = 0; app_relayout(); break;
                default: if (h->arg >= 100) plug_toggle(h->arg - 100); break;
            }
            app_config_save(); return;
        default: return;
    }
}

/* ---------------------------------------------------------------- right-click menus */
enum { CX_TAB_CLOSE = 600, CX_TAB_OTHERS, CX_TAB_SAVE, CX_TAB_PATH, CX_SIDE_OPEN, CX_SIDE_PATH, CX_SIDE_REFRESH, CX_SIDE_UP, CX_PROB_GO, CX_PROB_COPY, CX_PANEL_CLOSE };
static int ctx_arg;
static const cpop_theme *ctx_theme(void) {
    static cpop_theme t;
    t.panel = T->panel; t.border = T->border; t.text = T->text; t.muted = T->dim; t.accent = T->accent; t.accent_text = T->on_accent;
    return &t;
}
static void ctx_item(int cmd, const char *label, const char *key) { cpop_add(&ctxm, label, key, cmd, cmd_enabled(cmd), cmd_checked(cmd)); }
static void ctx_open_at(int x, int y) { cpop_show(&ctxm, x, y, A.W, A.H, g_ui, g_ui_small); A.menu_open = -1; A.dirty = true; }
static void ctx_run(int cmd) {
    A.dirty = true;
    switch (cmd) {
        case CX_TAB_CLOSE: app_close_tab(ctx_arg, false); return;
        case CX_TAB_OTHERS: {
            if (ctx_arg < 0 || ctx_arg >= A.ndocs) return;
            Doc *keep = A.docs[ctx_arg];
            for (int i = A.ndocs - 1; i >= 0; --i) if (A.docs[i] != keep && !A.docs[i]->dirty) app_close_tab(i, true);
            for (int i = 0; i < A.ndocs; ++i) if (A.docs[i] == keep) { tab_switch(i); break; }
            return; }
        case CX_TAB_SAVE: if (ctx_arg >= 0 && ctx_arg < A.ndocs) { tab_switch(ctx_arg); app_save(A.docs[ctx_arg], false); } return;
        case CX_TAB_PATH: if (ctx_arg >= 0 && ctx_arg < A.ndocs) { Doc *d = A.docs[ctx_arg]; const char *p = d->untitled ? d->name : d->path; cos_clip_set(p, strlen(p)); app_status("Copied %s", p); } return;
        case CX_SIDE_OPEN: case CX_SIDE_PATH: {
            if (ctx_arg < 0 || ctx_arg >= A.nents) return;
            cos_dirent_ex_t *e = &A.ents[ctx_arg]; char p[300]; path_join(p, sizeof p, A.dir, e->name);
            if (cmd == CX_SIDE_PATH) { cos_clip_set(p, strlen(p)); app_status("Copied %s", p); }
            else if (e->is_dir) explorer_cd(p); else app_open(p);
            return; }
        case CX_SIDE_REFRESH: explorer_refresh(); return;
        case CX_SIDE_UP: path_parent(A.dir); explorer_cd(A.dir); return;
        case CX_PROB_GO: if (ctx_arg >= 0 && ctx_arg < A.nprobs && A.probs[ctx_arg].line > 0) { view_goto_line(&A.ed, A.probs[ctx_arg].line); A.focus = 0; } return;
        case CX_PROB_COPY: if (ctx_arg >= 0 && ctx_arg < A.nprobs) { char b[400]; int n = snprintf(b, sizeof b, "line %d, col %d: %s", A.probs[ctx_arg].line, A.probs[ctx_arg].col, A.probs[ctx_arg].msg); cos_clip_set(b, (size_t)n); } return;
        case CX_PANEL_CLOSE: app_do(CMD_VIEW_PANEL); return;
        default: app_do(cmd); return;
    }
}
/* a right button press at (x, y) */
static void ctx_right_click(int x, int y, const cos_win_event_t *ev) {
    cpop_clear(&ctxm);
    if (A.dlg) return;
    Hit *h = hit_at(x, y, 0);
    if (h && h->id == H_TAB) {
        ctx_arg = h->arg; Doc *d = (h->arg >= 0 && h->arg < A.ndocs) ? A.docs[h->arg] : NULL; if (!d) return;
        cpop_add(&ctxm, "Close", "Ctrl+W", CX_TAB_CLOSE, true, false);
        cpop_add(&ctxm, "Close Other Tabs", "", CX_TAB_OTHERS, A.ndocs > 1, false);
        cpop_sep(&ctxm);
        cpop_add(&ctxm, "Save", "Ctrl+S", CX_TAB_SAVE, d->dirty || d->untitled, false);
        cpop_add(&ctxm, "Copy Path", "", CX_TAB_PATH, true, false);
        ctx_open_at(x, y); return;
    }
    if (h && (h->id == H_SIDE || h->id == H_SIDE_EX)) {
        ctx_arg = h->arg;
        if (h->id == H_SIDE_EX) { cpop_add(&ctxm, "Open Example", "", CMD_EXAMPLE + h->arg, true, false); ctx_open_at(x, y); return; }
        bool dir = h->arg >= 0 && h->arg < A.nents && A.ents[h->arg].is_dir;
        cpop_add(&ctxm, dir ? "Open Folder" : "Open", "", CX_SIDE_OPEN, true, false);
        cpop_add(&ctxm, "Copy Path", "", CX_SIDE_PATH, true, false);
        cpop_sep(&ctxm);
        cpop_add(&ctxm, "Parent Folder", "", CX_SIDE_UP, true, false);
        cpop_add(&ctxm, "Refresh", "", CX_SIDE_REFRESH, true, false);
        ctx_open_at(x, y); return;
    }
    if (h && h->id == H_PROB) {
        ctx_arg = h->arg; A.prob_sel = h->arg;
        cpop_add(&ctxm, "Go to Line", "", CX_PROB_GO, A.probs[h->arg].line > 0, false);
        cpop_add(&ctxm, "Copy Message", "", CX_PROB_COPY, true, false);
        cpop_sep(&ctxm);
        cpop_add(&ctxm, "Clear Problems", "", CMD_PANEL_CLEAR, true, false);
        ctx_open_at(x, y); return;
    }
    if (L.side_w && x >= L.rail_w && x < L.rail_w + L.side_w && y >= L.tabs_y && y < L.bottom) {
        cpop_add(&ctxm, "Parent Folder", "", CX_SIDE_UP, true, false);
        cpop_add(&ctxm, "Refresh", "", CX_SIDE_REFRESH, true, false);
        cpop_sep(&ctxm);
        cpop_add(&ctxm, "Hide Sidebar", "Ctrl+B", CMD_VIEW_SIDEBAR, true, false);
        ctx_open_at(x, y); return;
    }
    View *views[3] = { &A.ed, &A.cv_view, &A.pv };
    for (int k = 0; k < 3; ++k) {
        View *v = views[k];
        if (!v->d || x < v->x || x >= v->x + v->w || y < v->y || y >= v->y + v->h) continue;
        A.focus = k; imeui_cancel();
        if (!doc_has_sel(v->d)) { view_mouse(v, 0, x, y, ev->buttons, ev->mods); v->drag = false; }   /* the caret goes where the right button went */
        if (k == 0) {
            ctx_item(CMD_UNDO, "Undo", "Ctrl+Z"); ctx_item(CMD_REDO, "Redo", "Ctrl+Y"); cpop_sep(&ctxm);
            ctx_item(CMD_CUT, "Cut", "Ctrl+X"); ctx_item(CMD_COPY, "Copy", "Ctrl+C"); ctx_item(CMD_PASTE, "Paste", "Ctrl+V"); ctx_item(CMD_SELECT_ALL, "Select All", "Ctrl+A"); cpop_sep(&ctxm);
            ctx_item(CMD_FIND, "Find...", "Ctrl+F"); ctx_item(CMD_REPLACE, "Replace...", "Ctrl+H"); ctx_item(CMD_GOTO_LINE, "Go to Line...", "Ctrl+G"); cpop_sep(&ctxm);
            ctx_item(CMD_COMMENT, "Toggle Comment", "Ctrl+/"); ctx_item(CMD_INDENT, "Indent", "Tab"); ctx_item(CMD_OUTDENT, "Outdent", "Shift+Tab"); ctx_item(CMD_DUP_LINE, "Duplicate Line", "Ctrl+D"); cpop_sep(&ctxm);
            ctx_item(CMD_CONVERT, "Convert to C", "F5"); ctx_item(CMD_RUN, "Run", "F6"); ctx_item(CMD_BUILD, "Build Executable (.c-os)", "F7");
        } else if (k == 1) {
            ctx_item(CMD_COPY, "Copy", "Ctrl+C"); ctx_item(CMD_SELECT_ALL, "Select All", "Ctrl+A"); cpop_sep(&ctxm);
            ctx_item(CMD_SAVE_C, "Save Converted As...", ""); ctx_item(CMD_OPEN_IN_STUDIO, "Open in C-OS Studio", ""); cpop_sep(&ctxm);
            cpop_add(&ctxm, "Hide Converted Pane", "Ctrl+K", CMD_VIEW_CPANE, true, false);
        } else {
            ctx_item(CMD_COPY, "Copy", "Ctrl+C");
            if (v->editable) { ctx_item(CMD_CUT, "Cut", "Ctrl+X"); ctx_item(CMD_PASTE, "Paste", "Ctrl+V"); ctx_item(CMD_UNDO, "Undo", "Ctrl+Z"); }
            ctx_item(CMD_SELECT_ALL, "Select All", "Ctrl+A"); cpop_sep(&ctxm);
            cpop_add(&ctxm, "Copy All", "", CMD_PANEL_COPY, true, false); cpop_add(&ctxm, "Clear", "", CMD_PANEL_CLEAR, true, false); cpop_sep(&ctxm);
            cpop_add(&ctxm, "Hide Panel", "Ctrl+J", CX_PANEL_CLOSE, true, false);
        }
        ctx_open_at(x, y); return;
    }
}

void ui_mouse(int type, const cos_win_event_t *ev) {
    int x = ev->x, y = ev->y;
    A.mx = x; A.my = y; A.dirty = true;
    if (ctxm.open) {                                                   /* the context menu takes the pointer while it is open */
        if (type == 1) { cpop_mouse_move(&ctxm, x, y); return; }
        if (type == 0) { int r = cpop_mouse_down(&ctxm, x, y); if (r >= 0) ctx_run(r); return; }
        return;
    }
    if (type == 0 && ev->button == COS_MOUSE_BTN_RIGHT) { ctx_right_click(x, y, ev); return; }
    if (type == 1) {                                                   /* move */
        if (A.drag_split == 1) { A.side_w = x - L.rail_w; if (A.side_w < 140) A.side_w = 140; if (A.side_w > A.W / 2) A.side_w = A.W / 2; app_relayout(); return; }
        if (A.drag_split == 2) { A.panel_h = L.bottom - y; if (A.panel_h < 80) A.panel_h = 80; app_relayout(); return; }
        if (A.drag_split == 3) { A.cpane_w = L.main_x + L.main_w - x; if (A.cpane_w < 200) A.cpane_w = 200; app_relayout(); return; }
        if (A.dlg) return;
        if (A.ed.drag) view_mouse(&A.ed, 1, x, y, ev->buttons, ev->mods);
        else if (A.cv_view.drag) view_mouse(&A.cv_view, 1, x, y, ev->buttons, ev->mods);
        else if (A.pv.drag) view_mouse(&A.pv, 1, x, y, ev->buttons, ev->mods);
        return;
    }
    if (type == 2) {                                                   /* up */
        A.drag_split = 0; A.ed.drag = A.cv_view.drag = A.pv.drag = false; return;
    }
    if (ev->button != COS_MOUSE_BTN_LEFT) return;
    /* down */
    A.mdown = true;
    if (A.dlg) { Hit *h = hit_at(x, y, modal_from); if (h) dlg_hit(h); else if (A.dlg == DLG_OPEN) { } return; }
    if (A.menu_open >= 0) {
        Hit *h = hit_at(x, y, 0);
        if (h && (h->id == H_MITEM || h->id == H_MENU)) { hit_action(h); return; }
        A.menu_open = -1; if (h && h->id == H_MENUBAR_BG) return; /* fall through: the click also lands where it was aimed, except on the bar */
    }
    /* splitters */
    if (L.side_w && x >= L.rail_w + L.side_w - 3 && x <= L.rail_w + L.side_w + 3 && y >= L.tabs_y) { A.drag_split = 1; return; }
    if (L.panel_h && y >= L.panel_y - 3 && y <= L.panel_y + 3 && x >= L.main_x) { A.drag_split = 2; return; }
    if (L.c_w && x >= L.c_x - 3 && x <= L.c_x + 3 && y >= L.body_y && y < L.panel_y) { A.drag_split = 3; return; }
    Hit *h = hit_at(x, y, 0);
    if (h && h->id != H_NONE && h->id != H_MENUBAR_BG) { hit_action(h); return; }
    if (app_doc() && in(x, y, A.ed.x, A.ed.y, A.ed.w, A.ed.h)) { A.focus = 0; imeui_cancel(); view_mouse(&A.ed, 0, x, y, ev->buttons, ev->mods); return; }
    if (L.c_w && A.cdoc && in(x, y, A.cv_view.x, A.cv_view.y, A.cv_view.w, A.cv_view.h)) { A.focus = 1; view_mouse(&A.cv_view, 0, x, y, ev->buttons, ev->mods); return; }
    if (L.panel_h && A.pv.d && in(x, y, A.pv.x, A.pv.y, A.pv.w, A.pv.h)) { A.focus = 2; imeui_cancel(); view_mouse(&A.pv, 0, x, y, ev->buttons, ev->mods); return; }
}
void ui_wheel(int dy, bool shift) {
    int x = A.mx, y = A.my;
    if (A.dlg == DLG_OPEN || A.dlg == DLG_SAVE_AS || A.dlg == DLG_SAVE_C) { A.bscroll -= dy > 0 ? 3 : -3; return; }
    if (A.dlg) return;
    if (in(x, y, A.ed.x, A.ed.y, A.ed.w, A.ed.h) && A.ed.d) view_wheel(&A.ed, dy, shift);
    else if (L.c_w && A.cdoc && in(x, y, A.cv_view.x, A.cv_view.y, A.cv_view.w, A.cv_view.h)) view_wheel(&A.cv_view, dy, shift);
    else if (L.panel_h && A.pv.d && in(x, y, A.pv.x, A.pv.y, A.pv.w, A.pv.h)) view_wheel(&A.pv, dy, shift);
    else if (L.side_w && x >= L.rail_w && x < L.rail_w + L.side_w) A.side_scroll -= dy > 0 ? 3 : -3;
    else if (L.panel_h && A.panel_tab == PANEL_PROBLEMS && y >= L.panel_y) { }
}
