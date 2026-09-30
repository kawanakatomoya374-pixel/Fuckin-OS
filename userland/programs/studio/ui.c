/* ui.c - everything around the editor: menu bar, toolbar, activity bar,
 * sidebar (Explorer / Outline / Search / Problems), tabs, the bottom panel and
 * the status bar. Coordinates come from measuring cos_studio_gui_mockup.png;
 * where the mockup shows a font-fallback box instead of an icon (the search
 * and new-file glyphs) a real icon is drawn.
 */
#include "studio.h"

App G;
Layout L;

extern const unsigned char studio_font_mono[], studio_font_mono_bold[];

/* ---------------------------------------------------------------- */
/* fonts                                                             */
/* ---------------------------------------------------------------- */
cui_font *font_ui(int px) { return cui_font_default(px); }
cui_font *font_ui_bold(int px) { return cui_font_bold(px); }
cui_font *font_code_px(int px, bool bold) {
    static cui_font *cache[2][40];
    if (px < 6) px = 6;
    if (px > 39) px = 39;
    if (!cache[bold][px]) cache[bold][px] = cui_font_load(bold ? studio_font_mono_bold : studio_font_mono, px);
    return cache[bold][px];
}
cui_font *font_code(void) { return font_code_px(G.code_px, false); }
cui_font *font_code_bold(void) { return font_code_px(G.code_px, true); }

/* draw text with its BASELINE at `base` (the mockup was measured that way) */
static int tb(int x, int base, cui_font *f, uint32_t col, const char *s) {
    return cui_text(&G.cv, f, x, base - cui_font_ascent(f), s, col);
}
/* letter-spaced caps for section headers */
static int tb_spaced(int x, int base, cui_font *f, uint32_t col, const char *s, float spacing) {
    float fx = (float)x;
    for (const char *p = s; *p; ++p) {
        char one[2] = { *p, 0 };
        cui_text(&G.cv, f, (int)(fx + 0.5f), base - cui_font_ascent(f), one, col);
        fx += (float)cui_text_width(f, one) + spacing;
    }
    return (int)(fx - (float)x);
}

/* ---------------------------------------------------------------- */
/* layout                                                            */
/* ---------------------------------------------------------------- */
void layout_compute(void) {
    L.W = G.cv.w; L.H = G.cv.h;
    L.side_x = ACT_W;
    L.side_w = G.sidebar_open ? G.side_w : 0;
    L.main_x = L.side_x + L.side_w;
    L.main_w = L.W - L.main_x;
    L.status_y = L.H - STATUS_H;
    L.tab_y = MAIN_Y;
    L.ed_y = MAIN_Y + TAB_H;
    L.panel_h = G.panel_open ? G.panel_h : 0;
    L.panel_y = L.panel_h ? L.status_y - L.panel_h : L.status_y;
    L.ed_h = L.panel_y - L.ed_y;
    if (L.ed_h < 40) L.ed_h = 40;
}

/* ---------------------------------------------------------------- */
/* small widgets                                                     */
/* ---------------------------------------------------------------- */
void ui_field(int x, int y, int w, int h, const char *text, bool focus, const char *hint) {
    cui_canvas *c = &G.cv;
    cui_round_rect(c, x, y, w, h, 4, T->field);
    cui_round_rect_outline(c, x, y, w, h, 4, 1.0f, focus ? T->accent : T->border);
    cui_font *f = font_ui(12);
    int ty = y + (h - cui_font_height(f)) / 2;
    if (text[0]) cui_text_fit(c, f, x + 8, ty, w - 16, text, T->text);
    else if (hint) cui_text(c, f, x + 8, ty, hint, T->faint);
    if (focus && ((cos_time_ms() - G.caret_t) / 530) % 2 == 0) {
        int tw = text[0] ? cui_text_width(f, text) : 0;
        if (tw > w - 18) tw = w - 18;
        cui_fill(c, x + 8 + tw, y + 5, 1, h - 10, T->text);
    }
}
void ui_button(int x, int y, int w, int h, const char *label, bool primary, bool hover) {
    cui_canvas *c = &G.cv;
    uint32_t bg = primary ? (hover ? cui_mix(T->accent, 0xFFFFFF, 30) : T->accent) : (hover ? T->btn_hi : T->btn);
    cui_round_rect(c, x, y, w, h, 5, bg);
    cui_text_center(c, font_ui(12), x, y, w, h, label, primary ? T->on_accent : T->text);
}

/* ---------------------------------------------------------------- */
/* menus                                                             */
/* ---------------------------------------------------------------- */
typedef struct { const char *label; int cmd; const char *sc; } MI;      /* label NULL = separator (cmd -1) or the list's end (cmd 0) */
static const MI M_FILE[] = {
    {"New File", CMD_NEW, "Ctrl+N"}, {"New Project...", CMD_NEW_PROJECT, "Ctrl+Shift+N"}, {"Open...", CMD_OPEN, "Ctrl+O"},
    {"Open Folder...", CMD_OPEN_FOLDER, ""}, {NULL, -1, 0},
    {"Save", CMD_SAVE, "Ctrl+S"}, {"Save As...", CMD_SAVE_AS, "Ctrl+Shift+S"}, {"Save All", CMD_SAVE_ALL, "Ctrl+Alt+S"}, {NULL, -1, 0},
    {"Close Tab", CMD_CLOSE_TAB, "Ctrl+W"}, {"Exit", CMD_EXIT, ""}, {0}
};
static const MI M_EDIT[] = {
    {"Undo", CMD_UNDO, "Ctrl+Z"}, {"Redo", CMD_REDO, "Ctrl+Y"}, {NULL, -1, 0},
    {"Cut", CMD_CUT, "Ctrl+X"}, {"Copy", CMD_COPY, "Ctrl+C"}, {"Paste", CMD_PASTE, "Ctrl+V"}, {"Select All", CMD_SELECT_ALL, "Ctrl+A"}, {NULL, -1, 0},
    {"Find", CMD_FIND, "Ctrl+F"}, {"Replace", CMD_REPLACE, "Ctrl+H"}, {"Find in Project", CMD_FIND_PROJECT, "Ctrl+Shift+F"}, {NULL, -1, 0},
    {"Toggle Comment", CMD_TOGGLE_COMMENT, "Ctrl+/"}, {"Indent", CMD_INDENT, "Tab"}, {"Outdent", CMD_OUTDENT, "Shift+Tab"}, {"Duplicate Line", CMD_DUP_LINE, "Ctrl+D"}, {0}
};
static const MI M_VIEW[] = {
    {"Sidebar", CMD_VIEW_SIDEBAR, "Ctrl+B"}, {"Panel", CMD_VIEW_PANEL, "Ctrl+J"}, {NULL, -1, 0},
    {"Zoom In", CMD_ZOOM_IN, "Ctrl++"}, {"Zoom Out", CMD_ZOOM_OUT, "Ctrl+-"}, {NULL, -1, 0},
    {"Dark Theme", CMD_THEME_DARK, ""}, {"Light Theme", CMD_THEME_LIGHT, ""}, {0}
};
static const MI M_GO[] = {
    {"Go to Line...", CMD_GOTO_LINE, "Ctrl+G"}, {"Go to File...", CMD_GOTO_FILE, "Ctrl+P"}, {"Go to Symbol...", CMD_GOTO_SYMBOL, "Ctrl+Shift+O"}, {NULL, -1, 0},
    {"Next Problem", CMD_NEXT_PROBLEM, "F8"}, {"Previous Problem", CMD_PREV_PROBLEM, "Shift+F8"}, {NULL, -1, 0},
    {"Back", CMD_NAV_BACK, "Alt+Left"}, {"Forward", CMD_NAV_FWD, "Alt+Right"}, {0}
};
static const MI M_RUN[] = {
    {"Run", CMD_RUN, "F5"}, {"Build", CMD_BUILD, "F7"}, {"Stop", CMD_STOP, "Shift+F5"}, {"Clean", CMD_CLEAN, ""}, {NULL, -1, 0},
    {"Compile to Object (.o)", CMD_BUILD_OBJ, ""}, {"Preprocess (-E)", CMD_BUILD_PREPROCESS, ""}, {"Build Shared Library (.c-osll)", CMD_BUILD_SHARED, ""}, {NULL, -1, 0},
    {"Debug Configuration", CMD_CFG_DEBUG, ""}, {"Release Configuration", CMD_CFG_RELEASE, ""}, {0}
};
static const MI M_TOOLS[] = {
    {"Terminal", CMD_OPEN_TERMINAL, ""}, {NULL, -1, 0},
    {"Insert Snippet: main", CMD_SNIPPET_MAIN, ""}, {"Insert Snippet: window", CMD_SNIPPET_WINDOW, ""}, {"Insert Snippet: loop", CMD_SNIPPET_LOOP, ""}, {NULL, -1, 0},
    {"Settings...", CMD_SETTINGS, ""}, {0}
};
static const MI M_HELP[] = {
    {"SDK Reference", CMD_SDK_REF, ""}, {"Keyboard Shortcuts", CMD_SHORTCUTS, "Ctrl+K"}, {"About C-OS Studio", CMD_ABOUT, ""}, {0}
};
static const MI M_CFG[] = { {"Debug", CMD_CFG_DEBUG, ""}, {"Release", CMD_CFG_RELEASE, ""}, {0} };
static const char *const MENU_NAMES[] = { "File", "Edit", "View", "Go", "Run", "Tools", "Help" };
static const int MENU_LABEL_X[] = { 11, 61, 110, 160, 197, 239, 297 };
static const MI *const MENUS[] = { M_FILE, M_EDIT, M_VIEW, M_GO, M_RUN, M_TOOLS, M_HELP, M_CFG };
#define MENU_CFG 7

