/**
 * enhanced_context_menu.c - C-OS 4.0.8 alpha Enhanced Context Menu Implementation
 * 
 * アイコン付き・アニメーション付き・階層型右クリックメニュー
 */

#include "enhanced_context_menu.h"
#include "../include/types.h"
#include "vga.h"
#include "../include/serial.h"
#include <string.h>

/* 外部依存 */
extern void vga_fill_rect(int x, int y, int w, int h, uint64_t color);
extern void vga_draw_rect(int x, int y, int w, int h, uint64_t color);
extern void vga_fill_rounded_rect(int x, int y, int w, int h, int r, uint64_t color);
extern void vga_draw_rounded_rect(int x, int y, int w, int h, int r, uint64_t color);
extern void vga_draw_string(int x, int y, const char* s, uint64_t fg, uint64_t bg);
extern void vga_set_pixel(int x, int y, uint64_t color);
extern bool gui_is_japanese(void);

/* カラーテーマ */
#define ECM_LIGHT_BG        0x00F8F9FA
#define ECM_LIGHT_BORDER    0x00D0D8E8
#define ECM_LIGHT_HOVER     0x00E8F0FE
#define ECM_LIGHT_TEXT      0x00202428
#define ECM_LIGHT_MUTED     0x00808898
#define ECM_LIGHT_SEPARATOR 0x00E0E4EC
#define ECM_LIGHT_HEADER_BG 0x00EEF2F8
#define ECM_LIGHT_ACCENT    0x001A73E8
#define ECM_LIGHT_DANGER    0x00D93025
#define ECM_LIGHT_SHADOW    0x40000000

#define ECM_DARK_BG         0x00252830
#define ECM_DARK_BORDER     0x00404858
#define ECM_DARK_HOVER      0x00353A48
#define ECM_DARK_TEXT       0x00E8ECF4
#define ECM_DARK_MUTED      0x00909AAA
#define ECM_DARK_SEPARATOR  0x00363C4A
#define ECM_DARK_HEADER_BG  0x001E2230
#define ECM_DARK_ACCENT     0x004D9FFF
#define ECM_DARK_DANGER     0x00FF6B5B
#define ECM_DARK_SHADOW     0x60000000

/* メニューサイズ定数 */
#define ECM_ITEM_H          32
#define ECM_SEP_H           9
#define ECM_HEADER_H        26
#define ECM_PADDING_X       8
#define ECM_PADDING_Y       6
#define ECM_ICON_SIZE       16
#define ECM_ICON_MARGIN     8
#define ECM_MIN_WIDTH       200
#define ECM_MAX_WIDTH       360
#define ECM_CORNER_R        8
#define ECM_ANIM_FRAMES     6

/* グローバル状態 */
ecm_context_menu_t g_ecm = {0};
static int g_last_action = 0;

/* ============================================================
 * カラーヘルパー
 * ============================================================ */
static uint64_t ecm_bg(void)     { return g_ecm.dark_mode ? ECM_DARK_BG     : ECM_LIGHT_BG; }
static uint64_t ecm_border(void) { return g_ecm.dark_mode ? ECM_DARK_BORDER : ECM_LIGHT_BORDER; }
static uint64_t ecm_hover(void)  { return g_ecm.dark_mode ? ECM_DARK_HOVER  : ECM_LIGHT_HOVER; }
static uint64_t ecm_text(void)   { return g_ecm.dark_mode ? ECM_DARK_TEXT   : ECM_LIGHT_TEXT; }
static uint64_t ecm_muted(void)  { return g_ecm.dark_mode ? ECM_DARK_MUTED  : ECM_LIGHT_MUTED; }
static uint64_t ecm_sep(void)    { return g_ecm.dark_mode ? ECM_DARK_SEPARATOR : ECM_LIGHT_SEPARATOR; }
static uint64_t ecm_hdr(void)    { return g_ecm.dark_mode ? ECM_DARK_HEADER_BG : ECM_LIGHT_HEADER_BG; }
static uint64_t ecm_accent(void) { return g_ecm.dark_mode ? ECM_DARK_ACCENT : ECM_LIGHT_ACCENT; }
static uint64_t ecm_danger(void) { return g_ecm.dark_mode ? ECM_DARK_DANGER : ECM_LIGHT_DANGER; }

/* ============================================================
 * アイコン描画
 * ============================================================ */
