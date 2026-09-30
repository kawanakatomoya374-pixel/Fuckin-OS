/* ui.c - drawing and hit-testing for Files. Every clickable thing has a *_hit()
 * function that shares its geometry with the code that draws it, so what is
 * painted and what responds can not drift apart. */
#include "files.h"

#define C (&G.cv)

/* ---------------------------------------------------------------- */
/* small helpers                                                     */
/* ---------------------------------------------------------------- */
static int ty(cui_font *f, int y, int h) { return y + (h - cui_font_height(f)) / 2; }
static int txt(int x, int y, int h, cui_font *f, uint32_t col, const char *s) { return cui_text(C, f, x, ty(f, y, h), s, col); }
static void txtfit(int x, int y, int h, int w, cui_font *f, uint32_t col, const char *s) { cui_text_fit(C, f, x, ty(f, y, h), w, s, col); }
static bool inr(R r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static uint32_t kind_color(int k) {
    switch (k) {
        case K_FOLDER: return 0xF5A623; case K_IMAGE: return 0x34A853; case K_AUDIO: return 0xEA4335; case K_VIDEO: return 0xFBBC04;
        case K_CODE: return 0x4285F4; case K_ARCHIVE: return 0x9E3AB5; case K_EXEC: return 0x3B4A6B; case K_TEXT: return 0x7B8794;
        case K_DOC: return 0x1A73E8; default: return 0x9AA3B2;
    }
}

R tree_rect(void)   { R r = { 0, BODY_Y, G.tree_vis ? TREE_W : 0, WIN_H - STATUS_H - BODY_Y }; return r; }
R prev_rect(void)   { int w = G.preview ? PREV_W : 0; R r = { WIN_W - w, BODY_Y, w, WIN_H - STATUS_H - BODY_Y }; return r; }
R center_rect(void) { R t = tree_rect(), p = prev_rect(); R r = { t.w, BODY_Y, WIN_W - t.w - p.w, WIN_H - STATUS_H - BODY_Y }; return r; }
R content_rect(void) { R c = center_rect(); if (G.view == V_DETAILS) { c.y += HDR_H; c.h -= HDR_H; } return c; }

/* grid geometry for the non-details views */
static void cell_size(int *cw, int *ch) {
    switch (G.view) { case V_LIST: *cw = 232; *ch = 28; break; case V_ICONS: *cw = 108; *ch = 100; break; default: *cw = 152; *ch = 138; break; }
}
static int grid_cols(void) { int cw, ch; cell_size(&cw, &ch); R c = content_rect(); int n = (c.w - 12) / cw; return n < 1 ? 1 : n; }

int nav_cols(void) { return G.view == V_DETAILS ? 1 : grid_cols(); }

int content_height(void) {
    if (G.view == V_DETAILS) return G.nvis * ROW_H + 4;
    int cw, ch; cell_size(&cw, &ch);
    int cols = grid_cols(), rows = (G.nvis + cols - 1) / cols;
    return rows * ch + 16;
}
bool item_rect(int vi, R *out) {
    if (vi < 0 || vi >= G.nvis) return false;
    R c = content_rect();
    if (G.view == V_DETAILS) { out->x = c.x; out->y = c.y + vi * ROW_H - G.scroll; out->w = c.w; out->h = ROW_H; return true; }
    int cw, ch; cell_size(&cw, &ch);
    int cols = grid_cols();
    out->x = c.x + 8 + (vi % cols) * cw; out->y = c.y + 8 + (vi / cols) * ch - G.scroll; out->w = cw; out->h = ch;
    return true;
}
int hit_item(int x, int y) {
    R c = content_rect();
    if (!inr(c, x, y)) return -1;
    int vi;
    if (G.view == V_DETAILS) vi = (y - c.y + G.scroll) / ROW_H;
    else {
        int cw, ch; cell_size(&cw, &ch);
        int cols = grid_cols(), col = (x - c.x - 8) / cw;
        if (x < c.x + 8 || col >= cols) return -1;
        int row = (y - c.y - 8 + G.scroll) / ch;
        if (y - c.y - 8 + G.scroll < 0) return -1;
        vi = row * cols + col;
    }
    return (vi >= 0 && vi < G.nvis) ? vi : -1;
}
void ensure_visible(int vi) {
    R r, c = content_rect();
    if (!item_rect(vi, &r)) return;
    if (r.y < c.y) G.scroll -= c.y - r.y + (G.view == V_DETAILS ? 0 : 8);
    else if (r.y + r.h > c.y + c.h) G.scroll += r.y + r.h - (c.y + c.h) + (G.view == V_DETAILS ? 0 : 8);
    int m = content_height() - c.h; if (m < 0) m = 0;
    if (G.scroll > m) G.scroll = m;
    if (G.scroll < 0) G.scroll = 0;
    G.dirty = true;
}

/* ---------------------------------------------------------------- */
/* icons: anti-aliased vector art at any size                          */
/* ---------------------------------------------------------------- */
static void page(int x, int y, int s, uint32_t accent) {
    uint32_t fill = G.dark ? 0x3A4256 : 0xFFFFFF, edge = G.dark ? 0x5A6580 : 0xC5CDDC;
    int pw = s * 68 / 100, px = x + (s - pw) / 2, fold = s * 24 / 100;
    cui_round_rect(C, px - 1, y - 1, pw + 2, s + 2, 4, edge);
    cui_round_rect(C, px, y, pw, s, 3, fill);
    cui_triangle(C, (float)(px + pw - fold), (float)y, (float)(px + pw), (float)(y + fold), (float)(px + pw - fold), (float)(y + fold), edge);
    cui_round_rect(C, px, y + s * 78 / 100, pw, s * 22 / 100, 3, accent);            /* the coloured tab at the foot */
    cui_fill(C, px, y + s * 78 / 100, pw, s * 6 / 100, accent);
}
void draw_icon(int kind, int x, int y, int s) {
    uint32_t col = kind_color(kind);
    float w = s < 24 ? 1.2f : (float)s / 20.0f;
    if (kind == K_FOLDER) {
        uint32_t back = cui_mix(col, 0x000000, 40), front = col;
        cui_round_rect(C, x + s * 4 / 100, y + s * 14 / 100, s * 44 / 100, s * 26 / 100, s / 12 + 1, back);
        cui_round_rect(C, x, y + s * 24 / 100, s, s * 62 / 100, s / 10 + 2, back);
        cui_round_rect(C, x, y + s * 32 / 100, s, s * 54 / 100, s / 10 + 2, front);
        cui_fill_alpha(C, x + 2, y + s * 34 / 100, s - 4, s * 8 / 100, 0xFFFFFF, 70);
        return;
    }
    page(x, y, s, col);
    int pw = s * 68 / 100, px = x + (s - pw) / 2, cx = px + pw / 2, top = y + s * 26 / 100, bot = y + s * 74 / 100;
    uint32_t ink = cui_mix(col, G.dark ? 0xFFFFFF : 0x000000, G.dark ? 40 : 30);
    switch (kind) {
        case K_IMAGE:
            cui_circle(C, (float)(px + pw * 68 / 100), (float)(top + (bot - top) / 4), (float)pw / 9.0f, 0xFBBC04);
            cui_triangle(C, (float)(px + pw * 12 / 100), (float)bot, (float)(px + pw * 45 / 100), (float)(top + (bot - top) * 35 / 100), (float)(px + pw * 75 / 100), (float)bot, ink);
            cui_triangle(C, (float)(px + pw * 50 / 100), (float)bot, (float)(px + pw * 72 / 100), (float)(top + (bot - top) * 55 / 100), (float)(px + pw * 92 / 100), (float)bot, cui_mix(ink, 0xFFFFFF, 60));
            break;
        case K_AUDIO:
            cui_circle(C, (float)(cx - pw / 6), (float)(bot - pw / 8), (float)pw / 7.0f, ink);
            cui_line(C, (float)(cx - pw / 6 + pw / 8), (float)(bot - pw / 8), (float)(cx - pw / 6 + pw / 8), (float)top, w * 1.2f, ink);
            cui_line(C, (float)(cx - pw / 6 + pw / 8), (float)top, (float)(cx + pw / 4), (float)(top + pw / 8), w * 1.4f, ink);
            break;
        case K_VIDEO:
            cui_triangle(C, (float)(cx - pw / 6), (float)top, (float)(cx - pw / 6), (float)bot, (float)(cx + pw / 4), (float)((top + bot) / 2), ink);
            break;
        case K_CODE:
            cui_line(C, (float)(cx - pw / 8), (float)top, (float)(cx - pw / 3), (float)((top + bot) / 2), w, ink);
            cui_line(C, (float)(cx - pw / 3), (float)((top + bot) / 2), (float)(cx - pw / 8), (float)bot, w, ink);
            cui_line(C, (float)(cx + pw / 8), (float)top, (float)(cx + pw / 3), (float)((top + bot) / 2), w, ink);
            cui_line(C, (float)(cx + pw / 3), (float)((top + bot) / 2), (float)(cx + pw / 8), (float)bot, w, ink);
            break;
        case K_ARCHIVE:
            for (int i = 0; i < 5; ++i) cui_fill(C, cx - pw / 12 + ((i & 1) ? pw / 12 : 0), top + i * (bot - top) / 5, pw / 12 + 1, (bot - top) / 8 + 1, ink);
            break;
        case K_EXEC:
            cui_line(C, (float)(cx - pw / 4), (float)(top + (bot - top) / 5), (float)(cx), (float)((top + bot) / 2), w * 1.2f, ink);
            cui_line(C, (float)(cx), (float)((top + bot) / 2), (float)(cx - pw / 4), (float)(bot - (bot - top) / 5), w * 1.2f, ink);
            cui_line(C, (float)(cx + pw / 20), (float)(bot - (bot - top) / 5), (float)(cx + pw / 3), (float)(bot - (bot - top) / 5), w * 1.2f, ink);
            break;
        default:
            for (int i = 0; i < 4; ++i) cui_fill(C, px + pw / 6, top + i * (bot - top) / 4, i == 3 ? pw / 2 : pw * 2 / 3, s / 24 + 1, ink);
            break;
    }
}

/* toolbar glyphs, 18 px, drawn around (cx, cy) */
static void glyph(int a, float cx, float cy, uint32_t c) {
    float w = 1.7f;
    switch (a) {
        case A_BACK:    cui_line(C, cx + 6, cy, cx - 6, cy, w, c); cui_line(C, cx - 6, cy, cx - 1, cy - 5, w, c); cui_line(C, cx - 6, cy, cx - 1, cy + 5, w, c); break;
        case A_FORWARD: cui_line(C, cx - 6, cy, cx + 6, cy, w, c); cui_line(C, cx + 6, cy, cx + 1, cy - 5, w, c); cui_line(C, cx + 6, cy, cx + 1, cy + 5, w, c); break;
        case A_UP:      cui_line(C, cx, cy + 6, cx, cy - 6, w, c); cui_line(C, cx, cy - 6, cx - 5, cy - 1, w, c); cui_line(C, cx, cy - 6, cx + 5, cy - 1, w, c); break;
        case A_REFRESH: cui_ring(C, cx, cy, 6.0f, 1.6f, c); cui_triangle(C, cx + 2, cy - 9, cx + 8, cy - 4, cx + 1, cy - 3, c); break;
        case A_NEW_FOLDER:
            cui_round_rect(C, (int)cx - 8, (int)cy - 5, 16, 12, 2, 0xF5A623); cui_round_rect(C, (int)cx - 8, (int)cy - 7, 7, 4, 1, 0xF5A623);
            cui_line(C, cx + 5, cy - 1, cx + 5, cy + 7, 1.6f, c); cui_line(C, cx + 1, cy + 3, cx + 9, cy + 3, 1.6f, c); break;
        case A_CUT:
            cui_ring(C, cx - 4, cy + 5, 3.0f, 1.4f, c); cui_ring(C, cx + 4, cy + 5, 3.0f, 1.4f, c);
            cui_line(C, cx - 4, cy + 2, cx + 3, cy - 8, 1.4f, c); cui_line(C, cx + 4, cy + 2, cx - 3, cy - 8, 1.4f, c); break;
        case A_COPY:
            cui_round_rect_outline(C, (int)cx - 3, (int)cy - 8, 11, 13, 2, 1.5f, c); cui_round_rect_outline(C, (int)cx - 8, (int)cy - 4, 11, 13, 2, 1.5f, c); break;
        case A_PASTE:
            cui_round_rect_outline(C, (int)cx - 7, (int)cy - 6, 14, 15, 2, 1.5f, c); cui_round_rect(C, (int)cx - 3, (int)cy - 9, 7, 5, 2, c);
            cui_line(C, cx - 4, cy + 1, cx + 4, cy + 1, 1.3f, c); cui_line(C, cx - 4, cy + 5, cx + 2, cy + 5, 1.3f, c); break;
        case A_DELETE:
            cui_line(C, cx - 7, cy - 5, cx + 7, cy - 5, 1.6f, c); cui_line(C, cx - 2, cy - 8, cx + 2, cy - 8, 1.6f, c);
            cui_round_rect_outline(C, (int)cx - 5, (int)cy - 4, 10, 13, 2, 1.5f, c); cui_line(C, cx - 2, cy - 1, cx - 2, cy + 6, 1.2f, c); cui_line(C, cx + 2, cy - 1, cx + 2, cy + 6, 1.2f, c); break;
        default: break;
    }
}

/* ---------------------------------------------------------------- */
/* menus and popups                                                  */
/* ---------------------------------------------------------------- */
typedef struct { int a; char label[80]; char key[24]; bool enabled, check, sep; } PI;
static bool has_sel(void) { return sel_count() > 0; }
static bool one_sel(void) { return sel_count() == 1; }

bool action_enabled(int a) {
    switch (a) {
        case A_OPEN: case A_CUT: case A_COPY: case A_DELETE: case A_COPY_PATH: return has_sel() || a == A_COPY_PATH;
        case A_RENAME: case A_PROPS: return one_sel() || (a == A_PROPS && !has_sel());
        case A_PASTE: return clip_has_files();
        case A_BACK: return G.hi > 0;
        case A_FORWARD: return G.hi + 1 < G.hn;
        case A_UP: return strcmp(G.path, "/") != 0;
        case A_REMOVE_MARK: for (int i = 0; i < G.nmarks; ++i) if (!G.marks[i].builtin && !strcmp(G.marks[i].path, G.path)) return true; return false;
        default: return !G.job.mode || (a != A_NEW_FOLDER && a != A_NEW_FILE && a != A_DELETE && a != A_PASTE);
    }
}
static void pi_add(PI *v, int *n, int a, const char *en, const char *ja, const char *key) {
    PI *p = &v[(*n)++]; memset(p, 0, sizeof *p);
    p->a = a; snprintf(p->label, sizeof p->label, "%s", G.jp ? ja : en); if (key) snprintf(p->key, sizeof p->key, "%s", key);
    p->sep = a == A_SEP; p->enabled = a == A_SEP ? false : action_enabled(a);
    switch (a) {
        case A_VIEW_DETAILS: p->check = G.view == V_DETAILS; break; case A_VIEW_LIST: p->check = G.view == V_LIST; break;
        case A_VIEW_ICONS: p->check = G.view == V_ICONS; break; case A_VIEW_THUMBS: p->check = G.view == V_THUMBS; break;
        case A_TOGGLE_PREVIEW: p->check = G.preview; break; case A_TOGGLE_HIDDEN: p->check = G.show_hidden; break; case A_TOGGLE_TREE: p->check = G.tree_vis; break;
        case A_SORT_NAME: p->check = G.sort == S_NAME; break; case A_SORT_SIZE: p->check = G.sort == S_SIZE; break;
        case A_SORT_DATE: p->check = G.sort == S_DATE; break; case A_SORT_TYPE: p->check = G.sort == S_TYPE; break;
        case A_SORT_ORDER: p->check = !G.sort_asc; break;
        default: break;
    }
}
#define ADD(a, en, ja, k) pi_add(v, &n, a, en, ja, k)
#define SEP() pi_add(v, &n, A_SEP, "", "", NULL)
static const char *const MENU_EN[5] = { "File", "Edit", "View", "Go", "Help" };
static const char *const MENU_JA[5] = { "ファイル", "編集", "表示", "移動", "ヘルプ" };
static int menu_build(int m, PI *v) {
    int n = 0;
    switch (m) {
        case 0: ADD(A_OPEN, "Open", "開く", "Enter"); SEP(); ADD(A_NEW_FOLDER, "New folder", "新しいフォルダー", "Ctrl+Shift+N"); ADD(A_NEW_FILE, "New file", "新しいファイル", "Ctrl+N");
                SEP(); ADD(A_RENAME, "Rename", "名前の変更", "F2"); ADD(A_DELETE, "Delete", "削除", "Del"); ADD(A_PROPS, "Properties", "プロパティ", "Alt+Enter"); SEP(); ADD(A_CLOSE, "Close", "閉じる", "Alt+F4"); break;
        case 1: ADD(A_CUT, "Cut", "切り取り", "Ctrl+X"); ADD(A_COPY, "Copy", "コピー", "Ctrl+C"); ADD(A_PASTE, "Paste", "貼り付け", "Ctrl+V"); SEP();
                ADD(A_COPY_PATH, "Copy path", "パスをコピー", "Ctrl+Shift+C"); SEP(); ADD(A_SELECT_ALL, "Select all", "すべて選択", "Ctrl+A"); ADD(A_INVERT, "Invert selection", "選択の切り替え", "Ctrl+I"); break;
        case 2: ADD(A_VIEW_DETAILS, "Details", "詳細", "Ctrl+1"); ADD(A_VIEW_LIST, "List", "一覧", "Ctrl+2"); ADD(A_VIEW_ICONS, "Icons", "アイコン", "Ctrl+3"); ADD(A_VIEW_THUMBS, "Thumbnails", "サムネイル", "Ctrl+4"); SEP();
                ADD(A_TOGGLE_PREVIEW, "Preview pane", "プレビュー", "Ctrl+P"); ADD(A_TOGGLE_HIDDEN, "Hidden files", "隠しファイル", "Ctrl+H"); ADD(A_TOGGLE_TREE, "Folder tree", "フォルダーツリー", "Ctrl+B"); SEP();
                ADD(A_SORT_NAME, "Sort by name", "名前で並べ替え", ""); ADD(A_SORT_SIZE, "Sort by size", "サイズで並べ替え", ""); ADD(A_SORT_DATE, "Sort by date", "日付で並べ替え", ""); ADD(A_SORT_TYPE, "Sort by type", "種類で並べ替え", "");
                ADD(A_SORT_ORDER, "Descending", "降順", ""); SEP(); ADD(A_REFRESH, "Refresh", "最新の情報に更新", "F5"); break;
        case 3: ADD(A_BACK, "Back", "戻る", "Alt+Left"); ADD(A_FORWARD, "Forward", "進む", "Alt+Right"); ADD(A_UP, "Up", "上へ", "Backspace"); SEP();
                ADD(A_GO_ROOT, "Disk (/)", "ディスク (/)", ""); ADD(A_GO_STUDIO, "C-OS Studio", "C-OS Studio", ""); ADD(A_GO_DESKTOP, "Desktop", "デスクトップ", ""); ADD(A_GO_DOCS, "Documents", "ドキュメント", "");
                ADD(A_GO_MUSIC, "Music", "ミュージック", ""); ADD(A_GO_PICTURES, "Pictures", "ピクチャ", ""); ADD(A_GO_DOWNLOADS, "Downloads", "ダウンロード", ""); ADD(A_GO_BIN, "System programs (/bin)", "システム (/bin)", ""); SEP();
                ADD(A_FOCUS_ADDR, "Go to path...", "パスを入力...", "Ctrl+L"); ADD(A_ADD_MARK, "Add bookmark", "ブックマークに追加", "Ctrl+D"); ADD(A_REMOVE_MARK, "Remove bookmark", "ブックマークを削除", ""); break;
        default: ADD(A_ABOUT, "About File Manager", "ファイルマネージャーについて", ""); break;
    }
    return n;
}
static int menu_title_x(int m) {
    int x = 10;
    for (int k = 0; k < m; ++k) x += cui_text_width(FN(13, false), G.jp ? MENU_JA[k] : MENU_EN[k]) + 22;
    return x;
}
static void popup_size(const PI *v, int n, int *w, int *h) {
    int mw = 150;
    for (int i = 0; i < n; ++i) {
        if (v[i].sep) continue;
        int iw = cui_text_width(FN(13, false), v[i].label) + (v[i].key[0] ? cui_text_width(FN(12, false), v[i].key) + 44 : 34) + 22;
        if (iw > mw) mw = iw;
    }
    int hh = 10;
    for (int i = 0; i < n; ++i) hh += v[i].sep ? 9 : 28;
    *w = mw; *h = hh;
}
static void menu_box(int m, R *r) {
    PI v[32]; int n = menu_build(m, v), w, h;
    popup_size(v, n, &w, &h);
    r->x = menu_title_x(m) - 8; r->y = MENU_H; r->w = w; r->h = h;
    if (r->x + r->w > WIN_W - 4) r->x = WIN_W - r->w - 4;
}
int menubar_hit(int x, int y) {
    if (y >= MENU_H) return -1;
    for (int m = 0; m < 5; ++m) {
        int x0 = menu_title_x(m) - 8, w = cui_text_width(FN(13, false), G.jp ? MENU_JA[m] : MENU_EN[m]) + 16;
        if (x >= x0 && x < x0 + w) return m;
    }
    return -1;
}
static int popup_item_at(const PI *v, int n, R box, int x, int y) {
    if (x < box.x || x >= box.x + box.w || y < box.y || y >= box.y + box.h) return -1;
    int cy = box.y + 5;
    for (int i = 0; i < n; ++i) { int ih = v[i].sep ? 9 : 28; if (y >= cy && y < cy + ih) return v[i].sep ? -1 : i; cy += ih; }
    return -1;
}
/* returns the action under the pointer in the open menu, 0 if none, -1 if the pointer is outside */
int menu_item_hit(int m, int x, int y, int *index) {
    PI v[32]; int n = menu_build(m, v); R box; menu_box(m, &box);
    int i = popup_item_at(v, n, box, x, y);
    if (index) *index = i;
    if (x < box.x || x >= box.x + box.w || y < box.y || y >= box.y + box.h) return -1;
    return i >= 0 && v[i].enabled ? v[i].a : 0;
}
static void draw_popup(R box, const PI *v, int n, int hover) {
    cui_round_rect_alpha(C, box.x - 1, box.y + 3, box.w + 2, box.h + 2, 9, 0x000000, 70);
    cui_round_rect(C, box.x - 1, box.y - 1, box.w + 2, box.h + 2, 8, P.border);
    cui_round_rect(C, box.x, box.y, box.w, box.h, 7, P.panel);
    int cy = box.y + 5;
    cui_font *f = FN(13, false), *fs = FN(12, false);
    for (int i = 0; i < n; ++i) {
        if (v[i].sep) { cui_fill(C, box.x + 10, cy + 4, box.w - 20, 1, P.border); cy += 9; continue; }
        bool hv = i == hover && v[i].enabled;
        if (hv) cui_round_rect(C, box.x + 5, cy, box.w - 10, 26, 5, P.accent);
        uint32_t col = !v[i].enabled ? cui_mix(P.muted, P.panel, 90) : hv ? 0xFFFFFF : P.text;
        if (v[i].check) { cui_line(C, (float)(box.x + 13), (float)(cy + 13), (float)(box.x + 17), (float)(cy + 17), 1.8f, col); cui_line(C, (float)(box.x + 17), (float)(cy + 17), (float)(box.x + 24), (float)(cy + 8), 1.8f, col); }
        txt(box.x + 32, cy, 26, f, col, v[i].label);
        if (v[i].key[0]) txt(box.x + box.w - 14 - cui_text_width(fs, v[i].key), cy, 26, fs, hv ? 0xE8F0FF : P.muted, v[i].key);
        cy += 28;
    }
}

/* context menu ---------------------------------------------------- */
static int ctx_build(PI *v) {
    int n = 0;
    switch (G.ctx_kind) {
        case 0: ADD(A_OPEN, "Open", "開く", "Enter"); SEP(); ADD(A_CUT, "Cut", "切り取り", "Ctrl+X"); ADD(A_COPY, "Copy", "コピー", "Ctrl+C"); ADD(A_PASTE, "Paste", "貼り付け", "Ctrl+V"); SEP();
                ADD(A_RENAME, "Rename", "名前の変更", "F2"); ADD(A_DELETE, "Delete", "削除", "Del"); SEP(); ADD(A_COPY_PATH, "Copy path", "パスをコピー", ""); ADD(A_PROPS, "Properties", "プロパティ", "Alt+Enter"); break;
        case 1: ADD(A_NEW_FOLDER, "New folder", "新しいフォルダー", ""); ADD(A_NEW_FILE, "New file", "新しいファイル", ""); SEP(); ADD(A_PASTE, "Paste", "貼り付け", "Ctrl+V"); ADD(A_SELECT_ALL, "Select all", "すべて選択", "Ctrl+A"); SEP();
                ADD(A_REFRESH, "Refresh", "最新の情報に更新", "F5"); ADD(A_PROPS, "Properties", "プロパティ", ""); break;
        case 2: ADD(A_OPEN, "Open", "開く", ""); if (G.ctx_target >= 0 && G.ctx_target < G.nmarks && !G.marks[G.ctx_target].builtin) { SEP(); ADD(A_REMOVE_MARK, "Remove bookmark", "ブックマークを削除", ""); } break;
        default: ADD(A_OPEN, "Open", "開く", ""); ADD(A_ADD_MARK, "Add bookmark", "ブックマークに追加", ""); ADD(A_NEW_FOLDER, "New folder", "新しいフォルダー", ""); ADD(A_REFRESH, "Refresh", "最新の情報に更新", "F5"); break;
    }
    if (G.ctx_kind == 2 || G.ctx_kind == 3) for (int i = 0; i < n; ++i) if (v[i].a == A_OPEN || v[i].a == A_ADD_MARK || v[i].a == A_REMOVE_MARK || v[i].a == A_REFRESH || v[i].a == A_NEW_FOLDER) v[i].enabled = true;
    return n;
}
static R ctx_box(const PI *v, int n) {
    int w, h; popup_size(v, n, &w, &h);
    R r = { G.ctx_x, G.ctx_y, w, h };
    if (r.x + r.w > WIN_W - 4) r.x = WIN_W - r.w - 4;
    if (r.y + r.h > WIN_H - 4) r.y = WIN_H - r.h - 4;
    return r;
}
int ctx_hit(int x, int y, int *index) {
    PI v[16]; int n = ctx_build(v); R box = ctx_box(v, n);
    int i = popup_item_at(v, n, box, x, y);
    if (index) *index = i;
    if (!inr(box, x, y)) return -1;
    return i >= 0 && v[i].enabled ? v[i].a : 0;
}

/* ---------------------------------------------------------------- */
/* toolbar, places, address bar                                      */
/* ---------------------------------------------------------------- */
static const int TB_ACT[] = { A_BACK, A_FORWARD, A_UP, A_REFRESH, 0, A_NEW_FOLDER, 0, A_CUT, A_COPY, A_PASTE, A_DELETE };
#define TB_N ((int)(sizeof TB_ACT / sizeof TB_ACT[0]))
static R tb_rect(int i) { int x = 10; for (int k = 0; k < i; ++k) x += TB_ACT[k] ? 38 : 14; R r = { x, TOOL_Y + 5, 34, 34 }; return r; }
static const int VIEW_ACT[4] = { A_VIEW_DETAILS, A_VIEW_LIST, A_VIEW_ICONS, A_VIEW_THUMBS };
static R view_rect(int i) { R r = { WIN_W - 12 - (4 - i) * 70 + 4, TOOL_Y + 8, 66, 28 }; return r; }
int tb_hit(int x, int y) {
    for (int i = 0; i < TB_N; ++i) if (TB_ACT[i]) { R r = tb_rect(i); if (inr(r, x, y)) return TB_ACT[i]; }
    for (int i = 0; i < 4; ++i) { R r = view_rect(i); if (inr(r, x, y)) return VIEW_ACT[i]; }
    return 0;
}
static const char *tb_tip(int a) {
    switch (a) {
        case A_BACK: return L("Back", "戻る"); case A_FORWARD: return L("Forward", "進む"); case A_UP: return L("Up one level", "上へ"); case A_REFRESH: return L("Refresh", "最新の情報に更新");
        case A_NEW_FOLDER: return L("New folder", "新しいフォルダー"); case A_CUT: return L("Cut", "切り取り"); case A_COPY: return L("Copy", "コピー"); case A_PASTE: return L("Paste", "貼り付け"); case A_DELETE: return L("Delete", "削除");
        default: return NULL;
    }
}
static const char *view_label(int i) {
    static const char *en[4] = { "Details", "List", "Icons", "Thumbs" }, *ja[4] = { "詳細", "一覧", "アイコン", "サムネイル" };
    return G.jp ? ja[i] : en[i];
}

static R place_rect(int i) {
    int x = 12;
    for (int k = 0; k < i; ++k) x += cui_text_width(FN(13, false), G.marks[k].name) + 46;
    R r = { x, PLACES_Y + 3, cui_text_width(FN(13, false), G.marks[i].name) + 40, 28 };
    return r;
}
int place_hit(int x, int y) { for (int i = 0; i < G.nmarks; ++i) { R r = place_rect(i); if (inr(r, x, y)) return i; } return -1; }
static int mark_active(void) {           /* the deepest bookmark that contains the current folder */
    int best = -1; size_t bl = 0;
    for (int i = 0; i < G.nmarks; ++i) {
        size_t l = strlen(G.marks[i].path);
        bool in = !strcmp(G.path, G.marks[i].path) || (l > 1 && !strncmp(G.path, G.marks[i].path, l) && G.path[l] == '/');
        if (in && l >= bl) { best = i; bl = l; }
    }
    return best;
}

/* breadcrumb: segment k covers the path up to and including component k */
static struct { R r; char path[PATHN]; } s_crumb[24]; static int s_ncrumb;
static R search_rect(void) { R r = { WIN_W - 12 - 210, ADDR_Y + 4, 210, ADDR_H - 8 }; return r; }
static R addr_field(void) { R r = { 10, ADDR_Y + 4, WIN_W - 10 - 210 - 22, ADDR_H - 8 }; return r; }
static void crumbs_layout(void) {
    s_ncrumb = 0;
    R f = addr_field(); cui_font *fn = FN(13, false);
    int x = f.x + 12;
    snprintf(s_crumb[0].path, PATHN, "/"); s_crumb[0].r.x = x; s_crumb[0].r.y = f.y; s_crumb[0].r.h = f.h;
    s_crumb[0].r.w = cui_text_width(fn, "/") + 12; x += s_crumb[0].r.w; s_ncrumb = 1;
    char tmp[PATHN], acc[PATHN] = ""; snprintf(tmp, sizeof tmp, "%s", G.path);
    for (char *save = NULL, *t = strtok_r(tmp, "/", &save); t && s_ncrumb < 24; t = strtok_r(NULL, "/", &save)) {
        size_t l = strlen(acc); snprintf(acc + l, sizeof acc - l, "/%s", t);
        x += 14;                                                                 /* the chevron */
        s_crumb[s_ncrumb].r.x = x; s_crumb[s_ncrumb].r.y = f.y; s_crumb[s_ncrumb].r.h = f.h;
        s_crumb[s_ncrumb].r.w = cui_text_width(fn, t) + 12;
        snprintf(s_crumb[s_ncrumb].path, PATHN, "%s", acc);
        x += s_crumb[s_ncrumb].r.w; ++s_ncrumb;
    }
}
int crumb_hit(int x, int y) {
    R f = addr_field();
    if (!inr(f, x, y)) return -1;
    crumbs_layout();
    for (int i = 0; i < s_ncrumb; ++i) if (inr(s_crumb[i].r, x, y)) return i;
    return -2;
}
const char *crumb_path(int i) { return (i >= 0 && i < s_ncrumb) ? s_crumb[i].path : "/"; }
bool search_hit(int x, int y) { return inr(search_rect(), x, y); }
bool addr_hit(int x, int y) { return inr(addr_field(), x, y); }

/* ---------------------------------------------------------------- */
/* tree                                                              */
/* ---------------------------------------------------------------- */
#define TREE_ROW 26
#define TREE_TOP (BODY_Y + 30)
int tree_hit(int x, int y, bool *on_arrow) {
    R t = tree_rect();
    if (!inr(t, x, y) || y < TREE_TOP) return -1;
    int i = (y - TREE_TOP + G.tree_scroll) / TREE_ROW;
    if (i < 0 || i >= G.ntree) return -1;
    if (on_arrow) *on_arrow = x >= 4 + G.tree[i].depth * 16 && x < 24 + G.tree[i].depth * 16 && i > 0;
    return i;
}
static void draw_tree(void) {
    R t = tree_rect();
    if (!t.w) return;
    cui_fill(C, t.x, t.y, t.w, t.h, P.side);
    cui_fill(C, t.x + t.w - 1, t.y, 1, t.h, P.border);
    txt(14, t.y + 4, 24, FN(12, true), P.muted, L("FOLDERS", "フォルダー"));
    cui_clip(C, t.x, TREE_TOP, t.w - 1, t.y + t.h - TREE_TOP);
    int max = G.ntree * TREE_ROW - (t.y + t.h - TREE_TOP); if (max < 0) max = 0;
    if (G.tree_scroll > max) G.tree_scroll = max;
    for (int i = 0; i < G.ntree; ++i) {
        TNode *n = &G.tree[i];
        int y = TREE_TOP + i * TREE_ROW - G.tree_scroll;
        if (y + TREE_ROW < TREE_TOP || y > t.y + t.h) continue;
        bool cur = !strcmp(n->path, G.path);
        bool hv = G.my >= y && G.my < y + TREE_ROW && G.mx < t.w && G.mx >= 0;
        if (cur) cui_fill(C, 0, y, t.w - 1, TREE_ROW, P.select);
        else if (hv) cui_fill(C, 0, y, t.w - 1, TREE_ROW, P.hover);
        if (G.drag_kind == 2 && hv && i != 0 + 0) cui_fill_alpha(C, 0, y, t.w - 1, TREE_ROW, P.accent, 60);
        int x = 8 + n->depth * 16;
        if (i > 0) {
            float cx = (float)x + 5, cy = (float)y + TREE_ROW / 2;
            if (n->open) cui_triangle(C, cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3, P.muted);
            else cui_triangle(C, cx - 2, cy - 4, cx - 2, cy + 4, cx + 3, cy, P.muted);
        }
        draw_icon(K_FOLDER, x + 14, y + 5, 16);
        txtfit(x + 36, y, TREE_ROW, t.w - x - 42, FN(13, cur), P.text, n->name);
    }
    cui_unclip(C);
}

/* ---------------------------------------------------------------- */
/* file list                                                         */
/* ---------------------------------------------------------------- */
static int col_x(int col, int *w) {           /* details columns: name | size | modified | type */
    R c = center_rect();
    int wt = 84, wd = 146, wk = c.w >= 600 ? 104 : 0, wn = c.w - wt - wd - wk - 24;    /* the Type column goes when the pane is narrow */
    int x[4] = { c.x + 12, c.x + 12 + wn, c.x + 12 + wn + wt, c.x + 12 + wn + wt + wd };
    int ww[4] = { wn, wt, wd, wk };
    if (w) *w = ww[col];
    return x[col];
}
int header_hit(int x, int y) {
    R c = center_rect();
    if (G.view != V_DETAILS || y < c.y || y >= c.y + HDR_H) return -1;
    for (int i = 0; i < 4; ++i) { int w, cx = col_x(i, &w); if (w > 0 && x >= cx - 8 && x < cx + w) return i; }
    return -1;
}
static void scroll_thumb(int *ty0, int *th) {
    R c = content_rect();
    int total = content_height(), vp = c.h;
    if (total <= vp) { *th = 0; *ty0 = c.y; return; }
    *th = vp * vp / total; if (*th < 32) *th = 32;
    int m = total - vp;
    *ty0 = c.y + (vp - *th) * G.scroll / m;
}
bool scrollbar_hit(int x, int y, int *thumb_y, int *thumb_h) {
    R c = content_rect();
    int ty0, th; scroll_thumb(&ty0, &th);
    if (!th) return false;
    if (thumb_y) *thumb_y = ty0;
    if (thumb_h) *thumb_h = th;
    return x >= c.x + c.w - 14 && x < c.x + c.w && y >= c.y && y < c.y + c.h;
}

static void draw_details_header(void) {
    R c = center_rect();
    cui_fill(C, c.x, c.y, c.w, HDR_H, P.tool);
    cui_fill(C, c.x, c.y + HDR_H - 1, c.w, 1, P.border);
    static const char *en[4] = { "Name", "Size", "Modified", "Type" }, *ja[4] = { "名前", "サイズ", "更新日時", "種類" };
    for (int i = 0; i < 4; ++i) {
        int w, x = col_x(i, &w);
        if (w <= 0) continue;
        bool hv = header_hit(G.mx, G.my) == i;
        if (hv) cui_fill(C, x - 8, c.y, w + 8, HDR_H - 1, P.hover);
        int tw = txt(x, c.y, HDR_H, FN(12, true), G.sort == i ? P.accent : P.muted, G.jp ? ja[i] : en[i]);
        if (G.sort == i) {
            float ax = (float)(x + tw + 10), ay = (float)(c.y + HDR_H / 2);
            if (G.sort_asc) cui_triangle(C, ax - 4, ay + 2, ax + 4, ay + 2, ax, ay - 3, P.accent); else cui_triangle(C, ax - 4, ay - 2, ax + 4, ay - 2, ax, ay + 3, P.accent);
        }
        if (i) cui_fill(C, x - 9, c.y + 6, 1, HDR_H - 12, P.border);
    }
}

static void draw_selection_bg(R r, bool sel, bool hov, bool focus, int radius) {
    if (sel) cui_round_rect(C, r.x, r.y, r.w, r.h, radius, P.select);
    else if (hov) cui_round_rect(C, r.x, r.y, r.w, r.h, radius, P.hover);
    if (focus && G.menu_open < 0) cui_round_rect_outline(C, r.x, r.y, r.w, r.h, radius, 1.2f, P.accent);
}

static void wrap2(const char *s, int maxw, cui_font *f, char *l1, char *l2, size_t cap) {      /* two lines, UTF-8 safe */
    l1[0] = l2[0] = 0;
    if (cui_text_width(f, s) <= maxw) { snprintf(l1, cap, "%s", s); return; }
    size_t i = 0, last = 0;
    while (s[i]) {
        size_t j = i + 1; while ((s[j] & 0xC0) == 0x80) ++j;
        char t[300]; size_t n = j < sizeof t - 1 ? j : sizeof t - 1; memcpy(t, s, n); t[n] = 0;
        if (cui_text_width(f, t) > maxw) break;
        last = j; i = j;
    }
    if (last == 0) last = 1;
    snprintf(l1, cap, "%.*s", (int)last, s);
    snprintf(l2, cap, "%s", s + last);
}

static void draw_list(void) {
    R cr = center_rect(), c = content_rect();
    cui_fill(C, cr.x, cr.y, cr.w, cr.h, P.panel);
    if (G.view == V_DETAILS) draw_details_header();
    cui_clip(C, c.x, c.y, c.w, c.h);
    int m = content_height() - c.h; if (m < 0) m = 0;
    if (G.scroll > m) G.scroll = m;
    if (G.scroll < 0) G.scroll = 0;
    cui_font *fn = FN(13, false), *fs = FN(12, false);
    for (int vi = 0; vi < G.nvis; ++vi) {
        R r;
        item_rect(vi, &r);
        if (r.y + r.h < c.y) continue;
        if (r.y > c.y + c.h) { if (G.view == V_DETAILS) break; else continue; }
        Ent *e = ent_at(vi);
        bool hov = vi == G.hover, sel = e->sel, dropt = G.drag_kind == 2 && hov && e->is_dir && !e->sel;
        uint32_t tcol = P.text;
        bool hidden = e->name[0] == '.' || (e->attr & COS_ATTR_HIDDEN);
        if (hidden) tcol = P.muted;
        if (G.view == V_DETAILS) {
            if (!sel && !hov && (vi & 1)) cui_fill(C, r.x, r.y, r.w, r.h, G.dark ? 0x232838 : 0xFAFBFD);
            R rr = { r.x + 4, r.y + 1, r.w - 8, r.h - 2 };
            draw_selection_bg(rr, sel, hov, vi == G.focus, 5);
            if (dropt) cui_round_rect_outline(C, rr.x, rr.y, rr.w, rr.h, 5, 2.0f, P.accent);
            int w, x = col_x(S_NAME, &w);
            draw_icon(e->kind, x, r.y + 3, 20);
            txtfit(x + 28, r.y, r.h, w - 34, fn, tcol, e->name);
            char b[64];
            if (!e->is_dir) { fmt_size(e->size, b, sizeof b); x = col_x(S_SIZE, &w); txt(x + w - 22 - cui_text_width(fs, b), r.y, r.h, fs, P.muted, b); }
            fmt_date(e->mtime, b, sizeof b); x = col_x(S_DATE, &w); txt(x, r.y, r.h, fs, P.muted, b);
            x = col_x(S_TYPE, &w); if (w > 0) txtfit(x, r.y, r.h, w - 6, fs, P.muted, kind_name(e->kind));
        } else if (G.view == V_LIST) {
            R rr = { r.x + 2, r.y + 1, r.w - 6, r.h - 2 };
            draw_selection_bg(rr, sel, hov, vi == G.focus, 6);
            if (dropt) cui_round_rect_outline(C, rr.x, rr.y, rr.w, rr.h, 6, 2.0f, P.accent);
            draw_icon(e->kind, r.x + 8, r.y + 4, 20);
            txtfit(r.x + 36, r.y, r.h, r.w - 44, fn, tcol, e->name);
        } else if (G.view == V_ICONS) {
            R rr = { r.x + 3, r.y + 2, r.w - 6, r.h - 4 };
            draw_selection_bg(rr, sel, hov, vi == G.focus, 8);
            if (dropt) cui_round_rect_outline(C, rr.x, rr.y, rr.w, rr.h, 8, 2.0f, P.accent);
            draw_icon(e->kind, r.x + (r.w - 52) / 2, r.y + 8, 52);
            char l1[300], l2[300]; wrap2(e->name, r.w - 14, fs, l1, l2, sizeof l1);
            cui_text_center(C, fs, r.x + 4, r.y + 64, r.w - 8, 16, l1, tcol);
            if (l2[0]) { char t2[300]; cui_text_fit(C, fs, r.x + 7 + (r.w - 14 - cui_text_width(fs, l2) > 0 ? (r.w - 14 - cui_text_width(fs, l2)) / 2 : 0), r.y + 64 + 16, r.w - 14, l2, tcol); (void)t2; }
        } else {
            R rr = { r.x + 3, r.y + 2, r.w - 6, r.h - 4 };
            draw_selection_bg(rr, sel, hov, vi == G.focus, 8);
            if (dropt) cui_round_rect_outline(C, rr.x, rr.y, rr.w, rr.h, 8, 2.0f, P.accent);
            Thumb *t = e->kind == K_IMAGE ? thumb_lookup(e) : NULL;
            if (t && t->state == 1) {
                int tx = r.x + (r.w - t->w) / 2, tyy = r.y + 8 + (100 - t->h) / 2;
                cui_round_rect(C, r.x + (r.w - 136) / 2, r.y + 6, 136, 100, 4, G.dark ? 0x2C3242 : 0xE9EDF5);
                blit_argb(C, tx, tyy, t->px, t->w, t->h);
            } else draw_icon(e->kind, r.x + (r.w - 64) / 2, r.y + 20, 64);
            char l1[300], l2[300]; wrap2(e->name, r.w - 14, fs, l1, l2, sizeof l1);
            cui_text_center(C, fs, r.x + 4, r.y + 108, r.w - 8, 16, l1, tcol);
            if (l2[0]) cui_text_fit(C, fs, r.x + 7, r.y + 108 + 15, r.w - 14, l2, tcol);
        }
    }
    if (!G.nvis) {
        const char *msg = G.search[0] ? L("No items match your search", "検索に一致する項目がありません") : L("This folder is empty", "このフォルダーは空です");
        cui_text_center(C, FN(14, false), c.x, c.y, c.w, c.h - 30, msg, P.muted);
    }
    /* rubber band */
    if (G.drag_kind == 1) {
        int x0 = G.drag_x0 < G.mx ? G.drag_x0 : G.mx, y0 = G.drag_y0 < G.my ? G.drag_y0 : G.my;
        int x1 = G.drag_x0 < G.mx ? G.mx : G.drag_x0, y1 = G.drag_y0 < G.my ? G.my : G.drag_y0;
        cui_fill_alpha(C, x0, y0, x1 - x0, y1 - y0, P.accent, 50);
        cui_round_rect_outline(C, x0, y0, x1 - x0 + 1, y1 - y0 + 1, 1, 1.0f, P.accent);
    }
    /* scrollbar */
    { int ty0, th; scroll_thumb(&ty0, &th);
      if (th) { bool act = G.drag_kind == 3 || (G.mx >= c.x + c.w - 14 && inr(c, G.mx, G.my)); int w = act ? 8 : 5;
                cui_round_rect_alpha(C, c.x + c.w - w - 3, ty0, w, th, w / 2, G.dark ? 0xFFFFFF : 0x000000, act ? 90 : 50); } }
    cui_unclip(C);
}

/* ---------------------------------------------------------------- */
/* preview pane                                                      */
/* ---------------------------------------------------------------- */
static void draw_preview(void) {
    R p = prev_rect();
    if (!p.w) return;
    cui_fill(C, p.x, p.y, p.w, p.h, P.side);
    cui_fill(C, p.x, p.y, 1, p.h, P.border);
    txt(p.x + 14, p.y + 4, 24, FN(12, true), P.muted, L("PREVIEW", "プレビュー"));
    cui_clip(C, p.x + 1, p.y + 28, p.w - 1, p.h - 28);
    int ns = sel_count();
    Ent *e = NULL;
    if (ns == 1) for (int i = 0; i < G.nvis; ++i) if (ent_at(i)->sel) { e = ent_at(i); break; }
    int y = p.y + 40;
    cui_font *fb = FN(15, true), *fn = FN(12, false), *fs = FN(12, false);
    if (ns > 1) {
        uint64_t total = 0; for (int i = 0; i < G.nvis; ++i) if (ent_at(i)->sel && !ent_at(i)->is_dir) total += ent_at(i)->size;
        char b[64], s[96]; fmt_size(total, b, sizeof b);
        draw_icon(K_OTHER, p.x + (p.w - 72) / 2, y, 72);
        snprintf(s, sizeof s, L("%d items selected", "%d個の項目を選択中"), ns);
        cui_text_center(C, fb, p.x, y + 84, p.w, 24, s, P.text);
        cui_text_center(C, fn, p.x, y + 110, p.w, 20, b, P.muted);
    } else if (e) {
        char pth[PATHN]; path_join(pth, sizeof pth, G.path, e->name);
        int iy = y;
        if (e->kind == K_IMAGE) {
            Thumb *t = preview_image(e);
            if (t) { cui_round_rect(C, p.x + 16, y, p.w - 32, 200, 6, G.dark ? 0x2C3242 : 0xE9EDF5); blit_argb(C, p.x + (p.w - t->w) / 2, y + (200 - t->h) / 2, t->px, t->w, t->h); iy = y + 208; }
            else { draw_icon(e->kind, p.x + (p.w - 88) / 2, y, 88); iy = y + 100; }
        } else if (e->kind == K_TEXT || e->kind == K_CODE) {
            const char *tx = preview_text(e);
            cui_round_rect(C, p.x + 16, y, p.w - 32, 150, 6, P.panel);
            cui_round_rect_outline(C, p.x + 16, y, p.w - 32, 150, 6, 1.0f, P.border);
            if (tx[0]) {
                cui_clip(C, p.x + 22, y + 6, p.w - 44, 138);
                int ly = y + 8; const char *q = tx;
                for (int ln = 0; ln < 9 && *q; ++ln) {
                    char row[160]; size_t k = 0;
                    while (*q && *q != '\n' && k < sizeof row - 1) row[k++] = *q++;
                    if (*q == '\n') ++q;
                    row[k] = 0;
                    for (char *t = row; *t; ++t) if (*t == '\t') *t = ' ';
                    cui_text_fit(C, FN(11, false), p.x + 24, ly, p.w - 48, row, P.text); ly += 15;
                }
                cui_unclip(C); cui_clip(C, p.x + 1, p.y + 28, p.w - 1, p.h - 28);
            } else cui_text_center(C, fs, p.x + 16, y, p.w - 32, 150, L("No preview", "プレビューなし"), P.muted);
            iy = y + 160;
        } else if (e->kind == K_FOLDER) { draw_icon(e->kind, p.x + (p.w - 88) / 2, y, 88); iy = y + 100; }
        else { draw_icon(e->kind, p.x + (p.w - 88) / 2, y, 88); iy = y + 100; }
        char l1[300], l2[300]; wrap2(e->name, p.w - 28, fb, l1, l2, sizeof l1);
        cui_text_center(C, fb, p.x + 8, iy, p.w - 16, 22, l1, P.text); iy += 22;
        if (l2[0]) { cui_text_center(C, fb, p.x + 8, iy, p.w - 16, 22, l2, P.text); iy += 22; }
        iy += 8;
        char b[96];
        static const char *en[4] = { "Type", "Size", "Modified", "Location" }, *ja[4] = { "種類", "サイズ", "更新日時", "場所" };
        const char *vals[4]; char sz[64], dt[64];
        fmt_size(e->size, sz, sizeof sz); fmt_date(e->mtime, dt, sizeof dt);
        vals[0] = kind_name(e->kind); vals[1] = e->is_dir ? "-" : sz; vals[2] = dt; vals[3] = G.path;
        for (int i = 0; i < 4; ++i) {
            txt(p.x + 16, iy, 22, fs, P.muted, G.jp ? ja[i] : en[i]);
            cui_text_fit(C, fs, p.x + 88, ty(fs, iy, 22), p.w - 100, vals[i], P.text); iy += 24;
        }
        (void)b; (void)pth;
    } else {
        draw_icon(K_FOLDER, p.x + (p.w - 72) / 2, y, 72);
        const char *nm = strcmp(G.path, "/") ? path_base(G.path) : L("Disk", "ディスク");
        cui_text_center(C, fb, p.x + 8, y + 84, p.w - 16, 24, nm, P.text);
        char s[96]; snprintf(s, sizeof s, L("%d items", "%d個の項目"), G.nvis);
        cui_text_center(C, fn, p.x, y + 110, p.w, 20, s, P.muted);
    }
    cui_unclip(C);
}

/* ---------------------------------------------------------------- */
/* bars                                                              */
/* ---------------------------------------------------------------- */
static void draw_menubar(void) {
    cui_fill(C, 0, 0, WIN_W, MENU_H, P.side);
    cui_fill(C, 0, MENU_H - 1, WIN_W, 1, P.border);
    for (int m = 0; m < 5; ++m) {
        const char *s = G.jp ? MENU_JA[m] : MENU_EN[m];
        int x = menu_title_x(m), w = cui_text_width(FN(13, false), s);
        bool on = G.menu_open == m, hv = menubar_hit(G.mx, G.my) == m;
        if (on) cui_round_rect(C, x - 8, 2, w + 16, MENU_H - 5, 5, P.accent);
        else if (hv) cui_round_rect(C, x - 8, 2, w + 16, MENU_H - 5, 5, P.hover);
        txt(x, 0, MENU_H - 1, FN(13, false), on ? 0xFFFFFF : P.text, s);
    }
}
static void draw_toolbar(void) {
    cui_fill(C, 0, TOOL_Y, WIN_W, TOOL_H, P.tool);
    for (int i = 0; i < TB_N; ++i) {
        int a = TB_ACT[i];
        R r = tb_rect(i);
        if (!a) { cui_fill(C, r.x + 5, TOOL_Y + 10, 1, 24, P.border); continue; }
        bool en = action_enabled(a), hv = inr(r, G.mx, G.my) && en && G.menu_open < 0 && !G.dlg;
        if (hv) cui_round_rect(C, r.x, r.y, r.w, r.h, 7, G.mdown ? P.select : P.hover);
        glyph(a, (float)r.x + 17, (float)r.y + 17, en ? P.text : cui_mix(P.muted, P.tool, 110));
    }
    for (int i = 0; i < 4; ++i) {
        R r = view_rect(i);
        bool on = G.view == i, hv = inr(r, G.mx, G.my);
        if (on) cui_round_rect(C, r.x, r.y, r.w, r.h, 14, P.accent);
        else if (hv) cui_round_rect(C, r.x, r.y, r.w, r.h, 14, P.hover);
        cui_text_center(C, FN(12, on), r.x, r.y, r.w, r.h, view_label(i), on ? 0xFFFFFF : P.text);
    }
    cui_fill(C, 0, TOOL_Y + TOOL_H - 1, WIN_W, 1, P.border);
}
static void draw_places(void) {
    cui_fill(C, 0, PLACES_Y, WIN_W, PLACES_H, P.bg);
    int act = mark_active();
    for (int i = 0; i < G.nmarks; ++i) {
        R r = place_rect(i);
        bool on = i == act, hv = inr(r, G.mx, G.my) && G.menu_open < 0;
        bool drop = G.drag_kind == 2 && hv;
        if (on) cui_round_rect(C, r.x, r.y, r.w, r.h, 14, P.select);
        else if (hv) cui_round_rect(C, r.x, r.y, r.w, r.h, 14, P.hover);
        if (drop) cui_round_rect_outline(C, r.x, r.y, r.w, r.h, 14, 2.0f, P.accent);
        if (!strcmp(G.marks[i].path, "/")) {                                           /* the disk: a drive glyph */
            cui_round_rect(C, r.x + 10, r.y + 9, 16, 10, 3, P.muted); cui_fill(C, r.x + 13, r.y + 15, 3, 2, P.bg);
        } else draw_icon(K_FOLDER, r.x + 10, r.y + 6, 16);
        txt(r.x + 32, r.y, r.h, FN(13, on), on ? P.accent : P.text, G.marks[i].name);
    }
    cui_fill(C, 0, PLACES_Y + PLACES_H - 1, WIN_W, 1, P.border);
}
static void draw_addr(void) {
    cui_fill(C, 0, ADDR_Y, WIN_W, ADDR_H, P.tool);
    R f = addr_field(), s = search_rect();
    cui_round_rect(C, f.x, f.y, f.w, f.h, 6, P.panel);
    cui_round_rect_outline(C, f.x, f.y, f.w, f.h, 6, 1.0f, G.addr_edit ? P.accent : P.border);
    cui_font *fn = FN(13, false);
    if (G.addr_edit) {
        txtfit(f.x + 10, f.y, f.h, f.w - 20, fn, P.text, G.addr);
        if (((cos_time_ms() - G.caret_t) / 530) % 2 == 0) cui_fill(C, f.x + 10 + cui_text_width(fn, G.addr), f.y + 6, 1, f.h - 12, P.text);
    } else {
        crumbs_layout();
        int hv = crumb_hit(G.mx, G.my);
        for (int i = 0; i < s_ncrumb; ++i) {
            R r = s_crumb[i].r;
            if (i == hv && G.menu_open < 0) cui_round_rect(C, r.x, r.y + 3, r.w, r.h - 6, 5, P.hover);
            const char *name = i == 0 ? "/" : path_base(s_crumb[i].path);
            bool last = i == s_ncrumb - 1;
            cui_text_center(C, FN(13, last), r.x, r.y, r.w, r.h, name, last ? P.text : P.muted);
            if (!last) { float cx = (float)(r.x + r.w + 7), cy = (float)(r.y + r.h / 2); cui_line(C, cx - 2, cy - 4, cx + 2, cy, 1.3f, P.muted); cui_line(C, cx + 2, cy, cx - 2, cy + 4, 1.3f, P.muted); }
        }
    }
    cui_round_rect(C, s.x, s.y, s.w, s.h, s.h / 2, P.panel);
    cui_round_rect_outline(C, s.x, s.y, s.w, s.h, s.h / 2, 1.0f, G.search_focus ? P.accent : P.border);
    cui_ring(C, (float)(s.x + 16), (float)(s.y + s.h / 2 - 1), 5.0f, 1.5f, P.muted);
    cui_line(C, (float)(s.x + 19.5f), (float)(s.y + s.h / 2 + 2.5f), (float)(s.x + 23), (float)(s.y + s.h / 2 + 6), 1.6f, P.muted);
    if (G.search[0]) {
        txtfit(s.x + 30, s.y, s.h, s.w - 58, fn, P.text, G.search);
        float cx = (float)(s.x + s.w - 14), cy = (float)(s.y + s.h / 2);
        cui_line(C, cx - 4, cy - 4, cx + 4, cy + 4, 1.5f, P.muted); cui_line(C, cx + 4, cy - 4, cx - 4, cy + 4, 1.5f, P.muted);
        if (G.search_focus && ((cos_time_ms() - G.caret_t) / 530) % 2 == 0) cui_fill(C, s.x + 30 + cui_text_width(fn, G.search), s.y + 6, 1, s.h - 12, P.text);
    } else {
        txt(s.x + 30, s.y, s.h, fn, P.muted, L("Search", "検索"));
        if (G.search_focus && ((cos_time_ms() - G.caret_t) / 530) % 2 == 0) cui_fill(C, s.x + 30, s.y + 6, 1, s.h - 12, P.text);
    }
    cui_fill(C, 0, ADDR_Y + ADDR_H - 1, WIN_W, 1, P.border);
}
static void draw_status(void) {
    int y = WIN_H - STATUS_H;
    cui_fill(C, 0, y, WIN_W, STATUS_H, P.side);
    cui_fill(C, 0, y, WIN_W, 1, P.border);
    cui_font *f = FN(12, false);
    char s[200]; int ns = sel_count();
    if (G.job.mode) snprintf(s, sizeof s, "%s", G.job.mode == 3 ? L("Deleting...", "削除中...") : G.job.mode == 2 ? L("Moving...", "移動中...") : L("Copying...", "コピー中..."));
    else if (G.status[0] && cos_time_ms() - G.status_t < 5000) snprintf(s, sizeof s, "%s", G.status);
    else {
        int k = (int)snprintf(s, sizeof s, L("%d items", "%d個の項目"), G.nvis);
        if (ns) {
            uint64_t tot = 0; for (int i = 0; i < G.nvis; ++i) if (ent_at(i)->sel && !ent_at(i)->is_dir) tot += ent_at(i)->size;
            char b[48]; fmt_size(tot, b, sizeof b);
            if (ns == 1) snprintf(s + k, sizeof s - (size_t)k, L("   |   1 selected  %s", "   |   1個選択  %s"), b);
            else snprintf(s + k, sizeof s - (size_t)k, L("   |   %d selected  %s", "   |   %d個選択  %s"), ns, b);
        }
    }
    txt(14, y, STATUS_H, f, P.muted, s);
    if (G.sp_total) {
        char a[48], b[48], r[120]; fmt_size(G.sp_free, a, sizeof a); fmt_size(G.sp_total, b, sizeof b);
        snprintf(r, sizeof r, L("Free: %s of %s", "空き: %s / %s"), a, b);
        txt(WIN_W - 14 - cui_text_width(f, r), y, STATUS_H, f, P.muted, r);
    }
}

/* ---------------------------------------------------------------- */
/* dialogs                                                           */
/* ---------------------------------------------------------------- */
static struct { R r; int id; } s_dbtn[8]; static int s_ndbtn;
static void dbtn(int x, int y, int w, const char *label, int id, bool primary, bool danger) {
    R r = { x, y, w, 30 };
    bool hv = inr(r, G.mx, G.my);
    uint32_t bg = primary ? (danger ? P.danger : P.accent) : P.tool;
    if (hv) bg = cui_mix(bg, primary ? 0xFFFFFF : P.text, primary ? 40 : 20);
    cui_round_rect(C, x, y, w, 30, 7, bg);
    if (!primary) cui_round_rect_outline(C, x, y, w, 30, 7, 1.0f, P.border);
    cui_text_center(C, FN(13, primary), x, y, w, 30, label, primary ? 0xFFFFFF : P.text);
    if (s_ndbtn < 8) { s_dbtn[s_ndbtn].r = r; s_dbtn[s_ndbtn++].id = id; }
}
int dlg_hit(int x, int y) { for (int i = s_ndbtn - 1; i >= 0; --i) if (inr(s_dbtn[i].r, x, y)) return s_dbtn[i].id; return 0; }

static R dlg_frame(int w, int h, const char *title) {
    cui_fill_alpha(C, 0, 0, WIN_W, WIN_H, 0x000000, 110);
    R b = { (WIN_W - w) / 2, (WIN_H - h) / 2 - 10, w, h };
    cui_round_rect_alpha(C, b.x - 3, b.y + 5, b.w + 6, b.h + 6, 14, 0x000000, 80);
    cui_round_rect(C, b.x - 1, b.y - 1, b.w + 2, b.h + 2, 11, P.border);
    cui_round_rect(C, b.x, b.y, b.w, b.h, 10, P.panel);
    txt(b.x + 20, b.y + 8, 34, FN(15, true), P.text, title);
    cui_fill(C, b.x + 1, b.y + 46, b.w - 2, 1, P.border);
    return b;
}
static void field(int x, int y, int w, const char *text) {
    cui_font *f = FN(13, false);
    cui_round_rect(C, x, y, w, 32, 6, P.bg);
    cui_round_rect_outline(C, x, y, w, 32, 6, 1.5f, P.accent);
    cui_clip(C, x + 8, y + 2, w - 16, 28);
    bool has_sel = G.dlg_sel0 != G.dlg_sel1;
    if (has_sel) {
        int a = G.dlg_sel0 < G.dlg_sel1 ? G.dlg_sel0 : G.dlg_sel1, b = G.dlg_sel0 < G.dlg_sel1 ? G.dlg_sel1 : G.dlg_sel0;
        char pre[256]; snprintf(pre, sizeof pre, "%.*s", a, text);
        char sel[256]; snprintf(sel, sizeof sel, "%.*s", b - a, text + a);
        int x0 = x + 10 + cui_text_width(f, pre), w0 = cui_text_width(f, sel);
        cui_fill(C, x0, y + 6, w0 > 0 ? w0 : 2, 20, P.accent);
        cui_text(C, f, x + 10, y + (32 - cui_font_height(f)) / 2, pre, P.text);
        cui_text(C, f, x0, y + (32 - cui_font_height(f)) / 2, sel, 0xFFFFFF);
        cui_text(C, f, x0 + w0, y + (32 - cui_font_height(f)) / 2, text + b, P.text);
    } else {
        txtfit(x + 10, y, 32, w - 20, f, P.text, text);
        if (((cos_time_ms() - G.caret_t) / 530) % 2 == 0) {
            int c = G.dlg_sel0; int n = (int)strlen(text); if (c < 0) c = 0; if (c > n) c = n;
            char pre[256]; snprintf(pre, sizeof pre, "%.*s", c, text);
            cui_fill(C, x + 10 + cui_text_width(f, pre), y + 7, 1, 18, P.text);
        }
    }
    cui_unclip(C);
}
static void draw_dialog(void) {
    s_ndbtn = 0;
    cui_font *fn = FN(13, false), *fs = FN(12, false);
    char s[400];
    switch (G.dlg) {
        case D_INPUT: {
            const char *title = G.dlg_in == IN_RENAME ? L("Rename", "名前の変更") : G.dlg_in == IN_NEWFOLDER ? L("New folder", "新しいフォルダー") : L("New file", "新しいファイル");
            R b = dlg_frame(420, 170, title);
            txt(b.x + 20, b.y + 56, 22, fs, P.muted, L("Name", "名前"));
            field(b.x + 20, b.y + 80, b.w - 40, G.dlg_text);
            dbtn(b.x + b.w - 20 - 90, b.y + 126, 90, "OK", 1, true, false);
            dbtn(b.x + b.w - 20 - 90 - 10 - 90, b.y + 126, 90, L("Cancel", "キャンセル"), 2, false, false);
            break; }
        case D_DELETE: {
            int n = sel_count();
            R b = dlg_frame(440, 168, L("Delete", "削除"));
            Ent *e = NULL; for (int i = 0; i < G.nvis; ++i) if (ent_at(i)->sel) { e = ent_at(i); break; }
            if (n == 1 && e) snprintf(s, sizeof s, L("Delete \"%s\"?", "「%s」を削除しますか？"), e->name); else snprintf(s, sizeof s, L("Delete these %d items?", "%d個の項目を削除しますか？"), n);
            cui_text_fit(C, FN(14, false), b.x + 20, b.y + 62, b.w - 40, s, P.text);
            txt(b.x + 20, b.y + 88, 22, fs, P.muted, L("This cannot be undone. Folders are deleted with everything inside.", "元に戻せません。フォルダーは中身ごと削除されます。"));
            dbtn(b.x + b.w - 20 - 100, b.y + 124, 100, L("Delete", "削除"), 1, true, true);
            dbtn(b.x + b.w - 20 - 100 - 10 - 100, b.y + 124, 100, L("Cancel", "キャンセル"), 2, false, false);
            break; }
        case D_CONFLICT: {
            R b = dlg_frame(480, 210, L("File already exists", "同名のファイルがあります"));
            snprintf(s, sizeof s, L("\"%s\" already exists in the destination.", "移動先に「%s」が既にあります。"), path_base(G.job.conflict_dst));
            cui_text_fit(C, FN(14, false), b.x + 20, b.y + 60, b.w - 40, s, P.text);
            txt(b.x + 20, b.y + 86, 22, fs, P.muted, L("What do you want to do with it?", "どうしますか？"));
            R cb = { b.x + 20, b.y + 118, 18, 18 };
            cui_round_rect(C, cb.x, cb.y, 18, 18, 4, G.dlg_all ? P.accent : P.bg);
            if (!G.dlg_all) cui_round_rect_outline(C, cb.x, cb.y, 18, 18, 4, 1.2f, P.border);
            else { cui_line(C, (float)cb.x + 4, (float)cb.y + 9, (float)cb.x + 8, (float)cb.y + 13, 2.0f, 0xFFFFFF); cui_line(C, (float)cb.x + 8, (float)cb.y + 13, (float)cb.x + 14, (float)cb.y + 5, 2.0f, 0xFFFFFF); }
            txt(cb.x + 28, cb.y - 2, 22, fs, P.text, L("Do this for all conflicts", "以降すべてに適用"));
            if (s_ndbtn < 8) { R hit = { cb.x, cb.y - 4, 260, 26 }; s_dbtn[s_ndbtn].r = hit; s_dbtn[s_ndbtn++].id = 6; }
            dbtn(b.x + b.w - 20 - 130, b.y + 160, 130, L("Keep both", "両方残す"), 5, false, false);
            dbtn(b.x + b.w - 20 - 130 - 10 - 90, b.y + 160, 90, L("Skip", "スキップ"), 4, false, false);
            dbtn(b.x + b.w - 20 - 130 - 10 - 90 - 10 - 100, b.y + 160, 100, L("Replace", "置き換え"), 3, true, false);
            break; }
        case D_PROGRESS: {
            Job *j = &G.job;
            R b = dlg_frame(460, 190, j->mode == 3 ? L("Deleting", "削除中") : j->mode == 2 ? L("Moving", "移動中") : L("Copying", "コピー中"));
            cui_text_fit(C, FN(14, false), b.x + 20, b.y + 60, b.w - 40, j->cur_name, P.text);
            int pct = j->mode == 3 ? (j->nt ? j->cur * 100 / j->nt : 100) : (j->total ? (int)(j->done * 100 / j->total) : (j->nt ? j->cur * 100 / j->nt : 100));
            if (pct > 100) pct = 100;
            cui_round_rect(C, b.x + 20, b.y + 96, b.w - 40, 12, 6, P.bg);
            if (pct > 0) cui_round_rect(C, b.x + 20, b.y + 96, (b.w - 40) * pct / 100 < 12 ? 12 : (b.w - 40) * pct / 100, 12, 6, P.accent);
            char a[48], t[48]; fmt_size(j->done, a, sizeof a); fmt_size(j->total, t, sizeof t);
            if (j->mode == 3) snprintf(s, sizeof s, L("%d of %d", "%d / %d"), j->cur, j->nt); else snprintf(s, sizeof s, "%s / %s   (%d%%)", a, t, pct);
            txt(b.x + 20, b.y + 114, 22, fs, P.muted, s);
            dbtn(b.x + b.w - 20 - 110, b.y + 144, 110, L("Cancel", "キャンセル"), 2, false, false);
            break; }
        case D_PROPS: {
            Ent *e = &G.props_ent;
            R b = dlg_frame(480, e->is_dir ? 320 : 300, L("Properties", "プロパティ"));
            draw_icon(e->kind, b.x + 24, b.y + 62, 48);
            cui_text_fit(C, FN(15, true), b.x + 90, b.y + 66, b.w - 110, e->name, P.text);
            txt(b.x + 90, b.y + 90, 22, fs, P.muted, kind_name(e->kind));
            cui_fill(C, b.x + 20, b.y + 122, b.w - 40, 1, P.border);
            static const char *en[5] = { "Location", "Size", "Modified", "Attributes", "Contains" }, *ja[5] = { "場所", "サイズ", "更新日時", "属性", "内容" };
            char loc[PATHN]; path_parent(G.props_path, loc, sizeof loc);
            char sz[64], sz2[96], dt[64], at[64] = "", ct[96] = "";
            fmt_size(e->is_dir ? G.props_bytes : e->size, sz, sizeof sz);
            snprintf(sz2, sizeof sz2, "%s  (%llu bytes)", sz, (unsigned long long)(e->is_dir ? G.props_bytes : e->size));
            fmt_date(e->mtime, dt, sizeof dt);
            if (e->attr & COS_ATTR_RDONLY) strlcat(at, L("Read-only  ", "読み取り専用  "), sizeof at);
            if (e->attr & COS_ATTR_HIDDEN || e->name[0] == '.') strlcat(at, L("Hidden  ", "隠しファイル  "), sizeof at);
            if (e->attr & COS_ATTR_SYSTEM) strlcat(at, L("System  ", "システム  "), sizeof at);
            if (!at[0]) snprintf(at, sizeof at, "-");
            snprintf(ct, sizeof ct, L("%llu files, %llu folders", "ファイル%llu個、フォルダー%llu個"), (unsigned long long)G.props_files, (unsigned long long)G.props_dirs);
            const char *vals[5] = { loc, sz2, dt, at, ct };
            int rows = e->is_dir ? 5 : 4, y = b.y + 134;
            for (int i = 0; i < rows; ++i) {
                txt(b.x + 24, y, 26, fs, P.muted, G.jp ? ja[i] : en[i]);
                cui_text_fit(C, fn, b.x + 120, ty(fn, y, 26), b.w - 144, vals[i], P.text); y += 30;
            }
            dbtn(b.x + b.w - 20 - 100, b.y + b.h - 46, 100, L("Close", "閉じる"), 1, true, false);
            break; }
        case D_ABOUT: {
            R b = dlg_frame(440, 250, L("About File Manager", "ファイルマネージャーについて"));
            draw_icon(K_FOLDER, b.x + 24, b.y + 62, 56);
            cui_text_fit(C, FN(16, true), b.x + 96, b.y + 64, b.w - 120, L("C-OS File Manager", "C-OS ファイルマネージャー"), P.text);
            txt(b.x + 96, b.y + 90, 22, fs, P.muted, L("A ring-3 .c-os program", "リング3の .c-os プログラム"));
            cui_fill(C, b.x + 20, b.y + 122, b.w - 40, 1, P.border);
            txt(b.x + 24, b.y + 132, 24, fn, P.text, L("Drag files onto a folder to move them (Ctrl = copy).", "フォルダーへドラッグで移動（Ctrl でコピー）"));
            txt(b.x + 24, b.y + 158, 24, fn, P.text, L("Ctrl+C / X / V share the system clipboard.", "Ctrl+C / X / V はシステム共通クリップボード"));
            txt(b.x + 24, b.y + 184, 24, fn, P.text, L("F2 rename   Del delete   Alt+Enter properties", "F2 名前変更   Del 削除   Alt+Enter プロパティ"));
            dbtn(b.x + b.w - 20 - 100, b.y + b.h - 46, 100, L("Close", "閉じる"), 1, true, false);
            break; }
        default: break;
    }
}

/* ---------------------------------------------------------------- */
void ui_draw(void) {
    cui_unclip(C);
    cui_fill(C, 0, 0, WIN_W, WIN_H, P.panel);
    if (G.focus >= 0 && G.focus >= G.nvis) G.focus = G.nvis - 1;
    draw_tree();
    draw_list();
    draw_preview();
    draw_places();
    draw_addr();
    draw_toolbar();
    draw_menubar();
    draw_status();
    /* toolbar tooltip */
    if (G.tip_btn > 0 && G.menu_open < 0 && !G.dlg && cos_time_ms() - G.tip_t > 450) {
        const char *tip = tb_tip(G.tip_btn);
        if (tip) {
            int w = cui_text_width(FN(12, false), tip) + 20;
            for (int i = 0; i < TB_N; ++i) if (TB_ACT[i] == G.tip_btn) {
                R r = tb_rect(i);
                int x = r.x + r.w / 2 - w / 2; if (x < 4) x = 4;
                cui_round_rect(C, x, r.y + r.h + 4, w, 24, 6, P.border);
                cui_round_rect(C, x + 1, r.y + r.h + 5, w - 2, 22, 5, P.panel);
                cui_text_center(C, FN(12, false), x, r.y + r.h + 5, w, 22, tip, P.text);
            }
        }
    }
    /* file drag ghost */
    if (G.drag_kind == 2) {
        int n = sel_count(); char s[64];
        snprintf(s, sizeof s, n == 1 ? L("1 item", "1個") : L("%d items", "%d個"), n);
        if (G.drag_copy) strlcat(s, "  +", sizeof s);
        int w = cui_text_width(FN(12, true), s) + 24;
        cui_round_rect_alpha(C, G.mx + 14, G.my + 10, w, 26, 13, P.accent, 220);
        cui_text_center(C, FN(12, true), G.mx + 14, G.my + 10, w, 26, s, 0xFFFFFF);
    }
    if (G.menu_open >= 0) {
        PI v[32]; int n = menu_build(G.menu_open, v); R box; menu_box(G.menu_open, &box);
        draw_popup(box, v, n, G.menu_hover);
    }
    if (G.ctx_open) { PI v[16]; int n = ctx_build(v); R box = ctx_box(v, n); draw_popup(box, v, n, G.ctx_hover); }
    if (G.dlg) draw_dialog();
}