static int menu_count(const MI *m) { int n = 0; while (m[n].label || m[n].cmd || (n < 40 && m[n].sc)) { ++n; if (n > 30) break; } return n; }
static int menu_len(const MI *m) {
    /* a separator is {NULL,-1,0} (see the MI comment); only the true terminator {0} has cmd==0,
     * distinguishing "there's more after this blank line" from "the menu ends here" - both used
     * to be written as {NULL,0,0} and were indistinguishable, silently truncating every menu that
     * had a separator followed by more items. */
    int n = 0; for (;; ++n) { if (!m[n].label && m[n].cmd == 0 && !m[n].sc) break; } return n;
}
static bool menu_item_enabled(int cmd) {
    Doc *d = app_doc();
    switch (cmd) {
        case CMD_UNDO: return d && d->u.head > 0;
        case CMD_REDO: return d && d->u.head < d->u.n;
        case CMD_CUT: case CMD_COPY: return d != NULL;
        case CMD_SAVE: case CMD_SAVE_AS: case CMD_CLOSE_TAB: case CMD_FIND: case CMD_REPLACE: case CMD_GOTO_LINE: return d != NULL;
        case CMD_STOP: return G.bld.st == B_COMPILING || G.bld.st == B_RUNNING || term_busy();
        case CMD_RUN: case CMD_BUILD: return G.bld.st != B_COMPILING;
        default: return true;
    }
}

typedef struct { int x, y, w, h, n; } MenuBox;
static MenuBox menu_box(int m) {
    const MI *mi = MENUS[m];
    int n = menu_len(mi), maxw = 120, h = 8;
    cui_font *f = font_ui(12);
    for (int i = 0; i < n; ++i) {
        if (!mi[i].label) { h += 9; continue; }
        int w = cui_text_width(f, mi[i].label) + (mi[i].sc[0] ? cui_text_width(font_ui(11), mi[i].sc) + 44 : 30);
        if (w > maxw) maxw = w;
        h += 24;
    }
    MenuBox b;
    b.n = n; b.w = maxw + 8; b.h = h;
    if (m == MENU_CFG) { b.x = 534; b.y = 56; } else { b.x = MENU_LABEL_X[m] - 10; b.y = MENU_H + 1; }
    if (b.x + b.w > L.W - 4) b.x = L.W - b.w - 4;
    return b;
}
void menu_close(void) { G.open_menu = -1; G.menu_hover = -1; G.dirty_frame = true; }

static int menu_item_at(int m, int mx, int my) {
    MenuBox b = menu_box(m);
    if (!pt_in(mx, my, b.x, b.y, b.w, b.h)) return -1;
    const MI *mi = MENUS[m];
    int y = b.y + 4;
    for (int i = 0; i < b.n; ++i) {
        int ih = mi[i].label ? 24 : 9;
        if (my >= y && my < y + ih) return mi[i].label ? i : -1;
        y += ih;
    }
    return -1;
}

static void draw_menu_dropdown(void) {
    if (G.open_menu < 0) return;
    cui_canvas *c = &G.cv;
    int m = G.open_menu;
    MenuBox b = menu_box(m);
    const MI *mi = MENUS[m];
    cui_round_rect_alpha(c, b.x - 1, b.y + 2, b.w + 2, b.h + 2, 8, 0x000000, 90);          /* soft shadow */
    cui_round_rect(c, b.x - 1, b.y - 1, b.w + 2, b.h + 2, 7, T->border);
    cui_round_rect(c, b.x, b.y, b.w, b.h, 6, T->tip_bg);
    cui_font *f = font_ui(12), *fs = font_ui(11);
    int y = b.y + 4;
    for (int i = 0; i < b.n; ++i) {
        if (!mi[i].label) { cui_fill(c, b.x + 8, y + 4, b.w - 16, 1, T->border); y += 9; continue; }
        bool en = menu_item_enabled(mi[i].cmd);
        if (i == G.menu_hover && en) cui_round_rect(c, b.x + 4, y, b.w - 8, 24, 4, T->accent);
        uint32_t col = !en ? T->faint : (i == G.menu_hover ? T->on_accent : T->text);
        tb(b.x + 14, y + 16, f, col, mi[i].label);
        if (mi[i].sc[0]) tb(b.x + b.w - 14 - cui_text_width(fs, mi[i].sc), y + 16, fs, i == G.menu_hover && en ? cui_mix(T->on_accent, T->accent, 60) : T->dim, mi[i].sc);
        if (m == MENU_CFG && ((mi[i].cmd == CMD_CFG_DEBUG) == !strcmp(G.bld.cfg, "Debug"))) tb(b.x + b.w - 22, y + 16, f, col, "\xE2\x9C\x93");
        y += 24;
    }
    /* recent files under File */
    if (m == 0 && G.nrecent) {
        /* drawn as an extra block below the list */
    }
}

/* ---------------------------------------------------------------- */
/* menu bar + toolbar                                                */
/* ---------------------------------------------------------------- */
typedef struct { const char *label; int x, w, cmd; } TBtn;
static const TBtn TB_BTNS[] = {
    {"New", 8, 44, CMD_NEW}, {"Open", 57, 51, CMD_OPEN}, {"Save", 113, 51, CMD_SAVE},
    {"Undo", 179, 51, CMD_UNDO}, {"Redo", 235, 51, CMD_REDO},
    {"Run", 300, 91, CMD_RUN}, {"Stop", 395, 65, CMD_STOP}, {"Build", 465, 58, CMD_BUILD}, {"Debug", 534, 97, CMD_CFG_TOGGLE},
};
#define TB_N ((int)(sizeof TB_BTNS / sizeof TB_BTNS[0]))
#define TB_BTN_Y 30
#define TB_BTN_H 25
#define TB_SEARCH_X 750
#define TB_SEARCH_W 200