void ecm_draw_icon(int x, int y, ecm_icon_t icon, uint64_t color) {
    int s = ECM_ICON_SIZE;
    switch (icon) {
        case ECM_ICON_OPEN:
            /* フォルダアイコン */
            vga_fill_rounded_rect(x, y+3, s, s-3, 2, color);
            vga_fill_rect(x, y+6, s/2, 3, color);
            break;
        case ECM_ICON_EDIT:
            /* 鉛筆アイコン */
            vga_fill_rect(x+2, y+2, s-6, s-6, color);
            vga_fill_rect(x+s-5, y+1, 4, 4, color);
            vga_fill_rect(x+1, y+s-5, 4, 4, color);
            break;
        case ECM_ICON_COPY:
            /* コピーアイコン (重なった四角) */
            vga_draw_rect(x+3, y+3, s-6, s-6, color);
            vga_draw_rect(x, y, s-6, s-6, color);
            break;
        case ECM_ICON_CUT:
            /* ハサミアイコン */
            vga_fill_rect(x+s/2-1, y, 2, s/2, color);
            vga_fill_rect(x, y+s/2, s/2-1, 2, color);
            vga_fill_rect(x+s/2+1, y+s/2, s/2-1, 2, color);
            break;
        case ECM_ICON_PASTE:
            /* クリップボードアイコン */
            vga_draw_rect(x+1, y+2, s-2, s-2, color);
            vga_fill_rect(x+s/2-3, y, 6, 4, color);
            break;
        case ECM_ICON_DELETE:
            /* ゴミ箱アイコン */
            vga_fill_rect(x+2, y+4, s-4, s-4, color);
            vga_fill_rect(x, y+2, s, 2, color);
            vga_fill_rect(x+s/2-2, y, 4, 3, color);
            break;
        case ECM_ICON_RENAME:
            /* テキスト+鉛筆 */
            vga_fill_rect(x, y+2, s-4, 2, color);
            vga_fill_rect(x, y+6, s-6, 2, color);
            vga_fill_rect(x, y+10, s-4, 2, color);
            vga_fill_rect(x+s-4, y+s-6, 4, 6, color);
            break;
        case ECM_ICON_NEW_FILE:
            /* 新規ファイル */
            vga_draw_rect(x+1, y+1, s-2, s-2, color);
            vga_fill_rect(x+s/2-1, y+4, 2, s-8, color);
            vga_fill_rect(x+4, y+s/2-1, s-8, 2, color);
            break;
        case ECM_ICON_NEW_FOLDER:
            /* 新規フォルダ */
            vga_fill_rounded_rect(x, y+3, s, s-3, 2, color);
            vga_fill_rect(x+s/2-1, y+6, 2, s-10, color);
            vga_fill_rect(x+4, y+s/2-1, s-8, 2, 0x00FFFFFF);
            break;
        case ECM_ICON_PROPERTIES:
            /* プロパティ (i アイコン) */
            vga_fill_rect(x+s/2-1, y+2, 2, 2, color);
            vga_fill_rect(x+s/2-1, y+6, 2, s-8, color);
            break;
        case ECM_ICON_SETTINGS:
            /* 歯車アイコン */
            vga_fill_rounded_rect(x+3, y+3, s-6, s-6, 3, color);
            vga_draw_rounded_rect(x+1, y+1, s-2, s-2, 4, color);
            break;
        case ECM_ICON_REFRESH:
            /* 更新アイコン (円弧) */
            vga_draw_rounded_rect(x+2, y+2, s-4, s-4, 4, color);
            vga_fill_rect(x+s-6, y+1, 4, 4, color);
            break;
        case ECM_ICON_SORT:
            /* 並べ替え (3本線) */
            vga_fill_rect(x, y+2, s, 2, color);
            vga_fill_rect(x+2, y+7, s-4, 2, color);
            vga_fill_rect(x+4, y+12, s-8, 2, color);
            break;
        case ECM_ICON_VIEW:
            /* 表示 (グリッド) */
            vga_fill_rect(x, y, s/2-1, s/2-1, color);
            vga_fill_rect(x+s/2+1, y, s/2-1, s/2-1, color);
            vga_fill_rect(x, y+s/2+1, s/2-1, s/2-1, color);
            vga_fill_rect(x+s/2+1, y+s/2+1, s/2-1, s/2-1, color);
            break;
        case ECM_ICON_SEARCH:
            /* 虫眼鏡 */
            vga_draw_rounded_rect(x+1, y+1, s-6, s-6, 4, color);
            vga_fill_rect(x+s-6, y+s-6, 4, 4, color);
            break;
        case ECM_ICON_INFO:
            /* i アイコン (円) */
            vga_draw_rounded_rect(x+1, y+1, s-2, s-2, 6, color);
            vga_fill_rect(x+s/2-1, y+4, 2, 2, color);
            vga_fill_rect(x+s/2-1, y+8, 2, s-10, color);
            break;
        case ECM_ICON_CLOSE:
            /* X アイコン */
            vga_fill_rect(x+2, y+2, 2, s-4, color);
            vga_fill_rect(x+s-4, y+2, 2, s-4, color);
            /* 斜め線 (簡易) */
            for (int i = 0; i < s-4; i++) {
                vga_set_pixel(x+2+i, y+2+i, color);
                vga_set_pixel(x+s-4-i, y+2+i, color);
            }
            break;
        case ECM_ICON_MINIMIZE:
            /* _ アイコン */
            vga_fill_rect(x+2, y+s-4, s-4, 2, color);
            break;
        case ECM_ICON_MAXIMIZE:
            /* □ アイコン */
            vga_draw_rect(x+2, y+2, s-4, s-4, color);
            vga_fill_rect(x+2, y+2, s-4, 2, color);
            break;
        case ECM_ICON_RESTORE:
            /* 重なった□ */
            vga_draw_rect(x+4, y+4, s-6, s-6, color);
            vga_draw_rect(x, y, s-6, s-6, color);
            vga_fill_rect(x, y, s-6, 2, color);
            break;
        case ECM_ICON_MOVE:
            /* 十字矢印 */
            vga_fill_rect(x+s/2-1, y, 2, s, color);
            vga_fill_rect(x, y+s/2-1, s, 2, color);
            break;
        case ECM_ICON_PLAY:
            /* 三角形 (再生) */
            for (int i = 0; i < s/2; i++) {
                vga_fill_rect(x+2+i, y+i, 2, s-i*2, color);
            }
            break;
        case ECM_ICON_TERMINAL:
            /* > _ */
            vga_fill_rect(x, y+2, s, 2, color);
            vga_fill_rect(x, y+s-4, s, 2, color);
            vga_fill_rect(x, y+2, 2, s-4, color);
            vga_draw_string(x+3, y+4, ">", color, 0xFFFFFFFF);
            break;
        case ECM_ICON_WALLPAPER:
            /* 山+太陽 */
            vga_fill_rect(x, y+s/2, s, s/2, color);
            vga_fill_rounded_rect(x+s/2-2, y, 4, 4, 2, color);
            break;
        case ECM_ICON_POWER:
            /* 電源ボタン */
            vga_draw_rounded_rect(x+2, y+4, s-4, s-6, 5, color);
            vga_fill_rect(x+s/2-1, y, 2, s/2, color);
            break;
        case ECM_ICON_LOCK:
            /* 鍵 */
            vga_fill_rect(x+2, y+s/2, s-4, s/2, color);
            vga_draw_rounded_rect(x+3, y+2, s-6, s/2, 4, color);
            break;
        case ECM_ICON_STAR:
            /* 星 (簡易) */
            vga_fill_rect(x+s/2-1, y, 2, s, color);
            vga_fill_rect(x, y+s/2-1, s, 2, color);
            vga_fill_rect(x+2, y+2, s-4, s-4, color);
            break;
        case ECM_ICON_LUA:
            /* Lua アイコン (L) */
            vga_fill_rect(x+2, y+2, 2, s-4, color);
            vga_fill_rect(x+2, y+s-4, s-4, 2, color);
            break;
        case ECM_ICON_COMPRESS:
            /* 圧縮 (下矢印+箱) */
            vga_fill_rect(x+s/2-1, y, 2, s/2, color);
            vga_fill_rect(x+2, y+s/2-3, s-4, 2, color);
            vga_draw_rect(x+1, y+s/2, s-2, s/2-1, color);
            break;
        case ECM_ICON_EXTRACT:
            /* 展開 (上矢印+箱) */
            vga_draw_rect(x+1, y+1, s-2, s/2, color);
            vga_fill_rect(x+s/2-1, y+s/2+1, 2, s/2-1, color);
            vga_fill_rect(x+2, y+s/2+4, s-4, 2, color);
            break;
        case ECM_ICON_SEND_TO:
            /* 送る (矢印) */
            vga_fill_rect(x, y+s/2-1, s-4, 2, color);
            vga_fill_rect(x+s-6, y+s/2-4, 2, 8, color);
            vga_fill_rect(x+s-4, y+s/2-2, 2, 4, color);
            vga_fill_rect(x+s-2, y+s/2-1, 2, 2, color);
            break;
        case ECM_ICON_SHORTCUT:
            /* ショートカット (矢印付き) */
            vga_draw_rect(x+1, y+1, s-2, s-2, color);
            vga_fill_rect(x+2, y+s-5, 4, 4, color);
            break;
        default:
            /* デフォルト: 小さな四角 */
            vga_fill_rounded_rect(x+3, y+3, s-6, s-6, 2, color);
            break;
    }
}

