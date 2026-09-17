/**
 * gui_menus.c - GUIコア (コンテキストメニュー・サブメニュー・スタートメニュー)
 * gui.c (5,588行) から分割生成。詳細は gui_internal.h を参照。
 */

#include "gui.h"
#include "gui_internal.h"
#include "voxel_games_advanced.h"
#include "vga.h"
#include "mouse.h"
#include "../drivers/input/mouse_minimal.h"
#include "keyboard.h"
#include "fs.h"
#include "../bios/bios.h"
#include "io.h"
#include "serial.h"
#include "timer.h"
#include "../apps/development/python_ide_gui.h"
#include "gui_render_engine.h"
#include "gui_utils.h"
#include "../apps/system/password_screen.h"
#include "../components/boot_animation.h"
#include "notification_center.h"
#include "theme_system.h"
#include <shell.h>
#include <string.h>
#include <stdio.h>

static const char* submenu_title_for_kind(int kind);
static void gui_show_desktop(void);
static void draw_start_menu(void);
static void handle_start_menu_click(int x, int y);

/* Draw a small icon for context menu items */
static void draw_ctx_icon(int x, int y, int icon_id, bool hov) {
    gui_draw_ctx_icon_art(icon_id, x, y, hov);
}


static int ctx_menu_calc_width(void) {
    int max_chars = 0;
    for (int i = 0; i < ctx_menu.item_count; ++i) {
        if (ctx_menu.separator[i]) continue;
        int len = (int)strlen(ctx_menu.items[i]);
        if (len > max_chars) max_chars = len;
    }
    int base = max_chars * FONT_W + 78; /* icon + padding + arrow space */
    if (base < 240) base = 240;
    if (base > 420) base = 420;
    return base;
}

static int ctx_menu_calc_height(int item_h, int sep_h) {
    int h = 26; /* header */
    for (int i = 0; i < ctx_menu.item_count; ++i) {
        h += ctx_menu.separator[i] ? sep_h : item_h;
    }
    h += 8; /* bottom padding */
    if (submenu.visible && submenu.item_count > 0) {
        h += 0;
    }
    return h;
}

static int submenu_calc_width(void) {
    int max_chars = 0;
    for (int i = 0; i < submenu.item_count; ++i) {
        int len = (int)strlen(submenu.items[i]);
        if (len > max_chars) max_chars = len;
    }
    int base = max_chars * FONT_W + 36;
    if (base < 150) base = 150;
    if (base > 260) base = 260;
    return base;
}
void gui_draw_ctx_menu_panel(void) {
    if (!ctx_menu.visible) return;
    if (ctx_menu.context_type == 2) {
        draw_start_menu();
        return;
    }

    const int item_h = 32;
    const int sep_h = 10;
    const int header_h = 28;
    const int pad = 8;
    int mw = ctx_menu_calc_width();
    int total_h = ctx_menu_calc_height(item_h, sep_h);

    int mx = ctx_menu.x;
    int my = ctx_menu.y;
    if (mx + mw > (int)SCREEN_W - 4) mx = (int)SCREEN_W - mw - 4;
    if (mx < 4) mx = 4;
    if (my + total_h > (int)SCREEN_H - 4) my = (int)SCREEN_H - total_h - 4;
    if (my < 4) my = 4;

    /* Soft shadow */
    vga_fill_rounded_rect(mx + 4, my + 5, mw, total_h, 12, rgb(10, 16, 28));

    /* Body */
    vga_fill_rounded_rect(mx, my, mw, total_h, 12, rgb(250, 252, 255));
    vga_draw_rounded_rect(mx, my, mw, total_h, 12, rgb(188, 202, 226));
    vga_fill_rounded_rect(mx + 1, my + 1, mw - 2, header_h, 12, rgb(236, 242, 251));
    vga_fill_rect(mx + 1, my + header_h - 1, mw - 2, 1, rgb(206, 219, 238));

    const char* header = (ctx_menu.target_window == -1)
        ? gui_ctx_label("Desktop actions", "デスクトップ操作")
        : gui_ctx_label("Window actions", "ウィンドウ操作");
    vga_draw_string(mx + pad, my + 7, header, rgb(36, 54, 86), 0xFFFFFFFF);

    int cur_y = my + header_h;
    for (int i = 0; i < ctx_menu.item_count; ++i) {
        if (ctx_menu.separator[i]) {
            int sy = cur_y + sep_h / 2;
            vga_fill_rect(mx + 12, sy, mw - 24, 1, rgb(223, 231, 242));
            cur_y += sep_h;
            continue;
        }

        bool hov = (mouse.x >= mx && mouse.x < mx + mw && mouse.y >= cur_y && mouse.y < cur_y + item_h);
        bool dis = !ctx_menu.enabled[i];
        uint64_t row_bg = hov && !dis ? rgb(222, 234, 252) : rgb(250, 252, 255);
        uint64_t text_col = dis ? rgb(168, 176, 188) : (hov ? rgb(26, 50, 92) : rgb(42, 58, 84));
        uint64_t accent = hov && !dis ? rgb(90, 140, 220) : rgb(122, 150, 190);

        if (hov && !dis) {
            vga_fill_rounded_rect(mx + 6, cur_y + 2, mw - 12, item_h - 4, 8, row_bg);
        }

        draw_ctx_icon(mx + 9, cur_y + 6, ctx_menu.icons[i], hov && !dis);
        vga_draw_string(mx + 30, cur_y + 8, ctx_menu.items[i], text_col, 0xFFFFFFFF);

        if (ctx_menu.submenu[i]) {
            vga_draw_string(mx + mw - 22, cur_y + 8, ">", accent, 0xFFFFFFFF);
        }
        if (ctx_menu.checked[i]) {
            vga_draw_string(mx + mw - 34, cur_y + 8, "✓", rgb(60, 150, 90), 0xFFFFFFFF);
        }
        cur_y += item_h;
    }

    if (submenu.visible && submenu.item_count > 0) {
        int sm_w = submenu_calc_width();
        int sm_item_h = 32;
        int sm_h = submenu.item_count * sm_item_h + 12;
        int sm_x = submenu.x;
        int sm_y = submenu.y;
        if (sm_x + sm_w > (int)SCREEN_W - 4) sm_x = (int)SCREEN_W - sm_w - 4;
        if (sm_y + sm_h > (int)SCREEN_H - 4) sm_y = (int)SCREEN_H - sm_h - 4;
        if (sm_x < 4) sm_x = 4;
        if (sm_y < 4) sm_y = 4;

        vga_fill_rounded_rect(sm_x + 4, sm_y + 5, sm_w, sm_h, 10, rgb(10, 16, 28));
        vga_fill_rounded_rect(sm_x, sm_y, sm_w, sm_h, 10, rgb(252, 253, 255));
        vga_draw_rounded_rect(sm_x, sm_y, sm_w, sm_h, 10, rgb(188, 202, 226));
        vga_fill_rounded_rect(sm_x + 1, sm_y + 1, sm_w - 2, 24, 10, rgb(236, 242, 251));
        vga_draw_string(sm_x + 10, sm_y + 6, submenu_title_for_kind(submenu.kind), rgb(36, 54, 86), 0xFFFFFFFF);

        for (int i = 0; i < submenu.item_count; ++i) {
            int iy = sm_y + 24 + i * sm_item_h;
            bool hov = (mouse.x >= sm_x && mouse.x < sm_x + sm_w && mouse.y >= iy && mouse.y < iy + sm_item_h);
            if (hov) vga_fill_rounded_rect(sm_x + 4, iy + 2, sm_w - 8, sm_item_h - 4, 7, rgb(222, 234, 252));
            vga_draw_string(sm_x + 12, iy + 8, submenu.items[i], hov ? rgb(26, 50, 92) : rgb(42, 58, 84), 0xFFFFFFFF);
        }
    }
}


static void ctx_add_item_ex(const char* label, int icon, bool enabled, bool sep_before, int submenu_kind) {
    int i = ctx_menu.item_count;
    if (i >= CTX_MAX_ITEMS) return;
    if (sep_before && i > 0) {
        /* Insert separator */
        strncpy(ctx_menu.items[i], "", 63);
        ctx_menu.icons[i] = 0;
        ctx_menu.enabled[i] = FALSE;
        ctx_menu.checked[i] = FALSE;
        ctx_menu.separator[i] = TRUE;
        ctx_menu.submenu[i] = FALSE;
        ctx_menu.submenu_kind[i] = SUBMENU_KIND_NONE;
        ctx_menu.item_count++;
        i++;
        if (i >= CTX_MAX_ITEMS) return;
    }
    strncpy(ctx_menu.items[i], label, 63);
    ctx_menu.items[i][63] = '\0';
    ctx_menu.icons[i] = icon;
    ctx_menu.enabled[i] = enabled;
    ctx_menu.checked[i] = FALSE;
    ctx_menu.separator[i] = FALSE;
    ctx_menu.submenu[i] = (submenu_kind != SUBMENU_KIND_NONE);
    ctx_menu.submenu_kind[i] = submenu_kind;
    ctx_menu.item_count++;
}

static void ctx_add_item(const char* label, int icon, bool enabled, bool sep_before) {
    ctx_add_item_ex(label, icon, enabled, sep_before, SUBMENU_KIND_NONE);
}

static const char* submenu_title_for_kind(int kind) {
    switch (kind) {
        case SUBMENU_KIND_LANGUAGE: return gui_ctx_label("Language", "言語");
        case SUBMENU_KIND_OPEN:     return gui_ctx_label("Open Apps", "アプリを開く");
        case SUBMENU_KIND_SETTINGS: return gui_ctx_label("Settings", "設定");
        case SUBMENU_KIND_POWER:    return gui_ctx_label("Power", "電源");
        case SUBMENU_KIND_WINDOW:   return gui_ctx_label("Window", "ウィンドウ");
        case SUBMENU_KIND_NEW:      return gui_ctx_label("New", "新規作成");
        case SUBMENU_KIND_AUDIO:    return gui_ctx_label("Audio", "音声");
        case SUBMENU_KIND_WALLPAPER: return gui_ctx_label("Wallpaper", "壁紙");
        case SUBMENU_KIND_DESKTOP_ICON_SIZE: return gui_ctx_label("Icon Size", "アイコンサイズ");
        case SUBMENU_KIND_APP_ICON: return gui_ctx_label("App Icon", "アプリアイコン");
        default:                    return gui_ctx_label("Menu", "メニュー");
    }
}

void submenu_close(void) {
    submenu.visible = FALSE;
    submenu.item_count = 0;
    submenu.hovered = -1;
    submenu.parent_item = -1;
    submenu.kind = SUBMENU_KIND_NONE;
}

static void submenu_add_item(const char* label) {
    if (submenu.item_count >= 16) return;
    strncpy(submenu.items[submenu.item_count], label, 63);
    submenu.items[submenu.item_count][63] = '\0';
    submenu.item_count++;
}

