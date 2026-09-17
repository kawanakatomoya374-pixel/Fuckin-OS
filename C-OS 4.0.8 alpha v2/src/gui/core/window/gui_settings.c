/**
 * gui_settings.c - GUIコア (言語/テーマ/壁紙/自動起動/マウス設定の取得・保存・復元)
 * gui.c (5,588行) から分割生成。詳細は gui_internal.h を参照。
 */

#include "gui.h"

#ifndef COS_BROWSER_FILE_SMOKE
#define COS_BROWSER_FILE_SMOKE 0
#endif
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

extern int gui_clip_x, gui_clip_y, gui_clip_w, gui_clip_h;
extern bool gui_clip_enabled;

static void gui_copy_str(char* dst, size_t dst_size, const char* src);

/* ---- Wallpaper presets ---- (wallpaper_preset_t は gui_internal.h で定義) */
const wallpaper_preset_t wallpaper_presets[] = {
    { "Aurora",   0x192337, 0x0A0C1C, 0x5A96FF, false, NULL },
    { "Twilight", 0x2D3C5A, 0x12162A, 0x8264DC, false, NULL },
    { "Deep Blue",0x1E2D46, 0x081026, 0x4678D2, false, NULL },
    { "Violet",   0x323246, 0x16102D, 0xBE78E6, false, NULL },
    { "Forest",   0x233228, 0x0C1C18, 0x5AB478, false, NULL },
    { "Featured",  0x1C2230, 0x081018, 0x4F7FD6, true,  "/desktop/featured.png" },
};

int current_wallpaper = 0;
static char gui_wallpaper_image_path[FS_MAX_PATH];
bool gui_wallpaper_image_loaded = false;
static int current_theme_idx = 1;
bool gui_terminal_autoscroll = true;
static bool gui_window_animations = true;
static bool gui_fps_overlay_enabled = false;
static bool gui_notifications_enabled = true;
static bool gui_autostart_terminal = false;
static bool gui_autostart_file_manager = false;
/* GUI Google verification build: auto-open the browser at boot. */
static bool gui_autostart_browser = true;
static int gui_font_scale = 1;
static int gui_font_family = 0;
int gui_mouse_drag_threshold = 3;
int gui_dark_mode = 0;
static int current_language_idx = 0; /* 0=English, 1=Japanese */

/* Keyboard-operated secondary pointer. The pending click is consumed once by
 * gui_handle_input(), so it reuses the normal left/right click dispatcher. */
static bool gui_multi_cursor_enabled = false;
static int gui_multi_cursor_x = 0;
static int gui_multi_cursor_y = 0;
static bool gui_multi_cursor_position_initialized = false;
/* Keyboard clicks can arrive faster than a GUI frame. Keep a compact FIFO
 * so an Alt double-click remains two distinct desktop clicks instead of
 * being collapsed into one boolean pending flag. */
#define GUI_MULTI_CURSOR_CLICK_QUEUE 8u
static bool gui_multi_cursor_click_right_queue[GUI_MULTI_CURSOR_CLICK_QUEUE];
static uint8_t gui_multi_cursor_click_head = 0;
static uint8_t gui_multi_cursor_click_tail = 0;

/* Optional configuration manager hooks.
 * Declared weak so GUI can still link when the config module is not present.
 */
extern const char* config_get_string(const char* key) __attribute__((weak));
extern int config_set_string(const char* key, const char* value) __attribute__((weak));
extern int config_save_all(void) __attribute__((weak));
void gui_localize_desktop_icons(void) __attribute__((weak));

static const char* const gui_font_family_names[6] = {
    "Anthropic Serif", "Pixel Arcade", "Gothic Sans", "Noto Sans", "Mono Terminal", "Rounded UI"
};

int gui_get_font_family(void) {
    if (gui_font_family < 0 || gui_font_family >= 6) gui_font_family = 0;
    return gui_font_family;
}

const char* gui_get_font_family_name(int family) {
    if (family < 0 || family >= 6) family = 0;
    return gui_font_family_names[family];
}

void gui_set_font_family(int family) {
    if (family < 0 || family >= 6) family = 0;
    gui_font_family = family;
    /* The current kernel raster backend remains the safe fallback. */
    if (config_set_string) {
        char buf[8];
        gui_format_int(gui_font_family, buf, sizeof(buf));
        config_set_string("gui.font_family", buf);
        gui_schedule_settings_save();
    }
    gui_sys.needs_redraw = true;
}

/* Settings interactions must never synchronously serialize the entire config
 * table and issue ATA/FAT writes from an input handler.  Coalesce a short burst
 * of toggles and let gui_update() flush it during a normal frame.  The config
 * table is still updated synchronously, so only persistence is delayed. */
#define GUI_SETTINGS_SAVE_DEBOUNCE_TICKS 750u
static bool gui_settings_save_pending = false;
static uint64_t gui_settings_save_due_tick = 0;