/* ============================================================
 * メニュー幅計算
 * ============================================================ */
static int ecm_calc_item_width(const ecm_item_t* item) {
    const char* label = gui_is_japanese() ? item->label_ja : item->label;
    if (!label[0]) label = item->label;
    int w = (int)strlen(label) * FONT_W + ECM_PADDING_X * 2 + ECM_ICON_SIZE + ECM_ICON_MARGIN * 2;
    if (item->shortcut[0]) w += (int)strlen(item->shortcut) * FONT_W + 16;
    if (item->submenu_id >= 0) w += 16; /* サブメニュー矢印 */
    if (w < ECM_MIN_WIDTH) w = ECM_MIN_WIDTH;
    if (w > ECM_MAX_WIDTH) w = ECM_MAX_WIDTH;
    return w;
}

static int ecm_calc_menu_width(const ecm_item_t* items, int count) {
    int w = ECM_MIN_WIDTH;
    for (int i = 0; i < count; i++) {
        if (items[i].type == ECM_ITEM_SEPARATOR) continue;
        int iw = ecm_calc_item_width(&items[i]);
        if (iw > w) w = iw;
    }
    return w;
}

static int ecm_calc_menu_height(const ecm_item_t* items, int count) {
    int h = ECM_PADDING_Y * 2;
    for (int i = 0; i < count; i++) {
        if (items[i].type == ECM_ITEM_SEPARATOR) h += ECM_SEP_H;
        else if (items[i].type == ECM_ITEM_HEADER) h += ECM_HEADER_H;
        else h += ECM_ITEM_H;
    }
    return h;
}

/* ============================================================
 * メニュー項目追加ヘルパー
 * ============================================================ */
static void ecm_add_item(const char* label, const char* label_ja, ecm_icon_t icon,
                          int action_id, bool enabled, const char* shortcut) {
    if (g_ecm.item_count >= ECM_MAX_ITEMS) return;
    ecm_item_t* item = &g_ecm.items[g_ecm.item_count++];
    memset(item, 0, sizeof(*item));
    strncpy(item->label, label, ECM_LABEL_LEN-1);
    strncpy(item->label_ja, label_ja, ECM_LABEL_LEN-1);
    item->icon = icon;
    item->action_id = action_id;
    item->enabled = enabled;
    item->type = ECM_ITEM_NORMAL;
    item->submenu_id = -1;
    if (shortcut) strncpy(item->shortcut, shortcut, ECM_SHORTCUT_LEN-1);
}

static void ecm_add_danger(const char* label, const char* label_ja, ecm_icon_t icon,
                            int action_id, bool enabled) {
    if (g_ecm.item_count >= ECM_MAX_ITEMS) return;
    ecm_item_t* item = &g_ecm.items[g_ecm.item_count++];
    memset(item, 0, sizeof(*item));
    strncpy(item->label, label, ECM_LABEL_LEN-1);
    strncpy(item->label_ja, label_ja, ECM_LABEL_LEN-1);
    item->icon = icon;
    item->action_id = action_id;
    item->enabled = enabled;
    item->type = ECM_ITEM_DANGER;
    item->submenu_id = -1;
}

