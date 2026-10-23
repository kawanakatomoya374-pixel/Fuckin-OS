/* startmenu.c-os - the Start menu, as a ring-3 program.
 *
 * Shell layer 2: a surface the compositor draws above the taskbar while it is shown (the taskbar's Start button asks the
 * kernel to show it; COS_EV_SHELL_SHOW / COS_EV_SHELL_HIDE tell this program). It keeps mouse and keyboard while shown.
 * Apps are opened with COS_WINCTL_APP and system actions with COS_WINCTL_SYSACT; the kernel hides the menu afterwards. App
 * icons are not drawn here: the program hands their positions to the compositor with cos_shell_icons(), which owns the art.
 * If this process is gone the kernel's own start menu is used instead.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "cos.h"
#include "cos_ui.h"

#define APP 1
typedef struct { int action, kind; const char *en, *ja; } entry_t;

static const entry_t pinned[] = {
    { APP, COS_APPK_FILE_MGR,    "File Manager",    "ファイルマネージャー" },
    { APP, COS_APPK_BROWSER,     "NetSurf",         "NetSurf" },
    { APP, COS_APPK_MESSENGER,   "C-OS Message",    "C-OS Message" },
    { APP, COS_APPK_TERMINAL,    "Terminal",        "ターミナル" },
    { APP, COS_APPK_TEXT_EDITOR, "Text Editor",     "テキストエディター" },
    { APP, COS_APPK_TASK_MGR,    "Task Manager",    "タスクマネージャー" },
    { APP, COS_APPK_SETTINGS,    "Settings",        "設定" },
    { APP, COS_APPK_CALC,        "Calculator",      "電卓" },
    { COS_SA_CALC_HISSAN, COS_APPK_CALC, "Calculator (Hissan)", "電卓（ひっ算）" },
    { APP, COS_APPK_PAINT,       "Paint",           "ペイント" },
    { APP, COS_APPK_STORAGE,     "Storage",         "ストレージ" },
    { APP, COS_APPK_MUSIC,       "Music Player",    "ミュージック" },
    { APP, COS_APPK_STUDIO,      "C-OS Studio",     "C-OS Studio" },
    { APP, COS_APPK_HTTP_DOWNLOADER, "HTTP Downloader", "HTTPダウンローダー" },
    { APP, COS_APPK_VOXEL_GAME,  "Launcher", "Launcher" },
};
static const entry_t tools[] = {
    { APP, COS_APPK_CLOCK,       "Clock",           "時計" },
    { APP, COS_APPK_SYSINFO,     "System Info",     "システム情報" },
    { APP, COS_APPK_SHEET,       "Spreadsheet",     "表計算" },
    { APP, COS_APPK_PAINT,       "Paint",           "ペイント" },
    { APP, COS_APPK_CALC,        "Calculator",      "電卓" },
    { APP, COS_APPK_TEXT_EDITOR, "Text Editor",     "テキストエディター" },
};
static const entry_t more[] = {
    { APP, COS_APPK_JPEG,        "Image Viewer",    "画像ビューア" },
    { APP, COS_APPK_CALC_GRAPH,  "Graph",           "グラフ" },
    { APP, COS_APPK_MEMORY_MGR,  "Memory Manager",  "メモリマネージャー" },
    { APP, COS_APPK_TINYGL,      "TinyGL 3D Viewer", "TinyGL 3Dビューアー" },
    { APP, COS_APPK_2DGAMES,     "2D Games",        "2Dゲーム" },
};
static const entry_t sys[] = {
    { COS_SA_SHOW_DESKTOP, 0,    "Show Desktop",    "デスクトップを表示" },
    { APP, COS_APPK_SETTINGS,    "System Settings", "システム設定" },
    { COS_SA_LOCK, 0,            "Lock Screen",     "画面ロック" },
    { COS_SA_RESTART, 0,         "Restart",         "再起動" },
    { COS_SA_POWEROFF, 0,        "Power Off",       "電源を切る" },
    { COS_SA_LANG_EN, 0,         "Language: English", "言語: English" },
    { COS_SA_LANG_JA, 0,         "Language: 日本語", "言語: 日本語" },
    { COS_SA_DARK_MODE, 0,       "Toggle Dark Mode", "ダークモード切替" },
    { COS_SA_ANIMATIONS, 0,      "Toggle Animations", "アニメーション切替" },
    { COS_SA_NOTIFICATIONS, 0,   "Toggle Notifications", "通知切替" },
    { COS_SA_MOUSE_RAW, 0,       "Toggle Mouse Raw", "マウス生入力切替" },
    { COS_SA_REFRESH_DESKTOP, 0, "Refresh Desktop", "デスクトップ更新" },
    { COS_SA_TEST_AUDIO, 0,      "Test Audio",      "音声テスト" },
    { COS_SA_IMAGE_WALLPAPER, 0, "Image Wallpaper", "画像壁紙" },
    { COS_SA_ABOUT, 0,           "About C-OS",      "バージョン情報" },
};
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const uint32_t C_BG = 0xF8FBFF, C_HEAD = 0xEAF1FA, C_LINE = 0xCADAEF, C_BORDER = 0xB7C7E0, C_TILE = 0xF6F9FE, C_TILE_H = 0xE1ECFB,
                      C_TILE_B = 0xBECCE2, C_TILE_BH = 0x5C8AD8, C_TXT = 0x2A3A54, C_TXT_H = 0x1A325C, C_MUTED = 0x5C6C82, C_RAIL = 0xF0F5FC;

static cui_canvas c;
static int64_t win;
static int tab, selected = 0, hover = -1, pressed = -1;
static char search[64];
static const entry_t *results[48];
static int nresults;
static bool ja;
static cui_font *f_s, *f_b, *f_l;
static void *jp_ttf;

typedef struct { int x, y, w, h; int entry; } rect_t;
static rect_t tiles[48];
static int ntiles;
static rect_t tabs[4];
static int cols = 4;

static const entry_t *view_entry(int i)
{
    if (search[0]) return i < nresults ? results[i] : NULL;
    switch (tab) {
    case 1: return i < N(sys) ? &sys[i] : NULL;
    case 2: return i < N(tools) ? &tools[i] : NULL;
    case 3: return i < N(more) ? &more[i] : NULL;
    default: return i < N(pinned) ? &pinned[i] : NULL;
    }
}
static int view_count(void)
{
    if (search[0]) return nresults;
    return tab == 1 ? N(sys) : tab == 2 ? N(tools) : tab == 3 ? N(more) : N(pinned);
}
static const char *label(const entry_t *e) { return ja ? e->ja : e->en; }

static bool contains_ci(const char *h, const char *n)
{
    for (; *h; h++) {
        int j = 0;
        while (n[j] && h[j] && ((h[j] | 0x20) == (n[j] | 0x20))) j++;
        if (!n[j]) return true;
    }
    return false;
}

static void collect(void)
{
    nresults = 0;
    static const struct { const entry_t *a; int n; } lists[] = { { pinned, N(pinned) }, { tools, N(tools) }, { more, N(more) }, { sys, N(sys) } };
    for (int l = 0; l < 4; l++)
        for (int i = 0; i < lists[l].n; i++) {
            const entry_t *e = &lists[l].a[i];
            if (!contains_ci(e->en, search) && !contains_ci(e->ja, search)) continue;
            bool dup = false;
            for (int j = 0; j < nresults; j++) if (results[j]->action == e->action && results[j]->kind == e->kind && !strcmp(results[j]->en, e->en)) dup = true;
            if (!dup && nresults < 48) results[nresults++] = e;
        }
}

static void layout(void)
{
    const int header_h = 58, rail_w = 132, footer_h = 34, gap = 8;
    int cx = rail_w + 10, cy = header_h + 30, cw = c.w - rail_w - 22;
    ntiles = 0;
    int n = view_count();
    bool listy = search[0] || tab == 1;
    cols = listy ? 2 : 4;
    int tw = (cw - (cols - 1) * gap) / cols;
    int th = search[0] ? 40 : tab == 1 ? 30 : 62;
    for (int i = 0; i < n && ntiles < 48; i++) {
        int r = i / cols, k = i % cols;
        int y = cy + r * (th + (th > 40 ? gap : 4));
        if (y + th > c.h - footer_h - 4) break;
        tiles[ntiles++] = (rect_t){ cx + k * (tw + gap), y, tw, th, i };
    }
    for (int i = 0; i < 4; i++) tabs[i] = (rect_t){ 20, header_h + 24 + i * 54, rail_w - 32, 38, i };
}

static bool in(const rect_t *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }

static void present(void)
{
    layout();
    cui_fill(&c, 0, 0, c.w, c.h, C_BG);
    cui_fill(&c, 0, 0, c.w, 58, C_HEAD);
    cui_fill(&c, 0, 57, c.w, 1, C_LINE);
    cui_round_rect_outline(&c, 0, 0, c.w, c.h, 2, 1.5f, C_BORDER);
    cui_circle(&c, 24, 26, 12, 0x5A8CDC);
    cui_circle(&c, 24, 26, 8, 0xEBF2FC);
    cui_text(&c, f_b, 44, 11, ja ? "スタート" : "Start", C_TXT);
    cui_text(&c, f_s, 44, 31, ja ? "アプリやシステム操作を選択" : "Choose an app or system action", C_MUTED);
    int sx = c.w - 238, sw = 220;
    cui_round_rect(&c, sx, 14, sw, 28, 8, 0xFFFFFF);
    cui_round_rect_outline(&c, sx, 14, sw, 28, 8, 1.0f, 0xBDCBE0);
    cui_text_fit(&c, f_s, sx + 10, 21, sw - 20, search[0] ? search : (ja ? "アプリ検索..." : "Search apps..."), search[0] ? C_TXT : 0x7A8AA0);

    cui_round_rect(&c, 12, 70, 116, c.h - 70 - 46, 12, C_RAIL);
    static const char *en[4] = { "Pinned", "System", "Tools", "More Apps" }, *jl[4] = { "ピン留め", "システム", "ツール", "その他アプリ" };
    for (int i = 0; i < 4; i++) {
        bool sel = (tab == i && !search[0]), hov = (hover == 1000 + i);
        cui_round_rect(&c, tabs[i].x, tabs[i].y, tabs[i].w, tabs[i].h, 9, sel ? 0xDEEAFC : hov ? 0xE8F0FA : 0xF7FAFD);
        cui_round_rect_outline(&c, tabs[i].x, tabs[i].y, tabs[i].w, tabs[i].h, 9, 1.0f, sel ? C_TILE_BH : 0xCBD6E8);
        cui_text_fit(&c, f_s, tabs[i].x + 10, tabs[i].y + 11, tabs[i].w - 16, ja ? jl[i] : en[i], sel ? C_TXT_H : C_TXT);
    }

    const char *section = search[0] ? (ja ? "検索結果" : "Search results") : tab == 1 ? (ja ? "システム操作" : "System actions")
                        : tab == 2 ? (ja ? "ツール" : "Tools") : tab == 3 ? (ja ? "その他アプリ" : "More apps") : (ja ? "ピン留めアプリ" : "Pinned apps");
    cui_text(&c, f_b, 142, 66, section, C_TXT_H);
    if (search[0]) { char b[32]; snprintf(b, sizeof b, "%d", nresults); cui_text(&c, f_s, 142 + cui_text_width(f_b, section) + 10, 68, b, C_MUTED); }
    if (search[0] && !nresults) cui_text(&c, f_s, 142, 110, ja ? "一致する項目がありません" : "No matches", C_MUTED);

    cos_shell_icon_t ic[COS_SHELL_MAX_ICONS];
    int ni = 0;
    for (int i = 0; i < ntiles; i++) {
        const entry_t *e = view_entry(tiles[i].entry);
        if (!e) continue;
        bool hot = (hover == i) || (selected == tiles[i].entry);
        rect_t *t = &tiles[i];
        cui_round_rect(&c, t->x, t->y, t->w, t->h, 10, hot ? C_TILE_H : C_TILE);
        cui_round_rect_outline(&c, t->x, t->y, t->w, t->h, 10, 1.0f, selected == tiles[i].entry ? C_TILE_BH : hot ? 0x7EA1DE : C_TILE_B);
        bool has_icon = e->action == APP || e->action == COS_SA_CALC_HISSAN;
        if (t->h > 50) {            /* grid tile: icon above, label below */
            cui_text_fit(&c, f_s, t->x + 6 + (t->w - 12 - (cui_text_width(f_s, label(e)) < t->w - 12 ? cui_text_width(f_s, label(e)) : t->w - 12)) / 2,
                         t->y + 40, t->w - 12, label(e), hot ? C_TXT_H : C_TXT);
            if (has_icon && ni < COS_SHELL_MAX_ICONS) ic[ni++] = (cos_shell_icon_t){ e->kind, t->x + (t->w - 32) / 2, t->y + 6, 32 };
        } else {
            int tx = t->x + 10;
            if (has_icon && t->h >= 36 && ni < COS_SHELL_MAX_ICONS) { ic[ni++] = (cos_shell_icon_t){ e->kind, t->x + 6, t->y + (t->h - 28) / 2, 28 }; tx = t->x + 42; }
            cui_text_fit(&c, f_s, tx, t->y + (t->h - cui_font_height(f_s)) / 2, t->w - (tx - t->x) - 8, label(e), hot ? C_TXT_H : C_TXT);
        }
    }
    cui_round_rect(&c, 12, c.h - 38, c.w - 24, 28, 9, C_RAIL);
    cui_text(&c, f_s, 22, c.h - 32, ja ? "ヒント: 左のタブをクリック、または文字を入力して検索" : "Tip: click the tabs on the left, or just type to search", C_MUTED);
    cos_shell_icons(2, ic, ni, 0);
    cos_win2_present(win);
}

