#ifndef GUI_H
#define GUI_H
#include "types.h"
#include "mouse_minimal.h"
#include "keyboard.h"

/* C-OS 5.0.0 GUI System - Fixed Desktop Boot Sequence */

/* Window kinds */
#define WIN_NONE        0
#define WIN_FILE_MGR    1
#define WIN_TEXT_EDITOR 2
#define WIN_TERMINAL    3
#define WIN_SETTINGS    4
#define WIN_ABOUT       5
#define WIN_CALC        6
#define WIN_STORAGE     7
#define WIN_BROWSER     8
#define WIN_TASK_MGR    9
#define WIN_PAINT       10
#define WIN_MUSIC       11
#define WIN_CLOCK       12
#define WIN_SYSINFO     13
/* 14 was the Python IDE (removed with MicroPython); the slot now belongs to C-OS Studio, a ring-3 program
 * (cos_studio.c) - like WIN_MUSIC it never becomes a kernel-drawn window, it launches/raises the program. */
#define WIN_STUDIO      14
#define WIN_CALC_GRAPH  15
#define WIN_SHEET       16
#define WIN_VOXEL_GAME  17
#define WIN_JPEG        18
#define WIN_MEMORY_MGR  19
#define WIN_TCC_IDE     20
#define WIN_HTTP_DOWNLOADER 21
#define WIN_TINYGL_VIEWER 22
#define WIN_2DGAMES      23
/* Window owned by a ring3 .c-os program; its contents are painted from a
 * syscall-fed command queue (see kernel/cos_app_window.c). */
#define WIN_COS_APP      24
/* Not a real window - see gui_app_folder.c. A desktop icon with this
 * kind opens the iOS-style folder overlay (gui_app_folder_open())
 * instead of gui_open_window(); it must never reach gui_open_window()
 * itself, which has no case for it. */
#define WIN_APP_FOLDER   25
/* C-OS Messenger: a ring-3 program (cos_messenger.c); like WIN_PAINT it only launches/raises the program. */
#define WIN_MESSENGER    26

/* Limits */
#define MAX_WINDOWS     20
#define MAX_ICONS       64
#define MAX_NOTIFS      8

/* Dimensions */
#define TASKBAR_H       48
#define TITLEBAR_H      32
#define WIN_MIN_W       300
#define WIN_MIN_H       200
#define STATUSBAR_H     24

/* Animation */
#define ANIM_FRAMES     8

/* Cursor types */
typedef enum {
    CURSOR_ARROW = 0,
    CURSOR_HAND,
    CURSOR_TEXT,
    CURSOR_WAIT,
    CURSOR_CROSS,
    CURSOR_MOVE,
    CURSOR_RESIZE,
    CURSOR_COUNT
} cursor_type_t;

/* Dark mode */
extern int gui_dark_mode;

/* System tray */
#define TRAY_ICON_SIZE  24
#define TRAY_MAX_ICONS  6
typedef struct {
    int     x, y;
    bool    visible;
    char    tooltip[32];
} tray_icon_t;

/* Text editor */
#define TEXT_BUF_SIZE   32768   /* 32KB per editor */
#define TERM_LINES      50
#define TERM_LINE_LEN   256
#define TERM_HISTORY    100

/* Notification */
typedef struct {
    char    msg[128];
    int     timer;
    int     type;   /* 0=info 1=warn 2=error */
    bool    visible;
} notif_t;