void gui_schedule_settings_save(void) {
    if (config_save_all == NULL) return;
    gui_settings_save_pending = true;
    gui_settings_save_due_tick = get_timer_ticks() + GUI_SETTINGS_SAVE_DEBOUNCE_TICKS;
}

void gui_flush_settings_save(void) {
    if (!gui_settings_save_pending || config_save_all == NULL) return;
    if (get_timer_ticks() < gui_settings_save_due_tick) return;
    /* Clear first: a re-entrant notification or input event can schedule the
     * next write without causing recursion or a duplicate immediate flush. */
    gui_settings_save_pending = false;
    (void)config_save_all();
}

extern int image_viewer_load_file(const char* file_path) __attribute__((weak));
extern int image_viewer_draw_scaled(uint64_t x, uint64_t y, uint64_t width, uint64_t height) __attribute__((weak));
extern bool image_viewer_is_loaded(void) __attribute__((weak));
extern const char* image_viewer_get_filename(void) __attribute__((weak));
extern void music_player_open(const char* path) __attribute__((weak));
extern void mk_audio_start_playback(uint64_t sample_rate, uint64_t channels, uint64_t bits_per_sample) __attribute__((weak));
extern void mk_audio_write_samples(const int64_t* samples, uint64_t size) __attribute__((weak));
extern void mk_audio_stop_playback(void) __attribute__((weak));

const char* gui_language_label_english = "English";
const char* gui_language_label_japanese = "日本語";

static const char* gui_get_prompt_hostname(void) {
    const char* hostname = config_get_string("system.hostname");
    return (hostname && hostname[0]) ? hostname : "cos";
}

static const char* gui_get_language_config_value(void) {
    return config_get_string ? config_get_string("gui.language") : NULL;
}

int gui_get_language_idx(void) {
    const char* lang = gui_get_language_config_value();
    int idx = lang ? gui_parse_int_or_default(lang, current_language_idx) : current_language_idx;
    if (idx != 0 && idx != 1) idx = 0;
    current_language_idx = idx;
    return current_language_idx;
}

const char* gui_get_language_name(int idx) {
    return (idx == 1) ? gui_language_label_japanese : gui_language_label_english;
}

const char* gui_text(const char* en, const char* ja) {
    return (gui_get_language_idx() == 1) ? ja : en;
}

bool gui_is_japanese(void) {
    return gui_get_language_idx() == 1;
}

int gui_is_dark_mode(void) {
    return gui_dark_mode ? 1 : 0;
}

void gui_set_dark_mode(bool enabled) {
    gui_dark_mode = enabled ? 1 : 0;
    theme_set_mode(gui_dark_mode ? THEME_MODE_DARK : THEME_MODE_LIGHT);
    if (config_set_string) {
        char buf[8];
        gui_format_int(gui_dark_mode, buf, sizeof(buf));
        config_set_string("gui.dark_mode", buf);
        gui_schedule_settings_save();
    }
    gui_sys.needs_redraw = true;
}

void gui_toggle_dark_mode(void) {
    gui_set_dark_mode(!gui_dark_mode);
    gui_request_redraw();
}

void gui_set_font_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    gui_font_scale = scale;
    vga_set_font_scale(gui_font_scale);
    if (config_set_string) {
        char buf[8];
        gui_format_int(gui_font_scale, buf, sizeof(buf));
        config_set_string("gui.font_scale", buf);

        gui_format_int(gui_font_family, buf, sizeof(buf));
        config_set_string("gui.font_family", buf);
        gui_schedule_settings_save();
    }
    gui_sys.needs_redraw = true;
}

int gui_get_font_scale(void) {
    if (gui_font_scale < 1 || gui_font_scale > 4) gui_font_scale = 1;
    return gui_font_scale;
}