static void run(const entry_t *e)
{
    if (!e) return;
    if (e->action == APP) cos_win_ctl(COS_WINCTL_APP, 0, e->kind);
    else cos_win_ctl(COS_WINCTL_SYSACT, 0, e->action);
}

static void load_fonts(void)
{
    ja = (cos_ui_prefs() & COS_UI_JAPANESE) != 0;
    if (ja && !jp_ttf) {
        cos_stat_t st;
        if (cos_stat("/system/fonts/NotoSansJP-Regular.ttf", &st) == 0 && st.size > 0 && st.size < (8u << 20)) {
            jp_ttf = cos_malloc(st.size);
            if (jp_ttf && cos_read_file("/system/fonts/NotoSansJP-Regular.ttf", jp_ttf, st.size) != (ssize_t)st.size) jp_ttf = NULL;
        }
        if (jp_ttf) {
            cui_font *fs = cui_font_default(13), *fb = cui_font_bold(14);
            cui_font_set_fallback(fs, cui_font_load(jp_ttf, 13));
            cui_font_set_fallback(fb, cui_font_load(jp_ttf, 14));
        }
    }
    f_s = cui_font_default(13); f_b = cui_font_bold(14); f_l = cui_font_bold(16);
}

static void reset_state(void)
{
    load_fonts();
    tab = 0; selected = 0; hover = -1; pressed = -1; search[0] = 0; nresults = 0;
}