/* Window state */
typedef struct {
    int     x, y, w, h;
    int     kind;
    bool    visible;
    bool    minimized;
    bool    maximized;
    bool    focused;
    int     restore_x, restore_y, restore_w, restore_h;
    bool    fullscreen;                 /* covers the whole screen: no title bar, no taskbar (ring-3 games; SYS_WIN2_FULLSCREEN) */
    int     fs_x, fs_y, fs_w, fs_h;     /* where the window was before it went fullscreen */
    char    title[64];
    int     scroll_y;
    int     scroll_x;
    char    filename[128];

    /* Browser */
    char    browser_url[512];
    char    browser_title[128];
    int     browser_scroll;
    int     browser_url_focus;
    bool    browser_url_selected;
    int     browser_url_cursor;
    char    browser_search_text[256];
    int     browser_search_focus;
    bool    browser_search_selected;
    int     browser_search_cursor;
    bool    browser_initial_load_pending;
    /* Browser-only right-click menu; deliberately separate from the shared
     * desktop/window context menu so page actions never affect other apps. */
    bool    browser_context_visible;
    int     browser_context_x;
    int     browser_context_y;
    int     browser_context_page_x;
    int     browser_context_page_y;

    /* Per-window scroll */
    int     vscroll;
    /* Stable identity. windows[] is compacted on close and reordered by
     * gui_bring_to_front(), so a window_t* (or index) held across frames
     * can silently end up pointing at a different window. Anything that
     * must refer to "this window" over time (ring-3 surfaces, ...) stores
     * uid and resolves it with gui_window_index_by_uid(). Never 0. */
    uint32_t uid;
} window_t;

/* Context menu */
#define CTX_MAX_ITEMS   32
#define CTX_SEPARATOR   -1
typedef struct {
    bool    visible;
    int     x, y, w, h;
    char    items[CTX_MAX_ITEMS][64];
    int     icons[CTX_MAX_ITEMS];
    bool    enabled[CTX_MAX_ITEMS];
    bool    checked[CTX_MAX_ITEMS];
    bool    separator[CTX_MAX_ITEMS];
    bool    submenu[CTX_MAX_ITEMS];
    int     submenu_kind[CTX_MAX_ITEMS];
    int     item_count;
    int     hovered;
    int     target_window;
    int     target_icon;
    int     context_type;   /* 0=desktop 1=window 2=file 3=taskbar */
} context_menu_t;

/* Desktop icon */
typedef struct {
    int    x, y;
    char   label[32];
    char   path[128];      /* Full path to file/folder */
    int    win_kind;
    bool   selected;
    int    anim;
    bool   is_file;        /* TRUE=file, FALSE=folder or app */
    bool   is_dynamic;     /* TRUE=auto-created from filesystem */
    int    folder_id;      /* Meaningful ONLY when win_kind==WIN_APP_FOLDER:
                             * index into app_folders[] (see
                             * gui_app_folder.c). Every existing desktop
                             * icon initializer in this codebase predates
                             * this field and so leaves it at C's
                             * aggregate-initializer zero-fill (0) - safe,
                             * since nothing ever reads folder_id without
                             * checking win_kind first; 0 is a valid
                             * app_folders[] index but is never reached
                             * for a non-folder icon. */
} desktop_icon_t;

/* iOS-style "app folder": a desktop icon that holds other apps instead
 * of launching one itself. See gui_app_folder.c for the full behavior
 * (merge-on-drag-drop, the open/close spring animation, launching or
 * removing an item from inside the open overlay). */
#define MAX_APP_FOLDERS       16
#define MAX_APP_FOLDER_ITEMS  12  /* first 9 shown in the closed 3x3 preview */

typedef struct {
    bool in_use;
    char name[32];
    int  win_kind[MAX_APP_FOLDER_ITEMS];
    char label[MAX_APP_FOLDER_ITEMS][32];
    int  count;
} app_folder_t;

extern app_folder_t app_folders[MAX_APP_FOLDERS];

/* Submenu */
typedef struct {
    bool    visible;
    int     x, y;
    char    items[16][64];
    int     item_count;
    int     hovered;
    int     parent_item;
    int     kind;
} submenu_t;

/* Notification */
extern notif_t notifications[MAX_NOTIFS];
extern int     notif_count;

bool path_is_root(const char* path);

/* Globals */
extern window_t       windows[MAX_WINDOWS];
extern int            window_count;
extern context_menu_t ctx_menu;
extern submenu_t      submenu;
extern desktop_icon_t desktop_icons[MAX_ICONS];
extern int            desktop_icon_count;

/* API */
void      gui_init(void);
void      gui_update(void);
void      gui_draw(void);  /* FIXED: ensures desktop renders after boot animation */