static void ecm_add_separator(void) {
    if (g_ecm.item_count >= ECM_MAX_ITEMS) return;
    ecm_item_t* item = &g_ecm.items[g_ecm.item_count++];
    memset(item, 0, sizeof(*item));
    item->type = ECM_ITEM_SEPARATOR;
    item->submenu_id = -1;
    item->enabled = false;
}

static void ecm_add_header(const char* label, const char* label_ja) {
    if (g_ecm.item_count >= ECM_MAX_ITEMS) return;
    ecm_item_t* item = &g_ecm.items[g_ecm.item_count++];
    memset(item, 0, sizeof(*item));
    strncpy(item->label, label, ECM_LABEL_LEN-1);
    strncpy(item->label_ja, label_ja, ECM_LABEL_LEN-1);
    item->type = ECM_ITEM_HEADER;
    item->submenu_id = -1;
    item->enabled = false;
}

static void ecm_add_checkbox(const char* label, const char* label_ja, int action_id, bool checked) {
    if (g_ecm.item_count >= ECM_MAX_ITEMS) return;
    ecm_item_t* item = &g_ecm.items[g_ecm.item_count++];
    memset(item, 0, sizeof(*item));
    strncpy(item->label, label, ECM_LABEL_LEN-1);
    strncpy(item->label_ja, label_ja, ECM_LABEL_LEN-1);
    item->type = ECM_ITEM_CHECKBOX;
    item->action_id = action_id;
    item->checked = checked;
    item->enabled = true;
    item->submenu_id = -1;
}

static void ecm_add_submenu_item(const char* label, const char* label_ja, ecm_icon_t icon, int submenu_id) {
    if (g_ecm.item_count >= ECM_MAX_ITEMS) return;
    ecm_item_t* item = &g_ecm.items[g_ecm.item_count++];
    memset(item, 0, sizeof(*item));
    strncpy(item->label, label, ECM_LABEL_LEN-1);
    strncpy(item->label_ja, label_ja, ECM_LABEL_LEN-1);
    item->icon = icon;
    item->type = ECM_ITEM_SUBMENU;
    item->submenu_id = submenu_id;
    item->enabled = true;
}

/* ============================================================
 * メニュー構築
 * ============================================================ */

static void ecm_build_desktop_empty(void) {
    ecm_add_header("Desktop", "デスクトップ");
    ecm_add_item("New File",    "新規ファイル",   ECM_ICON_NEW_FILE,   ECM_ACT_NEW_FILE,   true, "Ctrl+N");
    ecm_add_item("New Folder",  "新規フォルダ",   ECM_ICON_NEW_FOLDER, ECM_ACT_NEW_FOLDER, true, "Ctrl+Shift+N");
    ecm_add_separator();
    ecm_add_item("Open File Manager", "ファイルマネージャー", ECM_ICON_OPEN, ECM_ACT_OPEN_FILE_MGR, true, NULL);
    ecm_add_item("Open Terminal",     "ターミナルを開く",     ECM_ICON_TERMINAL, ECM_ACT_OPEN_TERMINAL, true, NULL);
    ecm_add_item("Open NetSurf",      "NetSurfを開く",       ECM_ICON_OPEN, ECM_ACT_OPEN_BROWSER, true, NULL);
    ecm_add_separator();
    ecm_add_item("Refresh Desktop",   "表示の更新",           ECM_ICON_REFRESH,  ECM_ACT_DESKTOP_REFRESH, true, "F5");
    ecm_add_item("Save Layout",       "レイアウトを保存",     ECM_ICON_PROPERTIES, ECM_ACT_SAVE_LAYOUT, true, NULL);
    ecm_add_item("Reset Layout",      "レイアウトをリセット", ECM_ICON_RESTORE,  ECM_ACT_DESKTOP_RESET, true, NULL);
    ecm_add_separator();
    ecm_add_item("Wallpaper...",       "壁紙を変更...",        ECM_ICON_WALLPAPER, ECM_ACT_DESKTOP_WALLPAPER, true, NULL);
    ecm_add_item("Display Settings",  "表示設定",             ECM_ICON_SETTINGS,  ECM_ACT_OPEN_SETTINGS, true, NULL);
    ecm_add_separator();
    ecm_add_item("About C-OS 4.0.8 alpha",  "C-OS 4.0.8 alpha について", ECM_ICON_INFO,      ECM_ACT_ABOUT, true, NULL);
}

static void ecm_build_desktop_icon(int icon_idx) {
    (void)icon_idx;
    ecm_add_item("Open",         "開く",           ECM_ICON_OPEN,       ECM_ACT_OPEN,       true, "Enter");
    ecm_add_separator();
    ecm_add_item("Copy",         "コピー",          ECM_ICON_COPY,       ECM_ACT_COPY,       true, "Ctrl+C");
    ecm_add_item("Cut",          "切り取り",        ECM_ICON_CUT,        ECM_ACT_CUT,        true, "Ctrl+X");
    ecm_add_separator();
    ecm_add_item("Rename",       "名前を変更",      ECM_ICON_RENAME,     ECM_ACT_RENAME,     true, "F2");
    ecm_add_danger("Delete",     "削除",            ECM_ICON_DELETE,     ECM_ACT_DELETE,     true);
    ecm_add_separator();
    ecm_add_item("Icon Size: Small",  "アイコン小",  ECM_ICON_VIEW, ECM_ACT_ICON_SIZE_SMALL,  true, NULL);
    ecm_add_item("Icon Size: Medium", "アイコン中",  ECM_ICON_VIEW, ECM_ACT_ICON_SIZE_MEDIUM, true, NULL);
    ecm_add_item("Icon Size: Large",  "アイコン大",  ECM_ICON_VIEW, ECM_ACT_ICON_SIZE_LARGE,  true, NULL);
    ecm_add_separator();
    ecm_add_item("Properties",   "プロパティ",      ECM_ICON_PROPERTIES, ECM_ACT_PROPERTIES, true, "Alt+Enter");
}