static void move(int d)
{
    int n = view_count();
    if (n <= 0) return;
    int s = selected + d;
    selected = s < 0 ? 0 : s >= n ? n - 1 : s;
}

int main(void)
{
    cos_win_info_t wi;
    win = cos_shell_create(2, &wi);
    if (win <= 0) return 1;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    reset_state();
    present();                                   /* the first frame makes the layer eligible; it stays hidden until SHOW */

    for (;;) {
        cos_win_event_t ev;
        if (cos_win2_wait(win, &ev, 1000) <= 0) continue;
        bool dirty = false;
        do {
            switch (ev.type) {
            case COS_EV_SHELL_SHOW: reset_state(); dirty = true; break;
            case COS_EV_SHELL_HIDE: break;
            case COS_EV_MOUSE_MOVE: {
                layout();
                int h = -1;
                for (int i = 0; i < ntiles; i++) if (in(&tiles[i], ev.x, ev.y)) h = i;
                for (int i = 0; i < 4; i++) if (in(&tabs[i], ev.x, ev.y)) h = 1000 + i;
                if (h != hover) { hover = h; if (h >= 0 && h < 1000) selected = tiles[h].entry; dirty = true; }
                break;
            }
            case COS_EV_MOUSE_DOWN: pressed = hover; break;
            case COS_EV_MOUSE_UP:
                if (ev.button == 1 && pressed >= 0 && pressed == hover) {
                    if (pressed >= 1000) { tab = pressed - 1000; search[0] = 0; selected = 0; dirty = true; }
                    else run(view_entry(tiles[pressed].entry));
                }
                pressed = -1;
                break;
            case COS_EV_KEY:
                if (ev.special == COS_KEY_ESC) { if (search[0]) { search[0] = 0; selected = 0; } else cos_win_ctl(COS_WINCTL_MENU_HIDE, 0, 0); }
                else if (ev.special == COS_KEY_TAB) { tab = (tab + ((ev.mods & COS_MOD_SHIFT) ? 3 : 1)) % 4; search[0] = 0; selected = 0; }
                else if (ev.special == COS_KEY_ENTER) run(view_entry(selected));
                else if (ev.special == COS_KEY_LEFT) move(-1);
                else if (ev.special == COS_KEY_RIGHT) move(1);
                else if (ev.special == COS_KEY_UP) move(-cols);
                else if (ev.special == COS_KEY_DOWN) move(cols);
                else if (ev.special == COS_KEY_HOME) selected = 0;
                else if (ev.special == COS_KEY_END) selected = view_count() ? view_count() - 1 : 0;
                else if (ev.special == COS_KEY_BACKSPACE) { int l = (int)strlen(search); if (l) search[l - 1] = 0; collect(); selected = 0; }
                else if (ev.ascii >= 32 && ev.ascii < 127) { int l = (int)strlen(search); if (l < 40) { search[l] = ev.ascii; search[l + 1] = 0; } collect(); selected = 0; }
                dirty = true;
                break;
            }
        } while (cos_win2_poll(win, &ev) > 0);
        if (dirty) present();
    }
}