/* Cursor-only fast path (gui_render_loop.c). See the long comment above
 * cos_cursor_try_fast_move()'s definition for why this exists: ordinary
 * mouse movement was triggering the full desktop composition chain
 * (measured at ~14,500+ kcycles for the wallpaper/icon layer blit alone,
 * even on a cache hit) on every frame the cursor moved, which is most
 * frames during normal use. This restores the old cursor position from a
 * small backing buffer, captures and draws the new one, and flips only
 * those two small rects - independent of how expensive the rest of the
 * scene would be to rebuild, because it is never touched.
 *
 * Returns false (having drawn nothing) if the backing buffer does not
 * provably describe the cursor's current on-screen position - the first
 * frame, right after any full redraw, or with multi-cursor enabled - in
 * which case the caller must fall back to a normal full gui_draw(). This
 * is an expected, common outcome, not an error. */
bool      cos_cursor_try_fast_move(int old_x, int old_y, int new_x, int new_y);
/* Marks the backing buffer stale, forcing the next cursor move to fall
 * back to a full redraw once. Needed before any change that alters what
 * is on screen at the cursor's last-known position without going through
 * gui_draw() itself (there is no such caller today; kept for the next
 * one, so this does not have to be rediscovered). */
void      gui_set_clip(int x, int y, int w, int h);
void      gui_reset_clip(void);
void      gui_request_redraw(void);
/* Only the given screen rectangle changed: lets the next frame be composed and presented over just that area. */
void      gui_dirty_rect(int x, int y, int w, int h);
void      gui_refresh_file_managers_for_path(const char* fullpath);

/* Boot sequence integration */
void      gui_confirm_desktop_arrival(void);  /* Signal desktop has arrived */
void      gui_handle_input(void);
int gui_window_index_by_uid(uint32_t uid);
window_t* gui_open_window(int kind, const char* title, int x, int y, int w, int h);
void      gui_close_window(int idx);
void      gui_bring_to_front(int idx);
void      gui_minimize_window(int idx);
void      gui_maximize_window(int idx);
int       gui_fullscreen_index(void);   /* topmost visible fullscreen window, or -1 */
void      gui_restore_window(int idx);
bool      gui_save_window_state_snapshot(const window_t* w);
bool      gui_load_window_state_snapshot(window_t* w);
void      gui_notify(const char* msg, int type);
int       gui_find_window(int kind);
void      gui_focus_window(int idx);

/* Cursor functions */

/* Dark mode */
void      gui_toggle_dark_mode(void);
void      gui_set_dark_mode(bool enabled);
int       gui_is_dark_mode(void);
int       gui_get_wallpaper_idx(void);
bool      gui_get_terminal_autoscroll(void);
void      gui_set_terminal_autoscroll(bool enabled);
int       gui_get_font_scale(void);
void      gui_set_font_scale(int scale);
/* A fifth text-size step below Small - see vga_set_font_extra_small()
 * and the comment on gui_set_font_extra_small() in gui_settings.c. */
/* Exact text advance width in pixels (1..32, default 8). */
int       gui_get_font_pixel_width(void);
void      gui_set_font_pixel_width(int px);
int       gui_get_font_family(void);
void      gui_set_font_family(int family);
/* GUI rendering backend: 0 = CPU software rendering, 1 = VirtIO-GPU.
 * See the "GUI" tab in Settings and vga_set_render_backend() in vga.c. */
int       gui_get_render_backend(void);
bool      gui_set_render_backend(int backend);
bool      gui_render_backend_gpu_available(void);
bool      gui_set_gpu_full_draw(bool enable);
bool      gui_get_gpu_full_draw(void);
bool      gui_get_window_animations(void);
void      gui_set_window_animations(bool enabled);
bool      gui_get_fps_overlay(void);
void      gui_set_fps_overlay(bool enabled);
bool      gui_get_notifications_enabled(void);
void      gui_set_notifications_enabled(bool enabled);
bool      gui_get_autostart_terminal(void);
bool      gui_get_autostart_file_manager(void);
bool      gui_get_autostart_browser(void);
int       gui_get_mouse_sensitivity(void);
void      gui_set_mouse_sensitivity(int value);
bool      gui_get_mouse_raw_input(void);
void      gui_set_mouse_raw_input(bool enabled);
int       gui_get_mouse_drag_threshold(void);
void      gui_set_mouse_drag_threshold(int value);

