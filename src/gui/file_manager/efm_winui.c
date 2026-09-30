/* efm_winui.c - Windows-style shell for the Enhanced File Manager.
 *
 * Layout (top to bottom):
 *   menu bar      File  Edit  View  Go  Help            (drop-down menus)
 *   toolbar       back fwd up refresh | new folder | cut copy paste delete | views | search
 *   places bar    Disk  Desktop  Documents  Music  Pictures  Downloads  Apps  bin
 *   address       breadcrumb of the current folder
 *   body          folder tree | splitter | file list (details with sortable columns)
 *   status bar    items / selection / free space
 *
 * efm_compute_layout() is the single source of truth for where each region
 * is; renderer, click handler and the bridge's double-click detection all
 * use it. Drag and drop (efm_winui_tick) moves/copies files between folders
 * and hands files to other applications.
 */
#include "efm_internal.h"
#include "string.h"
#include "../../fs/fs.h"
#include "keyboard.h"
#include "mouse.h"

extern void vga_draw_line(int x0, int y0, int x1, int y1, uint64_t color);
extern void vga_fill_circle(int cx, int cy, int r, uint64_t color);
extern void vga_blend_rounded_rect(int x, int y, int w, int h, int r, uint64_t color, float alpha);
extern int  vga_get_font_width(void);
extern int  vga_get_font_height(void);
extern void efm_draw_tree(efm_state_t* state, int x, int y, int w, int h);
extern int  efm_tree_hit_test(efm_state_t* state, int x, int y, int tree_x, int tree_y, bool* hit_arrow);
extern void efm_draw_breadcrumb(efm_state_t* state, int x, int y, int w);
extern void efm_draw_file_list(efm_state_t* state, int x, int y, int w, int h);
extern void efm_draw_preview(efm_state_t* state, int x, int y, int w, int h);
extern void efm_draw_image_panel(efm_state_t* state, int x, int y, int w, int h);
extern void efm_draw_dialog(efm_state_t* state, int cx, int cy);
extern void efm_draw_file_context_menu(efm_state_t* state, int mx, int my);
extern void efm_format_time(uint64_t time, char* buf, size_t buf_size);
extern void efm_load_all_thumbnails(efm_state_t* state);
extern void gui_notify(const char* msg, int ms);
extern uint64_t get_timer_ticks(void);

#define FW (vga_get_font_width())
#define FH (vga_get_font_height())
#define TXT(en, ja) (gui_is_japanese() ? (ja) : (en))

static int tw(const char* s) {            /* text width in pixels, UTF-8 aware */
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) if ((*p & 0xC0) != 0x80) ++n;
    /* CJK glyphs are double width in the bitmap font */
    int wide = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) if (*p >= 0xE0) ++wide;
    return (n + wide) * FW;
}
static bool in(int mx, int my, int x, int y, int w, int h) { return mx >= x && my >= y && mx < x + w && my < y + h; }

/* ================================================================== */
/* layout                                                             */
/* ================================================================== */
void efm_compute_layout(const efm_state_t* st, int x, int y, int w, int h, efm_layout_t* L) {
    memset(L, 0, sizeof(*L));
    L->x = x; L->y = y; L->w = w; L->h = h;
    L->menu_y = y;                 L->menu_h = FH + 8;
    L->tool_y = L->menu_y + L->menu_h;   L->tool_h = 34;
    L->places_y = L->tool_y + L->tool_h; L->places_h = 30;
    L->crumb_y = L->places_y + L->places_h; L->crumb_h = 30;
    L->status_h = FH + 8;
    L->status_y = y + h - L->status_h;
    L->body_y = L->crumb_y + L->crumb_h;
    L->body_h = L->status_y - L->body_y;
    int tree_w = st->tree_w > 0 ? st->tree_w : 210;
    if (tree_w < 120) tree_w = 120;
    if (tree_w > w / 2) tree_w = w / 2;
    L->tree_x = x;
    L->tree_w = tree_w;
    L->split_x = x + tree_w;
    L->preview_w = st->show_preview ? EFM_PREVIEW_W : 0;
    L->list_x = L->split_x + 4;
    L->list_w = w - tree_w - 4 - L->preview_w;
    L->preview_x = L->list_x + L->list_w;
    L->header_h = FH + 8;
    L->row_h = FH + 8;
    /* details columns: name takes what is left */
    L->col_size_w = 12 * FW;
    L->col_type_w = 13 * FW;
    L->col_date_w = 17 * FW;
    L->col_attr_w = 6 * FW;
    int fixed = L->col_size_w + L->col_type_w + L->col_date_w + L->col_attr_w;
    L->col_name_w = L->list_w - 12 - fixed;
    if (L->col_name_w < 18 * FW) {            /* narrow: drop Type, then Attr */
        L->col_type_w = 0;
        L->col_name_w = L->list_w - 12 - L->col_size_w - L->col_date_w - L->col_attr_w;
        if (L->col_name_w < 16 * FW) { L->col_attr_w = 0; L->col_name_w = L->list_w - 12 - L->col_size_w - L->col_date_w; }
    }
}

int efm_list_hit_row(const efm_state_t* st, const efm_layout_t* L, int mx, int my) {
    if (!in(mx, my, L->list_x, L->body_y, L->list_w, L->body_h)) return -1;
    int idx;
    if (st->view_mode == EFM_VIEW_DETAILS) {
        int ry = my - (L->body_y + L->header_h);
        if (ry < 0) return -1;
        idx = ry / L->row_h + st->scroll_offset;
    } else if (st->view_mode == EFM_VIEW_LIST) {
        int ry = my - (L->body_y + 4);
        if (ry < 0) return -1;
        idx = ry / 26 + st->scroll_offset;
    } else {
        /* grid views: same geometry as efm_draw_file_list() */
        int cell_w = st->view_mode == EFM_VIEW_ICONS ? 80 : 88, row_h = st->view_mode == EFM_VIEW_ICONS ? 72 : 80;
        int cols = (L->list_w - 16) / cell_w;
        if (cols < 1) cols = 1;
        int col = (mx - (L->list_x + 8)) / (L->list_w / cols);
        int row = (my - (L->body_y + 4)) / row_h;
        if (col < 0 || col >= cols || row < 0) return -1;
        idx = (st->scroll_offset + row) * cols + col;
    }
    return (idx >= 0 && idx < st->entry_count) ? idx : -1;
}