static void submenu_open_kind(int kind, int x, int y) {
    submenu.visible = TRUE;
    submenu.x = x;
    submenu.y = y;
    submenu.item_count = 0;
    submenu.hovered = -1;
    submenu.parent_item = -1;
    submenu.kind = kind;

    switch (kind) {
        case SUBMENU_KIND_LANGUAGE:
            submenu_add_item(gui_language_label_english);
            submenu_add_item(gui_language_label_japanese);
            break;
        case SUBMENU_KIND_NEW:
            submenu_add_item(gui_ctx_label("New File", "新規ファイル"));
            submenu_add_item(gui_ctx_label("New Folder", "新規フォルダ"));
            break;
        case SUBMENU_KIND_APP_ICON:
            submenu_add_item(gui_ctx_label("File Manager", "ファイルマネージャー"));
            submenu_add_item(gui_ctx_label("Text Editor", "テキストエディター"));
            submenu_add_item(gui_ctx_label("Terminal", "ターミナル"));
            submenu_add_item(gui_ctx_label("Settings", "設定"));
            submenu_add_item(gui_ctx_label("NetSurf", "NetSurf"));
            submenu_add_item(gui_ctx_label("Calculator", "電卓"));
            submenu_add_item(gui_ctx_label("Storage", "ストレージ"));
            submenu_add_item(gui_ctx_label("Music Player", "ミュージック"));
            submenu_add_item(gui_ctx_label("Python IDE", "Python IDE"));
            submenu_add_item(gui_ctx_label("Spreadsheet", "表計算"));
            submenu_add_item(gui_ctx_label("Task Manager", "タスクマネージャー"));
            submenu_add_item(gui_ctx_label("Clock", "時計"));
            submenu_add_item(gui_ctx_label("System Info", "システム情報"));
            submenu_add_item(gui_ctx_label("Paint", "ペイント"));
            submenu_add_item(gui_ctx_label("Image Viewer", "画像ビューア"));
            submenu_add_item(gui_ctx_label("Graph", "グラフ"));
            submenu_add_item(gui_ctx_label("Memory Manager", "メモリマネージャー"));
            submenu_add_item(gui_ctx_label("C-OS Benchmark", "C-OS Benchmark"));
            submenu_add_item(gui_ctx_label("About", "情報"));
            break;
        case SUBMENU_KIND_OPEN:
            submenu_add_item(gui_ctx_label("Open Terminal", "ターミナル"));
            submenu_add_item(gui_ctx_label("Open File Manager", "ファイルマネージャー"));
            submenu_add_item(gui_ctx_label("Open Task Manager", "タスクマネージャー"));
            submenu_add_item(gui_ctx_label("Open NetSurf", "NetSurf"));
            submenu_add_item(gui_ctx_label("Open Calculator", "電卓"));
            submenu_add_item(gui_ctx_label("Open Spreadsheet", "表計算"));
            submenu_add_item(gui_ctx_label("Open Clock", "時計"));
            submenu_add_item(gui_ctx_label("Open System Info", "システム情報"));
            submenu_add_item(gui_ctx_label("Open Paint", "ペイント"));
            submenu_add_item(gui_ctx_label("Open Storage", "ストレージ"));
            submenu_add_item(gui_ctx_label("Open Music Player", "ミュージック"));
            submenu_add_item(gui_ctx_label("Open Image Viewer", "画像ビューア"));
            submenu_add_item(gui_ctx_label("Open Graph", "グラフ"));
            submenu_add_item(gui_ctx_label("Open Memory Manager", "メモリマネージャー"));
            submenu_add_item(gui_ctx_label("Open Python IDE", "Python IDE"));
            submenu_add_item(gui_ctx_label("Open C-OS Benchmark", "C-OS Benchmark"));
            break;
        case SUBMENU_KIND_SETTINGS:
            submenu_add_item(gui_ctx_label("System Settings", "システム設定"));
            submenu_add_item(gui_ctx_label("Toggle Dark Mode", "ダークモード切替"));
            submenu_add_item(gui_ctx_label("Toggle Window Animations", "アニメーション切替"));
            submenu_add_item(gui_ctx_label("Toggle Notifications", "通知切替"));
            submenu_add_item(gui_ctx_label("Toggle Mouse Raw Input", "マウス生入力切替"));
            submenu_add_item(gui_ctx_label("About C-OS", "バージョン情報"));
            break;
        case SUBMENU_KIND_AUDIO:
            submenu_add_item(gui_ctx_label("Open Music Player", "ミュージックプレーヤー"));
            submenu_add_item(gui_ctx_label("Test Audio", "音声テスト"));
            submenu_add_item(gui_ctx_label("Reload Music Library", "音楽ライブラリ再読込"));
            break;
        case SUBMENU_KIND_WALLPAPER:
            submenu_add_item(gui_ctx_label("Use Image Wallpaper", "画像壁紙を使う"));
            submenu_add_item(gui_ctx_label("Reload Image Wallpaper", "画像壁紙を再読込"));
            submenu_add_item(gui_ctx_label("Use Gradient Wallpaper", "グラデーション壁紙"));
            break;
        case SUBMENU_KIND_DESKTOP_ICON_SIZE:
            submenu_add_item(gui_ctx_label("Small", "小"));
            submenu_add_item(gui_ctx_label("Medium", "中"));
            submenu_add_item(gui_ctx_label("Large", "大"));
            break;
        case SUBMENU_KIND_POWER:
            submenu_add_item(gui_ctx_label("Show Desktop", "デスクトップを表示"));
            submenu_add_item(gui_ctx_label("Lock Screen", "画面ロック"));
            submenu_add_item(gui_ctx_label("Restart System", "再起動"));
            submenu_add_item(gui_ctx_label("Power Off", "電源を切る"));
            break;
        case SUBMENU_KIND_WINDOW:
            submenu_add_item(gui_ctx_label("Restore", "元に戻す"));
            submenu_add_item(gui_ctx_label("Minimize", "最小化"));
            submenu_add_item(gui_ctx_label("Maximize", "最大化"));
            submenu_add_item(gui_ctx_label("Center Window", "中央に配置"));
            submenu_add_item(gui_ctx_label("Move Left Half", "左半分へ"));
            submenu_add_item(gui_ctx_label("Move Right Half", "右半分へ"));
            submenu_add_item(gui_ctx_label("Show Desktop", "デスクトップを表示"));
            submenu_add_item(gui_ctx_label("Close Window", "閉じる"));
            break;
        default:
            submenu_close();
            break;
    }
}

static void open_submenu_for_item(int menu_x, int menu_w, int item_y, int kind) {
    submenu_open_kind(kind, menu_x + menu_w + 4, item_y - 2);

    if (!submenu.visible || submenu.item_count <= 0) return;

    const int sm_item_h = 32;
    const int sm_header_h = 24;
    const int sm_gap = 4;
    int sm_w = submenu_calc_width();
    int sm_h = sm_header_h + (submenu.item_count * sm_item_h) + 12 - 0;
    int sm_x = menu_x + menu_w + sm_gap;
    int sm_y = item_y - 2;

    if (sm_x + sm_w > (int)SCREEN_W - 4) {
        sm_x = menu_x - sm_w - sm_gap;
    }
    if (sm_x < 4) sm_x = 4;
    if (sm_y + sm_h > (int)SCREEN_H - 4) sm_y = (int)SCREEN_H - sm_h - 4;
    if (sm_y < 4) sm_y = 4;

    submenu.x = sm_x;
    submenu.y = sm_y;
}




/* Start menu - launcher style panel */
#define START_ENTRY_APP             1
#define START_ENTRY_SHOW_DESKTOP    2
#define START_ENTRY_LOCK            3
#define START_ENTRY_RESTART         4
#define START_ENTRY_POWEROFF        5
#define START_ENTRY_LANGUAGE_EN     6
#define START_ENTRY_LANGUAGE_JA     7
#define START_ENTRY_DARK_MODE       8
#define START_ENTRY_ANIMATIONS      9
#define START_ENTRY_NOTIFICATIONS  10
#define START_ENTRY_MOUSE_RAW      11
#define START_ENTRY_ABOUT          12
#define START_ENTRY_REFRESH_DESKTOP 13
#define START_ENTRY_TEST_AUDIO     14
#define START_ENTRY_USE_JPEG_WALLPAPER 15
#define START_ENTRY_APP_HISSAN      16

typedef struct {
    int action;
    int window_kind;
    const char* en;
    const char* ja;
} start_menu_entry_t;

static int start_menu_tab = 0;
static int start_menu_selected = -1;

static int start_menu_view_count(void);
static const start_menu_entry_t* start_menu_view_entry(int index);
static int start_menu_view_columns(void);
static void start_menu_sync_selection(bool reset_to_first);
static void start_menu_activate_selection(void);
static void start_menu_move_selection(int delta);

static const start_menu_entry_t start_pinned_entries[] = {
    { START_ENTRY_APP, WIN_FILE_MGR,    "File Manager",    "ファイルマネージャー" },
    { START_ENTRY_APP, WIN_BROWSER,      "NetSurf",         "NetSurf" },
    { START_ENTRY_APP, WIN_TERMINAL,     "Terminal",        "ターミナル" },
    { START_ENTRY_APP, WIN_TEXT_EDITOR,  "Text Editor",     "テキストエディター" },
    { START_ENTRY_APP, WIN_TASK_MGR,     "Task Manager",    "タスクマネージャー" },
    { START_ENTRY_APP, WIN_SETTINGS,     "Settings",        "設定" },
    { START_ENTRY_APP, WIN_CALC,         "Calculator",      "電卓" },
    { START_ENTRY_APP_HISSAN, WIN_CALC,   "Calculator (Hissan)", "電卓（ひっ算）" },
    { START_ENTRY_APP, WIN_PAINT,        "Paint",           "ペイント" },
    { START_ENTRY_APP, WIN_STORAGE,      "Storage",         "ストレージ" },
    { START_ENTRY_APP, WIN_MUSIC,        "Music Player",    "ミュージック" },
    { START_ENTRY_APP, WIN_PYTHON_IDE,   "Python IDE",      "Python IDE" },
    { START_ENTRY_APP, WIN_HTTP_DOWNLOADER, "HTTP Downloader", "HTTPダウンローダー" },
    { START_ENTRY_APP, WIN_VOXEL_GAME,   "C-OS Benchmark",  "C-OS Benchmark" },
    { START_ENTRY_APP, WIN_TINYGL_VIEWER, "TinyGL 3D Viewer", "TinyGL 3Dビューアー" },
    { START_ENTRY_APP, WIN_2DGAMES, "2DGAMES", "2Dゲーム" },
};

static const start_menu_entry_t start_tools_entries[] = {
    { START_ENTRY_APP, WIN_CLOCK,        "Clock",           "時計" },
    { START_ENTRY_APP, WIN_SYSINFO,      "System Info",     "システム情報" },
    { START_ENTRY_APP, WIN_SHEET,        "Spreadsheet",     "表計算" },
    { START_ENTRY_APP, WIN_PAINT,        "Paint",           "ペイント" },
    { START_ENTRY_APP, WIN_CALC,         "Calculator",      "電卓" },
    { START_ENTRY_APP, WIN_TEXT_EDITOR,  "Text Editor",     "テキストエディター" },
};

static const start_menu_entry_t start_more_entries[] = {
    { START_ENTRY_APP, WIN_JPEG,         "Image Viewer",     "画像ビューア" },
    { START_ENTRY_APP, WIN_CALC_GRAPH,   "Graph",           "グラフ" },
    { START_ENTRY_APP, WIN_MEMORY_MGR,   "Memory Manager",  "メモリマネージャー" },
};

static const start_menu_entry_t start_system_entries[] = {
    { START_ENTRY_SHOW_DESKTOP, -1,      "Show Desktop",    "デスクトップを表示" },
    { START_ENTRY_APP, WIN_SETTINGS,     "System Settings", "システム設定" },
    { START_ENTRY_LOCK,        -1,       "Lock Screen",     "画面ロック" },
    { START_ENTRY_RESTART,     -1,       "Restart",         "再起動" },
    { START_ENTRY_POWEROFF,    -1,       "Power Off",       "電源を切る" },
    { START_ENTRY_LANGUAGE_EN, -1,       "Language: English","言語: English" },
    { START_ENTRY_LANGUAGE_JA, -1,       "Language: 日本語", "言語: 日本語" },
    { START_ENTRY_DARK_MODE,   -1,       "Toggle Dark Mode", "ダークモード切替" },
    { START_ENTRY_ANIMATIONS,  -1,       "Toggle Animations", "アニメーション切替" },
    { START_ENTRY_NOTIFICATIONS,-1,      "Toggle Notifications", "通知切替" },
    { START_ENTRY_MOUSE_RAW,   -1,       "Toggle Mouse Raw", "マウス生入力切替" },
    { START_ENTRY_REFRESH_DESKTOP, -1,   "Refresh Desktop",  "デスクトップ更新" },
    { START_ENTRY_TEST_AUDIO,  -1,       "Test Audio",       "音声テスト" },
    { START_ENTRY_USE_JPEG_WALLPAPER, -1, "Image Wallpaper", "画像壁紙" },
    { START_ENTRY_ABOUT,       -1,       "About C-OS",       "バージョン情報" },
};