static void draw_menubar(void) {
    cui_canvas *c = &G.cv;
    cui_fill(c, 0, 0, L.W, MENU_H, T->bar);
    cui_fill(c, 0, MENU_H, L.W, 1, T->border);
    cui_font *f = font_ui(12);
    for (int i = 0; i < 7; ++i) {
        int w = cui_text_width(f, MENU_NAMES[i]);
        if (G.open_menu == i) cui_round_rect(c, MENU_LABEL_X[i] - 8, 3, w + 16, 19, 4, T->btn);
        tb(MENU_LABEL_X[i], 17, f, T->text, MENU_NAMES[i]);
    }
    /* right side of the bar: the window title, quiet */
    char t[160];
    Doc *d = app_doc();
    snprintf(t, sizeof t, "%s%s - C-OS Studio", d ? d->name : "C-OS Studio", d && d->dirty ? " *" : "");
    int tw = cui_text_width(f, t);
    if (!G.demo && d) tb(L.W - tw - 14, 17, f, T->faint, t);
}

static bool tb_enabled(int cmd) {
    if (cmd == CMD_STOP) return G.bld.st == B_COMPILING || G.bld.st == B_RUNNING || term_busy();
    if (cmd == CMD_UNDO || cmd == CMD_REDO || cmd == CMD_SAVE) return menu_item_enabled(cmd) || G.demo;
    return true;
}

static void draw_toolbar(void) {
    cui_canvas *c = &G.cv;
    cui_fill(c, 0, TOOL_Y, L.W, TOOL_H, T->bar2);
    cui_fill(c, 0, MAIN_Y - 1, L.W, 1, T->border);
    cui_font *f = font_ui(12), *fb = font_ui_bold(12);
    for (int i = 0; i < TB_N; ++i) {
        const TBtn *b = &TB_BTNS[i];
        bool hover = G.hover_btn == i, en = tb_enabled(b->cmd);
        if (b->cmd == CMD_RUN) {
            uint32_t bg = hover ? cui_mix(T->accent, 0xFFFFFF, 28) : 0x5A8AFB;
            if (G.dark == false) bg = hover ? cui_mix(T->accent, 0xFFFFFF, 28) : T->accent;
            cui_round_rect(c, b->x, TB_BTN_Y, b->w, TB_BTN_H, 5, bg);
            cui_triangle(c, (float)(b->x + 12), (float)(TB_BTN_Y + 7), (float)(b->x + 12), (float)(TB_BTN_Y + 18), (float)(b->x + 20), (float)(TB_BTN_Y + 12.5f), 0xFFFFFF);
            tb(b->x + 25, 47, fb, 0xFFFFFF, "Run");
            tb(b->x + 61, 47, fb, 0xE3ECFF, "F5");
            continue;
        }
        cui_round_rect(c, b->x, TB_BTN_Y, b->w, TB_BTN_H, 5, hover && en ? T->btn_hi : T->btn);
        uint32_t tc = en ? T->text : T->faint;
        if (b->cmd == CMD_STOP) {
            cui_round_rect(c, b->x + 12, TB_BTN_Y + 9, 9, 9, 1, en ? T->err : T->dim);
            tb(b->x + 26, 47, f, tc, "Stop");
        } else if (b->cmd == CMD_CFG_TOGGLE) {
            tb(b->x + 11, 47, f, tc, G.bld.cfg);
            float ax = (float)(b->x + b->w - 16), ay = (float)(TB_BTN_Y + 11);
            cui_triangle(c, ax, ay, ax + 6, ay, ax + 3, ay + 4, T->dim);
        } else tb(b->x + 12, 47, f, tc, b->label);
    }
    cui_fill(c, 174, 34, 1, 17, T->border);
    cui_fill(c, 290, 34, 1, 17, T->border);
    /* search pill */
    cui_round_rect(c, TB_SEARCH_X, TB_BTN_Y, TB_SEARCH_W, TB_BTN_H, 12, T->field);
    cui_round_rect_outline(c, TB_SEARCH_X, TB_BTN_Y, TB_SEARCH_W, TB_BTN_H, 12, 1.0f, T->bar2);
    tb(TB_SEARCH_X + 20, 47, f, T->dim, "Search (Ctrl+P)");
    /* magnifier (the mockup shows a fallback box here) */
    cui_ring(c, (float)(TB_SEARCH_X + 12), (float)(TB_BTN_Y + 11.5f), 3.6f, 1.3f, T->dim);
    cui_line(c, (float)(TB_SEARCH_X + 15), (float)(TB_BTN_Y + 14.5f), (float)(TB_SEARCH_X + 18), (float)(TB_BTN_Y + 17.5f), 1.4f, T->dim);
}

/* ---------------------------------------------------------------- */
/* activity bar                                                      */
/* ---------------------------------------------------------------- */
static void icon_explorer(float cx, float cy, uint32_t col) {
    cui_canvas *c = &G.cv;
    cui_round_rect_outline(c, (int)cx - 6, (int)cy - 7, 13, 14, 2, 1.3f, col);
    cui_line(c, cx - 3, cy - 2.5f, cx + 3, cy - 2.5f, 1.3f, col);
    cui_line(c, cx - 3, cy + 1.5f, cx + 3, cy + 1.5f, 1.3f, col);
}
static void icon_search(float cx, float cy, uint32_t col) {
    cui_canvas *c = &G.cv;
    cui_ring(c, cx - 1.5f, cy - 1.5f, 5.0f, 1.5f, col);
    cui_line(c, cx + 2.3f, cy + 2.3f, cx + 6.5f, cy + 6.5f, 1.8f, col);
}
static void icon_flag(float cx, float cy, uint32_t col) {
    cui_canvas *c = &G.cv;
    cui_line(c, cx - 4, cy - 6, cx - 4, cy + 6, 1.5f, col);
    cui_triangle(c, cx - 3.5f, cy - 6, cx + 4.5f, cy - 3, cx - 3.5f, cy, col);
}
static void icon_gear(float cx, float cy, uint32_t col) {
    cui_canvas *c = &G.cv;
    cui_ring(c, cx, cy, 4.2f, 1.7f, col);
    for (int i = 0; i < 8; ++i) {
        float a = (float)i * 0.785398f, s = 0, co = 0;
        /* no libm sin/cos needed: table of the 8 compass directions */
        static const float SX[8] = { 0, 0.7071f, 1, 0.7071f, 0, -0.7071f, -1, -0.7071f };
        s = SX[i]; co = SX[(i + 2) % 8]; (void)a;
        cui_line(c, cx + co * 5.0f, cy + s * 5.0f, cx + co * 7.0f, cy + s * 7.0f, 1.8f, col);
    }
}
static void draw_activity(void) {
    cui_canvas *c = &G.cv;
    cui_fill(c, 0, MAIN_Y, ACT_W, L.status_y - MAIN_Y, T->act_bg);
    for (int i = 0; i < 4; ++i) {
        float cy = (float)(MAIN_Y + 21 + 44 * i), cx = 20.0f;
        bool sel = (i == G.view && G.sidebar_open && i < 3) || (i == 3 && G.dlg == DLG_SETTINGS);
        uint32_t col = sel ? 0xFFFFFF : (G.hover_btn == 100 + i ? T->text : T->dim);
        if (sel && i < 3) cui_round_rect(c, 0, (int)cy - 16, 3, 32, 1, T->accent);
        switch (i) {
            case 0: icon_explorer(cx, cy, col); break;
            case 1: icon_search(cx, cy, col); break;
            case 2: icon_flag(cx, cy, col);
                    if (G.ndiag) cui_circle(c, cx + 8, cy - 7, 3.2f, T->err);
                    break;
            case 3: icon_gear(cx, cy, col); break;
        }
    }
}