/* Secondary green cursor: keyboard-operated accessibility/input mode. */
bool      gui_get_multi_cursor_enabled(void);
void      gui_set_multi_cursor_enabled(bool enabled);
void      gui_toggle_multi_cursor_mode(void);
void      gui_multi_cursor_move(int dx, int dy);
void      gui_multi_cursor_get_position(int* x, int* y);
void      gui_multi_cursor_request_click(bool right_button);
bool      gui_multi_cursor_take_click(int* x, int* y, bool* right_button);

void      gui_set_autostart_terminal(bool enabled);
void      gui_set_autostart_file_manager(bool enabled);
void      gui_set_autostart_browser(bool enabled);
void      gui_open_file_in_app(const char* path, int file_type);

/* Interactive graphics applications. */
int       gui_get_language_idx(void);
void      gui_set_language_idx(int idx);
void      gui_localize_desktop_icons(void);
const char* gui_text(const char* en, const char* ja);
bool      gui_is_japanese(void);

/* Animation */

/* System tray */

/* Wallpaper */
void      gui_set_wallpaper(int idx);
int       gui_get_wallpaper_count(void);
const char* gui_get_wallpaper_name(int idx);
int       gui_get_theme_idx(void);
void      gui_set_theme_idx(int idx);

/* App draw functions */
/* gui_apps_browser.c に実装。ファイルマネージャ等からブラウザへURLを渡す際に使用。
 * (gui_apps.c 分割前は static だったが、複数ファイルから参照されるため公開した) */
void browser_commit_navigation(window_t* w, const char* raw_url, bool add_history);
void draw_browser_app(int idx);
void draw_music_player(int idx);
void music_player_open(const char* path);

/* Input handlers */
void handle_browser_key(int idx, char ascii, int scancode, bool ctrl);
void handle_browser_click(int idx, int mx, int my);
void handle_browser_wheel(int idx, int wheel_delta);
void handle_browser_right_click(int idx, int mx, int my);


/* Clipboard */
typedef enum {
    GUI_CLIPBOARD_EMPTY = 0,
    GUI_CLIPBOARD_TEXT,
    GUI_CLIPBOARD_PATH
} gui_clipboard_kind_t;

void gui_clipboard_set_text(const char* text);
const char* gui_clipboard_get_text(void);

/* Desktop icon sync functions */
int  gui_add_desktop_icon(const char* path, const char* name, int is_file);
bool gui_create_desktop_app_icon(int win_kind, int x, int y);
bool gui_delete_desktop_icon_at(int idx);
void gui_reset_desktop_icons(void);
void gui_save_desktop_layout(void);
void gui_refresh_desktop_icons(void);
void gui_sync_desktop_with_fs(void);
void gui_init_desktop_icons(void);
void gui_clamp_desktop_icon_position(int* x, int* y);
void gui_snap_desktop_icon_position(int* x, int* y, int skip_index);
/* iOS-style icon "lift" while dragging - see gui_apps_core.c. */
void gui_desktop_icon_drag_begin(int index);
void gui_desktop_icon_drag_end(void);
float gui_desktop_icon_current_scale(int index);
int gui_get_desktop_icon_render_size(void);

/* Desktop icon sizing */
uint32_t gui_get_desktop_icon_size(void);
void gui_set_desktop_icon_size(uint32_t size);

/* FIX: draw_window_frame made non-static for gui_apps.c */
void draw_window_frame(int idx);

/* gui_apps.c - Additional application functions */
void gui_notify_simple(const char* msg);
minimal_mouse_t* _get_mouse(void);

typedef enum {
    GUI_IME_ACTION_NONE = 0,
    GUI_IME_ACTION_BACKSPACE,
    GUI_IME_ACTION_ENTER,
    GUI_IME_ACTION_TAB,
    GUI_IME_ACTION_SPACE,
} gui_ime_action_t;

bool gui_ime_translate_key(const keyboard_event_t* ev, char* out_text, size_t out_text_size, gui_ime_action_t* action);
void gui_ime_reset(void);
bool gui_ime_is_active(void);
int  gui_utf8_prev_char_start(const char* s, int pos);

#endif /* COS_INCLUDE_GUI_H */
