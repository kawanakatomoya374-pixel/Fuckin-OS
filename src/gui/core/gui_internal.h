/**
 * gui_internal.h - GUIコア: 内部共有ヘッダ
 *
 * gui.c (5,588行) を機能単位で分割した際に、複数の .c ファイルから参照される
 * 内部専用の関数・変数・型の宣言をまとめたもの。
 * 公開 API (GUI外部のアプリ/カーネルから呼ばれるもの) は gui.h を見ること。
 *
 * ファイル分割マップ:
 *   gui_lifecycle.c       - 初期化/終了、ウィンドウ基本操作、入力ディスパッチ、メインループ
 *   gui_settings.c        - 設定 (言語/テーマ/壁紙/自動起動/マウス) の取得・保存・復元
 *   gui_desktop_render.c  - 壁紙描画、デスクトップアイコン描画
 *   gui_taskbar_frame.c   - タスクバー描画
 *   gui_menus.c           - コンテキストメニュー・サブメニュー・スタートメニュー
 *   gui_input.c           - キーボード/ターミナル/テキストエディタ/IME 入力処理
 *   gui_render_loop.c     - メイン描画ループ、ウィンドウ生成/破棄、通知パネル
 *
 * 分割前は同一ファイル内の static だったため、複数ファイルにまたがって
 * 呼び出されるものだけを非staticにしてここで宣言している。
 * （他モジュールとの同名衝突を避けるため draw_notifications() は
 *  gui_draw_notifications_panel() に、draw_context_menu() は
 *  gui_draw_ctx_menu_panel() にリネーム済み）
 */
#ifndef GUI_INTERNAL_H
#define GUI_INTERNAL_H

#include "gui.h"
#include <stdint.h>
#include <stdbool.h>
#include "sync.h"

/* Shared rendering/runtime guards */
extern bool vga_has_framebuffer(void);

static inline bool gui_has_framebuffer(void) {
    return vga_has_framebuffer();
}

static inline uint64_t gui_state_lock(void) {
    return sync_irq_save();
}

static inline void gui_state_unlock(uint64_t flags) {
    sync_irq_restore(flags);
}

/* ============================================================
 * 内部共有型
 * ============================================================ */

/* GUIシステム状態 (簡易) */
typedef struct {
    uint64_t desktop_bg_color;
    int mouse_x, mouse_y;
    uint8_t mouse_buttons;
    bool mouse_moved;
    bool needs_redraw;
} gui_system_t;

/* 壁紙プリセット */
typedef struct {
    const char* name;
    uint64_t top;
    uint64_t bottom;
    uint64_t accent;
    bool is_jpeg;
    const char* jpeg_path;
} wallpaper_preset_t;

/* 設定マネージャへの弱依存 (未リンクでもGUI単体でビルド可能にするため) */
extern const char* config_get_string(const char* key) __attribute__((weak));
extern int config_set_string(const char* key, const char* value) __attribute__((weak));
extern int config_save_all(void) __attribute__((weak));

/* キーコード (複数ファイルで参照するため共有定義) */
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
#ifndef KEY_DELETE
#define KEY_DELETE   0x53
#endif

/* デスクトップ/ファイル関連の外部依存 (複数ファイルで参照) */
extern const char* fs_read_file_at(const char* path, const char* name);
extern uint32_t settings_get_desktop_icon_size(void) __attribute__((weak));
extern bool settings_set_desktop_icon_size(uint32_t size) __attribute__((weak));
extern void gui_snapshot_save_desktop(void);
extern void gui_clamp_desktop_icon_position(int* x, int* y);
extern void gui_normalize_desktop_icons(void);

/* ============================================================
 * 内部共有変数 (元々 static、複数ファイルにまたがるため公開)
 * 実体は各分割ファイルの元の定義位置にそのまま残している。
 * ============================================================ */

/* gui_lifecycle.c */
extern bool gui_persist_enabled;
extern gui_system_t gui_sys;
extern bool dragging, resizing;
extern bool drag_candidate;
extern int  drag_candidate_window;
extern int  drag_anchor_x, drag_anchor_y;
extern int  drag_off_x, drag_off_y;

/* Edge/corner resize, mirroring the drag_candidate pattern above:
 * armed on mouse-down over a grip, promoted to an active resize once
 * the pointer moves past gui_mouse_drag_threshold. resize_edge is a
 * bitmask, 1=right edge moving, 2=bottom edge moving (both set for
 * the bottom-right corner grip). resize_start_w/h is the window size
 * at the moment the grip was pressed, so the new size is always
 * computed from that fixed reference rather than accumulating
 * per-frame drift. */
extern bool resize_candidate;
extern int  resize_candidate_window;
extern int  resize_edge;
extern int  resize_anchor_x, resize_anchor_y;
extern int  resize_start_w, resize_start_h;
extern bool icon_dragging;
extern int  icon_drag_candidate;
extern int  icon_drag_anchor_x, icon_drag_anchor_y;
extern int  icon_drag_off_x, icon_drag_off_y;
extern int  last_click_time;
extern int  last_click_icon;

/* gui_settings.c */
extern const wallpaper_preset_t wallpaper_presets[];
extern int  current_wallpaper;
extern bool gui_wallpaper_image_loaded;
extern bool gui_terminal_autoscroll;
extern int  gui_mouse_drag_threshold;
extern const char* gui_language_label_english;
extern const char* gui_language_label_japanese;
extern int  mouse_sensitivity;      /* gui_lifecycle.c */
extern bool mouse_raw_input;        /* gui_lifecycle.c */

/* vga.c - shared clip rect used by gui_set_clip()/gui_reset_clip()
 * (gui_render_loop.c) and read during drawing from other gui/core
 * files; was previously only visible inside vga.c itself. */