/* ================================================================== */
/* icons (16 px)                                                      */
/* ================================================================== */
static bool ext_is(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    if (n <= e) return false;
    for (size_t i = 0; i < e; ++i) {
        char a = name[n - e + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != b) return false;
    }
    return true;
}
static void icon_page(int x, int y, uint64_t accent) {
    vga_fill_rect(x + 2, y, 9, 16, 0xFFFFFF);
    vga_fill_rect(x + 11, y + 4, 3, 12, 0xFFFFFF);
    vga_draw_rect(x + 2, y, 12, 16, 0x9098A8);
    vga_fill_rect(x + 11, y, 3, 4, 0xDDE2EA);                  /* folded corner */
    vga_draw_line(x + 10, y, x + 13, y + 3, 0x9098A8);
    if (accent) vga_fill_rect(x + 2, y + 13, 12, 3, accent);
}
void efm_icon16(int x, int y, const efm_entry_t* e) {
    if (e->is_dir || e->type == EFM_TYPE_FOLDER || e->type == EFM_TYPE_PARENT) {
        vga_fill_rect(x + 1, y + 3, 6, 2, 0xE8A020);
        vga_fill_rect(x + 1, y + 5, 14, 9, EFM_C_FOLDER);
        vga_fill_rect(x + 1, y + 5, 14, 2, 0xFFD27A);
        return;
    }
    switch (e->type) {
    case EFM_TYPE_AUDIO:
        icon_page(x, y, 0);
        vga_fill_circle(x + 6, y + 11, 2, EFM_C_AUDIO);
        vga_fill_rect(x + 8, y + 4, 1, 8, EFM_C_AUDIO);
        vga_fill_rect(x + 8, y + 4, 3, 2, EFM_C_AUDIO);
        break;
    case EFM_TYPE_IMAGE:
        icon_page(x, y, 0);
        vga_fill_rect(x + 4, y + 4, 8, 8, 0x8CC8F0);
        vga_fill_rect(x + 4, y + 9, 8, 3, EFM_C_IMAGE);
        vga_fill_circle(x + 9, y + 6, 1, 0xF5C542);
        break;
    case EFM_TYPE_EXECUTABLE:
        vga_fill_rect(x, y + 2, 16, 12, 0xFFFFFF);
        vga_draw_rect(x, y + 2, 16, 12, 0x5A6478);
        vga_fill_rect(x, y + 2, 16, 3, EFM_C_ACCENT);
        vga_fill_rect(x + 3, y + 7, 5, 1, 0x9098A8);
        vga_fill_rect(x + 3, y + 9, 8, 1, 0x9098A8);
        break;
    case EFM_TYPE_ARCHIVE:
        icon_page(x, y, 0);
        for (int i = 0; i < 6; ++i) vga_fill_rect(x + 7 + (i & 1), y + 2 + i * 2, 2, 1, EFM_C_ARCHIVE);
        vga_fill_rect(x + 6, y + 13, 4, 2, EFM_C_ARCHIVE);
        break;
    case EFM_TYPE_CODE: case EFM_TYPE_LUA:
        icon_page(x, y, e->type == EFM_TYPE_LUA ? EFM_C_LUA : EFM_C_CODE);
        vga_draw_line(x + 6, y + 5, x + 4, y + 8, EFM_C_CODE);
        vga_draw_line(x + 4, y + 8, x + 6, y + 11, EFM_C_CODE);
        vga_draw_line(x + 9, y + 5, x + 11, y + 8, EFM_C_CODE);
        vga_draw_line(x + 11, y + 8, x + 9, y + 11, EFM_C_CODE);
        break;
    case EFM_TYPE_VIDEO:
        icon_page(x, y, 0);
        vga_fill_rect(x + 4, y + 5, 7, 6, EFM_C_VIDEO);
        break;
    default:
        if (ext_is(e->name, ".html") || ext_is(e->name, ".htm")) {
            icon_page(x, y, 0);
            vga_fill_circle(x + 8, y + 8, 4, EFM_C_CODE);
            vga_fill_rect(x + 4, y + 8, 9, 1, 0xFFFFFF);
        } else {
            icon_page(x, y, 0);
            for (int i = 0; i < 4; ++i) vga_fill_rect(x + 4, y + 4 + i * 2, i == 3 ? 5 : 8, 1, 0xA8B0C0);
        }
    }
}

static const char* type_name(const efm_entry_t* e) {
    if (e->is_dir) return TXT("File folder", "フォルダー");
    switch (e->type) {
    case EFM_TYPE_AUDIO: return ext_is(e->name, ".mp3") ? TXT("MP3 audio", "MP3 音声") : ext_is(e->name, ".ogg") ? TXT("OGG audio", "OGG 音声") : TXT("WAV audio", "WAV 音声");
    case EFM_TYPE_IMAGE: return TXT("Image", "画像");
    case EFM_TYPE_TEXT: return TXT("Text document", "テキスト");
    case EFM_TYPE_CODE: return TXT("Source code", "ソースコード");
    case EFM_TYPE_LUA: return TXT("Lua script", "Lua スクリプト");
    case EFM_TYPE_EXECUTABLE: return TXT("Application", "アプリケーション");
    case EFM_TYPE_ARCHIVE: return TXT("Archive", "アーカイブ");
    case EFM_TYPE_VIDEO: return TXT("Video", "動画");
    case EFM_TYPE_CONFIG: return TXT("Settings", "設定ファイル");
    default: break;
    }
    if (ext_is(e->name, ".html") || ext_is(e->name, ".htm")) return TXT("Web page", "Web ページ");
    return TXT("File", "ファイル");
}

/* "1,474,560" */
static void fmt_thousands(uint64_t v, char* out, size_t cap) {
    char tmp[32];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 30);
    size_t o = 0;
    for (int i = n - 1; i >= 0 && o + 1 < cap; --i) {
        out[o++] = tmp[i];
        if (i > 0 && i % 3 == 0 && o + 1 < cap) out[o++] = ',';
    }
    out[o] = '\0';
}
/* Explorer-style size: "15 KB", "1,024 KB" (KB rounded up), exact bytes under 1 KB */
static void fmt_size(uint64_t bytes, char* out, size_t cap) {
    char num[32];
    if (bytes < 1024) { fmt_thousands(bytes, num, sizeof num); }
    else fmt_thousands((bytes + 1023) / 1024, num, sizeof num);
    size_t n = strlen(num);
    if (n + 4 >= cap) { out[0] = '\0'; return; }
    memcpy(out, num, n);
    memcpy(out + n, bytes < 1024 ? " B" : " KB", bytes < 1024 ? 3 : 4);
}
static void fmt_mb(uint64_t bytes, char* out, size_t cap) {
    char num[32];
    fmt_thousands(bytes / (1024 * 1024), num, sizeof num);
    size_t n = strlen(num);
    if (n + 4 >= cap) { out[0] = '\0'; return; }
    memcpy(out, num, n);
    memcpy(out + n, " MB", 4);
}

/* ================================================================== */
/* menus                                                              */
/* ================================================================== */
enum {
    A_NONE, A_NEW_FOLDER, A_NEW_FILE, A_OPEN, A_RENAME, A_DELETE, A_CLOSE,
    A_CUT, A_COPY, A_PASTE, A_SELECT_ALL,
    A_VIEW_DETAILS, A_VIEW_LIST, A_VIEW_ICONS, A_VIEW_THUMBS, A_TOGGLE_PREVIEW, A_TOGGLE_HIDDEN, A_REFRESH,
    A_BACK, A_FORWARD, A_UP, A_GO_ROOT, A_GO_DESKTOP, A_GO_DOCS, A_GO_MUSIC, A_GO_PICTURES, A_GO_DOWNLOADS, A_GO_BIN, A_GO_STUDIO,
    A_ABOUT, A_SEP
};
typedef struct { int action; const char* en; const char* ja; const char* key; } menu_item_t;
typedef struct { const char* en; const char* ja; const menu_item_t* items; int count; } menu_t;