static void ecm_build_window(int win_idx) {
    (void)win_idx;
    ecm_add_header("Window", "ウィンドウ");
    ecm_add_item("Restore",      "元に戻す",        ECM_ICON_RESTORE,    ECM_ACT_WIN_RESTORE,    true, NULL);
    ecm_add_item("Minimize",     "最小化",          ECM_ICON_MINIMIZE,   ECM_ACT_WIN_MINIMIZE,   true, NULL);
    ecm_add_item("Maximize",     "最大化",          ECM_ICON_MAXIMIZE,   ECM_ACT_WIN_MAXIMIZE,   true, NULL);
    ecm_add_item("Fullscreen",   "全画面",          ECM_ICON_MAXIMIZE,   ECM_ACT_WIN_FULLSCREEN, true, "F11");
    ecm_add_separator();
    ecm_add_item("Center",       "中央に配置",      ECM_ICON_MOVE,       ECM_ACT_WIN_CENTER,     true, NULL);
    ecm_add_item("Left Half",    "左半分へ",        ECM_ICON_MOVE,       ECM_ACT_WIN_LEFT_HALF,  true, "Win+Left");
    ecm_add_item("Right Half",   "右半分へ",        ECM_ICON_MOVE,       ECM_ACT_WIN_RIGHT_HALF, true, "Win+Right");
    ecm_add_separator();
    ecm_add_item("Show Desktop", "デスクトップを表示", ECM_ICON_VIEW,    ECM_ACT_SHOW_DESKTOP,   true, "Win+D");
    ecm_add_separator();
    ecm_add_danger("Close Window", "閉じる",        ECM_ICON_CLOSE,      ECM_ACT_WIN_CLOSE,      true);
}

static void ecm_build_file(const char* path, bool is_dir) {
    (void)path;
    if (is_dir) {
        ecm_add_item("Open",          "開く",             ECM_ICON_OPEN,       ECM_ACT_OPEN,       true, "Enter");
        ecm_add_item("Open in Terminal", "ターミナルで開く", ECM_ICON_TERMINAL, ECM_ACT_OPEN_TERMINAL, true, NULL);
        ecm_add_separator();
    } else {
        ecm_add_item("Open",          "開く",             ECM_ICON_OPEN,       ECM_ACT_OPEN,       true, "Enter");
        ecm_add_item("Open With...",  "プログラムから開く", ECM_ICON_OPEN,     ECM_ACT_OPEN_WITH,  true, NULL);
        ecm_add_separator();
    }
    ecm_add_item("Copy",          "コピー",           ECM_ICON_COPY,       ECM_ACT_COPY,       true, "Ctrl+C");
    ecm_add_item("Cut",           "切り取り",         ECM_ICON_CUT,        ECM_ACT_CUT,        true, "Ctrl+X");
    ecm_add_item("Paste",         "貼り付け",         ECM_ICON_PASTE,      ECM_ACT_PASTE,      g_ecm.clipboard_has_data, "Ctrl+V");
    ecm_add_separator();
    ecm_add_item("Copy Path",     "パスをコピー",     ECM_ICON_COPY,       ECM_ACT_COPY_PATH,  true, NULL);
    ecm_add_item("Copy To...",    "コピー先...",      ECM_ICON_COPY,       ECM_ACT_COPY_TO,    true, NULL);
    ecm_add_item("Move To...",    "移動先...",        ECM_ICON_MOVE,       ECM_ACT_MOVE_TO,    true, NULL);
    ecm_add_separator();
    ecm_add_item("Rename",        "名前を変更",       ECM_ICON_RENAME,     ECM_ACT_RENAME,     true, "F2");
    ecm_add_danger("Delete",      "削除",             ECM_ICON_DELETE,     ECM_ACT_DELETE,     true);
    ecm_add_separator();
    if (!is_dir) {
        ecm_add_item("Compress",  "圧縮...",          ECM_ICON_COMPRESS,   ECM_ACT_COMPRESS,   true, NULL);
    } else {
        ecm_add_item("Compress Folder", "フォルダを圧縮", ECM_ICON_COMPRESS, ECM_ACT_COMPRESS, true, NULL);
        ecm_add_item("Extract Here",    "ここに展開",  ECM_ICON_EXTRACT,    ECM_ACT_EXTRACT,    true, NULL);
    }
    ecm_add_item("Send to Desktop", "デスクトップに送る", ECM_ICON_SEND_TO, ECM_ACT_SEND_TO_DESKTOP, true, NULL);
    ecm_add_item("Create Shortcut", "ショートカットを作成", ECM_ICON_SHORTCUT, ECM_ACT_CREATE_SHORTCUT, true, NULL);
    ecm_add_separator();
    ecm_add_item("Add to Favorites", "お気に入りに追加", ECM_ICON_STAR,   ECM_ACT_ADD_FAVORITE, true, NULL);
    ecm_add_item("Run as Lua Script", "Luaスクリプトとして実行", ECM_ICON_LUA, ECM_ACT_RUN_LUA, !is_dir, NULL);
    ecm_add_separator();
    ecm_add_item("Properties",    "プロパティ",       ECM_ICON_PROPERTIES, ECM_ACT_PROPERTIES, true, "Alt+Enter");
}

