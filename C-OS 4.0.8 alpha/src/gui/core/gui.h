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
#define WIN_PYTHON_IDE  14
#define WIN_CALC_GRAPH  15
#define WIN_SHEET       16
#define WIN_VOXEL_GAME  17
#define WIN_JPEG        18
#define WIN_MEMORY_MGR  19
#define WIN_HTTP_DOWNLOADER 20
#define WIN_TINYGL_VIEWER 21
#define WIN_2DGAMES      22

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
    char    title[64];
    int     anim_frame;     /* open/close animation */
    int     opacity;        /* 0-255 */
    int     z_order;

    /* Text editor */
    char    text_buf[TEXT_BUF_SIZE];
    int     text_cursor;
    int     text_sel_start;
    int     text_sel_end;
    int     scroll_y;
    int     scroll_x;
    bool    text_modified;
    char    filename[128];

    /* Undo/redo: single-level, swap-based (Ctrl+Z swaps text_buf with
     * undo_buf, so pressing it again redoes) - see text_editor_maybe_snapshot()
     * in gui_input.c. A new snapshot is taken when a fresh "edit run"
     * starts (previous edit was over 500ms ago), not on every keystroke. */
    char     undo_buf[TEXT_BUF_SIZE];
    int      undo_cursor;
    bool     undo_valid;
    uint64_t last_edit_tick;

    /* Terminal */
    char    term_input[TERM_LINE_LEN];
    char    term_lines[TERM_LINES][TERM_LINE_LEN];
    int     term_line_count;
    int     term_scroll;
    char    term_history[TERM_HISTORY][TERM_LINE_LEN];
    int     term_hist_count;
    int     term_hist_pos;
    char    term_cwd[256];

    /* File manager */
    char    fm_path[1024];
    int     fm_selected;
    int     fm_scroll;
    int     fm_view_mode;   /* 0=list 1=icons 2=details */
    int     fm_action;      /* 0=none 1=create file 2=create folder 3=rename */
    char    fm_input[128];

    /* Calculator */
    char    calc_display[128];
    double  calc_acc;
    double  calc_mem;
    char    calc_op;
    bool    calc_clear_next;
    bool    calc_sci_mode;
    int     calc_mode;          /* 0=basic 1=scientific 2=study 3=hissan */
    bool    calc_angle_deg;     /* TRUE=degrees, FALSE=radians */
    char    calc_expr[512];     /* current expression */
    char    calc_steps[4096];    /* calculation trace / hints */
    char    calc_status[128];     /* mode/status message */
    int     calc_topic_idx;      /* selected study topic */
    bool    calc_initialized;    /* one-time calculator setup */

    /* Spreadsheet */
    char    sheet_cells[24][10][64];
    int     sheet_sel_row;
    int     sheet_sel_col;
    int     sheet_scroll_row;
    int     sheet_scroll_col;
    bool    sheet_editing;
    char    sheet_edit_buf[128];
    char    sheet_status[128];
    bool    sheet_initialized;

    /* Settings */
    int     settings_tab;
    bool    dark_mode;
    int     settings_theme_idx;
    int     wallpaper_idx;
    int     settings_scroll;
    char    settings_search[64];
    bool    settings_search_active;

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

    /* Task manager */
    int     taskmgr_tab;

    /* Paint */
    int     paint_tool;
    uint64_t paint_color;
    int     paint_size;

    /* Clock */
    bool    clock_analog;

    /* Memory manager */
    int     mem_mgr_tab;

    /* Context menu state */
    bool    context_is_dir;
    int     context_selected;

    /* Additional file manager state */
    bool    fm_sort_reverse;
    int     fm_sort_by;  /* 0=name 1=size 2=date 3=type */
    char    fm_search[64];
    bool    fm_search_active;
    bool    mem_auto_refresh;
    int     mem_refresh_interval;

    /* Birthday */
    bool    birthday_animation;
    uint64_t birthday_start_time;
    int     birthday_theme;

    /* Per-window scroll */
    int     hscroll;
    int     vscroll;
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
} desktop_icon_t;

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
void      gui_set_clip(int x, int y, int w, int h);
void      gui_reset_clip(void);
void      gui_request_redraw(void);
void      gui_refresh_file_managers_for_path(const char* fullpath);

/* Boot sequence integration */
bool      gui_desktop_arrived(void);  /* Check if desktop is ready */
void      gui_confirm_desktop_arrival(void);  /* Signal desktop has arrived */
void      gui_handle_input(void);
window_t* gui_open_window(int kind, const char* title, int x, int y, int w, int h);
window_t* gui_create_window(int x, int y, int w, int h, const char* title);
void      gui_close_window(int idx);
void      gui_bring_to_front(int idx);
void      gui_minimize_window(int idx);
void      gui_maximize_window(int idx);
void      gui_restore_window(int idx);
bool      gui_save_window_state_snapshot(const window_t* w);
bool      gui_load_window_state_snapshot(window_t* w);
void      gui_notify(const char* msg, int type);
int       gui_find_window(int kind);
void      gui_focus_window(int idx);

/* Cursor functions */
void      gui_set_cursor(cursor_type_t type);
cursor_type_t gui_get_cursor(void);