/* ---------------------------------------------------------------- */
/* sidebar                                                           */
/* ---------------------------------------------------------------- */
#define ROW_H 20
#define TREE_TOP 87

static int tree_row_y(int idx) { return TREE_TOP + idx * ROW_H + (idx >= 1 ? 2 : 0) - G.side_scroll; }
static int outline_top(void) {
    if (!G.proj.loaded) return L.status_y + 100;               /* no project: no outline, the empty state has the room */
    int rows = G.proj.loaded ? G.proj.nnodes : 0;
    int end = TREE_TOP + rows * ROW_H + (rows > 1 ? 2 : 0);
    int limit = L.status_y - 150;                       /* the outline always keeps room */
    if (end > limit) end = limit;
    return end + 10;
}

static bool doc_open_for(const char *path) { for (int i = 0; i < G.ndocs; ++i) if (!strcmp(G.docs[i]->path, path)) return true; return false; }

static void draw_folder_arrow(int x, int cy, bool open, uint32_t col) {
    cui_canvas *c = &G.cv;
    if (open) cui_triangle(c, (float)x, (float)cy - 2, (float)x + 7, (float)cy - 2, (float)x + 3.5f, (float)cy + 3, col);
    else cui_triangle(c, (float)x + 1, (float)cy - 4, (float)x + 1, (float)cy + 4, (float)x + 5.5f, (float)cy, col);
}

static void draw_side_header(const char *title, int base) {
    tb_spaced(L.side_x + 13, base, font_ui_bold(11), T->dim, title, 0.7f);
}

static void draw_explorer(void) {
    cui_canvas *c = &G.cv;
    int sx = L.side_x, sw = L.side_w;
    draw_side_header("EXPLORER", 79);
    /* header buttons: new file, refresh */
    { int x = sx + sw - 46;
      cui_round_rect_outline(c, x, 68, 9, 12, 1, 1.2f, T->dim);
      cui_line(c, (float)x + 4.5f, 71, (float)x + 4.5f, 77, 1.2f, T->dim); cui_line(c, (float)x + 1.5f, 74, (float)x + 7.5f, 74, 1.2f, T->dim);
      cui_ring(c, (float)(x + 20), 74, 4.0f, 1.3f, T->dim);
      cui_triangle(c, (float)(x + 22), 68.5f, (float)(x + 26), 72, (float)(x + 21), 73, T->dim); }
    if (!G.proj.loaded) {
        cui_text_center(c, font_ui(12), sx, TREE_TOP, sw, 22, "No project open", T->dim);
        cui_round_rect(c, sx + 20, TREE_TOP + 34, sw - 40, 26, 5, T->btn);
        cui_text_center(c, font_ui(12), sx + 20, TREE_TOP + 34, sw - 40, 26, "Open Folder...", T->text);
        cui_round_rect(c, sx + 20, TREE_TOP + 68, sw - 40, 26, 5, T->btn);
        cui_text_center(c, font_ui(12), sx + 20, TREE_TOP + 68, sw - 40, 26, "New Project...", T->text);
        return;
    }
    Doc *cur = app_doc();
    cui_font *f = font_ui(12), *fb = font_ui_bold(12);
    cui_clip(c, sx, 84, sw, outline_top() - 92);
    for (int i = 0; i < G.proj.nnodes; ++i) {
        Node *n = &G.proj.nodes[i];
        int y = tree_row_y(i);
        if (y + ROW_H < 84 || y > L.status_y) continue;
        bool active = cur && !n->is_dir && !strcmp(n->path, cur->path);
        bool hover = G.hover_btn == 1000 + i;
        if (active) cui_fill(c, sx, y, sw, ROW_H, T->row_sel);
        else if (hover) cui_fill(c, sx, y, sw, ROW_H, cui_mix(T->side, T->btn, 130));
        int x = sx + 12 + 14 * n->depth, base = y + 14;
        if (n->depth == 0) {
            draw_folder_arrow(x, y + 10, true, T->dim);
            tb(x + 14, base, fb, 0xF1F3F8, n->name);
        } else if (n->is_dir) {
            draw_folder_arrow(x, y + 10, n->open, T->folder);
            tb(x + 14, base, f, T->folder, n->name);
        } else {
            bool opened = doc_open_for(n->path);
            tb(x, base, f, active ? 0xFFFFFF : (opened ? T->accent : T->text), n->name);
        }
    }
    cui_unclip(c);
}

static void draw_outline(void) {
    cui_canvas *c = &G.cv;
    int sx = L.side_x, sw = L.side_w, top = outline_top();
    if (!G.proj.loaded) return;
    cui_fill(c, sx, top - 1, sw, 1, T->border);
    tb_spaced(sx + 12, top + 22, font_ui_bold(11), T->dim, "OUTLINE", 0.7f);
    Doc *d = app_doc();
    if (!d) return;
    if (G.syms_doc != G.cur || G.syms_ver != d->b.version) {
        G.nsyms = outline_scan(d, G.syms, 128);
        G.syms_doc = G.cur; G.syms_ver = d->b.version;
    }
    cui_font *f = font_ui(12), *fk = font_code_px(9, false);
    int y0 = top + 28;
    cui_clip(c, sx, top, sw, L.status_y - top);
    for (int i = 0; i < G.nsyms; ++i) {
        int y = y0 + i * 20;
        if (y + 20 > L.status_y) break;
        if (G.hover_btn == 2000 + i) cui_fill(c, sx, y, sw, 20, cui_mix(T->side, T->btn, 130));
        uint32_t kc = !strcmp(G.syms[i].kind, "fn") ? T->type : T->pp;
        tb(sx + 16, y + 14, fk, kc, G.syms[i].kind);
        tb(sx + 52, y + 14, f, T->text, G.syms[i].name);
    }
    if (!G.nsyms) tb(sx + 16, y0 + 14, font_ui(11), T->faint, "No symbols");
    cui_unclip(c);
}

static void draw_search_view(void) {
    cui_canvas *c = &G.cv;
    int sx = L.side_x, sw = L.side_w;
    draw_side_header("SEARCH", 79);
    ui_field(sx + 10, 90, sw - 20, 24, G.psearch, G.psearch_focus, "Find in project");
    cui_font *f = font_ui(11), *fc = font_code_px(11, false);
    char sum[48]; snprintf(sum, sizeof sum, "%d result%s", G.nhits, G.nhits == 1 ? "" : "s");
    if (G.psearch[0]) tb(sx + 12, 134, f, T->dim, sum);
    cui_clip(c, sx, 142, sw, L.status_y - 142);
    for (int i = 0; i < G.nhits; ++i) {
        int y = 146 + i * 34 - G.side_scroll;
        if (y + 34 < 142 || y > L.status_y) continue;
        if (G.hover_btn == 3000 + i) cui_fill(c, sx, y, sw, 34, cui_mix(T->side, T->btn, 130));
        char loc[300]; snprintf(loc, sizeof loc, "%s:%d", path_base(G.hits[i].path), G.hits[i].line);
        tb(sx + 12, y + 13, f, T->accent, loc);
        cui_text_fit(c, fc, sx + 12, y + 17, sw - 20, G.hits[i].text, T->text);
    }
    cui_unclip(c);
}