static void ecm_build_fm_empty(void) {
    ecm_add_header("File Manager", "ファイルマネージャー");
    ecm_add_item("New File",      "新規ファイル",     ECM_ICON_NEW_FILE,   ECM_ACT_NEW_FILE,   true, "Ctrl+N");
    ecm_add_item("New Folder",    "新規フォルダ",     ECM_ICON_NEW_FOLDER, ECM_ACT_NEW_FOLDER, true, "Ctrl+Shift+N");
    ecm_add_item("Paste",         "貼り付け",         ECM_ICON_PASTE,      ECM_ACT_PASTE,      g_ecm.clipboard_has_data, "Ctrl+V");
    ecm_add_separator();
    ecm_add_item("Select All",    "すべて選択",       ECM_ICON_COPY,       ECM_ACT_SELECT_ALL, true, "Ctrl+A");
    ecm_add_separator();
    ecm_add_item("Sort by Name",  "名前順",           ECM_ICON_SORT,       ECM_ACT_SORT_NAME,  true, NULL);
    ecm_add_item("Sort by Size",  "サイズ順",         ECM_ICON_SORT,       ECM_ACT_SORT_SIZE,  true, NULL);
    ecm_add_item("Sort by Date",  "日付順",           ECM_ICON_SORT,       ECM_ACT_SORT_DATE,  true, NULL);
    ecm_add_item("Sort by Type",  "種類順",           ECM_ICON_SORT,       ECM_ACT_SORT_TYPE,  true, NULL);
    ecm_add_separator();
    ecm_add_item("View: List",    "リスト表示",       ECM_ICON_VIEW,       ECM_ACT_VIEW_LIST,       true, NULL);
    ecm_add_item("View: Icons",   "アイコン表示",     ECM_ICON_VIEW,       ECM_ACT_VIEW_ICONS,      true, NULL);
    ecm_add_item("View: Details", "詳細表示",         ECM_ICON_VIEW,       ECM_ACT_VIEW_DETAILS,    true, NULL);
    ecm_add_item("View: Thumbnails", "サムネイル表示", ECM_ICON_VIEW,      ECM_ACT_VIEW_THUMBNAILS, true, NULL);
    ecm_add_separator();
    ecm_add_item("Search...",     "検索...",          ECM_ICON_SEARCH,     ECM_ACT_SEARCH,     true, "Ctrl+F");
    ecm_add_item("Refresh",       "更新",             ECM_ICON_REFRESH,    ECM_ACT_REFRESH,    true, "F5");
    ecm_add_item("Properties",    "プロパティ",       ECM_ICON_PROPERTIES, ECM_ACT_PROPERTIES, true, "Alt+Enter");
}

static void ecm_build_taskbar(void) {
    ecm_add_header("Taskbar", "タスクバー");
    ecm_add_item("Show Desktop",  "デスクトップを表示", ECM_ICON_VIEW,    ECM_ACT_SHOW_DESKTOP, true, "Win+D");
    ecm_add_separator();
    ecm_add_item("Open File Manager", "ファイルマネージャー", ECM_ICON_OPEN, ECM_ACT_OPEN_FILE_MGR, true, NULL);
    ecm_add_item("Open Terminal",     "ターミナル",           ECM_ICON_TERMINAL, ECM_ACT_OPEN_TERMINAL, true, NULL);
    ecm_add_item("Open NetSurf",      "NetSurf",             ECM_ICON_OPEN, ECM_ACT_OPEN_BROWSER, true, NULL);
    ecm_add_item("Open Settings",     "設定",                 ECM_ICON_SETTINGS, ECM_ACT_OPEN_SETTINGS, true, NULL);
    ecm_add_separator();
    ecm_add_item("Lock Screen",   "画面をロック",     ECM_ICON_LOCK,       ECM_ACT_LOCK_SCREEN, true, "Win+L");
    ecm_add_separator();
    ecm_add_item("Sleep",         "スリープ",         ECM_ICON_POWER,      ECM_ACT_POWER_SLEEP,    true, NULL);
    ecm_add_item("Restart",       "再起動",           ECM_ICON_POWER,      ECM_ACT_POWER_RESTART,  true, NULL);
    ecm_add_danger("Shutdown",    "シャットダウン",   ECM_ICON_POWER,      ECM_ACT_POWER_SHUTDOWN, true);
}

/* ============================================================
 * 公開 API
 * ============================================================ */

void ecm_init(void) {
    memset(&g_ecm, 0, sizeof(g_ecm));
    g_last_action = 0;
}

void ecm_cleanup(void) {
    memset(&g_ecm, 0, sizeof(g_ecm));
}