/* Dark mode */
void      gui_toggle_dark_mode(void);
void      gui_set_dark_mode(bool enabled);
int       gui_is_dark_mode(void);
int       gui_get_wallpaper_idx(void);
bool      gui_get_terminal_autoscroll(void);
void      gui_set_terminal_autoscroll(bool enabled);
void      gui_toggle_terminal_autoscroll(void);
int       gui_get_font_scale(void);
void      gui_set_font_scale(int scale);
int       gui_get_font_family(void);
void      gui_set_font_family(int family);
const char* gui_get_font_family_name(int family);
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
void      tinygl_viewer_draw(int idx);
void      tinygl_viewer_handle_key(int idx, const keyboard_event_t* ev);
void      tinygl_viewer_handle_click(int idx, int mx, int my);
void      games2d_draw(int idx);
void      games2d_handle_key(int idx, const keyboard_event_t* ev);
void      games2d_handle_click(int idx, int mx, int my);
int       gui_get_language_idx(void);
void      gui_set_language_idx(int idx);
const char* gui_get_language_name(int idx);
void      gui_localize_desktop_icons(void);
const char* gui_text(const char* en, const char* ja);
bool      gui_is_japanese(void);

/* Animation */
void      gui_animate_window_open(int idx);
void      gui_animate_window_close(int idx);

/* System tray */
void      gui_draw_tray(void);
int       gui_tray_hit_test(int mx, int my);

/* Wallpaper */
void      gui_set_wallpaper(int idx);
void      gui_cycle_wallpaper(void);
int       gui_get_wallpaper_count(void);
const char* gui_get_wallpaper_name(int idx);
uint64_t  gui_get_wallpaper_color_idx(int idx);
int       gui_get_theme_idx(void);
void      gui_set_theme_idx(int idx);

/* App draw functions */
void draw_file_manager(int idx);
void draw_text_editor(int idx);
void draw_terminal(int idx);
void draw_settings(int idx);
int  settings_scroll_max_for_window(int idx);
void handle_settings_key(int idx, char ascii, int key, bool ctrl);
void draw_calculator(int idx);
/* gui_apps_browser.c に実装。ファイルマネージャ等からブラウザへURLを渡す際に使用。
 * (gui_apps.c 分割前は static だったが、複数ファイルから参照されるため公開した) */
void browser_commit_navigation(window_t* w, const char* raw_url, bool add_history);
void draw_calc_graph(int idx);
void draw_sheet_app(int idx);
void handle_sheet_key(int idx, int key, char ascii, bool ctrl);
void handle_sheet_click(int idx, int mx, int my);
void draw_about(int idx);
void draw_storage_app(int idx);
void draw_browser_app(int idx);
void draw_task_manager(int idx);
void draw_music_player(int idx);
void music_player_open(const char* path);
void music_player_handle_click(int idx, int mx, int my);
void draw_paint_app(int idx);
void draw_clock_app(int idx);
void draw_sysinfo_app(int idx);
void draw_python_ide_app(int idx);

/* Input handlers */
void handle_text_editor_key(int idx, char ascii, int scancode);
void handle_terminal_key(int idx, char ascii, int scancode);
void handle_calculator_key(int idx, char ascii, int scancode, bool ctrl);
void handle_browser_key(int idx, char ascii, int scancode, bool ctrl);
void handle_browser_click(int idx, int mx, int my);
void handle_browser_wheel(int idx, int wheel_delta);
void handle_browser_right_click(int idx, int mx, int my);
void handle_paint_mouse(int idx, int mx, int my, bool clicked);
void handle_file_manager_click(int idx, int mx, int my);
void handle_file_manager_key(int idx, int key, char ascii, bool ctrl);
void handle_settings_click(int idx, int mx, int my);
void handle_python_ide_key(int idx, char ascii, int scancode);
void handle_python_ide_click(int idx, int mx, int my);


/* Clipboard */
typedef enum {
    GUI_CLIPBOARD_EMPTY = 0,
    GUI_CLIPBOARD_TEXT,
    GUI_CLIPBOARD_PATH
} gui_clipboard_kind_t;

void gui_clipboard_clear(void);
void gui_clipboard_set_text(const char* text);
const char* gui_clipboard_get_text(void);
void gui_clipboard_set_path(const char* path, bool cut);
const char* gui_clipboard_get_path(void);
bool gui_clipboard_has_path(void);
bool gui_clipboard_path_is_cut(void);

/* Desktop icon sync functions */
int  gui_add_desktop_icon(const char* path, const char* name, int is_file);
void gui_remove_desktop_icon(const char* path);
bool gui_create_desktop_app_icon(int win_kind, int x, int y);
bool gui_delete_desktop_icon_at(int idx);
void gui_relayout_desktop_icons(void);
void gui_reset_desktop_icons(void);
void gui_save_desktop_layout(void);
void gui_refresh_desktop_icons(void);
void gui_sync_desktop_with_fs(void);
void gui_init_desktop_icons(void);
void gui_clamp_desktop_icon_position(int* x, int* y);
void gui_snap_desktop_icon_position(int* x, int* y, int skip_index);
int gui_get_desktop_icon_render_size(void);

/* Desktop icon sizing */
uint32_t gui_get_desktop_icon_size(void);
void gui_set_desktop_icon_size(uint32_t size);

/* FIX: draw_window_frame made non-static for gui_apps.c */
void draw_window_frame(int idx);
void draw_window_frame_public(int idx);

/* gui_apps.c - Additional application functions */
void draw_memory_manager_app(int idx);
void draw_birthday_app(int idx);
void handle_calculator_click(int idx, int mx, int my);
void gui_notify_simple(const char* msg);
minimal_mouse_t* _get_mouse(void);

/* Python IDE functions */
void python_ide_init(void);
void python_ide_set_geometry(int x, int y, int w, int h);
void python_ide_draw(void);
void http_downloader_init(window_t* w);
void http_downloader_draw(int idx);
void http_downloader_handle_click(int idx, int mx, int my);
void http_downloader_handle_key(int idx, char ascii, int scancode, bool ctrl);
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
int  gui_utf8_char_count(const char* s, int byte_len);

#endif /* COS_INCLUDE_GUI_H */
