/**
 * gui_apps_common.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * 複数アプリから共有される汎用ユーティリティ関数
 *
 * 元は単一の gui_apps.c (11,638行) に含まれていたコードを、
 * 保守性向上のため機能単位に分割したものの一部。
 */
#include "gui.h"
#include "mk_desktop.h"
#include "system/password_screen.h"
#include "vga.h"
#include "mk_mp3.h"
#include "../../../apps/jpeg_viewer.h"
#include "string.h"
#include "serial.h"

#ifndef KEY_PAGEUP
#define KEY_PAGEUP   0x49
#endif
#ifndef KEY_PAGEDOWN
#define KEY_PAGEDOWN 0x51
#endif
#ifndef KEY_HOME
#define KEY_HOME     0x47
#endif
#ifndef KEY_END
#define KEY_END      0x4F
#endif
#include "memory.h"
#include "memory_physical.h"
#include "cos_version.h"
#include "rtc.h"
#include "scheduler.h"
#include "../../bios/bios.h"
#include "../../kernel/drivers/usb.h"
#include "../../kernel/drivers/pci.h"
#include "fs.h"
#include "keyboard.h"
#include "../../drivers/disk/storage.h"
#include "../../drivers/input/mouse_minimal.h"
#include <shell.h>
extern const char* fs_read_file_at(const char* path, const char* name);
extern const char* config_get_string(const char* key);
extern void gui_snapshot_save_desktop(void);
extern bool settings_set_desktop_icon_size(uint32_t size) __attribute__((weak));
extern void gui_normalize_desktop_icons(void);
#include "gui_apps_common.h"

const char* gui_get_prompt_hostname(void) {
    const char* hostname = config_get_string("system.hostname");
    return (hostname && hostname[0]) ? hostname : "cos";
}

void copy_cstr_local(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (i + 1 < dst_size && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

int slen(const char* s) { 
    int n=0; 
    while(s[n]) n++; 
    return n; 
}

void scopy(char* d, const char* s, int m) {
    int i=0; 
    while(i<m-1&&s[i]){d[i]=s[i];i++;} 
    d[i]=0;
}

void scat(char* d, const char* s, int m) {
    int dl=slen(d); 
    int i=0;
    while(dl+i<m-1&&s[i]){d[dl+i]=s[i];i++;} 
    d[dl+i]=0;
}

void fm_path_leaf(const char* path, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;
    const char* leaf = path;
    for (const char* p = path; *p; ++p) if (*p == '/') leaf = p + 1;
    copy_cstr_local(out, out_size, leaf);
}

bool fm_path_join(char* out, size_t out_size, const char* dir, const char* name) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!name || !name[0]) return false;
    if (!dir || path_is_root(dir)) {
        if (name[0] == '/') copy_cstr_local(out, out_size, name);
        else {
            copy_cstr_local(out, out_size, "/");
            scat(out, name, (int)out_size);
        }
        return true;
    }
    copy_cstr_local(out, out_size, dir);
    size_t len = strlen(out);
    if (len > 0 && out[len - 1] != '/' && len + 1 < out_size) { out[len++] = '/'; out[len] = '\0'; }
    scat(out, name, (int)out_size);
    return true;
}

/* fm_full_path_from_entry() は fm_effective_path()（core専用のstatic関数）に
 * 依存するため gui_apps_core.c 側に定義を置く。 */

void draw_menubar(int x, int y, int w) {
    vga_fill_rect(x, y, w, 24, C_TOOLBAR);
    vga_draw_rect(x, y, w, 24, C_BORDER);
    vga_draw_string(x+8, y+4, "File", C_TEXT, 0xFFFFFFFF);
    vga_draw_string(x+50, y+4, "Edit", C_TEXT, 0xFFFFFFFF);
    vga_draw_string(x+92, y+4, "View", C_TEXT, 0xFFFFFFFF);
    vga_draw_string(x+134, y+4, "Help", C_TEXT, 0xFFFFFFFF);
}