static void gui_open_window_by_kind(int kind);
static void gui_launch_window_kind(int kind);
static void start_menu_launch_entry(const start_menu_entry_t* e);

static char start_menu_search[64];
static bool start_menu_search_active = FALSE;
static const start_menu_entry_t* start_menu_results[32];
static int start_menu_result_count = 0;

static int start_menu_ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool start_menu_contains_ci(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return true;
    if (!hay || !hay[0]) return false;
    for (int i = 0; hay[i]; ++i) {
        int j = 0;
        while (needle[j] && hay[i + j] &&
               start_menu_ascii_lower((unsigned char)hay[i + j]) == start_menu_ascii_lower((unsigned char)needle[j])) {
            ++j;
        }
        if (!needle[j]) return true;
    }
    return false;
}

static bool start_menu_entry_matches(const start_menu_entry_t* e, const char* query) {
    if (!query || !query[0]) return true;
    return start_menu_contains_ci(e->en, query) || start_menu_contains_ci(e->ja, query);
}

static void start_menu_collect_results(const char* query) {
    start_menu_result_count = 0;
    const start_menu_entry_t* arrays[] = {
        start_pinned_entries,
        start_tools_entries,
        start_more_entries,
        start_system_entries,
    };
    const int counts[] = {
        (int)(sizeof(start_pinned_entries) / sizeof(start_pinned_entries[0])),
        (int)(sizeof(start_tools_entries) / sizeof(start_tools_entries[0])),
        (int)(sizeof(start_more_entries) / sizeof(start_more_entries[0])),
        (int)(sizeof(start_system_entries) / sizeof(start_system_entries[0])),
    };
    for (int a = 0; a < 4; ++a) {
        for (int i = 0; i < counts[a]; ++i) {
            const start_menu_entry_t* e = &arrays[a][i];
            if (!start_menu_entry_matches(e, query)) continue;
            bool seen = false;
            for (int j = 0; j < start_menu_result_count; ++j) {
                const start_menu_entry_t* s = start_menu_results[j];
                if (s->action == e->action && s->window_kind == e->window_kind) { seen = true; break; }
            }
            if (seen) continue;
            if (start_menu_result_count < (int)(sizeof(start_menu_results) / sizeof(start_menu_results[0]))) {
                start_menu_results[start_menu_result_count++] = e;
            }
        }
    }
}

static int start_menu_icon_for_entry(const start_menu_entry_t* e) {
    if (!e) return 11;

    switch (e->action) {
        case START_ENTRY_SHOW_DESKTOP: return 11;
        case START_ENTRY_LOCK: return 7;
        case START_ENTRY_RESTART: return 14;
        case START_ENTRY_POWEROFF: return 14;
        case START_ENTRY_LANGUAGE_EN:
        case START_ENTRY_LANGUAGE_JA: return 7;
        case START_ENTRY_DARK_MODE: return 11;
        case START_ENTRY_ANIMATIONS: return 9;
        case START_ENTRY_NOTIFICATIONS: return 7;
        case START_ENTRY_MOUSE_RAW: return 11;
        case START_ENTRY_ABOUT: return 7;
        case START_ENTRY_REFRESH_DESKTOP: return 11;
        case START_ENTRY_TEST_AUDIO: return 12;
        case START_ENTRY_USE_JPEG_WALLPAPER: return 4;
        case START_ENTRY_APP_HISSAN: return 8;
        default: break;
    }

    switch (e->window_kind) {
        case WIN_FILE_MGR: return 4;
        case WIN_BROWSER: return 15;
        case WIN_TERMINAL: return 3;
        case WIN_TEXT_EDITOR: return 1;
        case WIN_TASK_MGR: return 7;
        case WIN_SETTINGS: return 11;
        case WIN_CALC: return 8;
        case WIN_PAINT: return 13;
        case WIN_STORAGE: return 9;
        case WIN_MUSIC: return 12;
        case WIN_PYTHON_IDE: return 13;
        case WIN_VOXEL_GAME: return 6;
        case WIN_JPEG: return 4;
        case WIN_CALC_GRAPH: return 5;
        case WIN_MEMORY_MGR: return 9;
        case WIN_SYSINFO: return 7;
        case WIN_CLOCK: return 10;
        case WIN_SHEET: return 5;
        case WIN_ABOUT: return 7;
        default: return 11;
    }
}

void start_menu_clear_search(void) {
    start_menu_search[0] = 0;
    start_menu_search_active = TRUE;
    start_menu_result_count = 0;
    start_menu_selected = 0;
}

static void start_menu_type_char(char ascii) {
    int len = (int)strlen(start_menu_search);
    if (len < 63) {
        start_menu_search[len] = ascii;
        start_menu_search[len + 1] = 0;
    }
    start_menu_search_active = TRUE;
    start_menu_collect_results(start_menu_search);
    start_menu_selected = 0;
    gui_request_redraw();
}

void handle_start_menu_keyboard(void) {
    while (keyboard_has_event()) {
        keyboard_event_t ev;
        if (!gui_pop_keyboard_event(&ev)) break;
        if (!ev.pressed) continue;
        char ascii = ev.ascii;
        uint8_t key = ev.scancode;
        bool ctrl = ev.ctrl || ((ev.modifiers & KEYBOARD_MOD_CTRL) != 0);
        bool shift = ev.shift || ((ev.modifiers & KEYBOARD_MOD_SHIFT) != 0);

        if (key == KEY_ESC) {
            ctx_menu.visible = FALSE;
            submenu_close();
            start_menu_clear_search();
            return;
        }
        if (ctrl && (ascii == 'f' || ascii == 'F')) {
            start_menu_search_active = TRUE;
            continue;
        }
        if (key == KEY_TAB) {
            if (shift) {
                start_menu_tab = (start_menu_tab + 3) % 4;
            } else {
                start_menu_tab = (start_menu_tab + 1) % 4;
            }
            start_menu_clear_search();
            continue;
        }
        if (key == KEY_ENTER) {
            if (start_menu_search[0]) {
                start_menu_collect_results(start_menu_search);
            }
            start_menu_activate_selection();
            ctx_menu.visible = FALSE;
            submenu_close();
            start_menu_clear_search();
            return;
        }
        if (key == KEY_BACKSPACE || ascii == '\b') {
            int len = (int)strlen(start_menu_search);
            if (len > 0) start_menu_search[len - 1] = 0;
            start_menu_collect_results(start_menu_search);
            start_menu_selected = 0;
            gui_request_redraw();
            continue;
        }
        if (key == KEY_LEFT) { start_menu_move_selection(-1); continue; }
        if (key == KEY_RIGHT) { start_menu_move_selection(+1); continue; }
        if (key == KEY_UP) { start_menu_move_selection(-start_menu_view_columns()); continue; }
        if (key == KEY_DOWN) { start_menu_move_selection(start_menu_view_columns()); continue; }
        if (key == KEY_HOME) { start_menu_selected = 0; gui_request_redraw(); continue; }
        if (key == KEY_END) {
            int count = start_menu_view_count();
            if (count > 0) start_menu_selected = count - 1;
            gui_request_redraw();
            continue;
        }
        if (!ctrl && ascii >= 32 && ascii < 127) {
            start_menu_type_char(ascii);
            continue;
        }
    }
}


static void gui_open_window_by_kind(int kind) {
    switch (kind) {
        case WIN_TERMINAL:     gui_open_window(WIN_TERMINAL, gui_text("Terminal", "ターミナル"), 140, 120, 840, 560); break;
        case WIN_FILE_MGR:     gui_open_window(WIN_FILE_MGR, gui_text("File Manager", "ファイルマネージャー"), 72, 56, 1120, 760); break;
        case WIN_TEXT_EDITOR:  gui_open_window(WIN_TEXT_EDITOR, gui_text("Text Editor", "テキストエディター"), 120, 72, 980, 680); break;
        case WIN_BROWSER:      gui_open_window(WIN_BROWSER, gui_text("NetSurf", "NetSurf"), 96, 64, 1160, 760); break;
        case WIN_CALC:         gui_open_window(WIN_CALC, gui_text("Calculator", "電卓"), 160, 120, 760, 560); break;
        case WIN_SHEET:        gui_open_window(WIN_SHEET, gui_text("Spreadsheet", "表計算"), 120, 72, 1120, 760); break;
        case WIN_CLOCK:        gui_open_window(WIN_CLOCK, gui_text("Clock", "時計"), 220, 140, 540, 420); break;
        case WIN_SYSINFO:      gui_open_window(WIN_SYSINFO, gui_text("System Info", "システム情報"), 160, 90, 900, 620); break;
        case WIN_PAINT:        gui_open_window(WIN_PAINT, gui_text("Paint", "ペイント"), 120, 72, 1080, 720); break;
        case WIN_STORAGE:      gui_open_window(WIN_STORAGE, gui_text("Storage", "ストレージ"), 120, 80, 1060, 720); break;
        case WIN_TASK_MGR:     gui_open_window(WIN_TASK_MGR, gui_text("Task Manager", "タスクマネージャー"), 120, 72, 1040, 700); break;
        case WIN_MUSIC:        gui_open_window(WIN_MUSIC, gui_text("Music Player", "ミュージックプレーヤー"), 120, 72, 1120, 720); break;
        case WIN_JPEG:         gui_open_window(WIN_JPEG, gui_text("Image Viewer", "画像ビューア"), 140, 88, 1080, 760); break;
        case WIN_CALC_GRAPH:   gui_open_window(WIN_CALC_GRAPH, gui_text("Graph", "グラフ"), 180, 90, 760, 520); break;
        case WIN_MEMORY_MGR:   gui_open_window(WIN_MEMORY_MGR, gui_text("Memory Manager", "メモリマネージャー"), 120, 72, 1120, 760); break;
        case WIN_PYTHON_IDE:   gui_open_window(WIN_PYTHON_IDE, gui_text("Python IDE", "Python IDE"), 100, 50, 1000, 700); break;
        case WIN_HTTP_DOWNLOADER: gui_open_window(WIN_HTTP_DOWNLOADER, gui_text("HTTP Downloader", "HTTPダウンローダー"), 120, 72, 1060, 760); break;
        case WIN_VOXEL_GAME:    gui_open_window(WIN_VOXEL_GAME, gui_text("C-OS Benchmark", "C-OS Benchmark"), 32, 24, 1260, 820); break;
        case WIN_TINYGL_VIEWER: gui_open_window(WIN_TINYGL_VIEWER, gui_text("TinyGL 3D Viewer", "TinyGL 3Dビューアー"), 160, 96, 760, 620); break;
        case WIN_2DGAMES: gui_open_window(WIN_2DGAMES, gui_text("2DGAMES", "2Dゲーム"), 180, 96, 760, 620); break;
        case WIN_SETTINGS:     gui_open_window(WIN_SETTINGS, gui_text("Settings", "設定"), 140, 90, 1080, 760); break;
        case WIN_ABOUT:        gui_open_window(WIN_ABOUT, gui_text("About", "情報"), 260, 140, 640, 420); break;
        default: break;
    }
}

static void gui_launch_window_kind(int kind) {
    int existing = gui_find_window(kind);
    if (existing >= 0) {
        gui_restore_window(existing);
        gui_bring_to_front(existing);
        return;
    }
    gui_open_window_by_kind(kind);
}

