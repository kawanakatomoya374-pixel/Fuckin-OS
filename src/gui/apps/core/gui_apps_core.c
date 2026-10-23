/**
 * gui_apps_core.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * デスクトップアイコン・クリップボード・ファイルマネージャ(コア)
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
#include "timer.h"

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

void gui_snap_desktop_icon_position(int* x, int* y, int skip_index);
uint32_t gui_get_desktop_icon_size(void);
static const char* desktop_icon_label_for_kind(int kind);
static int desktop_icon_box_size(void);
int gui_get_desktop_icon_render_size(void);
static void desktop_icon_grid_position_for_index(int idx, int* out_x, int* out_y);
static void desktop_icon_grid_metrics(int* origin_x, int* origin_y, int* cell_w, int* cell_h, int* cols, int* rows);
static bool desktop_icon_slot_is_occupied(int slot_x, int slot_y, int skip_index);
static void desktop_icon_find_free_position(int* x, int* y, int skip_index);
static void desktop_icon_normalize_all_positions(void);
static int desktop_icon_guess_kind(const char* path, const char* name, int is_file);
/* 以下、複数アプリ共有の include / extern 宣言 / 定数マクロは
 * gui_apps_common.h に移動済み（cosnet_state.h, calc_engine.h, net.h,
 * tls_backend.h 等の追加includeと、weak-linkされたオプション機能の
 * extern宣言、virt_mem_stats_t、KEY_* マクロなど）。 */

const char* gui_text(const char* en, const char* ja);


#define GUI_DESKTOP_SNAPSHOT_PATH      "/desktop.state"
#define GUI_DESKTOP_SNAPSHOT_PATH_BAK  "/desktop.state.bak"
#define GUI_BROWSER_SNAPSHOT_PATH      "/browser.state"
#define GUI_WINDOW_SNAPSHOT_PATH_PREFIX "/window_"

/* #define 0x534E4150  0x4453434Du  DSCM */
#define GUI_DESKTOP_SNAPSHOT_VERSION 3

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    window_t state;
} gui_window_snapshot_t;

static void gui_window_snapshot_path(int kind, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (kind < 0) kind = 0;
    snprintf(out, out_size, "%s%d.state", GUI_WINDOW_SNAPSHOT_PATH_PREFIX, kind);
}

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t icon_size;
    int32_t count;
    uint64_t checksum;
    desktop_icon_t icons[MAX_ICONS];
    app_folder_t folders[MAX_APP_FOLDERS];
} gui_desktop_snapshot_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    char url[sizeof(((window_t*)0)->browser_url)];
    char title[sizeof(((window_t*)0)->browser_title)];
    char search_text[sizeof(((window_t*)0)->browser_search_text)];
    int32_t scroll;
    int32_t url_cursor;
    int32_t url_focus;
    int32_t url_selected;
    int32_t search_focus;
    int32_t search_selected;
    int32_t search_cursor;
} gui_browser_snapshot_t;