void ecm_show(ecm_context_type_t ctx_type, int x, int y, int target_window, int target_icon, const char* file_path) {
    /* メニューを開く前のクリップボード状態は保持しておく
     * (memsetでg_ecm全体を消す前に退避し、Paste項目の有効/無効判定に使う) */
    bool prev_clipboard_has_data = g_ecm.clipboard_has_data;
    char prev_clipboard_path[256];
    bool prev_clipboard_is_cut = g_ecm.clipboard_is_cut;
    strncpy(prev_clipboard_path, g_ecm.clipboard_path, sizeof(prev_clipboard_path) - 1);
    prev_clipboard_path[sizeof(prev_clipboard_path) - 1] = '\0';

    memset(&g_ecm, 0, sizeof(g_ecm));
    g_ecm.visible = true;
    g_ecm.x = x;
    g_ecm.y = y;
    g_ecm.target_window = target_window;
    g_ecm.target_icon = target_icon;
    g_ecm.context_type = (int)ctx_type;
    g_ecm.hovered = -1;
    g_ecm.active_submenu = -1;
    g_ecm.anim_frame = 0;

    g_ecm.clipboard_has_data = prev_clipboard_has_data;
    g_ecm.clipboard_is_cut = prev_clipboard_is_cut;
    strncpy(g_ecm.clipboard_path, prev_clipboard_path, sizeof(g_ecm.clipboard_path) - 1);

    if (file_path) {
        strncpy(g_ecm.target_path, file_path, sizeof(g_ecm.target_path) - 1);
        g_ecm.target_path[sizeof(g_ecm.target_path) - 1] = '\0';
    } else {
        g_ecm.target_path[0] = '\0';
    }

    /* 外部からダークモード設定を継承 */
    extern int gui_dark_mode;
    g_ecm.dark_mode = (gui_dark_mode != 0);
    
    /* コンテキスト別メニュー構築 */
    switch (ctx_type) {
        case ECM_CTX_DESKTOP_EMPTY:
            ecm_build_desktop_empty();
            break;
        case ECM_CTX_DESKTOP_ICON:
            ecm_build_desktop_icon(target_icon);
            break;
        case ECM_CTX_WINDOW:
            ecm_build_window(target_window);
            break;
        case ECM_CTX_TASKBAR:
            ecm_build_taskbar();
            break;
        case ECM_CTX_FILE:
            ecm_build_file(file_path, false);
            break;
        case ECM_CTX_FOLDER:
            ecm_build_file(file_path, true);
            break;
        case ECM_CTX_FM_FILE:
            ecm_build_file(file_path, false);
            break;
        case ECM_CTX_FM_FOLDER:
            ecm_build_file(file_path, true);
            break;
        case ECM_CTX_FM_EMPTY:
            ecm_build_fm_empty();
            break;
        default:
            ecm_build_desktop_empty();
            break;
    }
    
    /* 画面外にはみ出さないよう調整 */
    int w = ecm_calc_menu_width(g_ecm.items, g_ecm.item_count);
    int h = ecm_calc_menu_height(g_ecm.items, g_ecm.item_count);
    g_ecm.w = w;
    g_ecm.h = h;
    
    if ((uint64_t)(g_ecm.x + w) > SCREEN_W - 4) g_ecm.x = (int)(SCREEN_W - w - 4);
    if (g_ecm.x < 4) g_ecm.x = 4;
    if ((uint64_t)(g_ecm.y + h) > SCREEN_H - 4) g_ecm.y = (int)(SCREEN_H - h - 4);
    if (g_ecm.y < 4) g_ecm.y = 4;
    
    serial_puts("[ECM] Context menu shown, items=");
    serial_putdec(g_ecm.item_count);
    serial_puts("\n");
}

void ecm_close(void) {
    g_ecm.visible = false;
    g_ecm.active_submenu = -1;
    for (int i = 0; i < g_ecm.submenu_count; i++) {
        g_ecm.submenus[i].visible = false;
    }
}

bool ecm_is_visible(void) {
    return g_ecm.visible;
}

/* ============================================================
 * 描画
 * ============================================================ */

static void ecm_draw_menu(int mx, int my, int mw, const ecm_item_t* items, int count, int hovered) {
    int mh = ecm_calc_menu_height(items, count);
    
    /* シャドウ */
    uint64_t shadow = g_ecm.dark_mode ? ECM_DARK_SHADOW : ECM_LIGHT_SHADOW;
    vga_fill_rounded_rect(mx+4, my+4, mw, mh, ECM_CORNER_R, shadow);
    
    /* 背景 */
    vga_fill_rounded_rect(mx, my, mw, mh, ECM_CORNER_R, ecm_bg());
    vga_draw_rounded_rect(mx, my, mw, mh, ECM_CORNER_R, ecm_border());
    
    int iy = my + ECM_PADDING_Y;
    for (int i = 0; i < count; i++) {
        const ecm_item_t* item = &items[i];
        
        if (item->type == ECM_ITEM_SEPARATOR) {
            int sy = iy + ECM_SEP_H / 2;
            vga_fill_rect(mx + 8, sy, mw - 16, 1, ecm_sep());
            iy += ECM_SEP_H;
            continue;
        }
        
        if (item->type == ECM_ITEM_HEADER) {
            vga_fill_rounded_rect(mx + 2, iy, mw - 4, ECM_HEADER_H, 4, ecm_hdr());
            const char* lbl = gui_is_japanese() ? item->label_ja : item->label;
            if (!lbl[0]) lbl = item->label;
            vga_draw_string(mx + ECM_PADDING_X + ECM_ICON_SIZE + ECM_ICON_MARGIN, iy + 6, lbl, ecm_muted(), 0xFFFFFFFF);
            iy += ECM_HEADER_H;
            continue;
        }
        
        /* ホバー背景 */
        if (i == hovered && item->enabled) {
            vga_fill_rounded_rect(mx + 2, iy + 1, mw - 4, ECM_ITEM_H - 2, 4, ecm_hover());
        }
        
        /* アイコン */
        if (item->icon != ECM_ICON_NONE) {
            uint64_t icon_color = item->enabled ? 
                (item->type == ECM_ITEM_DANGER ? ecm_danger() : (i == hovered ? ecm_accent() : ecm_text())) :
                ecm_muted();
            ecm_draw_icon(mx + ECM_PADDING_X, iy + (ECM_ITEM_H - ECM_ICON_SIZE) / 2, item->icon, icon_color);
        }
        
        /* チェックボックス */
        if (item->type == ECM_ITEM_CHECKBOX) {
            int cx = mx + ECM_PADDING_X;
            int cy = iy + (ECM_ITEM_H - 12) / 2;
            vga_draw_rect(cx, cy, 12, 12, ecm_border());
            if (item->checked) {
                vga_fill_rect(cx+2, cy+2, 8, 8, ecm_accent());
            }
        }
        
        /* ラベル */
        const char* lbl = gui_is_japanese() ? item->label_ja : item->label;
        if (!lbl[0]) lbl = item->label;
        uint64_t text_color = item->enabled ?
            (item->type == ECM_ITEM_DANGER ? ecm_danger() : ecm_text()) :
            ecm_muted();
        vga_draw_string(mx + ECM_PADDING_X + ECM_ICON_SIZE + ECM_ICON_MARGIN,
                        iy + (ECM_ITEM_H - FONT_H) / 2, lbl, text_color, 0xFFFFFFFF);
        
        /* ショートカット */
        if (item->shortcut[0]) {
            int sw = (int)strlen(item->shortcut) * FONT_W;
            vga_draw_string(mx + mw - sw - ECM_PADDING_X - (item->submenu_id >= 0 ? 16 : 0),
                           iy + (ECM_ITEM_H - FONT_H) / 2, item->shortcut, ecm_muted(), 0xFFFFFFFF);
        }
        
        /* サブメニュー矢印 */
        if (item->submenu_id >= 0) {
            int ax = mx + mw - ECM_PADDING_X - 8;
            int ay = iy + ECM_ITEM_H / 2;
            vga_fill_rect(ax, ay-3, 2, 6, ecm_muted());
            vga_fill_rect(ax+2, ay-2, 2, 4, ecm_muted());
            vga_fill_rect(ax+4, ay-1, 2, 2, ecm_muted());
        }
        
        iy += ECM_ITEM_H;
    }
}