static void start_menu_launch_entry(const start_menu_entry_t* e) {
    if (!e) return;
    switch (e->action) {
        case START_ENTRY_APP:
            gui_launch_window_kind(e->window_kind);
            break;
        case START_ENTRY_SHOW_DESKTOP:
            gui_show_desktop();
            break;
        case START_ENTRY_LOCK:
            show_password_screen();
            break;
        case START_ENTRY_RESTART:
            gui_notify(gui_text("System restarting...", "システムを再起動しています..."), 1);
            bios_reboot();
            break;
        case START_ENTRY_POWEROFF:
            gui_notify(gui_text("Powering off...", "電源を切っています..."), 1);
            bios_shutdown();
            break;
        case START_ENTRY_LANGUAGE_EN:
            gui_switch_language_with_loading(0);
            break;
        case START_ENTRY_LANGUAGE_JA:
            gui_switch_language_with_loading(1);
            break;
        case START_ENTRY_DARK_MODE:
            gui_toggle_dark_mode();
            gui_notify_simple(gui_text("Dark mode toggled", "ダークモードを切り替えました"));
            break;
        case START_ENTRY_ANIMATIONS:
            gui_set_window_animations(!gui_get_window_animations());
            gui_notify_simple(gui_text("Window animations toggled", "アニメーションを切り替えました"));
            break;
        case START_ENTRY_NOTIFICATIONS:
            gui_set_notifications_enabled(!gui_get_notifications_enabled());
            gui_notify_simple(gui_text("Notifications toggled", "通知を切り替えました"));
            break;
        case START_ENTRY_MOUSE_RAW:
            gui_set_mouse_raw_input(!gui_get_mouse_raw_input());
            gui_notify_simple(gui_text("Mouse raw input toggled", "マウス生入力を切り替えました"));
            break;
        case START_ENTRY_REFRESH_DESKTOP:
            gui_sync_desktop_with_fs();
            gui_notify_simple(gui_text("Desktop refreshed", "デスクトップを更新しました"));
            break;
        case START_ENTRY_TEST_AUDIO:
            gui_play_test_tone();
            break;
        case START_ENTRY_USE_JPEG_WALLPAPER:
            gui_set_wallpaper(gui_get_wallpaper_count() - 1);
            gui_notify_simple(gui_text("Image wallpaper enabled", "画像壁紙を有効化しました"));
            break;
        case START_ENTRY_ABOUT:
            gui_launch_window_kind(WIN_ABOUT);
            break;
        case START_ENTRY_APP_HISSAN: {
            window_t* w = gui_open_window(WIN_CALC, gui_text("Calculator", "電卓"), 160, 120, 760, 560);
            if (w) {
                w->calc_mode = 3;
                w->calc_angle_deg = TRUE;
                w->calc_initialized = TRUE;
                w->calc_clear_next = TRUE;
                w->calc_acc = 0.0;
                w->calc_mem = 0.0;
                w->calc_op = 0;
                w->calc_topic_idx = 0;
                w->calc_expr[0] = '\0';
                w->calc_display[0] = '\0';
                w->calc_steps[0] = '\0';
                strncpy(w->calc_status, "Hissan mode", sizeof(w->calc_status) - 1);
                w->calc_status[sizeof(w->calc_status) - 1] = '\0';
            }
            break;
        }
        default:
            break;
    }
}

static void start_menu_rect(int* out_x, int* out_y, int* out_w, int* out_h) {
    /* Never re-apply a design minimum after clamping it to the actual
     * framebuffer.  The old 500x360 minimum expanded a menu back beyond a
     * 480px/low-height desktop, so opening Start drew outside the desktop and
     * broke both paint and hit-testing. */
    int avail_w = (int)SCREEN_W - 8;
    int avail_h = (int)SCREEN_H - TASKBAR_H - 8;
    if (avail_w < 1) avail_w = 1;
    if (avail_h < 1) avail_h = 1;

    int mw = 620;
    int mh = 430;
    if (mw > avail_w) mw = avail_w;
    if (mh > avail_h) mh = avail_h;

    int mx = ctx_menu.x;
    int my = ctx_menu.y;
    int max_x = (int)SCREEN_W - mw - 4;
    int max_y = (int)SCREEN_H - TASKBAR_H - mh - 4;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    if (mx > max_x) mx = max_x;
    if (my > max_y) my = max_y;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (out_x) *out_x = mx;
    if (out_y) *out_y = my;
    if (out_w) *out_w = mw;
    if (out_h) *out_h = mh;
}

static int start_menu_view_count(void) {
    if (start_menu_search[0]) return start_menu_result_count;
    switch (start_menu_tab) {
        case 1: return (int)(sizeof(start_system_entries) / sizeof(start_system_entries[0]));
        case 2: return (int)(sizeof(start_tools_entries) / sizeof(start_tools_entries[0]));
        case 3: return (int)(sizeof(start_more_entries) / sizeof(start_more_entries[0]));
        default: return (int)(sizeof(start_pinned_entries) / sizeof(start_pinned_entries[0]));
    }
}

static const start_menu_entry_t* start_menu_view_entry(int index) {
    if (index < 0) return NULL;
    if (start_menu_search[0]) {
        if (index < start_menu_result_count) return start_menu_results[index];
        return NULL;
    }
    switch (start_menu_tab) {
        case 1: {
            int count = (int)(sizeof(start_system_entries) / sizeof(start_system_entries[0]));
            return (index < count) ? &start_system_entries[index] : NULL;
        }
        case 2: {
            int count = (int)(sizeof(start_tools_entries) / sizeof(start_tools_entries[0]));
            return (index < count) ? &start_tools_entries[index] : NULL;
        }
        case 3: {
            int count = (int)(sizeof(start_more_entries) / sizeof(start_more_entries[0]));
            return (index < count) ? &start_more_entries[index] : NULL;
        }
        default: {
            int count = (int)(sizeof(start_pinned_entries) / sizeof(start_pinned_entries[0]));
            return (index < count) ? &start_pinned_entries[index] : NULL;
        }
    }
}

static int start_menu_view_columns(void) {
    if (start_menu_search[0]) {
        int mw = 620;
        if (mw > (int)SCREEN_W - 16) mw = (int)SCREEN_W - 16;
        if (mw < 500) mw = 500;
        int content_w = mw - 132 - 22;
        return (content_w >= 360) ? 2 : 1;
    }
    return (start_menu_tab == 1) ? 2 : ((start_menu_tab == 3) ? 2 : 3);
}

static void start_menu_sync_selection(bool reset_to_first) {
    int count = start_menu_view_count();
    if (count <= 0) {
        start_menu_selected = -1;
        return;
    }
    if (reset_to_first || start_menu_selected < 0 || start_menu_selected >= count) {
        start_menu_selected = 0;
    }
}

static void start_menu_activate_selection(void) {
    start_menu_sync_selection(FALSE);
    const start_menu_entry_t* e = start_menu_view_entry(start_menu_selected);
    if (!e) return;
    start_menu_launch_entry(e);
}

static void start_menu_move_selection(int delta) {
    int count = start_menu_view_count();
    if (count <= 0) {
        start_menu_selected = -1;
        return;
    }
    start_menu_sync_selection(FALSE);
    int next = start_menu_selected + delta;
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    start_menu_selected = next;
    gui_request_redraw();
}

static void draw_start_tile(int x, int y, int w, int h, int icon, const char* label, bool hov) {
    uint64_t bg = hov ? rgb(225, 236, 251) : rgb(246, 249, 254);
    uint64_t border = hov ? rgb(92, 138, 216) : rgb(190, 204, 226);
    vga_fill_rounded_rect(x, y, w, h, 10, bg);
    vga_draw_rounded_rect(x, y, w, h, 10, border);

    char label_buf[96];
    int label_max_w = w - 60;
    if (label_max_w < 40) label_max_w = 40;
    gui_fit_text_to_width(label, label_buf, sizeof(label_buf), label_max_w);

    draw_ctx_icon(x + 8, y + 8, icon, hov);
    vga_draw_string(x + 48, y + 12, label_buf, hov ? rgb(26, 50, 92) : rgb(42, 58, 84), 0xFFFFFFFF);
}