static void draw_problems_view(void) {
    cui_canvas *c = &G.cv;
    int sx = L.side_x, sw = L.side_w;
    draw_side_header("PROBLEMS", 79);
    cui_font *f = font_ui(12), *fs = font_ui(11);
    if (!G.ndiag) { tb(sx + 14, 104, fs, T->dim, "No problems detected"); return; }
    cui_clip(c, sx, 88, sw, L.status_y - 88);
    for (int i = 0; i < G.ndiag; ++i) {
        int y = 90 + i * 40 - G.side_scroll;
        if (y + 40 < 88 || y > L.status_y) continue;
        if (G.hover_btn == 4000 + i) cui_fill(c, sx, y, sw, 40, cui_mix(T->side, T->btn, 130));
        cui_circle(c, (float)(sx + 18), (float)(y + 14), 4.0f, G.diags[i].sev == 0 ? T->err : T->warn);
        cui_text_fit(c, f, sx + 30, y + 6, sw - 38, G.diags[i].msg, T->text);
        char loc[300]; snprintf(loc, sizeof loc, "%s:%d", path_base(G.diags[i].file), G.diags[i].line);
        tb(sx + 30, y + 33, fs, T->dim, loc);
    }
    cui_unclip(c);
}

static void draw_sidebar(void) {
    if (!L.side_w) return;
    cui_canvas *c = &G.cv;
    cui_fill(c, L.side_x, MAIN_Y, L.side_w, L.status_y - MAIN_Y, T->side);
    switch (G.view) {
        case VIEW_SEARCH: draw_search_view(); break;
        case VIEW_PROBLEMS: draw_problems_view(); break;
        default: draw_explorer(); draw_outline(); break;
    }
}

/* ---------------------------------------------------------------- */
/* tabs                                                              */
/* ---------------------------------------------------------------- */
static int tab_w(int i) {
    int w = cui_text_width(font_ui(12), G.docs[i]->name) + 96;
    return w < 96 ? 96 : w > 220 ? 220 : w;
}
static bool doc_has_error(Doc *d) {
    for (int i = 0; i < G.ndiag; ++i) if (G.diags[i].sev == 0 && !strcmp(path_base(G.diags[i].path), path_base(d->path))) return true;
    return false;
}
static void draw_tabs(void) {
    cui_canvas *c = &G.cv;
    cui_fill(c, L.main_x, L.tab_y, L.main_w, TAB_H, T->bar);
    cui_font *f = font_ui(12);
    int x = L.main_x;
    for (int i = 0; i < G.ndocs; ++i) {
        Doc *d = G.docs[i];
        int w = tab_w(i);
        bool act = i == G.cur, hov = G.hover_tab == i;
        if (act) {
            cui_fill(c, x, L.tab_y, w, TAB_H, T->tab_active);
            cui_fill(c, x, L.tab_y, w, 2, T->accent);
        } else if (hov) cui_fill(c, x, L.tab_y, w, TAB_H, T->bar2);
        tb(x + 13, L.tab_y + 20, f, act ? T->text : T->dim, d->name);
        int gx = x + w - 28, gy = L.tab_y + 16;
        if (doc_has_error(d)) cui_circle(c, (float)gx + 4.5f, (float)gy - 0.5f, 4.4f, 0xFF6975);
        else if (d->dirty) cui_circle(c, (float)gx + 4.5f, (float)gy - 0.5f, 4.0f, act ? T->text : T->dim);
        else if (act || hov) {
            uint32_t xc = G.hover_btn == 5000 + i ? T->text : T->dim;
            cui_line(c, (float)gx + 1, (float)gy - 4, (float)gx + 7, (float)gy + 2, 1.4f, xc);
            cui_line(c, (float)gx + 7, (float)gy - 4, (float)gx + 1, (float)gy + 2, 1.4f, xc);
        } else {
            cui_line(c, (float)gx + 1, (float)gy - 4, (float)gx + 7, (float)gy + 2, 1.4f, T->faint);
            cui_line(c, (float)gx + 7, (float)gy - 4, (float)gx + 1, (float)gy + 2, 1.4f, T->faint);
        }
        x += w;
        if (x > L.main_x + L.main_w) break;
    }
}
static int tab_at(int mx, int my, bool *on_close) {
    if (my < L.tab_y || my >= L.tab_y + TAB_H) return -1;
    int x = L.main_x;
    for (int i = 0; i < G.ndocs; ++i) {
        int w = tab_w(i);
        if (mx >= x && mx < x + w) { if (on_close) *on_close = mx >= x + w - 34 && mx < x + w - 14; return i; }
        x += w;
    }
    return -1;
}

/* ---------------------------------------------------------------- */
/* bottom panel                                                      */
/* ---------------------------------------------------------------- */
#define PANEL_LINE_H 17

static int out_line_count(void) { int n = 0; for (size_t i = 0; i < G.out_len; ++i) if (G.out[i] == '\n') ++n; return n; }
/* copy line `idx` into buf, return its length (marker chars stripped later by the caller) */
static int out_line(int idx, char *buf, size_t cap) {
    size_t i = 0; int l = 0;
    while (i < G.out_len && l < idx) { if (G.out[i] == '\n') ++l; ++i; }
    size_t k = 0;
    while (i < G.out_len && G.out[i] != '\n' && k + 1 < cap) buf[k++] = G.out[i++];
    buf[k] = 0;
    return (int)k;
}