void uitostr(uint64_t num, char* buf) {
    if (num == 0) { buf[0] = '0'; buf[1] = 0; return; }
    int i = 0, temp = num;
    while (temp > 0) { i++; temp /= 10; }
    buf[i] = 0;
    while (num > 0) { buf[--i] = '0' + (num % 10); num /= 10; }
}

bool smatch(const char* s1, const char* s2) {
    while (*s1 && *s2 && *s1 == *s2) { s1++; s2++; }
    return *s1 == *s2;
}

void draw_tbtn(int x, int y, int w, int h, const char* label, uint64_t col, bool hov) {
    if (hov) vga_fill_rect(x, y, w, h, rgb(0,100,180));
    else vga_fill_rect(x, y, w, h, col);
    vga_draw_rect(x, y, w, h, C_BORDER);
    vga_draw_string(x+4, y+4, label, C_TEXT_LIGHT, 0xFFFFFFFF);
}

void draw_scrollbar_v(int x, int y, int h, int total, int visible, int offset) {
    vga_fill_rect(x, y, C_SCROLLBAR_W, h, C_TOOLBAR);
    vga_draw_rect(x, y, C_SCROLLBAR_W, h, C_BORDER);
    if (total <= 0) return;
    int thumb_h = (visible * h) / total;
    if (thumb_h < 8) thumb_h = 8;
    int thumb_y = y + (offset * h) / total;
    vga_fill_rect(x+2, thumb_y, C_SCROLLBAR_W-4, thumb_h, C_ACCENT);
}

bool has_suffix(const char* str, const char* suffix) {
    int sl = slen(str);
    int pl = slen(suffix);
    if (pl > sl) return FALSE;

    /* FatFs without LFN commonly reports 8.3 names in uppercase.  Match
     * extensions without case sensitivity so ICON.BMP and icon.bmp launch
     * the same application. */
    const char* tail = str + sl - pl;
    for (int i = 0; i < pl; ++i) {
        char a = tail[i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return FALSE;
    }
    return TRUE;
}

void format_size_bytes(uint64_t bytes, char* out, int max) {
    if (bytes >= 1024 * 1024) {
        uint64_t whole = bytes / (1024 * 1024);
        uint64_t frac = (bytes % (1024 * 1024)) * 10 / (1024 * 1024);
        char n1[16], n2[8];
        uitostr(whole, n1);
        uitostr(frac, n2);
        scopy(out, n1, max);
        scat(out, ".", max);
        scat(out, n2, max);
        scat(out, " MB", max);
    } else if (bytes >= 1024) {
        char n1[16];
        uitostr(bytes / 1024, n1);
        scopy(out, n1, max);
        scat(out, " KB", max);
    } else {
        char n1[16];
        uitostr(bytes, n1);
        scopy(out, n1, max);
        scat(out, " B", max);
    }
}

bool sstartswith(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str != *prefix) return FALSE;
        str++; prefix++;
    }
    return TRUE;
}

void draw_statusbar(int x, int y, int w, const char* status, const char* path) {
    const char* left = (status && status[0]) ? status : "Ready";
    const char* right = (path && path[0]) ? path : "/";
    vga_fill_rect(x, y, w, C_STATUSBAR_H, rgb(238, 243, 251));
    vga_fill_rect(x, y, w, 1, rgb(206, 218, 236));
    vga_fill_rect(x, y + C_STATUSBAR_H - 1, w, 1, rgb(220, 228, 240));
    vga_fill_rounded_rect(x + 8, y + 3, w / 2 - 18, C_STATUSBAR_H - 6, 6, rgb(249, 251, 255));
    vga_fill_rounded_rect(x + w / 2, y + 3, w / 2 - 10, C_STATUSBAR_H - 6, 6, rgb(245, 248, 254));
    vga_draw_rounded_rect(x + 8, y + 3, w / 2 - 18, C_STATUSBAR_H - 6, 6, rgb(210, 221, 238));
    vga_draw_rounded_rect(x + w / 2, y + 3, w / 2 - 10, C_STATUSBAR_H - 6, 6, rgb(210, 221, 238));
    vga_draw_string(x + 14, y + 4, left, rgb(46, 60, 90), 0xFFFFFFFF);
    vga_draw_string(x + w / 2 + 8, y + 4, right, rgb(96, 110, 138), 0xFFFFFFFF);
}