static void draw_start_menu(void) {
    int mx, my, mw, mh;
    start_menu_rect(&mx, &my, &mw, &mh);

    const int header_h = 58;
    const int rail_w = 132;
    const int footer_h = 34;

    vga_fill_rounded_rect(mx + 4, my + 6, mw, mh, 16, rgb(10, 16, 28));
    vga_fill_rounded_rect(mx, my, mw, mh, 16, rgb(248, 251, 255));
    vga_draw_rounded_rect(mx, my, mw, mh, 16, rgb(183, 199, 224));
    vga_fill_rounded_rect(mx + 1, my + 1, mw - 2, header_h, 16, rgb(234, 241, 250));
    vga_fill_rect(mx + 1, my + header_h - 1, mw - 2, 1, rgb(202, 218, 239));

    vga_draw_circle(mx + 24, my + 26, 12, rgb(90, 140, 220));
    vga_fill_circle(mx + 24, my + 26, 7, rgb(235, 242, 252));
    vga_draw_string(mx + 44, my + 14, gui_text("Start", "スタート"), rgb(34, 54, 86), 0xFFFFFFFF);
    vga_draw_string(mx + 44, my + 30, gui_text("Choose an app or system action", "アプリやシステム操作を選択"), rgb(92, 108, 130), 0xFFFFFFFF);
    int search_w = 220;
    int search_x = mx + mw - search_w - 18;
    int search_y = my + 14;
    vga_fill_rounded_rect(search_x, search_y, search_w, 28, 8, start_menu_search_active ? rgb(255, 255, 255) : rgb(246, 249, 253));
    vga_draw_rounded_rect(search_x, search_y, search_w, 28, 8, rgb(189, 203, 224));
    vga_draw_string(search_x + 10, search_y + 8, start_menu_search[0] ? start_menu_search : gui_text("Search apps...", "アプリ検索..."), start_menu_search[0] ? rgb(34, 54, 86) : rgb(122, 138, 160), 0xFFFFFFFF);

    int rail_x = mx + 12;
    int rail_y = my + header_h + 12;
    int rail_h = mh - header_h - footer_h - 20;
    vga_fill_rounded_rect(rail_x, rail_y, rail_w - 16, rail_h, 12, rgb(240, 245, 252));
    vga_draw_rounded_rect(rail_x, rail_y, rail_w - 16, rail_h, 12, rgb(210, 223, 241));

    const char* tab_labels[4] = {
        gui_text("Pinned", "ピン留め"),
        gui_text("System", "システム"),
        gui_text("Tools", "ツール"),
        gui_text("More Apps", "その他アプリ")
    };
    for (int i = 0; i < 4; ++i) {
        int by = rail_y + 14 + i * 54;
        bool hov = (mouse.x >= rail_x + 8 && mouse.x < rail_x + rail_w - 24 && mouse.y >= by && mouse.y < by + 38);
        bool sel = (start_menu_tab == i);
        uint64_t bg = sel ? rgb(222, 234, 252) : (hov ? rgb(232, 240, 250) : rgb(247, 250, 253));
        vga_fill_rounded_rect(rail_x + 8, by, rail_w - 32, 38, 9, bg);
        vga_draw_rounded_rect(rail_x + 8, by, rail_w - 32, 38, 9, sel ? rgb(92, 138, 216) : rgb(203, 214, 232));
        vga_draw_string(rail_x + 18, by + 11, tab_labels[i], sel ? rgb(26, 50, 92) : rgb(42, 58, 84), 0xFFFFFFFF);
    }

    int content_x = mx + rail_w + 10;
    int content_y = my + header_h + 14;
    int content_w = mw - rail_w - 22;
    int gap = 10;
    int action_h = 40;

    if (start_menu_search[0]) {
        start_menu_collect_results(start_menu_search);
        vga_draw_string(content_x, content_y - 2, gui_text("Search results", "検索結果"), rgb(36, 54, 86), 0xFFFFFFFF);
        char hint[96];
        snprintf(hint, sizeof(hint), "%d matches", start_menu_result_count);
        vga_draw_string(content_x + 146, content_y - 2, hint, rgb(92, 108, 130), 0xFFFFFFFF);
        if (start_menu_result_count <= 0) {
            vga_draw_string(content_x, content_y + 42, gui_text("No matches", "一致する項目がありません"), rgb(92, 108, 130), 0xFFFFFFFF);
        } else {
            int cols = (content_w >= 360) ? 2 : 1;
            int tile_w = (content_w - (cols - 1) * gap) / cols;
            if (tile_w < 150) tile_w = 150;
            int base_y = content_y + 18;
            for (int i = 0; i < start_menu_result_count; ++i) {
                int row = i / cols;
                int col = i % cols;
                int bx = content_x + col * (tile_w + gap);
                int by = base_y + row * (action_h + 10);
                bool hov = (mouse.x >= bx && mouse.x < bx + tile_w && mouse.y >= by && mouse.y < by + action_h);
                bool sel = (start_menu_selected == i);
                draw_start_tile(bx, by, tile_w, action_h, start_menu_icon_for_entry(start_menu_results[i]), gui_ctx_label(start_menu_results[i]->en, start_menu_results[i]->ja), hov || sel);
            }
        }
    } else {
        const start_menu_entry_t* entries = start_pinned_entries;
        int count = (int)(sizeof(start_pinned_entries) / sizeof(start_pinned_entries[0]));
        const char* section = gui_text("Pinned apps", "ピン留めアプリ");
        int cols = 3;
        int tile_h = 58;

        if (start_menu_tab == 1) {
            entries = start_system_entries;
            count = (int)(sizeof(start_system_entries) / sizeof(start_system_entries[0]));
            section = gui_text("System actions", "システム操作");
            cols = 2;
            tile_h = 42;
        } else if (start_menu_tab == 2) {
            entries = start_tools_entries;
            count = (int)(sizeof(start_tools_entries) / sizeof(start_tools_entries[0]));
            section = gui_text("Tools", "ツール");
            cols = 2;
            tile_h = 58;
        } else if (start_menu_tab == 3) {
            entries = start_more_entries;
            count = (int)(sizeof(start_more_entries) / sizeof(start_more_entries[0]));
            section = gui_text("More apps", "その他アプリ");
            cols = 2;
            tile_h = 58;
        }

        vga_draw_string(content_x, content_y - 2, section, rgb(36, 54, 86), 0xFFFFFFFF);

        if (start_menu_tab == 1) {
            int row_y = content_y + 22;
            for (int i = 0; i < count; ++i) {
                int row = i / 2;
                int col = i % 2;
                int bw = (content_w - gap) / 2;
                int bx = content_x + col * (bw + gap);
                int by = row_y + row * (action_h + 8);
                bool hov = (mouse.x >= bx && mouse.x < bx + bw && mouse.y >= by && mouse.y < by + action_h);
                bool sel = (start_menu_selected == i);
                uint64_t bg = (hov || sel) ? rgb(222, 234, 252) : rgb(247, 250, 253);
                vga_fill_rounded_rect(bx, by, bw, action_h, 9, bg);
                vga_draw_rounded_rect(bx, by, bw, action_h, 9, rgb(201, 214, 233));
                char label_buf[96];
                int label_max_w = bw - 52;
                if (label_max_w < 40) label_max_w = 40;
                gui_fit_text_to_width(gui_ctx_label(entries[i].en, entries[i].ja), label_buf, sizeof(label_buf), label_max_w);
                int icon = start_menu_icon_for_entry(&entries[i]);
                draw_ctx_icon(bx + 9, by + 8, icon, hov);
                vga_draw_string(bx + 36, by + 11, label_buf, hov ? rgb(26, 50, 92) : rgb(42, 58, 84), 0xFFFFFFFF);
            }
        } else {
            int usable_w = content_w;
            int tile_w = (usable_w - (cols - 1) * gap) / cols;
            int base_y = content_y + 18;
            for (int i = 0; i < count; ++i) {
                int row = i / cols;
                int col = i % cols;
                int bx = content_x + col * (tile_w + gap);
                int by = base_y + row * (tile_h + gap);
                bool hov = (mouse.x >= bx && mouse.x < bx + tile_w && mouse.y >= by && mouse.y < by + tile_h);
                bool sel = (start_menu_selected == i);
                int icon = start_menu_icon_for_entry(&entries[i]);
                draw_start_tile(bx, by, tile_w, tile_h, icon, gui_ctx_label(entries[i].en, entries[i].ja), hov || sel);
            }
        }
    }

    vga_fill_rounded_rect(mx + 12, my + mh - footer_h - 6, mw - 24, footer_h, 10, rgb(240, 245, 252));
    vga_draw_rounded_rect(mx + 12, my + mh - footer_h - 6, mw - 24, footer_h, 10, rgb(210, 223, 241));
    vga_draw_string(mx + 22, my + mh - footer_h + 2, gui_text("Tip: click tabs on the left", "左のタブをクリックできます"), rgb(92, 108, 130), 0xFFFFFFFF);
}


static void handle_start_menu_click(int x, int y) {
    int mx, my, mw, mh;
    start_menu_rect(&mx, &my, &mw, &mh);

    if (x < mx || x >= mx + mw || y < my || y >= my + mh) {
        ctx_menu.visible = FALSE;
        start_menu_clear_search();
        return;
    }

    const int header_h = 58;
    const int rail_w = 132;
    const int footer_h = 34;
    const int rail_x = mx + 12;
    const int rail_y = my + header_h + 12;
    const int rail_btn_w = rail_w - 32;
    int search_w = 220;
    int search_x = mx + mw - search_w - 18;
    int search_y = my + 14;

    if (x >= search_x && x < search_x + search_w && y >= search_y && y < search_y + 28) {
        start_menu_search_active = TRUE;
        gui_request_redraw();
        return;
    }

    if (!start_menu_search[0]) {
        for (int i = 0; i < 4; ++i) {
            int by = rail_y + 14 + i * 54;
            if (x >= rail_x + 8 && x < rail_x + 8 + rail_btn_w && y >= by && y < by + 38) {
                start_menu_tab = i;
                start_menu_clear_search();
                gui_request_redraw();
                return;
            }
        }
    }

    int content_x = mx + rail_w + 10;
    int content_y = my + header_h + 14;
    int content_w = mw - rail_w - 22;
    int gap = 10;
    int action_h = 40;

    if (start_menu_search[0]) {
        start_menu_collect_results(start_menu_search);
        if (start_menu_result_count <= 0) return;
        int cols = (content_w >= 360) ? 2 : 1;
        int tile_w = (content_w - (cols - 1) * gap) / cols;
        if (tile_w < 150) tile_w = 150;
        int base_y = content_y + 18;
        for (int i = 0; i < start_menu_result_count; ++i) {
            int row = i / cols;
            int col = i % cols;
            int bx = content_x + col * (tile_w + gap);
            int by = base_y + row * (action_h + 10);
            if (x >= bx && x < bx + tile_w && y >= by && y < by + action_h) {
                start_menu_selected = i;
                start_menu_launch_entry(start_menu_results[i]);
                ctx_menu.visible = FALSE;
                submenu_close();
                start_menu_clear_search();
                return;
            }
        }
        return;
    }

    const start_menu_entry_t* entries = start_pinned_entries;
    int count = (int)(sizeof(start_pinned_entries) / sizeof(start_pinned_entries[0]));
    int cols = 3;
    int tile_h = 58;

    if (start_menu_tab == 1) {
        entries = start_system_entries;
        count = (int)(sizeof(start_system_entries) / sizeof(start_system_entries[0]));
        cols = 2;
        tile_h = 42;
    } else if (start_menu_tab == 2) {
        entries = start_tools_entries;
        count = (int)(sizeof(start_tools_entries) / sizeof(start_tools_entries[0]));
        cols = 2;
        tile_h = 58;
    } else if (start_menu_tab == 3) {
        entries = start_more_entries;
        count = (int)(sizeof(start_more_entries) / sizeof(start_more_entries[0]));
        cols = 2;
        tile_h = 58;
    }

    if (start_menu_tab == 1) {
        int row_y = content_y + 22;
        for (int i = 0; i < count; ++i) {
            int row = i / 2;
            int col = i % 2;
            int bw = (content_w - gap) / 2;
            int bx = content_x + col * (bw + gap);
            int by = row_y + row * (action_h + 8);
            if (x >= bx && x < bx + bw && y >= by && y < by + action_h) {
                start_menu_selected = i;
                start_menu_launch_entry(&entries[i]);
                ctx_menu.visible = FALSE;
                submenu_close();
                start_menu_clear_search();
                return;
            }
        }
    } else {
        int usable_w = content_w;
        int tile_w = (usable_w - (cols - 1) * gap) / cols;
        int base_y = content_y + 18;
        for (int i = 0; i < count; ++i) {
            int row = i / cols;
            int col = i % cols;
            int bx = content_x + col * (tile_w + gap);
            int by = base_y + row * (tile_h + gap);
            if (x >= bx && x < bx + tile_w && y >= by && y < by + tile_h) {
                start_menu_selected = i;
                start_menu_launch_entry(&entries[i]);
                ctx_menu.visible = FALSE;
                submenu_close();
                start_menu_clear_search();
                return;
            }
        }
    }

    if (x >= mx + 12 && x < mx + mw - 12 && y >= my + mh - footer_h - 6 && y < my + mh) {
        ctx_menu.visible = FALSE;
        start_menu_clear_search();
        return;
    }
}
static void gui_minimize_all_windows(void) {
    for (int i = 0; i < window_count; ++i) {
        if (!windows[i].visible) continue;
        windows[i].minimized = TRUE;
    }
    gui_request_redraw();
}

static void gui_show_desktop(void) {
    gui_minimize_all_windows();
    gui_notify_simple(gui_text("Desktop shown", "デスクトップを表示しました"));
}

static void gui_tile_window_half(int idx, bool left_side) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    if (!w->visible) return;
    gui_restore_window(idx);
    w->x = left_side ? 0 : ((int)SCREEN_W / 2);
    w->y = 0;
    w->w = (int)SCREEN_W / 2;
    if (left_side) {
        w->w = (int)SCREEN_W / 2;
    }
    w->h = (int)SCREEN_H - TASKBAR_H;
    w->maximized = FALSE;
    w->minimized = FALSE;
    gui_request_redraw();
}

void open_taskbar_context_menu(int x, int y) {
    ctx_menu.visible = TRUE;
    ctx_menu.x = x;
    ctx_menu.y = y;
    ctx_menu.target_window = -1;
    ctx_menu.target_icon = -1;
    ctx_menu.item_count = 0;
    ctx_menu.context_type = 3;
    submenu_close();

    ctx_add_item_ex(gui_ctx_label("Open Apps", "アプリを開く"), 11, TRUE, FALSE, SUBMENU_KIND_OPEN);
    ctx_add_item_ex(gui_ctx_label("Audio", "音声"), 12, TRUE, FALSE, SUBMENU_KIND_AUDIO);
    ctx_add_item_ex(gui_ctx_label("Wallpaper", "壁紙"), 11, TRUE, FALSE, SUBMENU_KIND_WALLPAPER);
    ctx_add_item_ex(gui_ctx_label("Settings", "設定"), 11, TRUE, FALSE, SUBMENU_KIND_SETTINGS);
    ctx_add_item_ex(gui_ctx_label("Power", "電源"), 14, TRUE, FALSE, SUBMENU_KIND_POWER);
    ctx_add_item_ex(gui_ctx_label("Language", "言語切替"), 7, TRUE, FALSE, SUBMENU_KIND_LANGUAGE);
    ctx_add_item(gui_ctx_label("Show Desktop", "デスクトップを表示"), 11, TRUE, FALSE);
    ctx_add_item(gui_ctx_label("Lock Screen", "画面ロック"), 7, TRUE, FALSE);
    ctx_add_item(gui_ctx_label("About C-OS", "バージョン情報"), 7, TRUE, FALSE);
}