static void draw_panel(void) {
    if (!L.panel_h) return;
    cui_canvas *c = &G.cv;
    int x = L.main_x, w = L.main_w, y = L.panel_y;
    cui_fill(c, x, y, w, 1, T->border);
    cui_fill(c, x, y + 1, w, PANEL_HDR_H, T->bar2);
    cui_fill(c, x, y + 1 + PANEL_HDR_H, w, L.panel_h - PANEL_HDR_H - 1, T->panel_bg);
    cui_font *fb = font_ui_bold(11), *f = font_ui(11);
    static const char *const TABS[] = { "OUTPUT", "PROBLEMS", "TERMINAL" };
    static const int TX[] = { 10, 87, 204 };                   /* relative to the panel's left edge (236 + n) */
    int base = y + 20;
    for (int i = 0; i < 3; ++i) {
        bool act = G.panel_tab == i;
        int tx = x + TX[i];
        int tw = tb_spaced(tx, base, fb, act ? 0xFFFFFF : T->dim, TABS[i], 0.5f);
        if (i == 1 && G.ndiag) { char n[8]; snprintf(n, sizeof n, "%d", G.ndiag); tb(tx + tw + 8, base, f, T->dim, n); }
        if (act) cui_fill(c, tx - 2, y + PANEL_HDR_H - 2, tw + 4, 2, T->accent);
    }
    /* right-hand buttons */
    tb(x + w - 122, base, f, T->dim, "Clear");
    tb(x + w - 79, base, f, T->dim, "Copy");
    { int cx = x + w - 36; cui_line(c, (float)cx, (float)base - 8, (float)cx + 6, (float)base - 2, 1.3f, T->dim); cui_line(c, (float)cx + 6, (float)base - 8, (float)cx, (float)base - 2, 1.3f, T->dim); }

    int cy = y + 1 + PANEL_HDR_H + 5, ch = L.panel_h - PANEL_HDR_H - 6;
    cui_clip(c, x, y + 1 + PANEL_HDR_H, w, L.panel_h - PANEL_HDR_H - 1);
    cui_font *fm = font_code_px(13, false);
    if (G.panel_tab == PANEL_OUTPUT) {
        int nl = out_line_count(), vis = ch / PANEL_LINE_H;
        if (G.out_follow) G.out_scroll = nl > vis ? nl - vis : 0;
        for (int i = 0; i < vis + 1 && G.out_scroll + i < nl; ++i) {
            char line[400]; out_line(G.out_scroll + i, line, sizeof line);
            int ly = y + 1 + PANEL_HDR_H + 5 + i * PANEL_LINE_H - 5 + 0;
            ly = y + 1 + PANEL_HDR_H + 5 + i * PANEL_LINE_H - 5;
            const char *s = line; bool bad = false;
            if (s[0] == '!') { bad = true; ++s; }
            Diag dg; int k = diag_parse_line(s, &dg);
            if (k == 1) bad = true;
            uint32_t col = bad ? T->err : (k == 2 ? T->warn : T->dim);
            if (!bad && (s[0] != '$') && (s[0] != '[') && k == 0 && s[0]) col = T->text;
            if (!strncmp(s, "[last run]", 10) || !strncmp(s, "[stopped]", 9) || s[0] == '$') col = T->dim;
            if (k == 1) cui_fill(c, x, ly, w, PANEL_LINE_H, T->errline);
            else if (G.hover_btn == 6000 + i && k) cui_fill(c, x, ly, w, PANEL_LINE_H, cui_mix(T->panel_bg, T->btn, 90));
            cui_text(c, fm, x + 13, ly + 1, s, col);
        }
    } else if (G.panel_tab == PANEL_PROBLEMS) {
        cui_font *fp = font_ui(12);
        if (!G.ndiag) tb(x + 13, cy + 14, fp, T->dim, "No problems have been detected.");
        for (int i = 0; i < G.ndiag; ++i) {
            int ly = cy + i * 22 - G.prob_scroll;
            if (ly + 22 < cy || ly > y + L.panel_h) continue;
            if (i == G.prob_sel) cui_fill(c, x, ly, w, 22, T->row_sel);
            else if (G.hover_btn == 7000 + i) cui_fill(c, x, ly, w, 22, cui_mix(T->panel_bg, T->btn, 90));
            cui_circle(c, (float)(x + 20), (float)(ly + 11), 4.0f, G.diags[i].sev == 0 ? T->err : T->warn);
            cui_text_fit(c, fp, x + 34, ly + 4, w - 200, G.diags[i].msg, T->text);
            char loc[300]; snprintf(loc, sizeof loc, "%s:%d", path_base(G.diags[i].file), G.diags[i].rep_line);
            tb(x + w - 150, ly + 15, font_ui(11), T->dim, loc);
        }
    } else {
        /* TERMINAL: scrollback above, the prompt line pinned at the bottom */
        int inp_h = 26, vis = (ch - inp_h) / PANEL_LINE_H;
        int nl = term_line_count();
        if (G.term_follow) G.term_scroll = nl > vis ? nl - vis : 0;
        if (G.term_scroll > (nl > vis ? nl - vis : 0)) G.term_scroll = nl > vis ? nl - vis : 0;
        for (int i = 0; i < vis + 1 && G.term_scroll + i < nl; ++i) {
            char line[400]; term_line_at(G.term_scroll + i, line, sizeof line);
            int ly = y + 1 + PANEL_HDR_H + 5 + i * PANEL_LINE_H - 5;
            Diag dg; int k = diag_parse_line(line, &dg);
            uint32_t col = k == 1 ? T->err : k == 2 ? T->warn : T->text;
            if (line[0] == '/' && strstr(line, "$ ")) col = T->accent;               /* an echoed command */
            else if (!strncmp(line, "make: ***", 9) || !strncmp(line, "[stopped]", 9)) col = T->err;
            else if (!strncmp(line, "make:", 5) || !strncmp(line, "  ->", 4) || !strncmp(line, "[", 1)) col = T->dim;
            if (k == 1) cui_fill(c, x, ly, w, PANEL_LINE_H, T->errline);
            cui_text(c, fm, x + 13, ly + 1, line, col);
        }
        int py = y + L.panel_h - inp_h;
        cui_fill(c, x, py - 1, w, 1, T->border);
        char pr[300]; term_prompt(pr, sizeof pr);
        int pw = cui_text_width(fm, pr);
        cui_text_fit(c, fm, x + 13, py + 5, w / 2, pr, T->accent);
        if (pw > w / 2) pw = w / 2;
        cui_text_fit(c, fm, x + 13 + pw + 8, py + 5, w - pw - 40, G.term_line, T->text);
        if (G.job.active) cui_text(c, font_ui(11), x + w - 130, py + 7, "running - Ctrl+C", T->warn);
        else if (G.term_focus && ((cos_time_ms() - G.caret_t) / 530) % 2 == 0) cui_fill(c, x + 13 + pw + 8 + cui_text_width(fm, G.term_line), py + 4, 2, 16, T->accent);
        if (!G.term_line[0] && !G.term_focus && !G.job.active) cui_text(c, fm, x + 13 + pw + 8, py + 5, "click here and type a command (help)", T->faint);
    }
    cui_unclip(c);
}

/* ---------------------------------------------------------------- */
/* status bar                                                        */
/* ---------------------------------------------------------------- */
static void draw_status(void) {
    cui_canvas *c = &G.cv;
    int y = L.status_y;
    cui_fill(c, 0, y, L.W, STATUS_H, T->accent);
    cui_font *f = font_ui(11), *fb = font_ui_bold(11);
    int base = y + 17;
    char st[64];
    build_state_text(st, sizeof st);
    uint32_t dotc = G.bld.st == B_FAILED ? 0xFFFFFF : (G.bld.st == B_RUNNING || G.bld.st == B_COMPILING ? 0xFFE9A8 : 0xFFFFFF);
    cui_circle(c, 13, (float)(y + 12), 3.2f, dotc);
    tb(22, base, fb, 0xFFFFFF, st);
    uint32_t dim = cui_mix(T->accent, 0xFFFFFF, 200);
    bool msg = G.status_msg[0] && cos_time_ms() - G.status_t < 4500;
    if (msg) tb(151, base, f, 0xFFFFFF, G.status_msg);
    else if (G.proj.loaded) tb(151, base, f, dim, G.proj.name);
    /* right side, laid out from the right edge */
    Doc *d = app_doc();
    char ln[48] = "";
    if (d) { int l, co; doc_line_col(d, d->caret, &l, &co); snprintf(ln, sizeof ln, "Ln %d, Col %d", l, co); if (doc_has_sel(d)) { size_t a, b; doc_sel_range(d, &a, &b); size_t n = b - a; snprintf(ln + strlen(ln), sizeof ln - strlen(ln), " (%zu sel)", n); } }
    const char *items[5] = { ln, "UTF-8", d && d->crlf ? "CRLF" : "LF", "C (TinyCC)", G.bld.cfg };
    int x = L.W - 18;
    int xs[5];
    for (int i = 4; i >= 0; --i) { int w = cui_text_width(f, items[i]); x -= w; xs[i] = x; x -= 28; }
    if (G.demo) { xs[0] = 631; xs[1] = 729; xs[2] = 785; xs[3] = 820; xs[4] = 909; }
    for (int i = 0; i < 5; ++i) if (items[i][0]) tb(xs[i], base, f, i == 0 ? 0xFFFFFF : dim, items[i]);
}

/* ---------------------------------------------------------------- */
/* frame                                                             */
/* ---------------------------------------------------------------- */
void ui_draw_all(void) {
    layout_compute();
    cui_canvas *c = &G.cv;
    cui_unclip(c);
    cui_fill(c, 0, 0, L.W, L.H, T->bg);
    draw_activity();
    draw_sidebar();
    draw_tabs();
    ed_draw();
    draw_panel();
    draw_status();
    draw_toolbar();
    draw_menubar();
    ed_draw_tooltip();
    draw_menu_dropdown();
    ctx_draw();
    dlg_draw();
}