static const menu_item_t k_file[] = {
    { A_OPEN, "Open", "開く", "Enter" }, { A_SEP, 0, 0, 0 },
    { A_NEW_FOLDER, "New folder", "新しいフォルダー", "" }, { A_NEW_FILE, "New file", "新しいファイル", "" },
    { A_SEP, 0, 0, 0 }, { A_RENAME, "Rename", "名前の変更", "F2" }, { A_DELETE, "Delete", "削除", "Del" },
    { A_SEP, 0, 0, 0 }, { A_CLOSE, "Close", "閉じる", "" } };
static const menu_item_t k_edit[] = {
    { A_CUT, "Cut", "切り取り", "Ctrl+X" }, { A_COPY, "Copy", "コピー", "Ctrl+C" }, { A_PASTE, "Paste", "貼り付け", "Ctrl+V" },
    { A_SEP, 0, 0, 0 }, { A_SELECT_ALL, "Select all", "すべて選択", "Ctrl+A" } };
static const menu_item_t k_view[] = {
    { A_VIEW_DETAILS, "Details", "詳細", "" }, { A_VIEW_LIST, "List", "一覧", "" },
    { A_VIEW_ICONS, "Icons", "アイコン", "" }, { A_VIEW_THUMBS, "Thumbnails", "サムネイル", "" },
    { A_SEP, 0, 0, 0 }, { A_TOGGLE_PREVIEW, "Preview pane", "プレビュー", "" }, { A_TOGGLE_HIDDEN, "Hidden files", "隠しファイル", "" },
    { A_SEP, 0, 0, 0 }, { A_REFRESH, "Refresh", "最新の情報に更新", "F5" } };
static const menu_item_t k_go[] = {
    { A_BACK, "Back", "戻る", "Alt+Left" }, { A_FORWARD, "Forward", "進む", "Alt+Right" }, { A_UP, "Up", "上へ", "Backspace" },
    { A_SEP, 0, 0, 0 }, { A_GO_ROOT, "Disk (/)", "ディスク (/)", "" }, { A_GO_DESKTOP, "Desktop", "デスクトップ", "" },
    { A_GO_DOCS, "Documents", "ドキュメント", "" }, { A_GO_MUSIC, "Music", "ミュージック", "" },
    { A_GO_PICTURES, "Pictures", "ピクチャ", "" }, { A_GO_DOWNLOADS, "Downloads", "ダウンロード", "" }, { A_GO_BIN, "System programs (/bin)", "システム (/bin)", "" },
    { A_GO_STUDIO, "C-OS Studio", "C-OS Studio", "" } };
static const menu_item_t k_help[] = { { A_ABOUT, "About File Manager", "ファイルマネージャーについて", "" } };
static const menu_t k_menus[] = {
    { "File", "ファイル", k_file, (int)(sizeof k_file / sizeof k_file[0]) },
    { "Edit", "編集", k_edit, (int)(sizeof k_edit / sizeof k_edit[0]) },
    { "View", "表示", k_view, (int)(sizeof k_view / sizeof k_view[0]) },
    { "Go", "移動", k_go, (int)(sizeof k_go / sizeof k_go[0]) },
    { "Help", "ヘルプ", k_help, (int)(sizeof k_help / sizeof k_help[0]) } };
#define MENU_COUNT ((int)(sizeof k_menus / sizeof k_menus[0]))

static int menu_title_x(const efm_layout_t* L, int i) {
    int x = L->x + 6;
    for (int k = 0; k < i; ++k) x += tw(TXT(k_menus[k].en, k_menus[k].ja)) + 2 * FW;
    return x;
}
static void menu_box(const efm_layout_t* L, int m, int* bx, int* by, int* bw, int* bh) {
    int w = 0;
    for (int i = 0; i < k_menus[m].count; ++i) {
        const menu_item_t* it = &k_menus[m].items[i];
        if (it->action == A_SEP) continue;
        int iw = tw(TXT(it->en, it->ja)) + tw(it->key) + 7 * FW;
        if (iw > w) w = iw;
    }
    int h = 6;
    for (int i = 0; i < k_menus[m].count; ++i) h += k_menus[m].items[i].action == A_SEP ? 7 : FH + 10;
    *bx = menu_title_x(L, m) - 4; *by = L->menu_y + L->menu_h; *bw = w + 16; *bh = h;
}

static void go(efm_state_t* st, const char* p) { efm_navigate(st, p); efm_tree_rebuild(st); }

static void run_action(efm_state_t* st, int a) {
    switch (a) {
    case A_NEW_FOLDER: st->dialog_type = 2; st->dialog_input[0] = '\0'; st->dialog_cursor = 0; st->dialog_active = true; break;
    case A_NEW_FILE:   st->dialog_type = 1; st->dialog_input[0] = '\0'; st->dialog_cursor = 0; st->dialog_active = true; break;
    case A_OPEN: if (st->focused_index >= 0) efm_open_entry(st, st->focused_index); break;
    case A_RENAME:
        if (st->focused_index >= 0) {
            st->dialog_type = 0;
            strncpy(st->dialog_input, st->entries[st->focused_index].name, sizeof(st->dialog_input) - 1);
            st->dialog_cursor = (int)strlen(st->dialog_input);
            st->dialog_active = true;
        }
        break;
    case A_DELETE:
        if (st->selected_count > 0 || st->focused_index >= 0) {
            st->pending_delete_idx = st->selected_count > 1 ? -1 : st->focused_index;
            st->dialog_type = 4; st->dialog_active = true;
        }
        break;
    case A_CLOSE: st->close_requested = true; break;
    case A_CUT: efm_clipboard_cut_selection(st); break;
    case A_COPY: efm_clipboard_copy_selection(st); break;
    case A_PASTE: efm_paste(st); efm_tree_rebuild(st); break;
    case A_SELECT_ALL: efm_select_all(st); break;
    case A_VIEW_DETAILS: st->view_mode = EFM_VIEW_DETAILS; break;
    case A_VIEW_LIST: st->view_mode = EFM_VIEW_LIST; break;
    case A_VIEW_ICONS: st->view_mode = EFM_VIEW_ICONS; break;
    case A_VIEW_THUMBS: st->view_mode = EFM_VIEW_THUMBNAILS; efm_load_all_thumbnails(st); break;
    case A_TOGGLE_PREVIEW: st->show_preview = !st->show_preview; break;
    case A_TOGGLE_HIDDEN: st->show_hidden = !st->show_hidden; efm_refresh(st); break;
    case A_REFRESH: efm_refresh(st); efm_tree_rebuild(st); break;
    case A_BACK: efm_navigate_back(st); efm_tree_rebuild(st); break;
    case A_FORWARD: efm_navigate_forward(st); efm_tree_rebuild(st); break;
    case A_UP: efm_navigate_up(st); efm_tree_rebuild(st); break;
    case A_GO_ROOT: go(st, "/"); break;
    case A_GO_DESKTOP: go(st, "/desktop"); break;
    case A_GO_DOCS: go(st, "/documents"); break;
    case A_GO_MUSIC: go(st, "/music"); break;
    case A_GO_PICTURES: go(st, "/pictures"); break;
    case A_GO_DOWNLOADS: go(st, "/downloads"); break;
    case A_GO_BIN: go(st, "/bin"); break;
    case A_GO_STUDIO: go(st, "/C-OS Studio"); break;
    case A_ABOUT: gui_notify(TXT("C-OS File Manager - drag files onto folders or apps", "C-OS ファイルマネージャー - ファイルはフォルダーやアプリへドラッグできます"), 2500); break;
    default: break;
    }
}