static void gui_copy_cstr(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (i + 1 < dst_size && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static bool g_desktop_layout_load_pending = false;
static uint64_t g_desktop_layout_load_after_frame = 0;
static bool g_desktop_layout_dirty = false;

void gui_snapshot_save_desktop(void);
static bool gui_snapshot_write_desktop_file(const char* path, const gui_desktop_snapshot_t* snap);
static uint64_t gui_snapshot_desktop_checksum(const gui_desktop_snapshot_t* snap);

// Implement missing functions
bool path_is_root(const char* path) {
    return !path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0');
}

void gui_notify(const char* msg, int type) {
    serial_puts("[GUI NOTIFY] ");
    serial_puts(msg);
    serial_puts("\n");
    
    /* Forward to new notification center */
    extern uint32_t notification_send_with_duration(const char* title, const char* message, int type, int priority, int duration_ms);
    int ntype = 0; /* NOTIFY_TYPE_INFO */
    if (type == 1) ntype = 3; /* NOTIFY_TYPE_SUCCESS */
    else if (type == 2) ntype = 1; /* NOTIFY_TYPE_WARNING */
    else if (type == 3) ntype = 2; /* NOTIFY_TYPE_ERROR */
    
    notification_send_with_duration("System", msg, ntype, 1, 3000); /* NOTIFY_PRIORITY_NORMAL, 3s duration */
}

void gui_notify_simple(const char* msg) {
    serial_puts("[GUI] ");
    serial_puts(msg);
    serial_puts("\n");
    extern uint32_t notification_send_with_duration(const char* title, const char* message, int type, int priority, int duration_ms);
    notification_send_with_duration("System", msg, 0, 1, 2000);
}

/* ============================================================
 * Clipboard helpers shared by file manager and text editor
 * ============================================================ */
static gui_clipboard_kind_t g_clipboard_kind = GUI_CLIPBOARD_EMPTY;
static char g_clipboard_text[TEXT_BUF_SIZE];
static char g_clipboard_path[FS_MAX_PATH];
static bool g_clipboard_cut = false;




void gui_clipboard_set_text(const char* text) {
    g_clipboard_kind = GUI_CLIPBOARD_TEXT;
    copy_cstr_local(g_clipboard_text, sizeof(g_clipboard_text), text ? text : "");
    g_clipboard_path[0] = '\0';
    g_clipboard_cut = false;
}

const char* gui_clipboard_get_text(void) {
    return (g_clipboard_kind == GUI_CLIPBOARD_TEXT) ? g_clipboard_text : NULL;
}















void gui_clamp_desktop_icon_position(int* x, int* y) {
    if (!x || !y) return;

    int origin_x = 16;
    int origin_y = 20;
    int cell_w = 0;
    int cell_h = 0;
    int cols = 0;
    int rows = 0;
    desktop_icon_grid_metrics(&origin_x, &origin_y, &cell_w, &cell_h, &cols, &rows);
    if (cell_w < 1) cell_w = 1;
    if (cell_h < 1) cell_h = 1;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    int min_x = origin_x;
    int min_y = origin_y;
    int max_x = origin_x + (cols - 1) * cell_w;
    int max_y = origin_y + (rows - 1) * cell_h;
    if (max_x < min_x) max_x = min_x;
    if (max_y < min_y) max_y = min_y;

    if (*x < min_x) *x = min_x;
    if (*y < min_y) *y = min_y;
    if (*x > max_x) *x = max_x;
    if (*y > max_y) *y = max_y;
}

// Missing GUI functions
static uint64_t gui_snapshot_desktop_checksum(const gui_desktop_snapshot_t* snap) {
    if (!snap) return 0;
    uint64_t hash = 1469598103934665603ULL; /* FNV-1a offset */
    const uint8_t* bytes = (const uint8_t*)&snap->icon_size;
    size_t len = sizeof(snap->icon_size) + sizeof(snap->count) + sizeof(snap->icons[0]) * (size_t)((snap->count > 0 && snap->count < MAX_ICONS) ? snap->count : 0);
    for (size_t i = 0; i < len; ++i) {
        hash ^= (uint64_t)bytes[i];
        hash *= 1099511628211ULL;
    }
    /* app_folders[] is always fully included (every slot, not just
     * "in_use" ones) - unlike icons[], there is no separate persisted
     * count to bound it by, and it is small (MAX_APP_FOLDERS is 16),
     * so hashing unused-but-zeroed slots too costs nothing meaningful
     * and keeps this loop's bounds simple. */
    const uint8_t* fbytes = (const uint8_t*)&snap->folders[0];
    size_t flen = sizeof(snap->folders);
    for (size_t i = 0; i < flen; ++i) {
        hash ^= (uint64_t)fbytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool gui_snapshot_write_desktop_file(const char* path, const gui_desktop_snapshot_t* snap) {
    if (!path || !path[0] || !snap || !storage_write_file) return false;
    return storage_write_file(path, snap, (uint64_t)sizeof(*snap));
}



void gui_snapshot_save_desktop(void) {
    if (!storage_write_file) return;
    desktop_icon_normalize_all_positions();
    gui_desktop_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.magic = 0x534E4150;
    snap.version = GUI_DESKTOP_SNAPSHOT_VERSION;
    snap.icon_size = gui_get_desktop_icon_size();
    snap.count = desktop_icon_count;
    if (snap.count < 0) snap.count = 0;
    if (snap.count > MAX_ICONS) snap.count = MAX_ICONS;
    for (int i = 0; i < snap.count; ++i) {
        snap.icons[i] = desktop_icons[i];
    }
    for (int i = 0; i < MAX_APP_FOLDERS; ++i) {
        snap.folders[i] = app_folders[i];
    }
    snap.checksum = gui_snapshot_desktop_checksum(&snap);

    (void)gui_snapshot_write_desktop_file(GUI_DESKTOP_SNAPSHOT_PATH, &snap);
    (void)gui_snapshot_write_desktop_file(GUI_DESKTOP_SNAPSHOT_PATH_BAK, &snap);
}



static void desktop_icon_normalize_position(desktop_icon_t* ic, int fallback_index) {
    if (!ic) return;
    gui_clamp_desktop_icon_position(&ic->x, &ic->y);
    desktop_icon_find_free_position(&ic->x, &ic->y, fallback_index);
}

static void desktop_icon_normalize_all_positions(void) {
    for (int i = 0; i < desktop_icon_count; ++i) {
        desktop_icon_normalize_position(&desktop_icons[i], i);
    }
}

void gui_normalize_desktop_icons(void) {
    desktop_icon_normalize_all_positions();
}

void gui_snap_desktop_icon_position(int* x, int* y, int skip_index) {
    desktop_icon_find_free_position(x, y, skip_index);
}

/* iOS-style "lift" feedback while dragging a desktop icon: the icon
 * smoothly shrinks a little the moment a drag becomes real (past the
 * click/drag threshold - see gui_input.c), stays small while it
 * follows the cursor, then smoothly grows back to full size the moment
 * it's dropped. Purely a rendering effect - gui_desktop_icon_index and
 * gui_desktop_icon_target_scale below never affect where the icon
 * actually snaps to, only how draw_desktop_icons() sizes it each
 * frame while the animation is in progress. */
#define GUI_ICON_DRAG_SCALE_MS   140
#define GUI_ICON_DRAG_SCALE_MIN  0.82f

static int      gui_icon_drag_anim_index = -1;
static uint64_t gui_icon_drag_anim_start_ms = 0;
static float    gui_icon_drag_anim_from = 1.0f;
static float    gui_icon_drag_anim_to = 1.0f;

void gui_desktop_icon_drag_begin(int index) {
    gui_icon_drag_anim_index = index;
    gui_icon_drag_anim_from = 1.0f; /* a drag always begins from full size */
    gui_icon_drag_anim_to = GUI_ICON_DRAG_SCALE_MIN;
    gui_icon_drag_anim_start_ms = get_timer_ticks();
}

void gui_desktop_icon_drag_end(void) {
    if (gui_icon_drag_anim_index < 0) return;
    /* Reverse from wherever the shrink animation currently is (it may
     * not have finished shrinking yet if the drag was very short) back
     * up to full size, rather than snapping the scale discontinuously. */
    gui_icon_drag_anim_from = gui_desktop_icon_current_scale(gui_icon_drag_anim_index);
    gui_icon_drag_anim_to = 1.0f;
    gui_icon_drag_anim_start_ms = get_timer_ticks();
}

float gui_desktop_icon_current_scale(int index) {
    if (index < 0 || index != gui_icon_drag_anim_index) return 1.0f;
    uint64_t elapsed = get_timer_ticks() - gui_icon_drag_anim_start_ms;
    float t = (float)elapsed / (float)GUI_ICON_DRAG_SCALE_MS;
    if (t >= 1.0f) {
        /* Animation finished. If it finished GROWING back to 1.0, this
         * index no longer needs tracking at all - free the slot so a
         * later drag of a *different* icon (or even the same one
         * again) starts clean. If it finished SHRINKING, leave it
         * tracked at the small size - the drag is still in progress. */
        if (gui_icon_drag_anim_to >= 1.0f) gui_icon_drag_anim_index = -1;
        return gui_icon_drag_anim_to;
    }
    /* Still mid-tween - keep the render loop refreshing every frame
     * until it finishes (matches how gui_app_folder_update() drives its
     * own open/close animation), otherwise this would only visibly
     * update whenever some OTHER redraw happened to occur. */
    gui_request_redraw();
    /* Simple ease-out - smooth and snappy without needing a spring
     * overshoot for a change this small (unlike the App Folder open
     * animation, a slightly-shrunk icon overshooting past its target
     * would just look like jitter, not a bounce). */
    float f = 1.0f - t;
    float eased = 1.0f - f * f;
    return gui_icon_drag_anim_from + (gui_icon_drag_anim_to - gui_icon_drag_anim_from) * eased;
}

static void desktop_icon_remove_at(int idx) {
    if (idx < 0 || idx >= desktop_icon_count) return;
    for (int i = idx; i + 1 < desktop_icon_count; ++i) {
        desktop_icons[i] = desktop_icons[i + 1];
    }
    if (desktop_icon_count > 0) {
        memset(&desktop_icons[desktop_icon_count - 1], 0, sizeof(desktop_icons[0]));
        desktop_icon_count--;
    }
}

void gui_save_desktop_layout(void) {
    gui_snapshot_save_desktop();
}

bool gui_create_desktop_app_icon(int win_kind, int x, int y) {
    if (desktop_icon_count >= MAX_ICONS) return false;

    desktop_icon_t ic;
    memset(&ic, 0, sizeof(ic));
    ic.win_kind = win_kind;
    ic.is_file = false;
    ic.is_dynamic = false;
    ic.selected = false;
    ic.anim = 0;
    gui_copy_cstr(ic.label, sizeof(ic.label), desktop_icon_label_for_kind(win_kind));

    if (x <= 0 && y <= 0) {
        desktop_icon_grid_position_for_index(desktop_icon_count, &ic.x, &ic.y);
    } else {
        ic.x = x;
        ic.y = y;
        gui_clamp_desktop_icon_position(&ic.x, &ic.y);
    }
    desktop_icon_normalize_position(&ic, desktop_icon_count);

    desktop_icons[desktop_icon_count++] = ic;
    g_desktop_layout_dirty = true;
    gui_request_redraw();
    return true;
}

bool gui_delete_desktop_icon_at(int idx) {
    if (idx < 0 || idx >= desktop_icon_count) return false;

    desktop_icon_t ic = desktop_icons[idx];
    bool ok = true;

    if (ic.is_dynamic && ic.path[0]) {
        if (ic.is_file) {
            ok = fs_delete_file(ic.path);
        } else {
            ok = fs_delete_dir(ic.path);
        }
    }

    desktop_icon_remove_at(idx);
    g_desktop_layout_dirty = true;
    gui_request_redraw();
    return ok;
}

void gui_refresh_desktop_icons(void) {
    desktop_icon_t preserved[MAX_ICONS];
    int preserved_count = 0;
    memset(preserved, 0, sizeof(preserved));

    for (int i = 0; i < desktop_icon_count; ++i) {
        if (!desktop_icons[i].is_dynamic) {
            if (preserved_count < MAX_ICONS) {
                preserved[preserved_count++] = desktop_icons[i];
            }
        }
    }

    fs_entry_t* entries = fs_list_dir("/desktop");
    int total = fs_entry_count_for_path("/desktop");
    for (int i = 0; entries && i < total; ++i) {
        if (entries[i].name[0] == '\0') continue;
        if (entries[i].is_hidden || entries[i].is_system) continue;

        const char* fullpath = entries[i].path[0] ? entries[i].path : NULL;
        int found = -1;
        for (int j = 0; j < desktop_icon_count; ++j) {
            if (!desktop_icons[j].is_dynamic) continue;
            if (fullpath && desktop_icons[j].path[0] && strcmp(desktop_icons[j].path, fullpath) == 0) {
                found = j;
                break;
            }
        }

        desktop_icon_t ic;
        memset(&ic, 0, sizeof(ic));
        copy_cstr_local(ic.path, sizeof(ic.path), fullpath ? fullpath : "");
        copy_cstr_local(ic.label, sizeof(ic.label), entries[i].name);
        ic.win_kind = desktop_icon_guess_kind(fullpath ? fullpath : entries[i].name, entries[i].name, !entries[i].is_dir);
        ic.selected = false;
        ic.anim = 0;
        ic.is_file = !entries[i].is_dir;
        ic.is_dynamic = true;

        if (found >= 0) {
            ic.x = desktop_icons[found].x;
            ic.y = desktop_icons[found].y;
        } else {
            desktop_icon_grid_position_for_index(preserved_count, &ic.x, &ic.y);
            desktop_icon_normalize_position(&ic, preserved_count);
        }

        if (preserved_count < MAX_ICONS) {
            preserved[preserved_count++] = ic;
        }
    }

    desktop_icon_count = 0;
    for (int i = 0; i < preserved_count && i < MAX_ICONS; ++i) {
        desktop_icons[desktop_icon_count++] = preserved[i];
    }

    gui_localize_desktop_icons();
    g_desktop_layout_dirty = true;
    gui_request_redraw();
}





bool gui_save_window_state_snapshot(const window_t* w) {
    if (!w || !storage_write_file) return false;
    char path[64];
    gui_window_snapshot_path(w->kind, path, sizeof(path));
    if (!path[0]) return false;

    gui_window_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    /* snap.magic = 0x57494E53;  WINS */
    snap.version = 1;
    snap.state = *w;
    return storage_write_file(path, &snap, (uint64_t)sizeof(snap));
}

bool gui_load_window_state_snapshot(window_t* w) {
    if (!w || !storage_read_file) return false;
    char path[64];
    gui_window_snapshot_path(w->kind, path, sizeof(path));
    if (!path[0]) return false;

    gui_window_snapshot_t snap;
    uint64_t read_size = 0;
    memset(&snap, 0, sizeof(snap));
    if (!storage_read_file(path, &snap, sizeof(snap), &read_size)) return false;
    if (read_size < sizeof(gui_window_snapshot_t)) return false;
    if (snap.magic != 0x57494E53 || snap.version != 1) return false;
    if (snap.state.kind != w->kind) return false;

    bool was_visible = w->visible;
    int kind = w->kind;
    *w = snap.state;
    w->kind = kind;
    w->visible = was_visible;
    return true;
}

static void desktop_icon_grid_position_for_index(int idx, int* out_x, int* out_y);

static void desktop_icon_set_defaults(void) {
    desktop_icon_count = 0;
    if (MAX_ICONS < 20) return;

    int x, y;

    desktop_icon_grid_position_for_index(0, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "File Manager",   "", WIN_FILE_MGR,   false, 0, false, false};
    desktop_icon_grid_position_for_index(1, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "Text Editor",    "", WIN_TEXT_EDITOR,false, 0, false, false};
    desktop_icon_grid_position_for_index(2, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "Terminal",       "", WIN_TERMINAL,   false, 0, false, false};
    desktop_icon_grid_position_for_index(3, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "Settings",       "", WIN_SETTINGS,   false, 0, false, false};
    desktop_icon_grid_position_for_index(4, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "About",          "", WIN_ABOUT,      false, 0, false, false};
    desktop_icon_grid_position_for_index(5, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "Calculator",     "", WIN_CALC,       false, 0, false, false};
    desktop_icon_grid_position_for_index(6, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "Storage",        "", WIN_STORAGE,    false, 0, false, false};
    desktop_icon_grid_position_for_index(7, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "NetSurf",    "", WIN_BROWSER,    false, 0, false, false};
    desktop_icon_grid_position_for_index(8, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "Task Manager",   "", WIN_TASK_MGR,   false, 0, false, false};
    desktop_icon_grid_position_for_index(9, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "Paint",          "", WIN_PAINT,      false, 0, false, false};
    desktop_icon_grid_position_for_index(10, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "MP3 Player",     "", WIN_MUSIC,      false, 0, false, false};
    desktop_icon_grid_position_for_index(11, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "Clock",          "", WIN_CLOCK,      false, 0, false, false};
    desktop_icon_grid_position_for_index(12, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "System Info",    "", WIN_SYSINFO,    false, 0, false, false};
    desktop_icon_grid_position_for_index(14, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "HTTP Downloader", "", WIN_HTTP_DOWNLOADER, false, 0, false, false};
    desktop_icon_grid_position_for_index(15, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "Spreadsheet",    "", WIN_SHEET,      false, 0, false, false};
    desktop_icon_grid_position_for_index(16, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "Launcher", "", WIN_VOXEL_GAME, false, 0, false, false};

    /* User-facing desktop file targets.  The path field preserves their
     * semantic identity while the icon kind selects the matching folder,
     * document, and photo artwork. */
    desktop_icon_grid_position_for_index(17, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "Folder", "/desktop/Documents", WIN_FILE_MGR, false, 0, false, false};
    desktop_icon_grid_position_for_index(18, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "README.txt", "/desktop/README.txt", WIN_TEXT_EDITOR, false, 0, true, false};
    desktop_icon_grid_position_for_index(19, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "PHOTO.jpg", "/desktop/PHOTO.jpg", WIN_JPEG, false, 0, true, false};
    desktop_icon_grid_position_for_index(20, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "C-OS Studio",   "", WIN_STUDIO,     false, 0, false, false};
    desktop_icon_grid_position_for_index(21, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "Py2C Studio",   "", WIN_TCC_IDE,    false, 0, false, false};
    desktop_icon_grid_position_for_index(22, &x, &y);
    desktop_icons[desktop_icon_count++] = (desktop_icon_t){x, y, "C-OS Message",  "", WIN_MESSENGER,  false, 0, false, false};
    gui_localize_desktop_icons();
}


static const char* desktop_icon_label_for_kind(int kind) {
    switch (kind) {
        case WIN_FILE_MGR: return gui_text("File Manager", "ファイルマネージャー");
        case WIN_TEXT_EDITOR: return gui_text("Text Editor", "テキストエディター");
        case WIN_TERMINAL: return gui_text("Terminal", "ターミナル");
        case WIN_SETTINGS: return gui_text("Settings", "設定");
        case WIN_ABOUT: return gui_text("About", "情報");
        case WIN_CALC: return gui_text("Calculator", "電卓");
        case WIN_STORAGE: return gui_text("Storage", "ストレージ");
        case WIN_BROWSER: return gui_text("NetSurf 3.11", "NetSurf 3.11");
        case WIN_TASK_MGR: return gui_text("Task Manager", "タスクマネージャー");
        case WIN_PAINT: return gui_text("Paint", "ペイント");
        case WIN_MESSENGER: return gui_text("C-OS Message", "C-OS Message");
        case WIN_MUSIC: return gui_text("MP3 Player", "MP3プレーヤー");
        case WIN_STUDIO: return gui_text("C-OS Studio", "C-OS Studio");
        case WIN_TCC_IDE: return gui_text("Py2C Studio", "Py2C Studio");
        case WIN_JPEG: return gui_text("Image Viewer", "画像ビューア");
        case WIN_CLOCK: return gui_text("Clock", "時計");
        case WIN_SYSINFO: return gui_text("System Info", "システム情報");
        case WIN_HTTP_DOWNLOADER: return gui_text("HTTP Downloader", "HTTPダウンローダー");
        case WIN_SHEET: return gui_text("Spreadsheet", "表計算");
        case WIN_VOXEL_GAME: return gui_text("Launcher", "Launcher");
        default: return gui_text("Window", "ウィンドウ");
    }
}

void gui_localize_desktop_icons(void) {
    for (int i = 0; i < desktop_icon_count; ++i) {
        /* Path-backed desktop targets carry meaningful file/folder labels;
         * preserve them instead of replacing them with the application name. */
        if (desktop_icons[i].path[0] != '\0') continue;
        copy_cstr_local(desktop_icons[i].label, sizeof(desktop_icons[i].label), desktop_icon_label_for_kind(desktop_icons[i].win_kind));
    }
}








static void desktop_icon_grid_metrics(int* origin_x, int* origin_y, int* cell_w, int* cell_h, int* cols, int* rows) {
    int box = desktop_icon_box_size();
    int ox = 16;
    int oy = 20;
    int cw = box + 28;
    int ch = box + 36;
    int min_cw = box + 16;
    int min_ch = box + 28;
    int usable_w = (int)SCREEN_W - ox - 16;
    int usable_h = (int)SCREEN_H - TASKBAR_H - oy - 16;

    if (usable_w < 1) usable_w = 1;
    if (usable_h < 1) usable_h = 1;
    if (cw < min_cw) cw = min_cw;
    if (ch < min_ch) ch = min_ch;

    int c = usable_w / cw;
    int r = usable_h / ch;
    if (c < 1) c = 1;
    if (r < 1) r = 1;

    while (c * r < MAX_ICONS && (cw > min_cw || ch > min_ch)) {
        if (cw > min_cw) cw -= 4;
        if (ch > min_ch) ch -= 4;
        c = usable_w / cw;
        r = usable_h / ch;
        if (c < 1) c = 1;
        if (r < 1) r = 1;
    }

    if (c * r < MAX_ICONS) {
        c = usable_w / min_cw;
        if (c < 1) c = 1;
        r = (MAX_ICONS + c - 1) / c;
        while (r * min_ch > usable_h && c < MAX_ICONS) {
            c++;
            r = (MAX_ICONS + c - 1) / c;
        }
        cw = min_cw;
        ch = min_ch;
    }

    if (origin_x) *origin_x = ox;
    if (origin_y) *origin_y = oy;
    if (cell_w) *cell_w = cw;
    if (cell_h) *cell_h = ch;
    if (cols) *cols = c;
    if (rows) *rows = r;
}



static int desktop_icon_box_size(void) {
    int size = (int)gui_get_desktop_icon_size();
    if (size < 24) size = 24;
    if (size > 104) size = 104;
    return size;
}

int gui_get_desktop_icon_render_size(void) {
    return desktop_icon_box_size();
}

static bool desktop_icon_slot_is_occupied(int slot_x, int slot_y, int skip_index) {
    for (int i = 0; i < desktop_icon_count; ++i) {
        if (i == skip_index) continue;
        if (desktop_icons[i].x == slot_x && desktop_icons[i].y == slot_y) return true;
    }
    return false;
}

static void desktop_icon_find_free_position(int* x, int* y, int skip_index) {
    if (!x || !y) return;

    int origin_x = 0;
    int origin_y = 0;
    int cell_w = 0;
    int cell_h = 0;
    int cols = 0;
    int rows = 0;
    desktop_icon_grid_metrics(&origin_x, &origin_y, &cell_w, &cell_h, &cols, &rows);
    if (cell_w < 1) cell_w = 1;
    if (cell_h < 1) cell_h = 1;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    int start_col = (*x - origin_x + cell_w / 2) / cell_w;
    int start_row = (*y - origin_y + cell_h / 2) / cell_h;
    if (start_col < 0) start_col = 0;
    if (start_row < 0) start_row = 0;
    if (start_col >= cols) start_col = cols - 1;
    if (start_row >= rows) start_row = rows - 1;

    for (int radius = 0; radius < cols + rows; ++radius) {
        for (int row = start_row - radius; row <= start_row + radius; ++row) {
            if (row < 0 || row >= rows) continue;
            for (int col = start_col - radius; col <= start_col + radius; ++col) {
                if (col < 0 || col >= cols) continue;
                int edge = col - start_col;
                if (edge < 0) edge = -edge;
                int edge_row = row - start_row;
                if (edge_row < 0) edge_row = -edge_row;
                if (edge < radius && edge_row < radius) continue;

                int slot_x = origin_x + col * cell_w;
                int slot_y = origin_y + row * cell_h;
                if (desktop_icon_slot_is_occupied(slot_x, slot_y, skip_index)) continue;
                *x = slot_x;
                *y = slot_y;
                return;
            }
        }
    }

    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            int slot_x = origin_x + col * cell_w;
            int slot_y = origin_y + row * cell_h;
            if (desktop_icon_slot_is_occupied(slot_x, slot_y, skip_index)) continue;
            *x = slot_x;
            *y = slot_y;
            return;
        }
    }

    if (*x < origin_x) *x = origin_x;
    if (*y < origin_y) *y = origin_y;
}

static void desktop_icon_grid_position_for_index(int idx, int* out_x, int* out_y) {
    int origin_x = 0;
    int origin_y = 0;
    int cell_w = 0;
    int cell_h = 0;
    int cols = 0;
    int rows = 0;
    desktop_icon_grid_metrics(&origin_x, &origin_y, &cell_w, &cell_h, &cols, &rows);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    int slot_count = cols * rows;
    if (slot_count < 1) slot_count = 1;
    if (idx < 0) idx = 0;
    if (idx >= slot_count) idx = idx % slot_count;

     /* Desktop-style flow: fill top-to-bottom in a column, then advance right. */
    int col = idx / rows;
    int row = idx % rows;
    if (col >= cols) {
        col = cols - 1;
        row = idx % rows;
    }

    if (out_x) *out_x = origin_x + col * cell_w;
    if (out_y) *out_y = origin_y + row * cell_h;
}

static void desktop_icon_apply_grid(void) {
    for (int i = 0; i < desktop_icon_count; ++i) {
        desktop_icon_grid_position_for_index(i, &desktop_icons[i].x, &desktop_icons[i].y);
    }
}

static int desktop_icon_guess_kind(const char* path, const char* name, int is_file) {

    const char* probe = name && name[0] ? name : path;
    if (!probe) return is_file ? WIN_TEXT_EDITOR : WIN_FILE_MGR;
    if (strstr(probe, "Terminal") || strstr(probe, "ターミナル")) return WIN_TERMINAL;
    if (strstr(probe, "File") || strstr(probe, "ファイル")) return WIN_FILE_MGR;
    if (strstr(probe, "Editor") || strstr(probe, "エディタ")) return WIN_TEXT_EDITOR;
    if (strstr(probe, "Settings") || strstr(probe, "設定")) return WIN_SETTINGS;
    if (strstr(probe, "Browser") || strstr(probe, "NetSurf") || strstr(probe, "ブラウザ")) return WIN_BROWSER;
    if (fm_is_jpeg_file(probe)) return WIN_JPEG;
    return is_file ? WIN_TEXT_EDITOR : WIN_FILE_MGR;
}





void gui_reset_desktop_icons(void) {
    desktop_icon_apply_grid();
    g_desktop_layout_dirty = true;
    gui_request_redraw();
}



void gui_init_desktop_icons(void) {
    desktop_icon_set_defaults();
    desktop_icon_apply_grid();

    /* Keep startup deterministic: do not restore or save the desktop layout
       during boot. Any later desktop changes will be persisted lazily. */
    g_desktop_layout_load_pending = false;
    g_desktop_layout_load_after_frame = 0;
    g_desktop_layout_dirty = false;
}

void gui_process_pending_desktop_layout_load(void) {
     /* Disabled by default to avoid scanning /desktop during startup. */
    (void)g_desktop_layout_load_pending;
    (void)g_desktop_layout_load_after_frame;
}

void gui_sync_desktop_with_fs(void) {
    /* The kernel may call this from its main loop, so persistence must be
       cheap when nothing changed. */
    if (!g_desktop_layout_dirty) return;
    if (gui_frame_counter < 240u) return;

    serial_puts("[GUI] Syncing desktop with filesystem\n");
    gui_snapshot_save_desktop();
    g_desktop_layout_dirty = false;
}







// Missing global variables
 /* desktop_icon_count defined in gui.c */
 /* desktop_icons defined in gui.c */










uint64_t kmemory_used(void) {
    phys_mem_stats_t* st = phys_memory_get_stats();
    if (!st) return 0;
    return st->used_pages * 4096ULL;
}

uint64_t kmemory_free(void) {
    phys_mem_stats_t* st = phys_memory_get_stats();
    if (!st) return 0;
    return st->free_pages * 4096ULL;
}
// Add missing functions (only those not already defined)
minimal_mouse_t* _get_mouse(void) {
    return minimal_mouse_get_state();
}

// Remove duplicate function definitions that are already in headers

// Add missing functions
/* rtc_get_datetime() / vga_isin() / vga_icos() / vga_draw_line_thick() の
 * extern宣言は gui_apps_common.h に移動済み（時計アプリからも必要なため）。 */
/* vga_fill_rect defined in drivers/video/vga.c */
/* vga_draw_string defined in drivers/video/vga.c */
/* vga_draw_rounded_rect defined in drivers/video/vga.c */

// Filesystem functions






/* 色/レイアウト定数は gui_apps_common.h に移動済み */

 /* Helper functions */






 /* Colors */










extern window_t windows[MAX_WINDOWS];

 /* mouse access via function */







/* gui_apps_common.c から移動: fm_effective_path()(static)に依存するためcore専用。 */













typedef struct {
    fs_entry_t* entry;
    bool parent;
} fm_row_item_t;


































 /* Create new folder in current directory */


 /* Create new file in current directory */


 /* Rename selected item */


 /* Delete selected item */