/* ---------------------------------------------------------------- */
/* mouse                                                             */
/* ---------------------------------------------------------------- */
static void set_hover(int v) { if (G.hover_btn != v) { G.hover_btn = v; G.dirty_frame = true; } }

static int menubar_at(int mx, int my) {
    if (my >= MENU_H) return -1;
    cui_font *f = font_ui(12);
    for (int i = 0; i < 7; ++i) if (mx >= MENU_LABEL_X[i] - 8 && mx < MENU_LABEL_X[i] + cui_text_width(f, MENU_NAMES[i]) + 8) return i;
    return -1;
}
static int toolbar_at(int mx, int my) {
    if (my < TB_BTN_Y || my >= TB_BTN_Y + TB_BTN_H) return -1;
    for (int i = 0; i < TB_N; ++i) if (mx >= TB_BTNS[i].x && mx < TB_BTNS[i].x + TB_BTNS[i].w) return i;
    if (mx >= TB_SEARCH_X && mx < TB_SEARCH_X + TB_SEARCH_W) return 50;
    return -1;
}

static void panel_goto_line(int line_idx) {
    char buf[400]; out_line(G.out_scroll + line_idx, buf, sizeof buf);
    const char *s = buf[0] == '!' ? buf + 1 : buf;
    Diag dg;
    if (!diag_parse_line(s, &dg)) return;
    diag_refine(&dg);
    app_open_file(dg.path, dg.line);
}

void ui_mouse_move(int x, int y) {
    G.mx = x; G.my = y;
    if (G.dlg) { dlg_mouse(x, y, 0, false); return; }
    if (G.open_menu >= 0) {
        int m = menubar_at(x, y);
        if (m >= 0 && m != G.open_menu && G.open_menu < 7) { G.open_menu = m; G.dirty_frame = true; }
        int it = menu_item_at(G.open_menu, x, y);
        if (it != G.menu_hover) { G.menu_hover = it; G.dirty_frame = true; }
        return;
    }
    if (G.ctx_open) { ctx_mouse(x, y, false); return; }
    int hv = -1;
    int t = toolbar_at(x, y);
    if (t >= 0 && t < TB_N) hv = t;
    else if (x < ACT_W && y >= MAIN_Y && y < L.status_y) { int i = (y - MAIN_Y) / 44; if (i < 4) hv = 100 + i; }
    else if (L.side_w && x >= L.side_x && x < L.side_x + L.side_w && y >= MAIN_Y && y < L.status_y) {
        if (G.view == VIEW_EXPLORER) {
            int top = outline_top();
            if (y >= top + 28) { int i = (y - top - 28) / 20; if (i < G.nsyms) hv = 2000 + i; }
            else for (int i = 0; i < G.proj.nnodes; ++i) if (y >= tree_row_y(i) && y < tree_row_y(i) + ROW_H) { hv = 1000 + i; break; }
        } else if (G.view == VIEW_SEARCH) { int i = (y - 146 + G.side_scroll) / 34; if (y >= 146 && i < G.nhits) hv = 3000 + i; }
        else if (G.view == VIEW_PROBLEMS) { int i = (y - 90 + G.side_scroll) / 40; if (y >= 90 && i < G.ndiag) hv = 4000 + i; }
    } else if (L.panel_h && y >= L.panel_y + 1 + PANEL_HDR_H && y < L.status_y && x >= L.main_x) {
        if (G.panel_tab == PANEL_OUTPUT) { int i = (y - (L.panel_y + PANEL_HDR_H + 1)) / PANEL_LINE_H; hv = 6000 + i; }
        else if (G.panel_tab == PANEL_PROBLEMS) { int i = (y - (L.panel_y + PANEL_HDR_H + 6) + G.prob_scroll) / 22; if (i >= 0 && i < G.ndiag) hv = 7000 + i; }
    }
    bool oc = false;
    int ti = tab_at(x, y, &oc);
    if (ti >= 0 && oc) hv = 5000 + ti;
    if (G.hover_tab != ti) { G.hover_tab = ti; G.dirty_frame = true; }
    set_hover(hv);
    ed_mouse_move(x, y);
}

void ui_mouse_drag(int x, int y) {
    G.mx = x; G.my = y;
    if (G.drag_kind == 2) {
        int w = x - ACT_W;
        if (w < SIDE_W_MIN) w = SIDE_W_MIN;
        if (w > SIDE_W_MAX) w = SIDE_W_MAX;
        G.side_w = w; G.dirty_frame = true;
    } else if (G.drag_kind == 3) {
        int h = L.status_y - y;
        if (h < PANEL_H_MIN) h = PANEL_H_MIN;
        if (h > PANEL_H_MAX) h = PANEL_H_MAX;
        G.panel_h = h; G.dirty_frame = true;
    } else ed_mouse_drag(x, y);
}
void ui_mouse_up(int x, int y) {
    (void)x; (void)y;
    if (G.drag_kind == 2 || G.drag_kind == 3) app_settings_save();
    G.drag_kind = 0;
}

void ui_wheel(int x, int y, int delta) {
    if (G.dlg) return;
    int step = delta > 0 ? -1 : 1;
    if (L.side_w && x >= L.side_x && x < L.main_x) { G.side_scroll += step * 40; if (G.side_scroll < 0) G.side_scroll = 0; G.dirty_frame = true; return; }
    if (L.panel_h && y >= L.panel_y) {
        if (G.panel_tab == PANEL_OUTPUT) { G.out_follow = false; G.out_scroll += step * 3; if (G.out_scroll < 0) G.out_scroll = 0; int nl = out_line_count(); if (G.out_scroll > nl) G.out_scroll = nl; }
        else if (G.panel_tab == PANEL_TERMINAL) { G.term_follow = false; G.term_scroll += step * 3; if (G.term_scroll < 0) G.term_scroll = 0; }
        else G.prob_scroll = G.prob_scroll + step * 44 < 0 ? 0 : G.prob_scroll + step * 44;
        G.dirty_frame = true; return;
    }
    ed_scroll_by(step * (int)(ed_line_h() * 3), 0);
}