extern int  gui_clip_x, gui_clip_y, gui_clip_w, gui_clip_h;
extern bool gui_clip_enabled;

/* gui_render_loop.c - which window currently has focus. Also
 * declared in gui_apps_common.h for the apps tree; declared here too
 * since not every gui/core source file pulls that header in. */
extern int active_window;

/* Weakly-linked image viewer hook - lets the desktop/wallpaper code
 * draw a loaded JPEG without gui/core hard-depending on the image
 * viewer app. */
extern int image_viewer_draw_scaled(uint64_t x, uint64_t y, uint64_t width, uint64_t height) __attribute__((weak));

/* Context-menu submenu identifiers (gui_menus.c). Previously used
 * throughout gui_menus.c without ever being defined anywhere, which
 * only "worked" as long as every user of ctx_add_item_ex()/submenu
 * lived in the same not-yet-split gui.c translation unit. */
typedef enum {
    SUBMENU_KIND_NONE = 0,
    SUBMENU_KIND_LANGUAGE,
    SUBMENU_KIND_OPEN,
    SUBMENU_KIND_SETTINGS,
    SUBMENU_KIND_POWER,
    SUBMENU_KIND_WINDOW,
    SUBMENU_KIND_NEW,
    SUBMENU_KIND_AUDIO,
    SUBMENU_KIND_WALLPAPER,
    SUBMENU_KIND_DESKTOP_ICON_SIZE,
    SUBMENU_KIND_APP_ICON,
} submenu_kind_t;

/* Shared desktop/window chrome colors (gui_lifecycle.c originally
 * #define'd these for its own use only; gui_desktop_render.c,
 * gui_taskbar_frame.c and gui_menus.c need the same palette). Each
 * #define is guarded so a file that already has its own local copy
 * (e.g. gui_windows.c) doesn't get a redefinition warning. */
#ifndef C_TITLEBAR
#define C_TITLEBAR     rgb(45,  85,  145)
#endif
#ifndef C_TITLEBAR_HOV
#define C_TITLEBAR_HOV rgb(55,  100, 170)
#endif
#ifndef C_TEXT_LIGHT
#define C_TEXT_LIGHT   rgb(235, 240, 250)
#endif
#ifndef C_TASKBAR
#define C_TASKBAR      rgb(30,  40,  60)
#endif
#ifndef C_TASKBAR_BTN
#define C_TASKBAR_BTN  rgb(40,  55,  80)
#endif
#ifndef C_TASKBAR_BHOV
#define C_TASKBAR_BHOV rgb(55,  75,  110)
#endif
#ifndef C_BORDER
#define C_BORDER       rgb(160, 175, 200)
#endif
#ifndef C_ACCENT
#define C_ACCENT       rgb(50,  100, 180)
#endif
#ifndef C_ACCENT2
#define C_ACCENT2      rgb(80,  140, 220)
#endif

/* ============================================================
 * 内部共有関数 (元々 static、複数ファイルにまたがるため公開)
 * ============================================================ */

/* gui_lifecycle.c */
bool gui_usb_recognized(void);
void gui_draw_usb_taskbar_icon(int x, int y, uint64_t color);
bool gui_create_desktop_item(bool is_dir);
void gui_cancel_pointer_actions(void);
void draw_jpeg_viewer(int idx);

/* gui_settings.c */
void gui_reflow_desktop_icons_for_size_change(int old_size, int new_size);
const char* gui_ctx_label(const char* en, const char* ja);
void gui_switch_language_with_loading(int target_idx);
void gui_save_settings_snapshot(void);
void gui_load_settings_snapshot(void);
void gui_schedule_settings_save(void);
void gui_flush_settings_save(void);
void gui_fit_text_to_width(const char* src, char* dst, size_t dst_size, int max_px);
void gui_reload_image_wallpaper(void);
void gui_play_test_tone(void);
void gui_launch_autostart_apps(void);
void gui_process_pending_desktop_layout_load(void);

/* gui_desktop_render.c */
void draw_wallpaper(void);
void gui_draw_ctx_icon_art(int icon_id, int x, int y, bool hov);
/* Draws one app icon badge + glyph for the given WIN_* kind at
 * (x,y), sized to a `size`-pixel square. Defined in
 * gui_desktop_render.c; shared by desktop icons, the taskbar's open
 * -window buttons, and the taskbar dock so every app icon in the OS
 * comes from the same art. */
void gui_draw_app_icon(int kind, int x, int y, int size, bool hov);
/* Window state transitions - defined in gui_render_loop.c. Previously
 * called from several files (gui_menus.c, gui_apps_common.c, ...)
 * with no header declaration anywhere, relying on implicit
 * declaration; declared properly here since gui_taskbar_frame.c now
 * calls them too. */
void gui_minimize_window(int idx);
void gui_maximize_window(int idx);
void gui_restore_window(int idx);
void draw_desktop_icons(void);

/* gui_taskbar_frame.c */
void draw_taskbar(void);

/* gui_menus.c */
void gui_draw_ctx_menu_panel(void);
void submenu_close(void);
void start_menu_clear_search(void);
void handle_start_menu_keyboard(void);
void open_taskbar_context_menu(int x, int y);
void open_start_menu(int x, int y);
void open_desktop_context_menu(int x, int y, int target_icon);
void open_window_context_menu(int x, int y, int win_idx);
void handle_context_menu_click(int x, int y);

/* gui_input.c */
bool gui_pop_keyboard_event(keyboard_event_t* ev);
int  gui_terminal_max_scroll(window_t* w);

/* gui_render_loop.c */
void gui_draw_notifications_panel(void);
void poll_mouse(void); /* 公開関数だが gui.h に未宣言のためここで補完 */

#endif /* GUI_INTERNAL_H */