/* ================================================================== */
/* toolbar / places                                                   */
/* ================================================================== */
enum { T_BACK, T_FWD, T_UP, T_REFRESH, T_NEWDIR, T_CUT, T_COPY, T_PASTE, T_DELETE, T_COUNT };
static const int k_tool_action[T_COUNT] = { A_BACK, A_FORWARD, A_UP, A_REFRESH, A_NEW_FOLDER, A_CUT, A_COPY, A_PASTE, A_DELETE };
static int tool_x(const efm_layout_t* L, int i) {
    int x = L->x + 6 + i * 32;
    if (i >= T_REFRESH) x += 8;
    if (i >= T_NEWDIR) x += 8;
    if (i >= T_CUT) x += 8;
    return x;
}
static void tool_icon(int i, int x, int y, uint64_t c) {
    int cx = x + 14, cy = y + 13;
    switch (i) {
    case T_BACK: vga_draw_line(cx + 4, cy, cx - 5, cy, c); vga_draw_line(cx - 5, cy, cx - 1, cy - 4, c); vga_draw_line(cx - 5, cy, cx - 1, cy + 4, c); break;
    case T_FWD:  vga_draw_line(cx - 4, cy, cx + 5, cy, c); vga_draw_line(cx + 5, cy, cx + 1, cy - 4, c); vga_draw_line(cx + 5, cy, cx + 1, cy + 4, c); break;
    case T_UP:   vga_draw_line(cx, cy + 5, cx, cy - 5, c); vga_draw_line(cx, cy - 5, cx - 4, cy - 1, c); vga_draw_line(cx, cy - 5, cx + 4, cy - 1, c); break;
    case T_REFRESH:
        for (int a = 0; a < 7; ++a) {                          /* 3/4 circle as short segments */
            static const int px[8] = { 5, 4, 0, -4, -5, -4, 0, 4 }, py[8] = { 0, -4, -5, -4, 0, 4, 5, 4 };
            vga_draw_line(cx + px[a], cy + py[a], cx + px[a + 1], cy + py[a + 1], c);
        }
        vga_draw_line(cx + 5, cy, cx + 5, cy - 4, c); vga_draw_line(cx + 5, cy, cx + 1, cy, c);
        break;
    case T_NEWDIR:
        vga_fill_rect(cx - 7, cy - 4, 5, 2, 0xE8A020); vga_fill_rect(cx - 7, cy - 2, 12, 8, EFM_C_FOLDER);
        vga_fill_rect(cx + 4, cy - 7, 1, 7, EFM_C_IMAGE); vga_fill_rect(cx + 1, cy - 4, 7, 1, EFM_C_IMAGE);
        break;
    case T_CUT:
        vga_draw_line(cx - 4, cy - 6, cx + 3, cy + 3, c); vga_draw_line(cx + 4, cy - 6, cx - 3, cy + 3, c);
        vga_fill_circle(cx - 4, cy + 5, 2, c); vga_fill_circle(cx + 4, cy + 5, 2, c);
        break;
    case T_COPY: vga_draw_rect(cx - 6, cy - 6, 8, 10, c); vga_fill_rect(cx - 2, cy - 2, 8, 10, EFM_C_PANEL); vga_draw_rect(cx - 2, cy - 2, 8, 10, c); break;
    case T_PASTE: vga_draw_rect(cx - 5, cy - 5, 10, 12, c); vga_fill_rect(cx - 2, cy - 7, 4, 3, c); vga_fill_rect(cx - 2, cy - 1, 5, 1, c); vga_fill_rect(cx - 2, cy + 2, 5, 1, c); break;
    case T_DELETE: vga_draw_line(cx - 5, cy - 5, cx + 5, cy + 5, EFM_C_DANGER); vga_draw_line(cx + 5, cy - 5, cx - 5, cy + 5, EFM_C_DANGER);
                   vga_draw_line(cx - 4, cy - 5, cx + 6, cy + 5, EFM_C_DANGER); vga_draw_line(cx + 6, cy - 5, cx - 4, cy + 5, EFM_C_DANGER); break;
    }
}
static bool tool_enabled(const efm_state_t* st, int i) {
    switch (i) {
    case T_BACK: return st->history_pos > 0;
    case T_FWD: return st->history_pos < st->history_count - 1;
    case T_UP: return !(st->current_path[0] == '/' && st->current_path[1] == '\0');
    case T_CUT: case T_COPY: case T_DELETE: return st->selected_count > 0 || st->focused_index >= 0;
    case T_PASTE: return st->clipboard_count > 0;
    default: return true;
    }
}
static const char* k_view_en[4] = { "List", "Icons", "Details", "Thumbs" };
static const char* k_view_ja[4] = { "一覧", "アイコン", "詳細", "サムネイル" };
static int view_btn_x(const efm_layout_t* L, int i, int* w) {
    int x = tool_x(L, T_COUNT) + 12;
    static const int order[4] = { EFM_VIEW_DETAILS, EFM_VIEW_LIST, EFM_VIEW_ICONS, EFM_VIEW_THUMBNAILS };
    for (int k = 0; k < 4; ++k) {
        int bw = tw(TXT(k_view_en[order[k]], k_view_ja[order[k]])) + 14;
        if (order[k] == i) { *w = bw; return x; }
        x += bw;
    }
    *w = 0;
    return x;
}

typedef struct { const char* en; const char* ja; const char* path; } place_t;
static const place_t k_places[] = {
    { "Disk", "ディスク", "/" }, { "C-OS Studio", "C-OS Studio", "/C-OS Studio" }, { "Desktop", "デスクトップ", "/desktop" }, { "Documents", "ドキュメント", "/documents" },
    { "Music", "ミュージック", "/music" }, { "Pictures", "ピクチャ", "/pictures" }, { "Downloads", "ダウンロード", "/downloads" },
    { "Apps", "アプリ", "/apps" }, { "bin", "bin", "/bin" } };
#define PLACE_COUNT ((int)(sizeof k_places / sizeof k_places[0]))
static int place_x(const efm_layout_t* L, int i, int* w) {
    int x = L->x + 6;
    for (int k = 0; k <= i; ++k) {
        int bw = tw(TXT(k_places[k].en, k_places[k].ja)) + 30;
        if (k == i) { *w = bw; return x; }
        x += bw + 4;
    }
    return x;
}
static bool path_is_or_under(const char* cur, const char* p) {
    size_t n = strlen(p);
    if (n == 1) return cur[0] == '/' && cur[1] == '\0';
    return strncmp(cur, p, n) == 0 && (cur[n] == '\0' || cur[n] == '/');
}