void open_start_menu(int x, int y) {
    ctx_menu.visible = TRUE;
    ctx_menu.x = x;
    ctx_menu.y = y;
    ctx_menu.target_window = -1;
    ctx_menu.target_icon = -1;
    ctx_menu.item_count = 0;
    ctx_menu.context_type = 2;
    submenu_close();
    start_menu_tab = 0;
    start_menu_clear_search();
    start_menu_selected = 0;

    /* Keep it above the taskbar but let the launcher draw itself. */
    const int start_w = 620;
    const int start_h = 430;
    ctx_menu.x = 12;
    ctx_menu.y = (int)SCREEN_H - TASKBAR_H - start_h - 8;
    if (ctx_menu.x + start_w > (int)SCREEN_W - 4) ctx_menu.x = (int)SCREEN_W - start_w - 4;
    if (ctx_menu.y < 4) ctx_menu.y = 4;
}
static bool menu_label_matches(const char* sel, const char* en, const char* ja) {
    if (!sel) return false;
    return (strncmp(sel, en, 63) == 0) || (strncmp(sel, ja, 63) == 0);
}

static int desktop_app_kind_from_label(const char* sel) {
    if (menu_label_matches(sel, "File Manager", "ファイルマネージャー")) return WIN_FILE_MGR;
    if (menu_label_matches(sel, "Text Editor", "テキストエディター")) return WIN_TEXT_EDITOR;
    if (menu_label_matches(sel, "Terminal", "ターミナル")) return WIN_TERMINAL;
    if (menu_label_matches(sel, "Settings", "設定")) return WIN_SETTINGS;
    if (menu_label_matches(sel, "NetSurf", "NetSurf")) return WIN_BROWSER;
    if (menu_label_matches(sel, "Calculator", "電卓")) return WIN_CALC;
    if (menu_label_matches(sel, "Storage", "ストレージ")) return WIN_STORAGE;
    if (menu_label_matches(sel, "Music Player", "ミュージック")) return WIN_MUSIC;
    if (menu_label_matches(sel, "Image Viewer", "画像ビューア")) return WIN_JPEG;
    if (menu_label_matches(sel, "Graph", "グラフ")) return WIN_CALC_GRAPH;
    if (menu_label_matches(sel, "Memory Manager", "メモリマネージャー")) return WIN_MEMORY_MGR;
    if (menu_label_matches(sel, "Python IDE", "Python IDE")) return WIN_PYTHON_IDE;
    if (menu_label_matches(sel, "HTTP Downloader", "HTTPダウンローダー")) return WIN_HTTP_DOWNLOADER;
    if (menu_label_matches(sel, "Spreadsheet", "表計算")) return WIN_SHEET;
    if (menu_label_matches(sel, "Task Manager", "タスクマネージャー")) return WIN_TASK_MGR;
    if (menu_label_matches(sel, "Clock", "時計")) return WIN_CLOCK;
    if (menu_label_matches(sel, "System Info", "システム情報")) return WIN_SYSINFO;
    if (menu_label_matches(sel, "Paint", "ペイント")) return WIN_PAINT;
    if (menu_label_matches(sel, "C-OS Benchmark", "C-OS Benchmark")) return WIN_VOXEL_GAME;
    if (menu_label_matches(sel, "About", "情報")) return WIN_ABOUT;
    return -1;
}

void open_desktop_context_menu(int x, int y, int target_icon) {
    gui_cancel_pointer_actions();
    ctx_menu.visible = TRUE;
    ctx_menu.x = x; ctx_menu.y = y;
    ctx_menu.target_window = -1;
    ctx_menu.target_icon = target_icon;
    ctx_menu.item_count = 0;
    ctx_menu.context_type = 0;
    submenu_close();
    if (target_icon >= 0) {
        ctx_add_item(gui_ctx_label("Open", "開く"), 11, TRUE, FALSE);
        ctx_add_item_ex(gui_ctx_label("Icon Size", "アイコンサイズ"), 11, TRUE, FALSE, SUBMENU_KIND_DESKTOP_ICON_SIZE);
        ctx_add_item(gui_ctx_label("Delete Icon", "アイコンを削除"), 14, TRUE, FALSE);
        ctx_add_item(gui_ctx_label("Save Layout", "保存"), 13, TRUE, FALSE);
        ctx_add_item(gui_ctx_label("Refresh Desktop", "表示の更新"), 11, TRUE, FALSE);
        ctx_add_item(gui_ctx_label("Show Desktop", "デスクトップを表示"), 11, TRUE, FALSE);
        ctx_add_item(gui_ctx_label("Reset Desktop", "アプリアイコンを左に整列"), 13, TRUE, FALSE);
    } else {
        ctx_add_item_ex(gui_ctx_label("New", "新規作成"), 1, TRUE, FALSE, SUBMENU_KIND_NEW);
        ctx_add_item_ex(gui_ctx_label("App Icon", "アプリアイコン"), 11, TRUE, FALSE, SUBMENU_KIND_APP_ICON);
        ctx_add_item_ex(gui_ctx_label("Icon Size", "アイコンサイズ"), 11, TRUE, FALSE, SUBMENU_KIND_DESKTOP_ICON_SIZE);
        ctx_add_item(gui_ctx_label("Save Layout", "保存"), 13, TRUE, FALSE);
        ctx_add_item(gui_ctx_label("Refresh Desktop", "表示の更新"),         11, TRUE,  FALSE);
        ctx_add_item(gui_ctx_label("Show Desktop", "デスクトップを表示"),   11, TRUE,  FALSE);
        ctx_add_item(gui_ctx_label("Reset Desktop", "アプリアイコンを左に整列"), 13, TRUE, FALSE);
        ctx_add_item_ex(gui_ctx_label("Open Apps", "アプリを開く"), 11, TRUE, FALSE, SUBMENU_KIND_OPEN);
        ctx_add_item_ex(gui_ctx_label("Audio", "音声"), 12, TRUE, FALSE, SUBMENU_KIND_AUDIO);
        ctx_add_item_ex(gui_ctx_label("Wallpaper", "壁紙"), 11, TRUE, FALSE, SUBMENU_KIND_WALLPAPER);
        ctx_add_item_ex(gui_ctx_label("Settings", "設定"), 11, TRUE, FALSE, SUBMENU_KIND_SETTINGS);
        ctx_add_item_ex(gui_ctx_label("Power", "電源"), 14, TRUE, FALSE, SUBMENU_KIND_POWER);
        ctx_add_item_ex(gui_ctx_label("Language", "言語切替"), 7, TRUE, FALSE, SUBMENU_KIND_LANGUAGE);
        ctx_add_item(gui_ctx_label("About C-OS", "バージョン情報"),        7, TRUE,  FALSE);
    }
}