void fm_join_path(char* out, size_t out_size, const char* dir, const char* name) {
    if (!out || out_size == 0) return;
    out[0] = 0;
    if (!name || !name[0]) return;
    if (!dir || path_is_root(dir)) {
        if (name[0] == '/') {
            scopy(out, name, (int)out_size - 1);
        } else {
            scopy(out, "/", (int)out_size - 1);
            scat(out, name, (int)out_size - 1);
        }
        return;
    }
    scopy(out, dir, (int)out_size - 1);
    if (out[strlen(out) - 1] != '/') scat(out, "/", (int)out_size - 1);
    scat(out, name, (int)out_size - 1);
}

bool fm_is_music_file(const char* name) {
    return has_suffix(name, ".mp3") || has_suffix(name, ".wav") || has_suffix(name, ".ogg");
}

bool fm_is_jpeg_file(const char* name) {
    return has_suffix(name, ".jpg") || has_suffix(name, ".jpeg") || has_suffix(name, ".jpe");
}

bool fm_is_image_file(const char* name) {
    return fm_is_jpeg_file(name) || has_suffix(name, ".png") || has_suffix(name, ".bmp") || has_suffix(name, ".gif");
}

bool fm_is_html_file(const char* name) {
    return has_suffix(name, ".html") || has_suffix(name, ".htm") ||
           has_suffix(name, ".xhtml") || has_suffix(name, ".shtml");
}

void fm_open_image_viewer(const char* filename) {
    int ex = gui_find_window(WIN_JPEG);
    if (ex < 0) {
        gui_open_window(WIN_JPEG, gui_text("Image Viewer", "画像ビューア"), 140, 88, 1080, 760);
        ex = gui_find_window(WIN_JPEG);
    }
    if (ex < 0) return;

    if (!image_viewer_load_file || image_viewer_load_file(filename) != 0) {
        gui_notify(gui_text("Image load failed", "画像の読み込みに失敗しました"), 2000);
        return;
    }

    window_t* view = &windows[ex];
    scopy(view->filename, filename, 127);
    view->text_modified = FALSE;
    gui_restore_window(ex);
    gui_focus_window(ex);
}

void fm_open_text_editor(const char* filename) {
    int ex = gui_find_window(WIN_TEXT_EDITOR);
    if (ex < 0) {
        gui_open_window(WIN_TEXT_EDITOR, gui_text("Text Editor", "テキストエディター"), 140, 90, 900, 650);
        ex = gui_find_window(WIN_TEXT_EDITOR);
    }
    if (ex < 0) return;

    window_t* ed = &windows[ex];
    fs_entry_t* entry = fs_find(filename);
    const char* text = fs_read_file(filename);
    scopy(ed->filename, filename, 127);
    if (entry && fs_is_text_file(entry) && text) {
        scopy(ed->text_buf, text, TEXT_BUF_SIZE - 1);
    } else if (text && fs_looks_like_text(text, (uint64_t)strlen(text))) {
        scopy(ed->text_buf, text, TEXT_BUF_SIZE - 1);
    } else {
        scopy(ed->text_buf, "[Binary file preview unavailable]\n", TEXT_BUF_SIZE - 1);
    }
    ed->text_cursor = slen(ed->text_buf);
    ed->scroll_y = 0;
    ed->scroll_x = 0;
    ed->text_modified = FALSE;
    gui_restore_window(ex);
    gui_focus_window(ex);
}