/* ================================================================== */
/* drawing                                                            */
/* ================================================================== */
static void draw_details(efm_state_t* st, const efm_layout_t* L) {
    int x = L->list_x, y = L->body_y, w = L->list_w, h = L->body_h;
    vga_fill_rect(x, y, w, h, EFM_C_PANEL);
    vga_draw_rect(x, y, w, h, EFM_C_BORDER);
    /* column header */
    vga_fill_rect(x + 1, y + 1, w - 2, L->header_h - 1, EFM_C_TOOLBAR);
    vga_fill_rect(x + 1, y + L->header_h, w - 2, 1, EFM_C_BORDER);
    struct { const char* en; const char* ja; int w; int sort; bool right; } cols[5] = {
        { "Name", "名前", L->col_name_w, EFM_SORT_NAME, false }, { "Size", "サイズ", L->col_size_w, EFM_SORT_SIZE, true },
        { "Type", "種類", L->col_type_w, EFM_SORT_TYPE, false }, { "Modified", "更新日時", L->col_date_w, EFM_SORT_DATE, false },
        { "Attr", "属性", L->col_attr_w, -1, false } };
    int cx = x + 6;
    int ty = y + (L->header_h - FH) / 2;
    for (int c = 0; c < 5; ++c) {
        if (cols[c].w <= 0) continue;
        const char* label = TXT(cols[c].en, cols[c].ja);
        int lx = cols[c].right ? cx + cols[c].w - tw(label) - 10 : cx + (c == 0 ? 22 : 4);
        vga_draw_string(lx, ty, label, EFM_C_MUTED, 0xFFFFFFFF);
        if (cols[c].sort >= 0 && (int)st->sort_mode == cols[c].sort) {          /* sort arrow */
            int ax = cols[c].right ? lx - 10 : lx + tw(label) + 6, ay = y + L->header_h / 2;
            if (st->sort_reverse) { vga_draw_line(ax - 3, ay - 2, ax, ay + 2, EFM_C_MUTED); vga_draw_line(ax, ay + 2, ax + 3, ay - 2, EFM_C_MUTED); }
            else { vga_draw_line(ax - 3, ay + 2, ax, ay - 2, EFM_C_MUTED); vga_draw_line(ax, ay - 2, ax + 3, ay + 2, EFM_C_MUTED); }
        }
        cx += cols[c].w;
        if (c < 4) vga_fill_rect(cx - 1, y + 4, 1, L->header_h - 8, EFM_C_BORDER);
    }
    /* rows */
    int ry0 = y + L->header_h + 1;
    int visible = (h - L->header_h - 2) / L->row_h;
    if (st->scroll_offset > st->entry_count - visible) st->scroll_offset = st->entry_count - visible;
    if (st->scroll_offset < 0) st->scroll_offset = 0;
    if (st->entry_count == 0) {
        vga_draw_string(x + 24, ry0 + 12, TXT("This folder is empty.", "このフォルダーは空です。"), EFM_C_MUTED, 0xFFFFFFFF);
    }
    for (int r = 0; r < visible; ++r) {
        int i = st->scroll_offset + r;
        if (i >= st->entry_count) break;
        efm_entry_t* e = &st->entries[i];
        int ry = ry0 + r * L->row_h;
        bool drop_target = st->dnd_active && e->is_dir && st->hover_row == i;
        if (e->selected) vga_fill_rect(x + 2, ry, w - 4, L->row_h, EFM_C_SELECT);
        else if (drop_target) vga_fill_rect(x + 2, ry, w - 4, L->row_h, EFM_C_HOVER);
        else if (i == st->hover_row) vga_fill_rect(x + 2, ry, w - 4, L->row_h, EFM_C_HOVER);
        else if (r & 1) vga_fill_rect(x + 2, ry, w - 4, L->row_h, gui_dark_mode ? 0x1D2130 : 0xF8FAFD);
        if (i == st->focused_index) vga_draw_rect(x + 2, ry, w - 4, L->row_h, EFM_C_ACCENT);
        if (drop_target) vga_draw_rect(x + 2, ry, w - 4, L->row_h, EFM_C_ACCENT);
        int tyr = ry + (L->row_h - FH) / 2;
        int c0 = x + 6;
        efm_icon16(c0 + 2, ry + (L->row_h - 16) / 2, e);
        /* name, clipped to its column */
        char nm[96];
        int maxc = (L->col_name_w - 30) / FW;
        if (maxc < 4) maxc = 4;
        if (maxc > (int)sizeof(nm) - 4) maxc = (int)sizeof(nm) - 4;
        size_t nl = strlen(e->name);
        if ((int)nl > maxc) { memcpy(nm, e->name, (size_t)maxc - 2); strcpy(nm + maxc - 2, ".."); }
        else strcpy(nm, e->name);
        vga_draw_string(c0 + 22, tyr, nm, EFM_C_TEXT, 0xFFFFFFFF);
        c0 += L->col_name_w;
        if (!e->is_dir) {
            char sz[32];
            fmt_size(e->size, sz, sizeof sz);
            vga_draw_string(c0 + L->col_size_w - tw(sz) - 10, tyr, sz, EFM_C_TEXT, 0xFFFFFFFF);
        }
        c0 += L->col_size_w;
        if (L->col_type_w) { vga_draw_string(c0 + 4, tyr, type_name(e), EFM_C_MUTED, 0xFFFFFFFF); c0 += L->col_type_w; }
        char tbuf[32];
        efm_format_time(e->modified_time, tbuf, sizeof tbuf);
        vga_draw_string(c0 + 4, tyr, tbuf, EFM_C_MUTED, 0xFFFFFFFF);
        c0 += L->col_date_w;
        if (L->col_attr_w) {
            char at[5] = { '-', '-', '-', '-', 0 };
            if (e->attr & 0x01) at[0] = 'R';
            if (e->attr & 0x02) at[1] = 'H';
            if (e->attr & 0x04) at[2] = 'S';
            if (e->attr & 0x20) at[3] = 'A';
            vga_draw_string(c0 + 4, tyr, at, EFM_C_MUTED, 0xFFFFFFFF);
        }
    }
    /* scrollbar */
    if (st->entry_count > visible && visible > 0) {
        int track_h = h - L->header_h - 4;
        int th = track_h * visible / st->entry_count;
        if (th < 16) th = 16;
        int tyb = y + L->header_h + 2 + (track_h - th) * st->scroll_offset / (st->entry_count - visible);
        vga_fill_rounded_rect(x + w - 7, tyb, 4, th, 2, EFM_C_BORDER);
    }
}