void gui_set_window_animations(bool enabled) {
    gui_window_animations = enabled ? true : false;
    if (config_set_string) {
        char buf[8];
        gui_format_int(gui_window_animations ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.window_animations", buf);
        gui_schedule_settings_save();
    }
    gui_sys.needs_redraw = true;
}

bool gui_get_window_animations(void) {
    return gui_window_animations;
}

void gui_set_fps_overlay(bool enabled) {
    gui_fps_overlay_enabled = enabled ? true : false;
    if (config_set_string) {
        char buf[8];
        gui_format_int(gui_fps_overlay_enabled ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.fps_overlay", buf);
        gui_schedule_settings_save();
    }
    gui_sys.needs_redraw = true;
}

bool gui_get_fps_overlay(void) {
    return gui_fps_overlay_enabled;
}

void gui_set_notifications_enabled(bool enabled) {
    gui_notifications_enabled = enabled ? true : false;
    if (config_set_string) {
        char buf[8];
        gui_format_int(gui_notifications_enabled ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.notifications_enabled", buf);
        gui_schedule_settings_save();
    }
    gui_sys.needs_redraw = true;
}

bool gui_get_notifications_enabled(void) {
    return gui_notifications_enabled;
}

static int gui_desktop_icon_max_size(void) {
    int max_w = (int)SCREEN_W - 12;
    int max_h = (int)SCREEN_H - TASKBAR_H - 28;
    int max_size = max_w < max_h ? max_w : max_h;
    if (max_size > 104) max_size = 104;
    if (max_size < 24) max_size = 24;
    return max_size;
}

static int gui_snap_desktop_icon_size(int size) {
    const int steps[] = {56, 64, 72};
    int max_size = gui_desktop_icon_max_size();
    int best = steps[0];
    int best_diff = 1 << 30;

    for (int i = 0; i < 3; ++i) {
        int candidate = steps[i];
        if (candidate > max_size) candidate = max_size;
        if (candidate < 24) candidate = 24;
        int diff = size - candidate;
        if (diff < 0) diff = -diff;
        if (diff < best_diff) {
            best_diff = diff;
            best = candidate;
        }
    }

    if (best < 24) best = 24;
    if (best > max_size) best = max_size;
    return best;
}

static int gui_clamp_desktop_icon_size(int size) {
    return gui_snap_desktop_icon_size(size);
}

void gui_reflow_desktop_icons_for_size_change(int old_size, int new_size) {
    if (old_size <= 0 || new_size <= 0 || old_size == new_size) return;

    int old_max_x = (int)SCREEN_W - old_size - 8;
    int old_max_y = (int)SCREEN_H - TASKBAR_H - old_size - 24;
    int new_max_x = (int)SCREEN_W - new_size - 8;
    int new_max_y = (int)SCREEN_H - TASKBAR_H - new_size - 24;
    if (old_max_x < 4) old_max_x = 4;
    if (old_max_y < 4) old_max_y = 4;
    if (new_max_x < 4) new_max_x = 4;
    if (new_max_y < 4) new_max_y = 4;

    int old_range_x = old_max_x - 4;
    int old_range_y = old_max_y - 4;
    int new_range_x = new_max_x - 4;
    int new_range_y = new_max_y - 4;
    if (old_range_x < 1) old_range_x = 1;
    if (old_range_y < 1) old_range_y = 1;
    if (new_range_x < 1) new_range_x = 1;
    if (new_range_y < 1) new_range_y = 1;

    /* Preserve relative layout when the desktop icon size changes. */
    for (int i = 0; i < desktop_icon_count; ++i) {
        int x = desktop_icons[i].x;
        int y = desktop_icons[i].y;

        x = 4 + ((x - 4) * new_range_x) / old_range_x;
        y = 4 + ((y - 4) * new_range_y) / old_range_y;

        desktop_icons[i].x = x;
        desktop_icons[i].y = y;
    }

    gui_normalize_desktop_icons();
}

uint32_t gui_get_desktop_icon_size(void) {
    uint32_t size = 64;
    if (settings_get_desktop_icon_size) {
        size = settings_get_desktop_icon_size();
    }
    return (uint32_t)gui_clamp_desktop_icon_size((int)size);
}

void gui_set_desktop_icon_size(uint32_t size) {
    int old_size = (int)gui_get_desktop_icon_size();
    int clamped = gui_clamp_desktop_icon_size((int)size);

    if (clamped != old_size) {
        gui_reflow_desktop_icons_for_size_change(old_size, clamped);
    }

    if (settings_set_desktop_icon_size) {
        (void)settings_set_desktop_icon_size((uint32_t)clamped);
    }
    gui_normalize_desktop_icons();
    gui_snapshot_save_desktop();
    gui_request_redraw();
}

void gui_set_autostart_terminal(bool enabled) {
    gui_autostart_terminal = enabled ? true : false;
    gui_save_settings_snapshot();
}

void gui_set_autostart_file_manager(bool enabled) {
    gui_autostart_file_manager = enabled ? true : false;
    gui_save_settings_snapshot();
}

void gui_set_autostart_browser(bool enabled) {
    gui_autostart_browser = enabled ? true : false;
    gui_save_settings_snapshot();
}

bool gui_get_autostart_terminal(void) { return gui_autostart_terminal; }
bool gui_get_autostart_file_manager(void) { return gui_autostart_file_manager; }
bool gui_get_autostart_browser(void) { return gui_autostart_browser; }

int gui_get_mouse_sensitivity(void) { return mouse_sensitivity; }
void gui_set_mouse_sensitivity(int value) {
    if (value < 1) value = 1;
    if (value > 4) value = 4;
    mouse_sensitivity = value;
    if (gui_persist_enabled && config_set_string) {
        char buf[16];
        gui_format_int(mouse_sensitivity, buf, sizeof(buf));
        config_set_string("gui.mouse_sensitivity", buf);
        gui_schedule_settings_save();
    }
}

bool gui_get_mouse_raw_input(void) { return mouse_raw_input; }
void gui_set_mouse_raw_input(bool enabled) {
    mouse_raw_input = enabled ? true : false;
    if (gui_persist_enabled && config_set_string) {
        char buf[16];
        gui_format_int(mouse_raw_input ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.mouse_raw_input", buf);
        gui_schedule_settings_save();
    }
}

int gui_get_mouse_drag_threshold(void) { return gui_mouse_drag_threshold; }
void gui_set_mouse_drag_threshold(int value) {
    if (value < 1) value = 1;
    if (value > 10) value = 10;
    gui_mouse_drag_threshold = value;
    if (gui_persist_enabled && config_set_string) {
        char buf[16];
        gui_format_int(gui_mouse_drag_threshold, buf, sizeof(buf));
        config_set_string("gui.mouse_drag_threshold", buf);
        gui_schedule_settings_save();
    }
}

bool gui_get_multi_cursor_enabled(void) {
    return gui_multi_cursor_enabled;
}

void gui_set_multi_cursor_enabled(bool enabled) {
    gui_multi_cursor_enabled = enabled;
    gui_multi_cursor_click_head = 0;
    gui_multi_cursor_click_tail = 0;
    if (!gui_multi_cursor_position_initialized ||
        gui_multi_cursor_x < 0 || gui_multi_cursor_x >= (int)SCREEN_W ||
        gui_multi_cursor_y < 0 || gui_multi_cursor_y >= (int)SCREEN_H) {
        gui_multi_cursor_x = (int)SCREEN_W / 2;
        gui_multi_cursor_y = (int)SCREEN_H / 2;
        gui_multi_cursor_position_initialized = true;
    }
    if (gui_persist_enabled && config_set_string) {
        config_set_string("gui.multi_cursor", enabled ? "1" : "0");
        gui_schedule_settings_save();
    }
    gui_request_redraw();
}

void gui_toggle_multi_cursor_mode(void) {
    gui_set_multi_cursor_enabled(!gui_multi_cursor_enabled);
}

void gui_multi_cursor_move(int dx, int dy) {
    if (!gui_multi_cursor_enabled) return;
    gui_multi_cursor_x += dx;
    gui_multi_cursor_y += dy;
    if (gui_multi_cursor_x < 0) gui_multi_cursor_x = 0;
    if (gui_multi_cursor_y < 0) gui_multi_cursor_y = 0;
    if (gui_multi_cursor_x > (int)SCREEN_W - 12) gui_multi_cursor_x = (int)SCREEN_W - 12;
    if (gui_multi_cursor_y > (int)SCREEN_H - 18) gui_multi_cursor_y = (int)SCREEN_H - 18;
    gui_request_redraw();
}

void gui_multi_cursor_get_position(int* x, int* y) {
    if (x) *x = gui_multi_cursor_x;
    if (y) *y = gui_multi_cursor_y;
}

void gui_multi_cursor_request_click(bool right_button) {
    if (!gui_multi_cursor_enabled) return;
    uint8_t next = (uint8_t)((gui_multi_cursor_click_tail + 1u) % GUI_MULTI_CURSOR_CLICK_QUEUE);
    if (next == gui_multi_cursor_click_head) {
        /* Preserve the oldest input when a pathological key burst exceeds
         * the small queue, rather than corrupting the click order. */
        return;
    }
    gui_multi_cursor_click_right_queue[gui_multi_cursor_click_tail] = right_button;
    gui_multi_cursor_click_tail = next;
    gui_request_redraw();
}

bool gui_multi_cursor_take_click(int* x, int* y, bool* right_button) {
    if (!gui_multi_cursor_enabled || gui_multi_cursor_click_head == gui_multi_cursor_click_tail) return false;
    bool queued_right = gui_multi_cursor_click_right_queue[gui_multi_cursor_click_head];
    gui_multi_cursor_click_head = (uint8_t)((gui_multi_cursor_click_head + 1u) % GUI_MULTI_CURSOR_CLICK_QUEUE);
    if (x) *x = gui_multi_cursor_x;
    if (y) *y = gui_multi_cursor_y;
    if (right_button) *right_button = queued_right;
    return true;
}

void gui_set_language_idx(int idx) {
    if (idx != 0 && idx != 1) idx = 0;
    current_language_idx = idx;
    if (config_set_string) {
        char buf[8];
        gui_format_int(current_language_idx, buf, sizeof(buf));
        config_set_string("gui.language", buf);
        gui_schedule_settings_save();
    }
    gui_localize_desktop_icons();
    gui_sys.needs_redraw = true;
}

const char* gui_ctx_label(const char* en, const char* ja) {
    return (gui_get_language_idx() == 1) ? ja : en;
}

static void gui_show_language_load_screen(int target_idx) {
    if (target_idx != 0 && target_idx != 1) target_idx = 0;
    if (!gui_has_framebuffer()) {
        serial_puts("[GUI] Language switch without framebuffer; skipping loading screen\n");
        return;
    }

    const char* title = (target_idx == 1) ? "Load Japanese" : "Load English";
    const char* subtitle = (target_idx == 1)
        ? "Applying language pack..."
        : "Applying language pack...";

    uint64_t start = get_timer_ticks();
    uint64_t duration = 720u;

    while (1) {
        uint64_t now = get_timer_ticks();
        uint64_t elapsed = (now > start) ? (now - start) : 0u;
        if (elapsed > duration) elapsed = duration;

        int cx = (int)SCREEN_W / 2;
        int cy = (int)SCREEN_H / 2;
        int box_w = 420;
        int box_h = 190;
        int box_x = cx - box_w / 2;
        int box_y = cy - box_h / 2;

        vga_fill_rect(0, 0, (int)SCREEN_W, (int)SCREEN_H, rgb(8, 12, 24));
        vga_fill_rounded_rect(box_x + 3, box_y + 4, box_w, box_h, 14, rgb(0, 0, 0));
        vga_fill_rounded_rect(box_x, box_y, box_w, box_h, 14, rgb(18, 28, 48));
        vga_draw_rounded_rect(box_x, box_y, box_w, box_h, 14, rgb(86, 132, 204));
        vga_draw_rounded_rect(box_x + 1, box_y + 1, box_w - 2, box_h - 2, 13, rgb(188, 212, 248));

        vga_draw_string(box_x + 18, box_y + 18, title, rgb(248, 252, 255), 0xFFFFFFFF);

        int bar_x = box_x + 18;
        int bar_y = box_y + 68;
        int bar_w = box_w - 36;
        int bar_h = 20;
        int fill_w = (int)(((uint64_t)(bar_w - 4) * elapsed) / duration);
        if (fill_w < 0) fill_w = 0;
        if (fill_w > bar_w - 4) fill_w = bar_w - 4;

        vga_fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, 8, rgb(34, 44, 64));
        vga_fill_rounded_rect(bar_x + 2, bar_y + 2, fill_w, bar_h - 4, 6, rgb(84, 160, 244));

        char dots[5];
        int dot_count = (int)((elapsed / 180u) % 4u);
        for (int i = 0; i < 4; ++i) {
            dots[i] = (i < dot_count) ? '.' : ' ';
        }
        dots[4] = '\0';

        vga_draw_string(box_x + 18, box_y + 104, subtitle, rgb(178, 196, 220), 0xFFFFFFFF);
        vga_draw_string(box_x + 18, box_y + 128, "Please wait" , rgb(214, 226, 244), 0xFFFFFFFF);
        vga_draw_string(box_x + box_w - 90, box_y + 128, dots, rgb(214, 226, 244), 0xFFFFFFFF);

        vga_flip();

        if (elapsed >= duration) break;
        timer_wait(16u);
    }
}

void gui_switch_language_with_loading(int target_idx) {
    gui_show_language_load_screen(target_idx);
    gui_set_language_idx(target_idx);
    vga_set_font_resolution(1);
    gui_request_redraw();
    gui_draw();
    vga_flip();
}

void gui_save_settings_snapshot(void) {
    if (!gui_persist_enabled) return;
    if (config_set_string) {
        char buf[16];
        gui_format_int(current_theme_idx, buf, sizeof(buf));
        config_set_string("gui.theme_idx", buf);

        gui_format_int(current_wallpaper, buf, sizeof(buf));
        config_set_string("gui.wallpaper_idx", buf);
        config_set_string("gui.wallpaper_mode", wallpaper_presets[current_wallpaper].is_jpeg ? "1" : "0");
        if (wallpaper_presets[current_wallpaper].is_jpeg && gui_wallpaper_image_path[0]) {
            config_set_string("gui.wallpaper_file", gui_wallpaper_image_path);
        }

        gui_format_int(gui_dark_mode, buf, sizeof(buf));
        config_set_string("gui.dark_mode", buf);

        gui_format_int(gui_font_scale, buf, sizeof(buf));
        config_set_string("gui.font_scale", buf);

        gui_format_int(gui_font_family, buf, sizeof(buf));
        config_set_string("gui.font_family", buf);

        gui_format_int(gui_window_animations ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.window_animations", buf);

        gui_format_int(gui_fps_overlay_enabled ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.fps_overlay", buf);

        gui_format_int(gui_notifications_enabled ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.notifications_enabled", buf);

        gui_format_int(gui_terminal_autoscroll ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.terminal_autoscroll", buf);

        gui_format_int(gui_autostart_terminal ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.autostart_terminal", buf);

        gui_format_int(gui_autostart_file_manager ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.autostart_file_manager", buf);

        gui_format_int(gui_autostart_browser ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.autostart_browser", buf);

        gui_format_int(mouse_sensitivity, buf, sizeof(buf));
        config_set_string("gui.mouse_sensitivity", buf);

        gui_format_int(mouse_raw_input ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.mouse_raw_input", buf);

        gui_format_int(gui_mouse_drag_threshold, buf, sizeof(buf));
        config_set_string("gui.mouse_drag_threshold", buf);

        gui_format_int(gui_multi_cursor_enabled ? 1 : 0, buf, sizeof(buf));
        config_set_string("gui.multi_cursor", buf);

        gui_format_int(gui_get_language_idx(), buf, sizeof(buf));
        config_set_string("gui.language", buf);

        gui_schedule_settings_save();
    }
}


void gui_load_settings_snapshot(void) {
    if (!config_get_string) return;

    const char* theme = config_get_string("gui.theme_idx");
    const char* wallpaper = config_get_string("gui.wallpaper_idx");
    const char* wallpaper_file = config_get_string("gui.wallpaper_file");
    const char* wallpaper_mode = config_get_string("gui.wallpaper_mode");
    const char* dark = config_get_string("gui.dark_mode");
    const char* font_scale = config_get_string("gui.font_scale");
    const char* font_family = config_get_string("gui.font_family");
    const char* animations = config_get_string("gui.window_animations");
    const char* notifications = config_get_string("gui.notifications_enabled");
    const char* auto_scroll = config_get_string("gui.terminal_autoscroll");
    const char* autostart_terminal = config_get_string("gui.autostart_terminal");
    const char* autostart_fm = config_get_string("gui.autostart_file_manager");
    const char* autostart_browser = config_get_string("gui.autostart_browser");
    const char* mouse_sens = config_get_string("gui.mouse_sensitivity");
    const char* mouse_raw = config_get_string("gui.mouse_raw_input");
    const char* mouse_drag = config_get_string("gui.mouse_drag_threshold");
    const char* multi_cursor = config_get_string("gui.multi_cursor");
    const char* language = config_get_string("gui.language");
    const char* fps_overlay = config_get_string("gui.fps_overlay");

    int theme_idx = gui_parse_int_or_default(theme, current_theme_idx);
    int wallpaper_idx = gui_parse_int_or_default(wallpaper, current_wallpaper);
    int wallpaper_mode_idx = gui_parse_int_or_default(wallpaper_mode, 0);
    int dark_mode = gui_parse_int_or_default(dark, gui_dark_mode);
    int font_scale_idx = gui_parse_int_or_default(font_scale, gui_font_scale);
    int font_family_idx = gui_parse_int_or_default(font_family, gui_font_family);
    int animations_enabled = gui_parse_int_or_default(animations, 1);
    int notifications_enabled = gui_parse_int_or_default(notifications, 1);
    int terminal_autoscroll = gui_parse_int_or_default(auto_scroll, 1);
    int autostart_terminal_enabled = gui_parse_int_or_default(autostart_terminal, 0);
    int autostart_fm_enabled = gui_parse_int_or_default(autostart_fm, 0);
    int autostart_browser_enabled = gui_parse_int_or_default(autostart_browser, 0);
    int mouse_sensitivity_value = gui_parse_int_or_default(mouse_sens, 1);
    int mouse_raw_value = gui_parse_int_or_default(mouse_raw, 0);
    int mouse_drag_value = gui_parse_int_or_default(mouse_drag, 3);
    int multi_cursor_enabled = gui_parse_int_or_default(multi_cursor, 0);
    int language_idx = gui_parse_int_or_default(language, current_language_idx);
    int fps_overlay_enabled = gui_parse_int_or_default(fps_overlay, 0);

    if (theme_idx < 1 || theme_idx > 4) theme_idx = 1;
    if (wallpaper_idx < 0 || wallpaper_idx >= gui_get_wallpaper_count()) wallpaper_idx = 0;
    if (wallpaper_mode_idx != 0 && wallpaper_mode_idx != 1) wallpaper_mode_idx = 0;
    if (wallpaper_mode_idx == 1 && !wallpaper_presets[wallpaper_idx].is_jpeg) {
        wallpaper_idx = gui_get_wallpaper_count() - 1;
    }
    if (dark_mode != 0 && dark_mode != 1) dark_mode = 0;
    if (font_scale_idx < 1 || font_scale_idx > 4) font_scale_idx = 1;
    if (font_family_idx < 0 || font_family_idx >= 6) font_family_idx = 0;
    if (animations_enabled != 0 && animations_enabled != 1) animations_enabled = 1;
    if (notifications_enabled != 0 && notifications_enabled != 1) notifications_enabled = 1;
    if (terminal_autoscroll != 0 && terminal_autoscroll != 1) terminal_autoscroll = 1;
    if (autostart_terminal_enabled != 0 && autostart_terminal_enabled != 1) autostart_terminal_enabled = 0;
    if (autostart_fm_enabled != 0 && autostart_fm_enabled != 1) autostart_fm_enabled = 0;
    if (autostart_browser_enabled != 0 && autostart_browser_enabled != 1) autostart_browser_enabled = 0;
    if (mouse_sensitivity_value < 1 || mouse_sensitivity_value > 4) mouse_sensitivity_value = 1;
    if (mouse_raw_value != 0 && mouse_raw_value != 1) mouse_raw_value = 0;
    if (mouse_drag_value < 1 || mouse_drag_value > 10) mouse_drag_value = 3;
    if (multi_cursor_enabled != 0 && multi_cursor_enabled != 1) multi_cursor_enabled = 0;
    if (language_idx != 0 && language_idx != 1) language_idx = 0;
    if (fps_overlay_enabled != 0 && fps_overlay_enabled != 1) fps_overlay_enabled = 0;

    current_theme_idx = theme_idx;
    current_wallpaper = wallpaper_idx;
    if (wallpaper_file && wallpaper_file[0]) {
        gui_copy_str(gui_wallpaper_image_path, sizeof(gui_wallpaper_image_path), wallpaper_file);
    } else {
        gui_copy_str(gui_wallpaper_image_path, sizeof(gui_wallpaper_image_path), "/desktop/featured.png");
    }
    gui_wallpaper_image_loaded = (wallpaper_mode_idx == 1 && wallpaper_presets[current_wallpaper].is_jpeg);
    gui_dark_mode = dark_mode;
    gui_font_scale = font_scale_idx;
    gui_font_family = font_family_idx;
    gui_window_animations = animations_enabled ? true : false;
    gui_notifications_enabled = notifications_enabled ? true : false;
    gui_terminal_autoscroll = terminal_autoscroll ? true : false;
    gui_autostart_terminal = autostart_terminal_enabled ? true : false;
    gui_autostart_file_manager = autostart_fm_enabled ? true : false;
#if COS_BROWSER_FILE_SMOKE
    /* Validation-only: open the Browser through the normal GUI owner-thread
     * lifecycle so the storage-backed file: document exercises the real
     * NetSurf/QuickJS callback context. This define is never enabled for a
     * distribution image. */
    gui_autostart_browser = true;
#else
    /* Distribution UX starts at the desktop/app selector. Do not resurrect a
     * Browser window from an older validation disk's persisted preference. */
    (void)autostart_browser_enabled;
    gui_autostart_browser = false;
#endif
    gui_fps_overlay_enabled = fps_overlay_enabled ? true : false;
    mouse_sensitivity = mouse_sensitivity_value;
    mouse_raw_input = mouse_raw_value ? true : false;
    gui_mouse_drag_threshold = mouse_drag_value;
    gui_multi_cursor_enabled = multi_cursor_enabled ? true : false;
    gui_multi_cursor_click_head = 0;
    gui_multi_cursor_click_tail = 0;
    gui_multi_cursor_x = (int)SCREEN_W / 2;
    gui_multi_cursor_y = (int)SCREEN_H / 2;
    current_language_idx = language_idx;
    gui_sys.desktop_bg_color = wallpaper_presets[current_wallpaper].top;
    vga_set_font_scale(gui_font_scale);
    vga_set_font_resolution(1);
}

int gui_get_wallpaper_count(void) {
    return (int)(sizeof(wallpaper_presets) / sizeof(wallpaper_presets[0]));
}

const char* gui_get_wallpaper_name(int idx) {
    static const char* const japanese_names[] = {
        "オーロラ", "黄昏", "深い青", "紫", "森", "おすすめ"
    };
    int count = gui_get_wallpaper_count();
    if (idx < 0 || idx >= count) return gui_text("Unknown", "不明");
    if (gui_is_japanese() && idx < (int)(sizeof(japanese_names) / sizeof(japanese_names[0]))) {
        return japanese_names[idx];
    }
    return wallpaper_presets[idx].name;
}

uint64_t gui_get_wallpaper_color_idx(int idx) {
    int count = gui_get_wallpaper_count();
    if (idx < 0 || idx >= count) idx = 0;
    return wallpaper_presets[idx].top;
}

void gui_set_wallpaper(int idx) {
    int count = gui_get_wallpaper_count();
    if (count <= 0) return;
    if (idx < 0 || idx >= count) idx = 0;
    current_wallpaper = idx;
    gui_sys.desktop_bg_color = wallpaper_presets[current_wallpaper].top;
    gui_reload_image_wallpaper();
    gui_sys.needs_redraw = true;
    gui_save_settings_snapshot();
}

void gui_cycle_wallpaper(void) {
    int count = gui_get_wallpaper_count();
    if (count <= 0) return;
    gui_set_wallpaper((current_wallpaper + 1) % count);
}

uint64_t gui_get_wallpaper_color(void) {
    return gui_get_wallpaper_color_idx(current_wallpaper);
}

int gui_get_theme_idx(void) {
    if (current_theme_idx < 1 || current_theme_idx > 4) current_theme_idx = 1;
    return current_theme_idx;
}

void gui_set_theme_idx(int idx) {
    if (idx < 1 || idx > 4) idx = 1;
    current_theme_idx = idx;
    gui_sys.needs_redraw = true;
    gui_save_settings_snapshot();
}

int gui_get_wallpaper_idx(void) {
    return current_wallpaper;
}

static void gui_copy_str(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = 0;
}

void gui_fit_text_to_width(const char* src, char* dst, size_t dst_size, int max_px) {
    if (!dst || dst_size == 0) return;
    dst[0] = 0;
    if (!src) return;

    int max_chars = max_px / FONT_W;
    if (max_chars < 0) max_chars = 0;

    size_t len = strlen(src);
    if ((int)len <= max_chars || max_chars == 0) {
        gui_copy_str(dst, dst_size, src);
        return;
    }

    if (max_chars < 3) {
        size_t copy = (size_t)((max_chars < 0) ? 0 : max_chars);
        if (copy >= dst_size) copy = dst_size - 1;
        memcpy(dst, src, copy);
        dst[copy] = 0;
        return;
    }

    size_t keep = (size_t)(max_chars - 3);
    if (keep >= dst_size) keep = dst_size - 4;
    memcpy(dst, src, keep);
    memcpy(dst + keep, "...", 3);
    dst[keep + 3] = 0;
}

static const char* gui_get_wallpaper_image_path(void) {
    const char* path = config_get_string ? config_get_string("gui.wallpaper_file") : NULL;
    if (path && path[0]) return path;
    return gui_wallpaper_image_path[0] ? gui_wallpaper_image_path : "/desktop/featured.png";
}

void gui_reload_image_wallpaper(void) {
    extern bool gui_wallpaper_decode_own_buffer(const char* path);
    const wallpaper_preset_t* wp = &wallpaper_presets[current_wallpaper];
    if (!wp->is_jpeg) {
        gui_wallpaper_image_loaded = false;
        return;
    }
    const char* path = gui_get_wallpaper_image_path();
    if (!path || !path[0]) {
        gui_wallpaper_image_loaded = false;
        return;
    }
    if (strcmp(gui_wallpaper_image_path, path) != 0 || !gui_wallpaper_image_loaded) {
        if (gui_wallpaper_decode_own_buffer(path)) {
            gui_copy_str(gui_wallpaper_image_path, sizeof(gui_wallpaper_image_path), path);
            gui_wallpaper_image_loaded = true;
        } else {
            gui_wallpaper_image_loaded = false;
        }
    }
}

void gui_play_test_tone(void) {
    if (!mk_audio_start_playback || !mk_audio_write_samples || !mk_audio_stop_playback) {
        gui_notify_simple(gui_text("Audio backend unavailable", "音声バックエンドがありません"));
        return;
    }

    mk_audio_start_playback(22050, 1, 16);
    int64_t buf[256];
    for (int block = 0; block < 24; ++block) {
        for (int i = 0; i < 256; ++i) {
            int phase = (block * 256 + i) % 48;
            buf[i] = (phase < 24) ? 14000 : -14000;
        }
        mk_audio_write_samples(buf, 256);
    }
    mk_audio_stop_playback();
    gui_notify_simple(gui_text("Test tone played", "テスト音を再生しました"));
}

bool gui_get_terminal_autoscroll(void) {
    return gui_terminal_autoscroll;
}

void gui_set_terminal_autoscroll(bool enabled) {
    gui_terminal_autoscroll = enabled ? true : false;
    gui_sys.needs_redraw = true;
    gui_save_settings_snapshot();
}

void gui_toggle_terminal_autoscroll(void) {
    gui_set_terminal_autoscroll(!gui_terminal_autoscroll);
}

void gui_animate_window_open(int idx) {
    if (!gui_window_animations) return;
    (void)idx;
    gui_sys.needs_redraw = true;
}

void gui_animate_window_close(int idx) {
    if (!gui_window_animations) return;
    (void)idx;
    gui_sys.needs_redraw = true;
}

void gui_launch_autostart_apps(void) {
    if (gui_autostart_terminal) {
        gui_open_window(WIN_TERMINAL, gui_text("Terminal", "ターミナル"), 120, 120, 840, 560);
    }
    if (gui_autostart_file_manager) {
        gui_open_window(WIN_FILE_MGR, gui_text("File Manager", "ファイルマネージャー"), 150, 140, 980, 640);
    }
    if (gui_autostart_browser) {
        gui_open_window(WIN_BROWSER, gui_text("NetSurf 3.11", "NetSurf 3.11"), 180, 160, 1040, 700);
    }
}

int mk_desktop_set_theme(uint64_t theme_id) {
    int idx = (int)theme_id;
    gui_set_theme_idx(idx);
    return 0;
}