void make_copy_name(const char* leaf, int n, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!leaf || !leaf[0]) return;

    const char* dot = NULL;
    for (const char* p = leaf; *p; ++p) {
        if (*p == '.') dot = p;
    }

    char base[FS_MAX_NAME];
    char ext[FS_MAX_NAME];
    base[0] = '\0';
    ext[0] = '\0';

    if (dot && dot != leaf) {
        size_t base_len = (size_t)(dot - leaf);
        if (base_len >= sizeof(base)) base_len = sizeof(base) - 1;
        memcpy(base, leaf, base_len);
        base[base_len] = '\0';
        copy_cstr_local(ext, sizeof(ext), dot);
    } else {
        copy_cstr_local(base, sizeof(base), leaf);
    }

    if (n <= 0) n = 1;
    char num[16] = {0};
    int idxn = 0;
    int value = n;
    char tmp[16];
    int ti = 0;
    while (value > 0 && ti < 15) {
        tmp[ti++] = (char)('0' + (value % 10));
        value /= 10;
    }
    if (ti == 0) tmp[ti++] = '0';
    while (ti > 0) num[idxn++] = tmp[--ti];
    num[idxn] = '\0';

    size_t pos2 = 0;
    for (size_t i = 0; base[i] && pos2 + 1 < out_size; ++i) out[pos2++] = base[i];
    const char* suffix = "_copy";
    for (size_t i = 0; suffix[i] && pos2 + 1 < out_size; ++i) out[pos2++] = suffix[i];
    for (size_t i = 0; num[i] && pos2 + 1 < out_size; ++i) out[pos2++] = num[i];
    for (size_t i = 0; ext[i] && pos2 + 1 < out_size; ++i) out[pos2++] = ext[i];
    out[pos2] = '\0';
}

void gui_open_file_in_app(const char* path, int file_type) {
    (void)file_type;
    if (!path || !path[0]) return;
    if (fm_is_music_file(path)) {
        extern void music_player_open(const char* path) __attribute__((weak));
        if (music_player_open) {
            music_player_open(path);
        }
    } else if (fm_is_image_file(path)) {
        fm_open_image_viewer(path);
    } else if (fm_is_html_file(path)) {
        int ex = gui_find_window(WIN_BROWSER);
        if (ex < 0) {
            gui_open_window(WIN_BROWSER, gui_text("NetSurf 3.11", "NetSurf 3.11"), 180, 160, 1040, 700);
            ex = gui_find_window(WIN_BROWSER);
        }
        if (ex >= 0) {
            browser_commit_navigation(&windows[ex], path, true);
            gui_restore_window(ex);
            gui_focus_window(ex);
        }
    } else {
        fm_open_text_editor(path);
    }
}


/* 元々 gui_apps.c 内でブラウザ部分にstaticで定義されていたが、
 * タスクマネージャからも参照されるため共通ファイルへ移動・非static化。 */
const char* window_kind_name(int kind) {
    switch (kind) {
        case WIN_FILE_MGR: return gui_text("File Manager", "ファイルマネージャー");
        case WIN_TEXT_EDITOR: return gui_text("Text Editor", "テキストエディター");
        case WIN_TERMINAL: return gui_text("Terminal", "ターミナル");
        case WIN_SETTINGS: return gui_text("Settings", "設定");
        case WIN_ABOUT: return gui_text("About", "情報");
        case WIN_CALC: return gui_text("Calculator", "電卓");
        case WIN_STORAGE: return gui_text("Storage", "ストレージ");
        case WIN_BROWSER: return gui_text("NetSurf", "NetSurf");
        case WIN_TASK_MGR: return gui_text("Task Manager", "タスクマネージャー");
        case WIN_PAINT: return gui_text("Paint", "ペイント");
        case WIN_MUSIC: return gui_text("MP3 Player", "MP3プレーヤー");
        case WIN_JPEG: return gui_text("Image Viewer", "画像ビューア");
        case WIN_CLOCK: return gui_text("Clock", "時計");
        case WIN_SYSINFO: return gui_text("System Info", "システム情報");
        case WIN_PYTHON_IDE: return gui_text("Python IDE", "Python IDE");
        case WIN_HTTP_DOWNLOADER: return gui_text("HTTP Downloader", "HTTPダウンローダー");
        case WIN_SHEET: return gui_text("Spreadsheet", "表計算");
        default: return gui_text("Window", "ウィンドウ");
    }
}