bool ui_mouse_down(int x, int y, uint8_t button, uint8_t mods) {
    G.mx = x; G.my = y;
    if (G.dlg) return dlg_mouse(x, y, button, true);
    if (G.ctx_open) { bool used = ctx_mouse(x, y, true); return used; }
    G.term_focus = false; G.psearch_focus = false;
    /* an open menu: pick an item or dismiss */
    if (G.open_menu >= 0) {
        int m = menubar_at(x, y);
        if (m >= 0) { G.open_menu = G.open_menu == m ? -1 : m; G.menu_hover = -1; G.dirty_frame = true; return true; }
        int it = menu_item_at(G.open_menu, x, y);
        if (it >= 0) {
            const MI *mi = MENUS[G.open_menu];
            int cmd = mi[it].cmd;
            if (menu_item_enabled(cmd)) { menu_close(); app_do(cmd); }
            return true;
        }
        menu_close();
        return true;
    }
    G.comp_open = false;
    if (y < MENU_H) { int m = menubar_at(x, y); if (m >= 0) { G.open_menu = m; G.menu_hover = -1; G.dirty_frame = true; } return true; }
    /* toolbar */
    if (y >= TOOL_Y && y < MAIN_Y) {
        int t = toolbar_at(x, y);
        if (t == 50) { app_do(CMD_GOTO_FILE); return true; }
        if (t >= 0 && t < TB_N) {
            int cmd = TB_BTNS[t].cmd;
            if (cmd == CMD_CFG_TOGGLE) { G.open_menu = MENU_CFG; G.menu_hover = -1; G.dirty_frame = true; }
            else if (tb_enabled(cmd)) app_do(cmd);
        }
        return true;
    }
    /* activity bar */
    if (x < ACT_W && y >= MAIN_Y && y < L.status_y) {
        int i = (y - MAIN_Y) / 44;
        if (i == 3) app_do(CMD_SETTINGS);
        else if (i < 3) {
            if (G.view == i && G.sidebar_open) G.sidebar_open = false;
            else { G.view = i; G.sidebar_open = true; G.side_scroll = 0; if (i == VIEW_SEARCH) G.psearch_focus = true; }
            G.dirty_frame = true;
        }
        return true;
    }
    /* splitters */
    if (L.side_w && x >= L.main_x - 3 && x <= L.main_x + 1 && y >= MAIN_Y && y < L.status_y) { G.drag_kind = 2; return true; }
    if (L.panel_h && y >= L.panel_y - 3 && y <= L.panel_y + 1 && x >= L.main_x) { G.drag_kind = 3; return true; }
    /* status bar */
    if (y >= L.status_y) {
        if (x > L.W - 120) { app_do(CMD_CFG_TOGGLE); }
        else if (x > L.W - 320 && x < L.W - 190) app_do(CMD_GOTO_LINE);
        else if (x < 140) { G.panel_open = true; G.panel_tab = PANEL_OUTPUT; G.dirty_frame = true; }
        return true;
    }
    /* sidebar */
    if (L.side_w && x >= L.side_x && x < L.side_x + L.side_w) {
        if (G.view == VIEW_EXPLORER) {
            if (!G.proj.loaded) {
                if (pt_in(x, y, L.side_x + 20, TREE_TOP + 34, L.side_w - 40, 26)) app_do(CMD_OPEN_FOLDER);
                else if (pt_in(x, y, L.side_x + 20, TREE_TOP + 68, L.side_w - 40, 26)) app_do(CMD_NEW_PROJECT);
                return true;
            }
            if (y >= 66 && y < 84 && x >= L.side_x + L.side_w - 48) { if (x < L.side_x + L.side_w - 28) app_do(CMD_NEW_FILE_HERE); else app_do(CMD_REFRESH); return true; }
            int top = outline_top();
            if (y >= top + 28) {
                int i = (y - top - 28) / 20;
                if (i >= 0 && i < G.nsyms && app_doc()) { Doc *d = app_doc(); doc_goto_line(d, G.syms[i].line, 1); d->center_req = true; ed_center_caret(d); G.dirty_frame = true; }
                return true;
            }
            for (int i = 0; i < G.proj.nnodes; ++i) {
                if (y >= tree_row_y(i) && y < tree_row_y(i) + ROW_H) {
                    if (button == COS_MOUSE_BTN_RIGHT) { ctx_open_at(x, y, i); return true; }
                    if (G.proj.nodes[i].is_dir) { project_toggle(&G.proj, i); G.dirty_frame = true; }
                    else app_open_file(G.proj.nodes[i].path, 0);
                    return true;
                }
            }
        } else if (G.view == VIEW_SEARCH) {
            if (pt_in(x, y, L.side_x + 10, 90, L.side_w - 20, 24)) { G.psearch_focus = true; G.caret_t = cos_time_ms(); }
            else { int i = (y - 146 + G.side_scroll) / 34; if (y >= 146 && i >= 0 && i < G.nhits) app_open_file(G.hits[i].path, G.hits[i].line); }
        } else if (G.view == VIEW_PROBLEMS) {
            int i = (y - 90 + G.side_scroll) / 40;
            if (y >= 90 && i >= 0 && i < G.ndiag) app_open_file(G.diags[i].path, G.diags[i].line);
        }
        G.dirty_frame = true;
        return true;
    }
    /* tabs */
    { bool oc = false; int ti = tab_at(x, y, &oc);
      if (ti >= 0) {
          if (button == COS_MOUSE_BTN_MIDDLE || (oc && button == COS_MOUSE_BTN_LEFT)) app_close_tab(ti, false);
          else { G.cur = ti; Doc *d = G.docs[ti]; ed_ensure_caret_visible(d); }
          G.dirty_frame = true;
          return true;
      }
      if (y >= L.tab_y && y < L.tab_y + TAB_H) return true; }
    /* panel */
    if (L.panel_h && y >= L.panel_y && x >= L.main_x) {
        int hy = L.panel_y + 1;
        if (y < hy + PANEL_HDR_H) {
            int rx = x - L.main_x;
            if (rx >= 4 && rx < 80) G.panel_tab = PANEL_OUTPUT;
            else if (rx >= 80 && rx < 196) G.panel_tab = PANEL_PROBLEMS;
            else if (rx >= 196 && rx < 290) { G.panel_tab = PANEL_TERMINAL; G.term_focus = true; G.caret_t = cos_time_ms(); }
            else if (x >= L.W - 132 && x < L.W - 92) app_do(CMD_PANEL_CLEAR);
            else if (x >= L.W - 90 && x < L.W - 50) app_do(CMD_PANEL_COPY);
            else if (x >= L.W - 46) { G.panel_open = false; }
            G.dirty_frame = true;
            return true;
        }
        if (G.panel_tab == PANEL_OUTPUT) { int i = (y - (hy + PANEL_HDR_H)) / PANEL_LINE_H; panel_goto_line(i); }
        else if (G.panel_tab == PANEL_PROBLEMS) { int i = (y - (hy + PANEL_HDR_H + 5) + G.prob_scroll) / 22; if (i >= 0 && i < G.ndiag) { G.prob_sel = i; app_open_file(G.diags[i].path, G.diags[i].line); } }
        else { G.term_focus = true; G.caret_t = cos_time_ms(); }
        G.dirty_frame = true;
        return true;
    }
    /* editor */
    return ed_mouse_down(x, y, button, mods);
}

/* ---------------------------------------------------------------- */
/* keys owned by chrome text fields (Search view, Terminal)          */
/* ---------------------------------------------------------------- */
static void edit_line(char *buf, size_t cap, const cos_win_event_t *ev) {
    size_t n = strlen(buf);
    if (ev->special == COS_KEY_BACKSPACE) { while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) --n; if (n) buf[n - 1] = 0; }
    else if (ev->special == COS_KEY_NONE && ev->ascii >= 32 && !(ev->mods & COS_MOD_CTRL) && n + 1 < cap) { buf[n] = ev->ascii; buf[n + 1] = 0; }
}

bool ui_key(const cos_win_event_t *ev) {
    if (G.psearch_focus) {
        if (ev->special == COS_KEY_ESC) { G.psearch_focus = false; G.dirty_frame = true; return true; }
        if (ev->special == COS_KEY_ENTER) { search_project(G.psearch, true); G.dirty_frame = true; return true; }
        if (ev->special == COS_KEY_NONE || ev->special == COS_KEY_BACKSPACE) {
            if (ev->mods & COS_MOD_CTRL) return false;
            edit_line(G.psearch, sizeof G.psearch, ev);
            G.caret_t = cos_time_ms(); G.dirty_frame = true; return true;
        }
    }
    if (G.term_focus && G.panel_open && G.panel_tab == PANEL_TERMINAL) {
        bool ctrl = (ev->mods & COS_MOD_CTRL) != 0;
        /* Ctrl+C / L / U belong to the terminal; other Ctrl shortcuts (save, find...) stay global */
        if (ctrl && ev->special == COS_KEY_NONE) { char c = ev->ascii | 32; if (c != 'c' && c != 'l' && c != 'u') return false; }
        if (ev->special >= COS_KEY_F1 && ev->special <= COS_KEY_F12) return false;
        if (ev->mods & COS_MOD_ALT) return false;
        term_key(ev);
        return true;
    }
    return false;
}