void ecm_draw(void) {
    if (!g_ecm.visible) return;
    
    /* アニメーション */
    if (g_ecm.anim_frame < ECM_ANIM_FRAMES) g_ecm.anim_frame++;
    
    ecm_draw_menu(g_ecm.x, g_ecm.y, g_ecm.w, g_ecm.items, g_ecm.item_count, g_ecm.hovered);
    
    /* サブメニュー描画 */
    for (int i = 0; i < g_ecm.submenu_count; i++) {
        ecm_submenu_t* sm = &g_ecm.submenus[i];
        if (!sm->visible) continue;
        int smw = ecm_calc_menu_width(sm->items, sm->item_count);
        ecm_draw_menu(sm->x, sm->y, smw, sm->items, sm->item_count, sm->hovered);
    }
}

/* ============================================================
 * 入力処理
 * ============================================================ */

static int ecm_hit_test(int mx, int my, const ecm_item_t* items, int count, int menu_x, int menu_y, int menu_w) {
    int iy = menu_y + ECM_PADDING_Y;
    for (int i = 0; i < count; i++) {
        const ecm_item_t* item = &items[i];
        int ih = (item->type == ECM_ITEM_SEPARATOR) ? ECM_SEP_H :
                 (item->type == ECM_ITEM_HEADER)    ? ECM_HEADER_H : ECM_ITEM_H;
        if (item->type != ECM_ITEM_SEPARATOR && item->type != ECM_ITEM_HEADER) {
            if (mx >= menu_x && mx < menu_x + menu_w && my >= iy && my < iy + ih) {
                return i;
            }
        }
        iy += ih;
    }
    return -1;
}

void ecm_handle_mouse_move(int mx, int my) {
    if (!g_ecm.visible) return;
    g_ecm.hovered = ecm_hit_test(mx, my, g_ecm.items, g_ecm.item_count, g_ecm.x, g_ecm.y, g_ecm.w);
}

bool ecm_handle_click(int mx, int my) {
    if (!g_ecm.visible) return false;
    
    /* メニュー外クリック */
    if (mx < g_ecm.x || mx >= g_ecm.x + g_ecm.w ||
        my < g_ecm.y || my >= g_ecm.y + g_ecm.h) {
        ecm_close();
        return true;
    }
    
    int idx = ecm_hit_test(mx, my, g_ecm.items, g_ecm.item_count, g_ecm.x, g_ecm.y, g_ecm.w);
    if (idx < 0) return true;
    
    const ecm_item_t* item = &g_ecm.items[idx];
    if (!item->enabled) return true;
    if (item->type == ECM_ITEM_SEPARATOR || item->type == ECM_ITEM_HEADER) return true;
    
    g_last_action = item->action_id;
    ecm_close();
    return true;
}

bool ecm_handle_key(int key) {
    if (!g_ecm.visible) return false;
    /* ESC で閉じる */
    if (key == 0x01) { ecm_close(); return true; }
    return false;
}

int ecm_get_last_action(void) {
    return g_last_action;
}

void ecm_clear_action(void) {
    g_last_action = 0;
}

void ecm_clipboard_copy(const char* path, bool is_cut) {
    if (!path) return;
    strncpy(g_ecm.clipboard_path, path, 255);
    g_ecm.clipboard_path[255] = '\0';
    g_ecm.clipboard_has_data = true;
    g_ecm.clipboard_is_cut = is_cut;
}

bool ecm_clipboard_has_data(void) { return g_ecm.clipboard_has_data; }
const char* ecm_clipboard_get_path(void) { return g_ecm.clipboard_path; }
bool ecm_clipboard_is_cut(void) { return g_ecm.clipboard_is_cut; }
void ecm_clipboard_clear(void) {
    g_ecm.clipboard_has_data = false;
    g_ecm.clipboard_path[0] = '\0';
    g_ecm.clipboard_is_cut = false;
}

const char* ecm_get_target_path(void) {
    return g_ecm.target_path;
}

void ecm_set_dark_mode(bool dark) {
    g_ecm.dark_mode = dark;
}