void open_window_context_menu(int x, int y, int win_idx) {
    gui_cancel_pointer_actions();
    ctx_menu.visible = TRUE;
    ctx_menu.x = x;
    ctx_menu.y = y;
    ctx_menu.target_window = win_idx;
    ctx_menu.target_icon = -1;
    ctx_menu.item_count = 0;
    ctx_menu.context_type = 1;
    submenu_close();

    /* Old-style direct window menu: keep it simple and stable. */
    ctx_add_item(gui_ctx_label("Restore", "元に戻す"), 7, TRUE, FALSE);
    ctx_add_item(gui_ctx_label("Minimize", "最小化"), 8, TRUE, FALSE);
    ctx_add_item(gui_ctx_label("Maximize", "最大化"), 8, TRUE, FALSE);
    ctx_add_item(gui_ctx_label("Center Window", "中央に配置"), 13, TRUE, FALSE);
    ctx_add_item(gui_ctx_label("Move Left Half", "左半分へ"), 13, TRUE, FALSE);
    ctx_add_item(gui_ctx_label("Move Right Half", "右半分へ"), 14, TRUE, FALSE);
    ctx_add_item(gui_ctx_label("Show Desktop", "デスクトップを表示"), 11, TRUE, FALSE);
    ctx_add_item(gui_ctx_label("Close Window", "閉じる"), 11, TRUE, TRUE);
}
void handle_context_menu_click(int x, int y) {
    if (ctx_menu.context_type == 2) {
        handle_start_menu_click(x, y);
        return;
    }

    const int item_h = 32;
    const int sep_h = 10;
    const int header_h = 28;
    int mw = ctx_menu_calc_width();
    int total_h = ctx_menu_calc_height(item_h, sep_h);
    int mx = ctx_menu.x, my = ctx_menu.y;
    if (mx + mw > (int)SCREEN_W - 4) mx = (int)SCREEN_W - mw - 4;
    if (mx < 4) mx = 4;
    if (my + total_h > (int)SCREEN_H - 4) my = (int)SCREEN_H - total_h - 4;
    if (my < 4) my = 4;

    /* Submenu click first */
    if (submenu.visible && submenu.item_count > 0) {
        int sm_w = submenu_calc_width();
        int sm_item_h = 32, sm_h = submenu.item_count * sm_item_h + 12;
        int sm_x = submenu.x, sm_y = submenu.y;
        if (sm_x + sm_w > (int)SCREEN_W - 4) sm_x = (int)SCREEN_W - sm_w - 4;
        if (sm_y + sm_h > (int)SCREEN_H - 4) sm_y = (int)SCREEN_H - sm_h - 4;
        if (sm_x < 4) sm_x = 4;
        if (sm_y < 4) sm_y = 4;
        if (x >= sm_x && x < sm_x + sm_w && y >= sm_y + 24 && y < sm_y + sm_h) {
            int rel = (y - (sm_y + 24)) / sm_item_h;
            if (rel >= 0 && rel < submenu.item_count) {
                const char* sel = submenu.items[rel];
                if (submenu.kind == SUBMENU_KIND_LANGUAGE) {
                    gui_switch_language_with_loading(rel == 1 ? 1 : 0);
                    gui_notify(rel == 1 ? "言語を日本語に変更しました" : "Language switched to English", 1800);
                } else if (submenu.kind == SUBMENU_KIND_OPEN) {
                    if (strncmp(sel, "Open Terminal", 63) == 0 || strncmp(sel, "ターミナル", 63) == 0) {
                        gui_open_window(WIN_TERMINAL, gui_text("Terminal", "ターミナル"), 140, 120, 840, 560);
                    } else if (strncmp(sel, "Open File Manager", 63) == 0 || strncmp(sel, "ファイルマネージャー", 63) == 0) {
                        gui_open_window(WIN_FILE_MGR, gui_text("File Manager", "ファイルマネージャー"), 72, 56, 1120, 760);
                    } else if (strncmp(sel, "Open Task Manager", 63) == 0 || strncmp(sel, "タスクマネージャー", 63) == 0) {
                        gui_open_window(WIN_TASK_MGR, gui_text("Task Manager", "タスクマネージャー"), 120, 72, 1040, 700);
                    } else if (strncmp(sel, "Open NetSurf", 63) == 0 || strncmp(sel, "NetSurf", 63) == 0) {
                        gui_open_window(WIN_BROWSER, gui_text("NetSurf", "NetSurf"), 96, 64, 1160, 760);
                    } else if (strncmp(sel, "Open Calculator", 63) == 0 || strncmp(sel, "電卓", 63) == 0) {
                        gui_open_window(WIN_CALC, gui_text("Calculator", "電卓"), 160, 120, 760, 560);
                    } else if (strncmp(sel, "Open Spreadsheet", 63) == 0 || strncmp(sel, "表計算", 63) == 0) {
                        gui_open_window(WIN_SHEET, gui_text("Spreadsheet", "表計算"), 120, 72, 1120, 760);
                    } else if (strncmp(sel, "Open Clock", 63) == 0 || strncmp(sel, "時計", 63) == 0) {
                        gui_open_window(WIN_CLOCK, gui_text("Clock", "時計"), 220, 140, 540, 420);
                    } else if (strncmp(sel, "Open System Info", 63) == 0 || strncmp(sel, "システム情報", 63) == 0) {
                        gui_open_window(WIN_SYSINFO, gui_text("System Info", "システム情報"), 160, 90, 900, 620);
                    } else if (strncmp(sel, "Open Paint", 63) == 0 || strncmp(sel, "ペイント", 63) == 0) {
                        gui_open_window(WIN_PAINT, gui_text("Paint", "ペイント"), 120, 72, 1080, 720);
                    } else if (strncmp(sel, "Open Storage", 63) == 0 || strncmp(sel, "ストレージ", 63) == 0) {
                        gui_open_window(WIN_STORAGE, gui_text("Storage", "ストレージ"), 120, 80, 1060, 720);
                    } else if (strncmp(sel, "Open Music Player", 63) == 0 || strncmp(sel, "ミュージック", 63) == 0) {
                        gui_open_window(WIN_MUSIC, gui_text("Music Player", "ミュージックプレーヤー"), 120, 72, 1120, 720);
                    } else if (strncmp(sel, "Open Python IDE", 63) == 0 || strncmp(sel, "Python IDE", 63) == 0) {
                        gui_open_window(WIN_PYTHON_IDE, gui_text("Python IDE", "Python IDE"), 100, 50, 1000, 700);
                    } else if (strncmp(sel, "Open HTTP Downloader", 63) == 0 || strncmp(sel, "HTTPダウンローダー", 63) == 0) {
                        gui_open_window(WIN_HTTP_DOWNLOADER, gui_text("HTTP Downloader", "HTTPダウンローダー"), 120, 72, 1060, 760);
                    } else if (strncmp(sel, "Open C-OS Benchmark", 63) == 0 || strncmp(sel, "C-OS Benchmark", 63) == 0) {
                        gui_open_window(WIN_VOXEL_GAME, gui_text("C-OS Benchmark", "C-OS Benchmark"), 32, 24, 1260, 820);
                    }
                } else if (submenu.kind == SUBMENU_KIND_SETTINGS) {
                    if (strncmp(sel, "System Settings", 63) == 0 || strncmp(sel, "システム設定", 63) == 0) {
                        gui_open_window(WIN_SETTINGS, gui_text("Settings", "設定"), 140, 90, 1080, 760);
                    } else if (strncmp(sel, "Toggle Dark Mode", 63) == 0 || strncmp(sel, "ダークモード切替", 63) == 0) {
                        gui_toggle_dark_mode();
                    } else if (strncmp(sel, "Toggle Window Animations", 63) == 0 || strncmp(sel, "アニメーション切替", 63) == 0) {
                        gui_set_window_animations(!gui_get_window_animations());
                    } else if (strncmp(sel, "Toggle Notifications", 63) == 0 || strncmp(sel, "通知切替", 63) == 0) {
                        gui_set_notifications_enabled(!gui_get_notifications_enabled());
                    } else if (strncmp(sel, "Toggle Mouse Raw Input", 63) == 0 || strncmp(sel, "マウス生入力切替", 63) == 0) {
                        gui_set_mouse_raw_input(!gui_get_mouse_raw_input());
                    } else if (strncmp(sel, "About C-OS", 63) == 0 || strncmp(sel, "バージョン情報", 63) == 0) {
                        gui_launch_window_kind(WIN_ABOUT);
                    }
                } else if (submenu.kind == SUBMENU_KIND_AUDIO) {
                    if (strncmp(sel, "Open Music Player", 63) == 0 || strncmp(sel, "ミュージックプレーヤー", 63) == 0) {
                        gui_launch_window_kind(WIN_MUSIC);
                    } else if (strncmp(sel, "Test Audio", 63) == 0 || strncmp(sel, "音声テスト", 63) == 0) {
                        gui_play_test_tone();
                    } else if (strncmp(sel, "Reload Music Library", 63) == 0 || strncmp(sel, "音楽ライブラリ再読込", 63) == 0) {
                        music_player_open("/");
                        gui_notify(gui_text("Music library reloaded", "音楽ライブラリを再読込しました"), 1500);
                    }
                } else if (submenu.kind == SUBMENU_KIND_WALLPAPER) {
                    int jpeg_idx = gui_get_wallpaper_count() - 1;
                    if (jpeg_idx < 0) jpeg_idx = 0;
                    if (strncmp(sel, "Use JPEG Wallpaper", 63) == 0 || strncmp(sel, "JPEG壁紙を使う", 63) == 0) {
                        gui_set_wallpaper(jpeg_idx);
                        gui_notify(gui_text("Image wallpaper enabled", "画像壁紙を有効化しました"), 1500);
                    } else if (strncmp(sel, "Reload JPEG Wallpaper", 63) == 0 || strncmp(sel, "JPEG壁紙を再読込", 63) == 0) {
                        gui_set_wallpaper(jpeg_idx);
                        gui_notify(gui_text("Image wallpaper reloaded", "画像壁紙を再読込しました"), 1500);
                    } else if (strncmp(sel, "Use Gradient Wallpaper", 63) == 0 || strncmp(sel, "グラデーション壁紙", 63) == 0) {
                        gui_set_wallpaper(gui_get_wallpaper_count() - 1);
                        gui_notify(gui_text("Gradient wallpaper enabled", "グラデーション壁紙を有効化しました"), 1500);
                    }
                } else if (submenu.kind == SUBMENU_KIND_DESKTOP_ICON_SIZE) {
                    if (strncmp(sel, "Small", 63) == 0 || strncmp(sel, "小", 63) == 0) {
                        gui_set_desktop_icon_size(56);
                        gui_notify_simple(gui_text("Desktop icons set to Small", "デスクトップアイコンを小に変更しました"));
                    } else if (strncmp(sel, "Medium", 63) == 0 || strncmp(sel, "中", 63) == 0) {
                        gui_set_desktop_icon_size(64);
                        gui_notify_simple(gui_text("Desktop icons set to Medium", "デスクトップアイコンを中に変更しました"));
                    } else if (strncmp(sel, "Large", 63) == 0 || strncmp(sel, "大", 63) == 0) {
                        gui_set_desktop_icon_size(72);
                        gui_notify_simple(gui_text("Desktop icons set to Large", "デスクトップアイコンを大に変更しました"));
                    }
                } else if (submenu.kind == SUBMENU_KIND_POWER) {
                    if (strncmp(sel, "Show Desktop", 63) == 0 || strncmp(sel, "デスクトップを表示", 63) == 0) {
                        gui_show_desktop();
                    } else if (strncmp(sel, "Lock Screen", 63) == 0 || strncmp(sel, "画面ロック", 63) == 0) {
                        show_password_screen();
                    } else if (strncmp(sel, "Restart System", 63) == 0 || strncmp(sel, "再起動", 63) == 0) {
                        gui_notify(gui_text("System restarting...", "システムを再起動しています..."), 1);
                        bios_reboot();
                    } else if (strncmp(sel, "Power Off", 63) == 0 || strncmp(sel, "電源を切る", 63) == 0) {
                        gui_notify(gui_text("Powering off...", "電源を切っています..."), 1);
                        bios_shutdown();
                    }
                } else if (submenu.kind == SUBMENU_KIND_WINDOW) {
                    if (ctx_menu.target_window >= 0 && ctx_menu.target_window < window_count) {
                        if (strncmp(sel, "Restore", 63) == 0 || strncmp(sel, "元に戻す", 63) == 0) {
                            gui_restore_window(ctx_menu.target_window);
                        } else if (strncmp(sel, "Minimize", 63) == 0 || strncmp(sel, "最小化", 63) == 0) {
                            windows[ctx_menu.target_window].minimized = TRUE;
                            gui_request_redraw();
                        } else if (strncmp(sel, "Maximize", 63) == 0 || strncmp(sel, "最大化", 63) == 0) {
                            gui_maximize_window(ctx_menu.target_window);
                        } else if (strncmp(sel, "Center Window", 63) == 0 || strncmp(sel, "中央に配置", 63) == 0) {
                            window_t* w = &windows[ctx_menu.target_window];
                            gui_restore_window(ctx_menu.target_window);
                            w->x = ((int)SCREEN_W - w->w) / 2;
                            w->y = (((int)SCREEN_H - TASKBAR_H) - w->h) / 2;
                            if (w->x < 0) w->x = 0;
                            if (w->y < 0) w->y = 0;
                            gui_request_redraw();
                        } else if (strncmp(sel, "Move Left Half", 63) == 0 || strncmp(sel, "左半分へ", 63) == 0) {
                            gui_tile_window_half(ctx_menu.target_window, true);
                        } else if (strncmp(sel, "Move Right Half", 63) == 0 || strncmp(sel, "右半分へ", 63) == 0) {
                            gui_tile_window_half(ctx_menu.target_window, false);
                        } else if (strncmp(sel, "Show Desktop", 63) == 0 || strncmp(sel, "デスクトップを表示", 63) == 0) {
                            gui_show_desktop();
                        } else if (strncmp(sel, "Close Window", 63) == 0 || strncmp(sel, "閉じる", 63) == 0) {
                            gui_close_window(ctx_menu.target_window);
                        }
                    }
                } else if (submenu.kind == SUBMENU_KIND_NEW) {
                    if (strncmp(sel, "New File", 63) == 0 || strncmp(sel, "新規ファイル", 63) == 0) {
                        if (!gui_create_desktop_item(false)) gui_notify("Create file failed", 2000);
                    } else if (strncmp(sel, "New Folder", 63) == 0 || strncmp(sel, "新規フォルダ", 63) == 0) {
                        if (!gui_create_desktop_item(true)) gui_notify("Create folder failed", 2000);
                    }
                } else if (submenu.kind == SUBMENU_KIND_APP_ICON) {
                    int kind = desktop_app_kind_from_label(sel);
                    if (kind >= 0) {
                        if (!gui_create_desktop_app_icon(kind, x, y)) {
                            gui_notify("Create app icon failed", 2000);
                        } else {
                            gui_notify_simple(gui_text("App icon created", "アプリアイコンを作成しました"));
                        }
                    }
                }
            }
            ctx_menu.visible = FALSE;
            submenu_close();
            return;
        }
        submenu_close();
    }

    if (x < mx || x >= mx + mw || y < my || y >= my + total_h) { ctx_menu.visible = FALSE; return; }

    int cur_y = my + header_h;
    int clicked = -1;
    for (int i = 0; i < ctx_menu.item_count; i++) {
        if (ctx_menu.separator[i]) { cur_y += sep_h; continue; }
        if (y >= cur_y && y < cur_y + item_h) { clicked = i; break; }
        cur_y += item_h;
    }

    if (clicked >= 0 && ctx_menu.enabled[clicked]) {
        char* sel = ctx_menu.items[clicked];

        if (ctx_menu.target_window == -1) {
            if (ctx_menu.submenu[clicked]) {
                open_submenu_for_item(mx, mw, cur_y, ctx_menu.submenu_kind[clicked]);
                gui_request_redraw();
                return;
            }

            if (ctx_menu.target_icon >= 0) {
                int icon_idx = ctx_menu.target_icon;
                if (icon_idx >= 0 && icon_idx < desktop_icon_count) {
                    if (strncmp(sel, "Open", 63) == 0 || strncmp(sel, "開く", 63) == 0) {
                        int existing = gui_find_window(desktop_icons[icon_idx].win_kind);
                        if (existing >= 0) {
                            gui_bring_to_front(existing);
                        } else {
                            int open_w = 700, open_h = 500, open_x = 100 + icon_idx * 30, open_y = 80 + icon_idx * 20;
                            if (desktop_icons[icon_idx].win_kind == WIN_VOXEL_GAME) {
                                open_w = 1120;
                                open_h = (int)SCREEN_H - 40;
                                open_x = 80;
                                open_y = 60;
                            }
                            gui_open_window(desktop_icons[icon_idx].win_kind, desktop_icons[icon_idx].label, open_x, open_y, open_w, open_h);
                        }
                    } else if (strncmp(sel, "Delete Icon", 63) == 0 || strncmp(sel, "アイコンを削除", 63) == 0) {
                        if (!gui_delete_desktop_icon_at(icon_idx)) {
                            gui_notify("Delete icon failed", 1600);
                        } else {
                            gui_notify_simple(gui_text("Icon deleted", "アイコンを削除しました"));
                        }
                    } else if (strncmp(sel, "Save Layout", 63) == 0 || strncmp(sel, "保存", 63) == 0) {
                        gui_save_desktop_layout();
                        gui_notify_simple(gui_text("Layout saved", "配置を保存しました"));
                    } else if (strncmp(sel, "Reset Icon Position", 63) == 0 || strncmp(sel, "位置を戻す", 63) == 0 || strncmp(sel, "Arrange Icons", 63) == 0 || strncmp(sel, "アイコンを整列", 63) == 0 || strncmp(sel, "Reset Desktop", 63) == 0 || strncmp(sel, "アプリアイコンを左に整列", 63) == 0) {
                        gui_reset_desktop_icons();
                    } else if (strncmp(sel, "Refresh Desktop", 63) == 0 || strncmp(sel, "表示の更新", 63) == 0 || strncmp(sel, "デスクトップ更新", 63) == 0) {
                        gui_sync_desktop_with_fs();
                        gui_notify("Desktop refreshed", 1600);
                    } else if (strncmp(sel, "Show Desktop", 63) == 0 || strncmp(sel, "デスクトップを表示", 63) == 0) {
                        gui_show_desktop();
                    }
                }
            } else if (strncmp(sel, "Show Desktop", 63) == 0 || strncmp(sel, "デスクトップを表示", 63) == 0) {
                gui_show_desktop();
            } else if (strncmp(sel, "Save Layout", 63) == 0 || strncmp(sel, "保存", 63) == 0) {
                gui_save_desktop_layout();
                gui_notify_simple(gui_text("Layout saved", "配置を保存しました"));
            } else if (strncmp(sel, "Reset Desktop", 63) == 0 || strncmp(sel, "アプリアイコンを左に整列", 63) == 0) {
                gui_reset_desktop_icons();
                gui_notify_simple(gui_text("Desktop icons aligned left", "アプリアイコンを左に整列しました"));
            } else if (strncmp(sel, "New", 63) == 0 || strncmp(sel, "新規作成", 63) == 0) {
                /* submenu only */
            } else if (strncmp(sel, "Refresh Desktop", 63) == 0 || strncmp(sel, "表示の更新", 63) == 0 || strncmp(sel, "デスクトップ更新", 63) == 0) {
                gui_sync_desktop_with_fs();
                gui_notify("Desktop refreshed", 1600);
            } else if (strncmp(sel, "Open Apps", 63) == 0 || strncmp(sel, "アプリを開く", 63) == 0) {
                /* submenu only */
            } else if (strncmp(sel, "Audio", 63) == 0 || strncmp(sel, "音声", 63) == 0) {
                /* submenu only */
            } else if (strncmp(sel, "Wallpaper", 63) == 0 || strncmp(sel, "壁紙", 63) == 0) {
                /* submenu only */
            } else if (strncmp(sel, "Settings", 63) == 0 || strncmp(sel, "設定", 63) == 0) {
                /* submenu only */
            } else if (strncmp(sel, "Power", 63) == 0 || strncmp(sel, "電源", 63) == 0) {
                /* submenu only */
            } else if (strncmp(sel, "Language", 63) == 0 || strncmp(sel, "言語切替", 63) == 0) {
                /* submenu only */
            } else if (strncmp(sel, "About C-OS", 63) == 0 || strncmp(sel, "バージョン情報", 63) == 0) {
                gui_launch_window_kind(WIN_ABOUT);
            } else if (strncmp(sel, "Open Terminal", 63) == 0 || strncmp(sel, "ターミナル", 63) == 0) {
                gui_open_window(WIN_TERMINAL, gui_text("Terminal", "ターミナル"), 140, 120, 840, 560);
            } else if (strncmp(sel, "Open File Manager", 63) == 0 || strncmp(sel, "ファイルマネージャー", 63) == 0) {
                gui_open_window(WIN_FILE_MGR, gui_text("File Manager", "ファイルマネージャー"), 72, 56, 1120, 760);
            } else if (strncmp(sel, "Open Text Editor", 63) == 0 || strncmp(sel, "テキストエディター", 63) == 0) {
                gui_open_window(WIN_TEXT_EDITOR, gui_text("Text Editor", "テキストエディター"), 120, 72, 980, 680);
            } else if (strncmp(sel, "Open NetSurf", 63) == 0 || strncmp(sel, "NetSurf", 63) == 0) {
                gui_open_window(WIN_BROWSER, gui_text("NetSurf", "NetSurf"), 96, 64, 1160, 760);
            } else if (strncmp(sel, "Open Calculator", 63) == 0 || strncmp(sel, "電卓", 63) == 0) {
                gui_open_window(WIN_CALC, gui_text("Calculator", "電卓"), 160, 120, 760, 560);
            } else if (strncmp(sel, "Open Spreadsheet", 63) == 0 || strncmp(sel, "表計算", 63) == 0) {
                gui_open_window(WIN_SHEET, gui_text("Spreadsheet", "表計算"), 120, 72, 1120, 760);
            } else if (strncmp(sel, "Open Clock", 63) == 0 || strncmp(sel, "時計", 63) == 0) {
                gui_open_window(WIN_CLOCK, gui_text("Clock", "時計"), 220, 140, 540, 420);
            } else if (strncmp(sel, "Open System Info", 63) == 0 || strncmp(sel, "システム情報", 63) == 0) {
                gui_open_window(WIN_SYSINFO, gui_text("System Info", "システム情報"), 160, 90, 900, 620);
            } else if (strncmp(sel, "Open Paint", 63) == 0 || strncmp(sel, "ペイント", 63) == 0) {
                gui_open_window(WIN_PAINT, gui_text("Paint", "ペイント"), 120, 72, 1080, 720);
            } else if (strncmp(sel, "Open Storage", 63) == 0 || strncmp(sel, "ストレージ", 63) == 0) {
                gui_open_window(WIN_STORAGE, gui_text("Storage", "ストレージ"), 120, 80, 1060, 720);
            } else if (strncmp(sel, "Open Task Manager", 63) == 0 || strncmp(sel, "タスクマネージャー", 63) == 0) {
                gui_open_window(WIN_TASK_MGR, gui_text("Task Manager", "タスクマネージャー"), 120, 72, 1040, 700);
            } else if (strncmp(sel, "Open C-OS Benchmark", 63) == 0 || strncmp(sel, "C-OS Benchmarkを開く", 63) == 0 || strncmp(sel, "Open 3DGame's", 63) == 0 || strncmp(sel, "3DGame'sを開く", 63) == 0) {
                gui_open_window(WIN_VOXEL_GAME, gui_text("C-OS Benchmark", "C-OS Benchmark"), 32, 24, 1260, 820);
            } else if (strncmp(sel, "Open Music Player", 63) == 0 || strncmp(sel, "ミュージック", 63) == 0) {
                gui_open_window(WIN_MUSIC, gui_text("Music Player", "ミュージックプレーヤー"), 120, 72, 1120, 720);
            } else if (strncmp(sel, "Open Image Viewer", 63) == 0 || strncmp(sel, "画像ビューア", 63) == 0) {
                gui_open_window(WIN_JPEG, gui_text("Image Viewer", "画像ビューア"), 140, 88, 1080, 760);
            } else if (strncmp(sel, "Open Graph", 63) == 0 || strncmp(sel, "グラフ", 63) == 0) {
                gui_open_window(WIN_CALC_GRAPH, gui_text("Graph", "グラフ"), 180, 90, 760, 520);
            } else if (strncmp(sel, "Open Memory Manager", 63) == 0 || strncmp(sel, "メモリマネージャー", 63) == 0) {
                gui_open_window(WIN_MEMORY_MGR, gui_text("Memory Manager", "メモリマネージャー"), 120, 72, 1120, 760);
            } else if (strncmp(sel, "Open Python IDE", 63) == 0 || strncmp(sel, "Python IDE", 63) == 0) {
                gui_open_window(WIN_PYTHON_IDE, gui_text("Python IDE", "Python IDE"), 100, 50, 1000, 700);
            } else if (strncmp(sel, "Open HTTP Downloader", 63) == 0 || strncmp(sel, "HTTPダウンローダー", 63) == 0) {
                gui_open_window(WIN_HTTP_DOWNLOADER, gui_text("HTTP Downloader", "HTTPダウンローダー"), 120, 72, 1060, 760);
            } else if (strncmp(sel, "System Settings", 63) == 0 || strncmp(sel, "システム設定", 63) == 0) {
                gui_open_window(WIN_SETTINGS, gui_text("Settings", "設定"), 140, 90, 1080, 760);
            } else if (strncmp(sel, "Restart System", 63) == 0 || strncmp(sel, "再起動", 63) == 0) {
                gui_notify(gui_text("System restarting...", "システムを再起動しています..."), 1);
                bios_reboot();
            } else if (strncmp(sel, "Power Off", 63) == 0 || strncmp(sel, "電源を切る", 63) == 0) {
                gui_notify(gui_text("Powering off...", "電源を切っています..."), 1);
                bios_shutdown();
            } else if (strncmp(sel, "Lock Screen", 63) == 0 || strncmp(sel, "画面ロック", 63) == 0) {
                show_password_screen();
            }
        } else {
            if (strncmp(sel, "Close Window", 63) == 0 || strncmp(sel, "閉じる", 63) == 0) {
                gui_close_window(ctx_menu.target_window);
            } else if (strncmp(sel, "Minimize", 63) == 0 || strncmp(sel, "最小化", 63) == 0) {
                windows[ctx_menu.target_window].minimized = TRUE;
                gui_request_redraw();
            } else if (strncmp(sel, "Maximize", 63) == 0 || strncmp(sel, "最大化", 63) == 0) {
                gui_maximize_window(ctx_menu.target_window);
            } else if (strncmp(sel, "Move Left Half", 63) == 0 || strncmp(sel, "左半分へ", 63) == 0) {
                gui_tile_window_half(ctx_menu.target_window, true);
            } else if (strncmp(sel, "Move Right Half", 63) == 0 || strncmp(sel, "右半分へ", 63) == 0) {
                gui_tile_window_half(ctx_menu.target_window, false);
            } else if (strncmp(sel, "Show Desktop", 63) == 0 || strncmp(sel, "デスクトップを表示", 63) == 0) {
                gui_show_desktop();
            } else if (strncmp(sel, "Center Window", 63) == 0 || strncmp(sel, "中央に配置", 63) == 0) {
                window_t* w = &windows[ctx_menu.target_window];
                gui_restore_window(ctx_menu.target_window);
                w->x = ((int)SCREEN_W - w->w) / 2;
                w->y = (((int)SCREEN_H - TASKBAR_H) - w->h) / 2;
                if (w->x < 0) w->x = 0;
                if (w->y < 0) w->y = 0;
                gui_request_redraw();
            }
        }
    }
    ctx_menu.visible = FALSE;
    submenu_close();
}