void efm_winui_draw(efm_state_t* st, int x, int y, int w, int h) {
    if (!st || !st->initialized) return;
    efm_layout_t L;
    efm_compute_layout(st, x, y, w, h, &L);
    vga_fill_rect(x, y, w, h, EFM_C_BG);

    /* menu bar */
    vga_fill_rect(x, L.menu_y, w, L.menu_h, EFM_C_PANEL);
    for (int m = 0; m < MENU_COUNT; ++m) {
        int mx = menu_title_x(&L, m);
        const char* t = TXT(k_menus[m].en, k_menus[m].ja);
        if (st->open_menu == m) vga_fill_rect(mx - 4, L.menu_y + 1, tw(t) + 8, L.menu_h - 2, EFM_C_SELECT);
        vga_draw_string(mx, L.menu_y + 4, t, EFM_C_TEXT, 0xFFFFFFFF);
    }
    vga_fill_rect(x, L.menu_y + L.menu_h - 1, w, 1, EFM_C_BORDER);

    /* toolbar */
    vga_fill_rect(x, L.tool_y, w, L.tool_h, EFM_C_TOOLBAR);
    for (int i = 0; i < T_COUNT; ++i) {
        int bx = tool_x(&L, i), by = L.tool_y + 3;
        bool en = tool_enabled(st, i);
        if (en && in(mouse.x, mouse.y, bx, by, 28, 28)) vga_fill_rounded_rect(bx, by, 28, 28, 5, EFM_C_HOVER);
        tool_icon(i, bx, by + 1, en ? EFM_C_TEXT : 0xB8BECA);
    }
    vga_fill_rect(tool_x(&L, T_REFRESH) - 6, L.tool_y + 8, 1, L.tool_h - 16, EFM_C_BORDER);
    vga_fill_rect(tool_x(&L, T_NEWDIR) - 6, L.tool_y + 8, 1, L.tool_h - 16, EFM_C_BORDER);
    vga_fill_rect(tool_x(&L, T_CUT) - 6, L.tool_y + 8, 1, L.tool_h - 16, EFM_C_BORDER);
    for (int v = 0; v < 4; ++v) {
        int bw, bx = view_btn_x(&L, v, &bw);
        bool on = (int)st->view_mode == v;
        if (on) vga_fill_rounded_rect(bx, L.tool_y + 5, bw, 24, 5, EFM_C_ACCENT);
        vga_draw_string(bx + 7, L.tool_y + 5 + (24 - FH) / 2, TXT(k_view_en[v], k_view_ja[v]), on ? 0xFFFFFF : EFM_C_TEXT, 0xFFFFFFFF);
    }
    int sw = 180, sx = x + w - sw - 8;
    if (sx > view_btn_x(&L, EFM_VIEW_THUMBNAILS, &sw) + 60) {
        sw = 180;
        vga_fill_rounded_rect(sx, L.tool_y + 5, sw, 24, 6, EFM_C_PANEL);
        vga_draw_rounded_rect(sx, L.tool_y + 5, sw, 24, 6, st->search_active ? EFM_C_ACCENT : EFM_C_BORDER);
        vga_draw_string(sx + 10, L.tool_y + 5 + (24 - FH) / 2,
                        st->search_text[0] ? st->search_text : TXT("Search", "検索"),
                        st->search_text[0] ? EFM_C_TEXT : EFM_C_MUTED, 0xFFFFFFFF);
    }

    /* places bar (the "drive bar") */
    vga_fill_rect(x, L.places_y, w, L.places_h, EFM_C_TOOLBAR);
    for (int p = 0; p < PLACE_COUNT; ++p) {
        int pw, px = place_x(&L, p, &pw);
        if (px + pw > x + w - 4) break;
        bool cur = path_is_or_under(st->current_path, k_places[p].path) &&
                   (p == 0 ? (st->current_path[1] == '\0') : true);
        bool hov = in(mouse.x, mouse.y, px, L.places_y + 3, pw, L.places_h - 6);
        bool drop = st->dnd_active && hov;
        if (cur || hov || drop) vga_fill_rounded_rect(px, L.places_y + 3, pw, L.places_h - 6, 6, cur ? EFM_C_SELECT : EFM_C_HOVER);
        if (drop) vga_draw_rounded_rect(px, L.places_y + 3, pw, L.places_h - 6, 6, EFM_C_ACCENT);
        efm_entry_t fake;
        memset(&fake, 0, sizeof fake);
        fake.is_dir = p != 0;
        if (p == 0) {                                  /* disk icon */
            vga_fill_rounded_rect(px + 6, L.places_y + 9, 16, 11, 2, 0x8A93A6);
            vga_fill_rect(px + 8, L.places_y + 16, 12, 2, 0xD8DDE6);
            vga_fill_rect(px + 18, L.places_y + 11, 2, 2, EFM_C_IMAGE);
        } else efm_icon16(px + 6, L.places_y + 7, &fake);
        vga_draw_string(px + 26, L.places_y + (L.places_h - FH) / 2, TXT(k_places[p].en, k_places[p].ja), EFM_C_TEXT, 0xFFFFFFFF);
    }
    vga_fill_rect(x, L.places_y + L.places_h - 1, w, 1, EFM_C_BORDER);

    /* address */
    efm_draw_breadcrumb(st, x + 4, L.crumb_y, w - 8);

    /* tree + splitter */
    efm_draw_tree(st, L.tree_x, L.body_y, L.tree_w, L.body_h);
    vga_fill_rect(L.split_x, L.body_y, 4, L.body_h, st->split_drag ? EFM_C_ACCENT : EFM_C_BG);
    vga_fill_rect(L.split_x + 1, L.body_y + L.body_h / 2 - 10, 2, 20, EFM_C_BORDER);

    /* list */
    if (st->view_mode == EFM_VIEW_DETAILS) draw_details(st, &L);
    else efm_draw_file_list(st, L.list_x, L.body_y, L.list_w, L.body_h);

    if (st->show_preview) {
        efm_draw_preview(st, L.preview_x, L.body_y, L.preview_w, L.body_h);
        efm_draw_image_panel(st, L.preview_x, L.body_y, L.preview_w, L.body_h);
    }

    /* status bar */
    vga_fill_rect(x, L.status_y, w, L.status_h, EFM_C_TOOLBAR);
    vga_fill_rect(x, L.status_y, w, 1, EFM_C_BORDER);
    char s1[96], num[32];
    fmt_thousands((uint64_t)st->entry_count, num, sizeof num);
    strcpy(s1, num);
    strcat(s1, TXT(" items", " 個の項目"));
    int sel = 0;
    uint64_t sel_bytes = 0;
    for (int i = 0; i < st->entry_count; ++i) if (st->entries[i].selected) { ++sel; sel_bytes += st->entries[i].size; }
    if (sel) {
        char sz[32];
        fmt_size(sel_bytes, sz, sizeof sz);
        fmt_thousands((uint64_t)sel, num, sizeof num);
        strcat(s1, "   |   ");
        strcat(s1, num);
        strcat(s1, TXT(" selected  ", " 個選択  "));
        strcat(s1, sz);
    }
    vga_draw_string(x + 8, L.status_y + 4, s1, EFM_C_TEXT, 0xFFFFFFFF);
    static uint64_t s_space_tick = 0, s_total = 0, s_free = 0;
    uint64_t now = get_timer_ticks();
    if (!s_space_tick || now - s_space_tick > 3000) { fs_get_space(&s_total, &s_free); s_space_tick = now ? now : 1; }
    if (s_total) {
        char a[32], b[32], s2[96];
        fmt_mb(s_free, a, sizeof a);
        fmt_mb(s_total, b, sizeof b);
        strcpy(s2, TXT("Free: ", "空き: "));
        strcat(s2, a);
        strcat(s2, TXT(" of ", " / "));
        strcat(s2, b);
        vga_draw_string(x + w - tw(s2) - 10, L.status_y + 4, s2, EFM_C_MUTED, 0xFFFFFFFF);
    }

    if (st->dialog_active) efm_draw_dialog(st, x + w / 2, y + h / 2);
    if (st->context_menu_visible) efm_draw_file_context_menu(st, mouse.x, mouse.y);

    /* open drop-down menu (on top of everything in this window) */
    if (st->open_menu >= 0) {
        int bx, by, bw, bh;
        menu_box(&L, st->open_menu, &bx, &by, &bw, &bh);
        vga_fill_rect(bx + 3, by + 3, bw, bh, gui_dark_mode ? 0x0A0C12 : 0xC8CED8);   /* shadow */
        vga_fill_rect(bx, by, bw, bh, EFM_C_PANEL);
        vga_draw_rect(bx, by, bw, bh, EFM_C_BORDER);
        int iy = by + 3;
        for (int i = 0; i < k_menus[st->open_menu].count; ++i) {
            const menu_item_t* it = &k_menus[st->open_menu].items[i];
            if (it->action == A_SEP) { vga_fill_rect(bx + 6, iy + 3, bw - 12, 1, EFM_C_BORDER); iy += 7; continue; }
            int ih = FH + 10;
            bool hov = in(mouse.x, mouse.y, bx, iy, bw, ih);
            if (hov) vga_fill_rect(bx + 2, iy, bw - 4, ih, EFM_C_SELECT);
            bool checked = (it->action == A_TOGGLE_PREVIEW && st->show_preview) || (it->action == A_TOGGLE_HIDDEN && st->show_hidden) ||
                           (it->action == A_VIEW_DETAILS && st->view_mode == EFM_VIEW_DETAILS) || (it->action == A_VIEW_LIST && st->view_mode == EFM_VIEW_LIST) ||
                           (it->action == A_VIEW_ICONS && st->view_mode == EFM_VIEW_ICONS) || (it->action == A_VIEW_THUMBS && st->view_mode == EFM_VIEW_THUMBNAILS);
            if (checked) vga_fill_circle(bx + 10, iy + ih / 2, 3, EFM_C_ACCENT);
            vga_draw_string(bx + 20, iy + 5, TXT(it->en, it->ja), EFM_C_TEXT, 0xFFFFFFFF);
            if (it->key && it->key[0]) vga_draw_string(bx + bw - tw(it->key) - 10, iy + 5, it->key, EFM_C_MUTED, 0xFFFFFFFF);
            iy += ih;
        }
    }

    /* drag ghost */
    if (st->dnd_active) {
        char label[64];
        if (st->dnd_count > 1) {
            fmt_thousands((uint64_t)st->dnd_count, num, sizeof num);
            strcpy(label, num);
            strcat(label, TXT(" items", " 個の項目"));
        } else {
            const char* p = strrchr(st->dnd_paths[0], '/');
            strncpy(label, p ? p + 1 : st->dnd_paths[0], sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
        }
        int gx = mouse.x + 14, gy = mouse.y + 14, gw = tw(label) + 34;
        vga_blend_rounded_rect(gx, gy, gw, FH + 12, 6, EFM_C_ACCENT, 0.85f);
        vga_draw_string(gx + 26, gy + 6, label, 0xFFFFFF, 0xFFFFFFFF);
        efm_entry_t fake;
        memset(&fake, 0, sizeof fake);
        if (st->dnd_item >= 0 && st->dnd_item < st->entry_count) fake = st->entries[st->dnd_item];
        efm_icon16(gx + 6, gy + (FH + 12 - 16) / 2, &fake);
    }
}

/* ================================================================== */
/* input                                                              */
/* ================================================================== */
bool efm_winui_click(efm_state_t* st, int mx, int my, int x, int y, int w, int h, bool right_click) {
    efm_layout_t L;
    efm_compute_layout(st, x, y, w, h, &L);

    /* an open drop-down eats the click */
    if (st->open_menu >= 0) {
        int bx, by, bw, bh;
        menu_box(&L, st->open_menu, &bx, &by, &bw, &bh);
        int m = st->open_menu;
        if (!right_click && in(mx, my, bx, by, bw, bh)) {
            int iy = by + 3;
            for (int i = 0; i < k_menus[m].count; ++i) {
                const menu_item_t* it = &k_menus[m].items[i];
                int ih = it->action == A_SEP ? 7 : FH + 10;
                if (it->action != A_SEP && my >= iy && my < iy + ih) { st->open_menu = -1; run_action(st, it->action); return true; }
                iy += ih;
            }
            return true;
        }
        st->open_menu = -1;
        if (in(mx, my, x, L.menu_y, w, L.menu_h)) {
            for (int k = 0; k < MENU_COUNT; ++k) {
                int tx = menu_title_x(&L, k), tww = tw(TXT(k_menus[k].en, k_menus[k].ja));
                if (mx >= tx - 4 && mx < tx + tww + 4 && k != m) { st->open_menu = k; return true; }
            }
        }
        return true;
    }
    if (right_click) return false;      /* item/background menus: efm_handle_click (ecm) */

    /* menu bar */
    if (in(mx, my, x, L.menu_y, w, L.menu_h)) {
        for (int k = 0; k < MENU_COUNT; ++k) {
            int tx = menu_title_x(&L, k), tww = tw(TXT(k_menus[k].en, k_menus[k].ja));
            if (mx >= tx - 4 && mx < tx + tww + 4) { st->open_menu = k; return true; }
        }
        return true;
    }
    /* toolbar */
    if (in(mx, my, x, L.tool_y, w, L.tool_h)) {
        for (int i = 0; i < T_COUNT; ++i)
            if (in(mx, my, tool_x(&L, i), L.tool_y + 3, 28, 28)) { if (tool_enabled(st, i)) run_action(st, k_tool_action[i]); return true; }
        for (int v = 0; v < 4; ++v) {
            int bw, bx = view_btn_x(&L, v, &bw);
            if (in(mx, my, bx, L.tool_y + 5, bw, 24)) {
                st->view_mode = (efm_view_mode_t)v;
                if (v == EFM_VIEW_THUMBNAILS) efm_load_all_thumbnails(st);
                return true;
            }
        }
        if (mx >= x + w - 188) { st->search_active = true; return true; }
        return true;
    }
    /* places */
    if (in(mx, my, x, L.places_y, w, L.places_h)) {
        for (int p = 0; p < PLACE_COUNT; ++p) {
            int pw, px = place_x(&L, p, &pw);
            if (in(mx, my, px, L.places_y + 3, pw, L.places_h - 6)) { go(st, k_places[p].path); return true; }
        }
        return true;
    }
    /* splitter */
    if (mx >= L.split_x - 1 && mx < L.split_x + 5 && my >= L.body_y && my < L.body_y + L.body_h) {
        st->split_drag = true;
        return true;
    }
    /* details column header: sort */
    if (st->view_mode == EFM_VIEW_DETAILS && in(mx, my, L.list_x, L.body_y, L.list_w, L.header_h)) {
        int cx = L.list_x + 6;
        int widths[4] = { L.col_name_w, L.col_size_w, L.col_type_w, L.col_date_w };
        int sorts[4] = { EFM_SORT_NAME, EFM_SORT_SIZE, EFM_SORT_TYPE, EFM_SORT_DATE };
        for (int c = 0; c < 4; ++c) {
            if (widths[c] > 0 && mx >= cx && mx < cx + widths[c]) {
                bool rev = ((int)st->sort_mode == sorts[c]) ? !st->sort_reverse : false;
                efm_sort(st, (efm_sort_mode_t)sorts[c], rev);
                return true;
            }
            cx += widths[c];
        }
        return true;
    }
    return false;       /* tree / list / dialog: handled by efm_handle_click */
}

/* Arms drag-and-drop when a list item is pressed (called by the click
 * handler after it updated the selection). */
void efm_winui_arm_drag(efm_state_t* st, int idx, int mx, int my) {
    if (idx < 0 || idx >= st->entry_count || st->entries[idx].type == EFM_TYPE_PARENT) return;
    st->dnd_armed = true;
    st->dnd_active = false;
    st->dnd_item = idx;
    st->dnd_start_x = mx;
    st->dnd_start_y = my;
}

/* ---- drop ---- */
static void join_path(char* out, size_t cap, const char* dir, const char* name) {
    size_t n = strlen(dir);
    if (n + 2 >= cap) { out[0] = '\0'; return; }
    memcpy(out, dir, n);
    if (n == 0 || dir[n - 1] != '/') out[n++] = '/';
    size_t m = strlen(name);
    if (n + m >= cap) m = cap - n - 1;
    memcpy(out + n, name, m);
    out[n + m] = '\0';
}
/* Moves (or, with copy, copies) each path into dest_dir and reports the
 * result. Shared by drops inside a file manager, onto another file manager
 * and onto the desktop. */
void efm_drop_paths_into(const char (*paths)[EFM_MAX_PATH], int count, const char* dest_dir, bool copy) {
    extern bool fs_copy_path(const char* src, const char* dst);
    extern bool fs_move_path(const char* src, const char* dst);
    int ok = 0, fail = 0;
    for (int i = 0; i < count; ++i) {
        const char* src = paths[i];
        const char* base = strrchr(src, '/');
        base = base ? base + 1 : src;
        char dst[EFM_MAX_PATH];
        join_path(dst, sizeof dst, dest_dir, base);
        if (strcmp(src, dst) == 0) continue;                        /* same folder */
        size_t sl = strlen(src);                                    /* a folder into itself */
        if (strncmp(dest_dir, src, sl) == 0 && (dest_dir[sl] == '/' || dest_dir[sl] == '\0')) { ++fail; continue; }
        if (copy ? fs_copy_path(src, dst) : fs_move_path(src, dst)) ++ok; else ++fail;
    }
    char msg[96], num[16];
    fmt_thousands((uint64_t)ok, num, sizeof num);
    strcpy(msg, num);
    strcat(msg, copy ? TXT(" item(s) copied to ", " 個をコピー: ") : TXT(" item(s) moved to ", " 個を移動: "));
    strncat(msg, dest_dir, sizeof(msg) - strlen(msg) - 1);
    if (fail) strncat(msg, TXT("  (some failed)", "  (一部失敗)"), sizeof(msg) - strlen(msg) - 1);
    gui_notify(msg, 2200);
}

static void drop_into_folder(efm_state_t* st, const char* dest_dir, bool copy) {
    efm_drop_paths_into((const char (*)[EFM_MAX_PATH])st->dnd_paths, st->dnd_count, dest_dir, copy);
    efm_refresh(st);
    efm_tree_rebuild(st);
}

/* Per frame (from the bridge's draw): hover, splitter drag, drag and drop.
 * Mouse state is read directly so drags keep working when the pointer
 * leaves this window. */
void efm_winui_tick(efm_state_t* st, void* window, int x, int y, int w, int h) {
    efm_layout_t L;
    efm_compute_layout(st, x, y, w, h, &L);
    bool left = mouse.left;
    bool released = st->last_left && !left;
    st->last_left = left;
    st->hover_row = efm_list_hit_row(st, &L, mouse.x, mouse.y);

    if (st->split_drag) {
        if (left) st->tree_w = mouse.x - x;
        else st->split_drag = false;
        return;
    }
    if (!st->dnd_armed) return;
    if (!st->dnd_active && left) {
        int dx = mouse.x - st->dnd_start_x, dy = mouse.y - st->dnd_start_y;
        if (dx * dx + dy * dy >= 36) {                     /* 6 px: it's a drag, not a click */
            st->dnd_active = true;
            st->dnd_count = 0;
            for (int i = 0; i < st->entry_count && st->dnd_count < EFM_MAX_SELECTED; ++i)
                if (st->entries[i].selected && st->entries[i].type != EFM_TYPE_PARENT)
                    strncpy(st->dnd_paths[st->dnd_count++], st->entries[i].full_path, EFM_MAX_PATH - 1);
            if (st->dnd_count == 0 && st->dnd_item >= 0) {
                strncpy(st->dnd_paths[0], st->entries[st->dnd_item].full_path, EFM_MAX_PATH - 1);
                st->dnd_count = 1;
            }
        }
        return;
    }
    if (!released) return;
    st->dnd_armed = false;
    if (!st->dnd_active) return;
    st->dnd_active = false;
    bool copy = keyboard_is_ctrl_pressed();

    /* 1) inside this window: folder row, tree folder, or a place */
    if (in(mouse.x, mouse.y, x, y, w, h)) {
        int row = efm_list_hit_row(st, &L, mouse.x, mouse.y);
        if (row >= 0 && st->entries[row].is_dir) { drop_into_folder(st, st->entries[row].full_path, copy); return; }
        if (in(mouse.x, mouse.y, L.tree_x, L.body_y, L.tree_w, L.body_h)) {
            bool arrow = false;
            int n = efm_tree_hit_test(st, mouse.x, mouse.y, L.tree_x, L.body_y, &arrow);
            if (n >= 0 && n < st->tree_count) drop_into_folder(st, st->tree[n].path, copy);
            return;
        }
        if (in(mouse.x, mouse.y, x, L.places_y, w, L.places_h)) {
            for (int p = 0; p < PLACE_COUNT; ++p) {
                int pw, px = place_x(&L, p, &pw);
                if (in(mouse.x, mouse.y, px, L.places_y + 3, pw, L.places_h - 6)) { drop_into_folder(st, k_places[p].path, copy); return; }
            }
        }
        return;                                            /* dropped on itself: nothing */
    }
    /* 2) somewhere else: another window, or the desktop */
    extern void gui_handle_file_drop(void* source_window, int mx, int my, const char (*paths)[EFM_MAX_PATH], int count, bool copy);
    gui_handle_file_drop(window, mouse.x, mouse.y, (const char (*)[EFM_MAX_PATH])st->dnd_paths, st->dnd_count, copy);
    efm_refresh(st);
}
