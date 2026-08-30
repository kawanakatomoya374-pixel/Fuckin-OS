/**
 * Enhanced GUI System - C-OS 5.0.0
 * Features: Advanced window management, input handling, rendering
 * 
 * Fixed boot sequence to ensure desktop reaches properly
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
#include "cos_netsurf_browser.h"
#include "cos_js_os_api.h"
#include "cos_js_scheduler.h"
#include "e1000.h"
#include "mk_mp3.h"

/* Modern UI backend (preferred renderer) */
void modern_ui_init(void);
void modern_ui_update(void);
extern void gui_process_pending_desktop_layout_load(void);
#include <shell.h>
#include <string.h>
#include <stdio.h>

#ifndef COS_BROWSER_FILE_SMOKE
#define COS_BROWSER_FILE_SMOKE 0
#endif

extern void gui_snap_desktop_icon_position(int* x, int* y, int skip_index);
extern void gui_reset_desktop_icons(void);

// GUI should not directly depend on kernel internals
// Use proper kernel API instead

// String function declarations for freestanding environment
char *strncpy(char *dest, const char *src, size_t n);
void *memset(void *s, int c, size_t n);
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcat(char *dest, const char *src);
void *memcpy(void *dest, const void *src, size_t n);

// Memory allocation should use kernel allocator
void* kmalloc(size_t size);
void kfree(void* ptr);

// Memory statistics
uint64_t kmemory_used(void);
uint64_t kmemory_free(void);


// VGA function declarations
void vga_set_pixel(int x, int y, uint64_t color);
void vga_draw_string(int x, int y, const char* s, uint64_t fg, uint64_t bg);

// App rendering hooks
void draw_music_player(int idx);
void draw_jpeg_viewer(int idx);

extern uint64_t cos_power_get_battery_percent(void);
extern uint64_t cos_power_get_estimated_minutes_remaining(void);
extern bool cos_power_is_charging(void);
extern const char* acpi_power_get_source_label(void);
extern bool usb_is_initialized(void) __attribute__((weak));

static bool gui_initialized = false;
bool gui_persist_enabled = true;
static bool gui_desktop_arrived_flag = false;

/* Boot sequence tracking */
bool gui_desktop_arrived(void) {
    return gui_desktop_arrived_flag || gui_boot_animation_completed();
}

void gui_confirm_desktop_arrival(void) {
    if (!gui_desktop_arrived_flag) {
        serial_puts("[GUI] Desktop arrival confirmed\n");
        gui_desktop_arrived_flag = true;
        gui_request_redraw();
    }
}

// Keyboard function declarations
bool keyboard_has_event(void);
keyboard_event_t keyboard_get_event(void);
char keyboard_get_ascii(void);

void gui_refresh_file_managers_for_path(const char* fullpath);

gui_system_t gui_sys = {0};
static uint64_t gui_last_frame_tick = 0;
static const uint64_t GUI_IDLE_REDRAW_TICKS = 16; /* ~16ms = ~60FPS at 1000Hz */

extern bool gui_load_window_state_snapshot(window_t* w);
extern bool gui_save_window_state_snapshot(const window_t* w);

void gui_request_redraw(void) {
    gui_sys.needs_redraw = true;
}

void keyboard_poll(void);

bool gui_usb_recognized(void) {
    if (!usb_is_initialized) return false;
    return usb_is_initialized();
}

void gui_draw_usb_taskbar_icon(int x, int y, uint64_t color) {
    /* Compact USB trident-style badge */
    vga_fill_rect(x + 6, y + 2, 4, 10, color);
    vga_fill_rect(x + 4, y + 6, 8, 2, color);
    vga_fill_rect(x + 2, y + 5, 3, 4, color);
    vga_fill_rect(x + 11, y + 5, 3, 4, color);
    vga_fill_rect(x + 5, y + 0, 2, 2, color);
    vga_fill_rect(x + 9, y + 0, 2, 2, color);
    vga_fill_rect(x + 5, y + 12, 2, 2, color);
}

bool gui_create_desktop_item(bool is_dir) {
    char name[FS_MAX_NAME];
    if (is_dir) {
        gui_make_unique_desktop_name("New Folder", "", name, sizeof(name));
        if (name[0] && fs_create_dir_at("/desktop", name)) {
            gui_notify_simple("Folder created");
            gui_sync_desktop_with_fs();
            gui_refresh_file_managers_for_path("/desktop");
            gui_sys.needs_redraw = true;
            return true;
        }
    } else {
        gui_make_unique_desktop_name("new_file", ".txt", name, sizeof(name));
        if (name[0] && fs_create_file_at("/desktop", name)) {
            gui_notify_simple("File created");
            gui_sync_desktop_with_fs();
            gui_refresh_file_managers_for_path("/desktop");
            gui_sys.needs_redraw = true;
            return true;
        }
    }
    return false;
}
void gui_refresh_file_managers_for_path(const char* fullpath) {
    if (!fullpath || !fullpath[0]) return;

    char parent[256];
    char leaf[256];
    gui_split_path(fullpath, parent, sizeof(parent), leaf, sizeof(leaf));
    (void)leaf;
    if (parent[0] == '\0') strcpy(parent, "/");

    for (int i = 0; i < window_count; i++) {
        window_t* w = &windows[i];
        if (!w->visible || w->kind != WIN_FILE_MGR) continue;
        const char* current = (w->fm_path[0] ? w->fm_path : "/");
        if (strcmp(current, parent) == 0 || strcmp(current, "/desktop") == 0) {
            w->fm_scroll = 0;
            w->fm_selected = 0;
        }
    }
    gui_request_redraw();
}

// Screen dimensions are supplied by vga.h at runtime.
// Enhanced keyboard constants
#ifndef KEY_BACKSPACE
#define KEY_BACKSPACE 0x0E
#endif
#ifndef KEY_ENTER
#define KEY_ENTER 0x1C
#endif
#ifndef KEY_UP
#define KEY_UP 0x48
#endif
#ifndef KEY_DOWN
#define KEY_DOWN 0x50
#endif
#ifndef KEY_LEFT
#define KEY_LEFT 0x4B
#endif
#ifndef KEY_RIGHT
#define KEY_RIGHT 0x4D
#endif
#ifndef KEY_DELETE
#define KEY_DELETE 0x53
#endif
#ifndef KEY_HOME
#define KEY_HOME 0x47
#endif
#ifndef KEY_END
#define KEY_END 0x4F
#endif
#ifndef KEY_ESC
#define KEY_ESC 0x01
#endif
#ifndef KEY_TAB
#define KEY_TAB 0x0F
#endif
#ifndef KEY_SPACE
#define KEY_SPACE 0x39
#endif
#ifndef KEY_LSHIFT
#define KEY_LSHIFT 0x2A
#endif
#ifndef KEY_RSHIFT
#define KEY_RSHIFT 0x36
#endif
#ifndef KEY_LCTRL
#define KEY_LCTRL 0x1D
#endif
#ifndef KEY_RCTRL
#define KEY_RCTRL 0x9D
#endif
#ifndef KEY_LALT
#define KEY_LALT 0x38
#endif
#ifndef KEY_RALT
#define KEY_RALT 0xB8
#endif
#ifndef KEY_CAPSLOCK
#define KEY_CAPSLOCK 0x3A
#endif
#ifndef KEY_NUMLOCK
#define KEY_NUMLOCK 0x45
#endif

// Global mouse state is owned by the input driver and declared in mouse.h.

// Enhanced GUI constants
// MAX_WINDOWS already defined in gui.h as 16
#define WINDOW_MIN_WIDTH 100
#define WINDOW_MIN_HEIGHT 80
#define WINDOW_TITLE_HEIGHT 24
#define WINDOW_BORDER_WIDTH 2
#define DESKTOP_BG_COLOR 0x1E3A8A
#define WINDOW_BG_COLOR 0xF3F4F6
#define WINDOW_BORDER_COLOR 0x374151
#define WINDOW_TITLE_COLOR 0x111827
#define WINDOW_TEXT_COLOR 0x000000


// Global window state
extern window_t windows[MAX_WINDOWS];
extern int window_count;
extern int active_window;
static uint64_t frame_counter = 0;
uint64_t gui_frame_counter = 0;

// Enhanced GUI helper functions
static void gui_draw_rect(int x, int y, int width, int height, uint64_t color) {
    gui_render_fill_rect(x, y, width, height, color);
}

static void gui_draw_text(int x, int y, const char* text, uint64_t color) {
    gui_render_draw_text(x, y, text ? text : "", color, 0xFFFFFFFF);
}

static void __attribute__((unused)) gui_draw_window_frame(window_t* win) {
    if (!win || !win->visible) return;
    
    // Draw window background
    gui_draw_rect(win->x, win->y, win->w, win->h, WINDOW_BG_COLOR);
    
    // Draw window border
    gui_draw_rect(win->x, win->y, win->w, WINDOW_BORDER_WIDTH, WINDOW_BORDER_COLOR);
    gui_draw_rect(win->x, win->y + win->h - WINDOW_BORDER_WIDTH, win->w, WINDOW_BORDER_WIDTH, WINDOW_BORDER_COLOR);
    gui_draw_rect(win->x, win->y, WINDOW_BORDER_WIDTH, win->h, WINDOW_BORDER_COLOR);
    gui_draw_rect(win->x + win->w - WINDOW_BORDER_WIDTH, win->y, WINDOW_BORDER_WIDTH, win->h, WINDOW_BORDER_COLOR);
    
    // Draw title bar
    if (win->title[0] != '\0') {
        gui_draw_rect(win->x + WINDOW_BORDER_WIDTH, win->y + WINDOW_BORDER_WIDTH, 
                     win->w - 2 * WINDOW_BORDER_WIDTH, WINDOW_TITLE_HEIGHT, WINDOW_TITLE_COLOR);
        gui_draw_text(win->x + WINDOW_BORDER_WIDTH + 4, win->y + WINDOW_BORDER_WIDTH + 4, 
                     win->title, WINDOW_TEXT_COLOR);
    }
}

// Simplified gui_bring_to_top function using window array
static void gui_bring_to_top(window_t* win) {
    if (!win) return;
    int idx = (int)(win - windows);
    if (idx >= 0 && idx < window_count) {
        gui_bring_to_front(idx);
    } else {
        gui_localize_desktop_icons();
        gui_sys.needs_redraw = true;
    }
}

// Duplicate gui_create_window function removed - use gui_open_window instead

// Duplicate gui_close_window function removed - use gui_close_window from header

void gui_set_window_position(window_t* win, int x, int y) {
    if (!win) return;
    
    win->x = x;
    win->y = y;
    gui_localize_desktop_icons();
    gui_sys.needs_redraw = true;
}

void gui_set_window_size(window_t* win, int width, int height) {
    if (!win) return;
    
    win->w = (width < WINDOW_MIN_WIDTH) ? WINDOW_MIN_WIDTH : width;
    win->h = (height < WINDOW_MIN_HEIGHT) ? WINDOW_MIN_HEIGHT : height;
    gui_localize_desktop_icons();
    gui_sys.needs_redraw = true;
}

void gui_set_window_title(window_t* win, const char* title) {
    if (!win || !title) return;
    
    strncpy(win->title, title, sizeof(win->title) - 1);
    win->title[sizeof(win->title) - 1] = '\0';
    gui_localize_desktop_icons();
    gui_sys.needs_redraw = true;
}

void gui_show_window(window_t* win) {
    if (!win) return;
    
    win->visible = true;
    gui_bring_to_top(win);
}

void gui_hide_window(window_t* win) {
    if (!win) return;
    
    win->visible = false;
    gui_localize_desktop_icons();
    gui_sys.needs_redraw = true;
}

void gui_focus_window(int idx) {
    if (idx < 0 || idx >= window_count) return;

    for (int i = 0; i < window_count; ++i) {
        windows[i].focused = false;
    }

    active_window = idx;
    windows[idx].focused = true;
    gui_bring_to_front(idx);
}

// Enhanced input handling
void gui_handle_mouse_event(int x, int y, uint8_t buttons) {
    gui_sys.mouse_x = x;
    gui_sys.mouse_y = y;
    gui_sys.mouse_buttons = buttons;
    gui_sys.mouse_moved = true;
    
    // Find window under mouse - simplified implementation
    // This will be updated to use window array instead of linked list
    for (int i = window_count - 1; i >= 0; i--) {
        if (windows[i].visible &&
            x >= windows[i].x && x < windows[i].x + windows[i].w &&
            y >= windows[i].y && y < windows[i].y + windows[i].h) {

            // Focus window on click
            if (buttons & 0x01) {  // Left click
                gui_bring_to_front(i);
            }
            break;
        }
    }
    
    gui_localize_desktop_icons();
    gui_sys.needs_redraw = true;
}

void gui_handle_key_event(uint8_t key, bool pressed) {
    // Simplified key event handling
    
    // Handle special keys
    if (pressed) {
        switch (key) {
            case KEY_ESC:
                // Close focused window
                if (active_window >= 0) {
                    gui_close_window(active_window);
                }
                break;
            case KEY_TAB:
                // Switch to next window
                if (window_count > 1 && active_window >= 0) {
                    int next = (active_window + 1) % window_count;
                    gui_bring_to_front(next);
                }
                break;
            case KEY_ENTER:
                // Keep Enter for focused widgets and terminal input instead of
                // converting it into a synthetic mouse click.
                break;
        }
    }
}

// Enhanced rendering system
/* gui_render_desktop removed: use gui_draw() for full-featured rendering */

void gui_update(void) {
    /* PS/2 keyboard/mouse and the timer-driven key-repeat path update their
     * queues in IRQ context. Do not touch the controller from every GUI frame:
     * the UI consumes the already-published state below. */

    // Route IRQ-published input through the modern UI event handler.
    gui_handle_input();

    /* Keep MP3 decoding and AC97 DMA fed from the owner context. The backend
     * performs bounded work and never blocks the GUI on a full audio ring. */
    mk_mp3_update();

    /* HTTP/TLS and NetSurf conversion are cooperative.  They must continue
     * independently of full scene composition: an idle BitBlt-only frame
     * otherwise leaves a just-clicked HTTPS link permanently at "loading".
     * The frontend invalidation callback requests the next full repaint only
     * when new page pixels are ready. */
    cos_netsurf_browser_poll();
    /* Fetch/XHR completion is owner-thread only. Keep it after NetSurf's
     * transport poll so page DOM mutations are serialized with browser
     * content updates, then let QuickJS run its Promise jobs below. */
    cos_js_pump_web_requests();
    /* Web timers share the GUI/NetSurf owner boundary.  This bounded pump is
     * deliberately outside IRQ/AP work and runs even when no full redraw is
     * otherwise due, so delayed page initialization remains responsive. */
    cos_js_pump_timers();
    /* Fetch/XHR completion resolves promises above; execute a bounded
     * microtask batch before composition so `.then()` DOM mutations become
     * visible without waiting for an unrelated input event. */
    cos_js_pump_pending_jobs();

#if COS_BROWSER_FILE_SMOKE
    /* Validation-only launch point.  This runs after the scheduler has entered
     * the ordinary GUI owner loop, avoiding re-entrant NetSurf startup during
     * the early desktop bootstrap paint. */
    static bool file_smoke_browser_started = false;
    if (!file_smoke_browser_started) {
        file_smoke_browser_started = true;
        serial_puts("[GUI] validation: opening file:// Browser from owner loop\n");
        (void)gui_open_window(WIN_BROWSER, "NetSurf file:// self-test", 90, 70, 820, 600);
    }
#endif

    static int last_present_mouse_x = -1;
    static int last_present_mouse_y = -1;
    static uint8_t last_present_mouse_buttons = 0xFF;

    int current_mouse_x = (int)mouse.x;
    int current_mouse_y = (int)mouse.y;
    uint8_t current_mouse_buttons = (mouse.left ? 0x01 : 0) |
                                    (mouse.right ? 0x02 : 0) |
                                    (mouse.middle ? 0x04 : 0);
    bool mouse_moved = (current_mouse_x != last_present_mouse_x) ||
                       (current_mouse_y != last_present_mouse_y) ||
                       (current_mouse_buttons != last_present_mouse_buttons);
    if (mouse_moved) {
        gui_request_redraw();
    }

    uint64_t now = get_timer_ticks();
    /* Persist settings only after the short debounce window. This runs before
     * the idle-frame early return, but never from the input callback that
     * changed the setting, avoiding ATA/FAT latency in the click path. */
    gui_flush_settings_save();
    /* Desktop mutations need full scene composition.  A static desktop does
     * not: retain a 60 Hz FPS-overlay cadence by copying only its rectangle
     * through BitBlt instead of redrawing three complete windows. */
    bool full_redraw = gui_sys.needs_redraw || gui_last_frame_tick == 0 ||
                       mouse_moved || mouse.wheel != 0 ||
                       mouse.left_click || mouse.right_click;
    bool overlay_due = gui_get_fps_overlay() &&
                       (now - gui_last_frame_tick) >= GUI_IDLE_REDRAW_TICKS;
    if (!full_redraw && !overlay_due) {
        /* Click flags remain pending until the frame that presents them. */
        return;
    }

    gui_last_frame_tick = now;
    frame_counter++;
    gui_frame_counter++;

    gui_sys.mouse_x = current_mouse_x;
    gui_sys.mouse_y = current_mouse_y;
    gui_sys.mouse_buttons = current_mouse_buttons;
    gui_sys.mouse_moved = mouse_moved;
    gui_sys.needs_redraw = false;

    /* Handle mouse wheel scrolling */
    if (mouse.wheel != 0) {
        int target = active_window;
        /* If context menu is visible, it takes priority (though usually doesn't scroll) */
        if (ctx_menu.visible) {
            /* Optional: handle context menu scroll if it's very long */
        } else if (target >= 0 && target < window_count) {
            window_t* w = &windows[target];
            if (w->visible && !w->minimized) {
                /* Scroll amount (invert PS/2 wheel direction for natural feel) */
                int scroll_amt = (int)(-mouse.wheel * 3);
                if (w->kind == WIN_TERMINAL) {
                    w->term_scroll += (int)(-mouse.wheel);
                    int max_s = gui_terminal_max_scroll(w);
                    if (w->term_scroll < 0) w->term_scroll = 0;
                    if (w->term_scroll > max_s) w->term_scroll = max_s;
                } else if (w->kind == WIN_FILE_MGR) {
                    w->fm_scroll += scroll_amt * 8;
                    if (w->fm_scroll < 0) w->fm_scroll = 0;
                } else if (w->kind == WIN_TEXT_EDITOR) {
                    w->scroll_y += scroll_amt * 12;
                    if (w->scroll_y < 0) w->scroll_y = 0;
                } else if (w->kind == WIN_PYTHON_IDE) {
                    /* The IDE keeps its own scroll_y inside its internal
                     * state, not in window_t, since (unlike the text editor)
                     * it isn't a window_t-backed app. Writing to
                     * w->scroll_y here used to silently do nothing. */
                    python_ide_scroll(scroll_amt / 3);
                } else if (w->kind == WIN_BROWSER) {
                    handle_browser_wheel(target, mouse.wheel);
                } else if (w->kind == WIN_SETTINGS) {
                    int max_scroll = settings_scroll_max_for_window(target);
                    w->settings_scroll += scroll_amt * 10;
                    if (w->settings_scroll < 0) w->settings_scroll = 0;
                    if (w->settings_scroll > max_scroll) w->settings_scroll = max_scroll;
                } else {
                    w->vscroll += scroll_amt * 10;
                    if (w->vscroll < 0) w->vscroll = 0;
                }
                gui_request_redraw();
            }
        }
        mouse.wheel = 0; /* Consume the wheel event */
    }

    if (full_redraw) {
        modern_ui_update();
        gui_draw();
        /* The privileged JavaScript overlay is composited only after the
         * normal desktop scene. It uses the same backbuffer primitive and is
         * never invoked from an IRQ/AP worker or from JS evaluation itself. */
        (void)cos_js_os_draw_overlay();
    }

    /* Optional on-screen FPS counter, toggled from Settings.
     * Counts frames actually presented (i.e. ones that passed the
     * "due" redraw-skip check above) within a rolling 1-second window,
     * so it reflects real presented frame rate rather than raw loop
     * iterations. Drawn to the backbuffer before vga_flip() so it
     * shows up like any other on-screen element. */
    if (gui_get_fps_overlay()) {
        static uint64_t s_fps_count = 0;
        static uint64_t s_fps_window_start = 0;
        static uint64_t s_fps_last_value = 0;

        s_fps_count++;
        if (s_fps_window_start == 0) {
            s_fps_window_start = now;
        } else if (now - s_fps_window_start >= 1000) {
            s_fps_last_value = s_fps_count;
            s_fps_count = 0;
            s_fps_window_start = now;
        }

        char fps_label[16];
        char fps_num[8];
        gui_format_int((int)s_fps_last_value, fps_num, sizeof(fps_num));
        fps_label[0] = 'F'; fps_label[1] = 'P'; fps_label[2] = 'S'; fps_label[3] = ':'; fps_label[4] = ' ';
        int fi = 5;
        for (int ci = 0; fps_num[ci] != '\0' && fi < (int)sizeof(fps_label) - 1; ci++, fi++) {
            fps_label[fi] = fps_num[ci];
        }
        fps_label[fi] = '\0';

        int fps_x = (int)SCREEN_W - 90;
        int fps_y = 6;
        vga_fill_rect(fps_x - 6, fps_y - 4, 86, 20, 0x00000000);
        vga_draw_string(fps_x, fps_y, fps_label, 0x0000FF66, 0x00000000);
    }

    if (full_redraw) {
        vga_flip();
    } else if (gui_get_fps_overlay()) {
        /* Only the FPS label changed on this idle frame. */
        vga_flip_rect((int)SCREEN_W - 96, 0, 96, 28);
    }

    // Remember what we actually presented so the next pass can skip redraws
    // when nothing has changed.
    last_present_mouse_x = current_mouse_x;
    last_present_mouse_y = current_mouse_y;
    last_present_mouse_buttons = current_mouse_buttons;

    // Clear one-shot click flags after the frame has been presented.
    // Only clear if they are currently set (to avoid clearing on every frame)
    if (mouse.left_click || mouse.right_click) {
        mouse.left_click = FALSE;
        mouse.right_click = FALSE;
        minimal_mouse_clear_clicks();
    }
    mouse.wheel = 0;
    /* Ensure wheel is also cleared in the driver's internal state */
    mouse_get_state()->wheel = 0;
    mouse.wheel = 0;

    last_present_mouse_x = current_mouse_x;
    last_present_mouse_y = current_mouse_y;
    last_present_mouse_buttons = current_mouse_buttons;
}


// GUI system initialization
void gui_init(void) {
    serial_puts("[GUI] Initializing enhanced GUI system\n");

    /* Initialize new subsystems */
    extern int notification_center_init(void);
    notification_center_init();
    theme_system_init();

    extern void ecm_init(void);
    ecm_init();

    /* Initialize JPEG viewer (allocate display buffer and decoder buffer) */
    extern int jpeg_viewer_init(void);
    if (jpeg_viewer_init() != 0) {
        serial_puts("[GUI] Warning: JPEG viewer initialization failed\n");
    }

    memset(&gui_sys, 0, sizeof(gui_system_t));
    gui_persist_enabled = false;

    gui_set_wallpaper(gui_get_wallpaper_count() - 1);
    gui_set_theme_idx(1);
    /* Deferred until after the first desktop frame to keep boot responsive. */
    vga_set_font_scale(gui_get_font_scale());

    /* Restores whatever was actually saved last session (dark mode,
     * font scale, wallpaper, mouse sensitivity, autostart apps, ...)
     * over the hardcoded defaults just set above. The settings screen
     * (gui_apps_settings.c) already saves every change correctly via
     * config_set_string()+config_save_all() - this was the missing
     * other half: nothing ever called this to read it back, so every
     * boot silently reverted to defaults regardless of what had been
     * saved. */
    gui_load_settings_snapshot();
    theme_set_mode(gui_is_dark_mode() ? THEME_MODE_DARK : THEME_MODE_LIGHT);

    /* Initialize the real MP3/AC97 route before the first interactive frame.
     * File loading remains lazy, so boot does not scan the whole filesystem. */
    mk_mp3_init();

    gui_persist_enabled = true;

    // Clear any stale input state left over from boot or earlier screens.
    keyboard_flush();
    minimal_mouse_clear_clicks();
    gui_sys.needs_redraw = true;

    // VGA already initialized in kernel.c
    gui_render_init();

    gui_sys.mouse_x = (int)mouse.x;
    gui_sys.mouse_y = (int)mouse.y;
    gui_sys.mouse_buttons = (mouse.left ? 0x01 : 0) |
                             (mouse.right ? 0x02 : 0) |
                             (mouse.middle ? 0x04 : 0);

    gui_init_desktop_icons();
    /* Clear any stale UI state from earlier screens so the first desktop
     * frame is guaranteed to be unobscured. */
    window_count = 0;
    active_window = -1;
    memset(&ctx_menu, 0, sizeof(ctx_menu));
    memset(&submenu, 0, sizeof(submenu));

    // Bring up the modern UI backend alongside the legacy state so the new
    // renderer becomes the visible front-end immediately.
    modern_ui_init();

    gui_initialized = true;

    /* Scheduler/tasking and the first GUI owner context have already been
     * established by kernel.c. Arm INTx before optional ACPICA battery work
     * inside the first desktop draw, so that a slow power probe cannot hide
     * IRQ11 bring-up or delay network receive availability. */
    e1000_enable_runtime_irq();

    /* FIXED: Signal desktop arrival after GUI init */
    gui_confirm_desktop_arrival();

    // Ensure the desktop is visible before entering the main loop.
    gui_request_redraw();
    gui_draw();
    vga_flip();

    serial_puts("[GUI] GUI system initialized\n");
    serial_puts("[GUI] Screen resolution: ");
    serial_putdec(SCREEN_W);
    serial_puts("x");
    serial_putdec(SCREEN_H);
    serial_puts("\n");
    serial_puts("[GUI] Desktop should now be visible\n");
    return;
}

void gui_shutdown(void) {
    // Persist desktop layout before shutdown so icon positions survive reboots.
    gui_snapshot_save_desktop();

    // Simplified shutdown function
    gui_initialized = false;
    serial_puts("[GUI] GUI system shutdown complete\n");
}

// GUI utility functions
uint64_t gui_get_window_count(void) {
    return window_count;
}

void gui_set_desktop_color(uint64_t color) {
    gui_sys.desktop_bg_color = color;
    gui_localize_desktop_icons();
    gui_sys.needs_redraw = true;
}

bool gui_is_initialized(void) {
    return gui_initialized;
}



// Clip region state is owned by the VGA backend
extern int gui_clip_x, gui_clip_y, gui_clip_w, gui_clip_h;
extern bool gui_clip_enabled;

// Mouse state variables for rich GUI
minimal_mouse_t mouse_state = {0};
int mouse_sensitivity = 1;
bool mouse_raw_input = false;

#define C_BG_DESKTOP   rgb(25,  35,  55)
#define C_BG_WINDOW    rgb(245, 248, 252)
#define C_CONTENT_BG   rgb(250, 252, 255)
#define C_TITLEBAR     rgb(45,  85,  145)
#define C_TITLEBAR_HOV rgb(55,  100, 170)
#define C_TITLEBAR_INV rgb(120, 130, 150)
#define C_BTN_CLOSE    rgb(200, 70,  60)
#define C_BTN_MIN      rgb(220, 175, 35)
#define C_BTN_MAX      rgb(45,  175, 70)
#define C_TEXT         rgb(40,  45,  60)
#define C_TEXT_LIGHT   rgb(235, 240, 250)
#define C_TASKBAR      rgb(30,  40,  60)
#define C_TASKBAR_BTN  rgb(40,  55,  80)
#define C_TASKBAR_BHOV rgb(55,  75,  110)
#define C_BORDER       rgb(160, 175, 200)
#define C_ACCENT       rgb(50,  100, 180)
#define C_ACCENT2      rgb(80,  140, 220)
#define C_HOVER        rgb(200, 215, 235)
#define C_SELECTED     rgb(50,  100, 180)
#define C_CTX_BG       rgb(245, 248, 252)
#define C_CTX_BORDER   rgb(160, 175, 200)
#define C_CTX_HOVER    rgb(50,  100, 180)
#define C_CTX_TEXT     rgb(40,  45,  60)
#define C_ICON_BG      rgb(240, 245, 250)
#define C_SCROLLBAR    rgb(170, 185, 210)
#define C_SCROLLTHUMB  rgb(110, 130, 170)
#define C_INPUT_BG     rgb(255, 255, 255)
#define C_INPUT_BORDER rgb(150, 165, 190)
#define C_INPUT_FOCUS  rgb(50,  100, 180)
#define C_TERM_BG      rgb(22,  28,  40)
#define C_TERM_TEXT    rgb(190, 235, 190)
#define C_TERM_PROMPT  rgb(120, 190, 255)
#define C_WALLPAPER1   rgb(25,  35,  55)
#define C_WALLPAPER2   rgb(35,  50,  80)

window_t       windows[MAX_WINDOWS];
int            window_count = 0;
context_menu_t ctx_menu = {0};
submenu_t submenu = {0};
desktop_icon_t desktop_icons[MAX_ICONS];
int            desktop_icon_count = 0;

int            active_window = -1;
bool    dragging = FALSE, resizing = FALSE;
bool    drag_candidate = FALSE;
int     drag_candidate_window = -1;
int     drag_anchor_x, drag_anchor_y;
int     drag_off_x, drag_off_y;
bool    icon_dragging = FALSE;
int     icon_drag_candidate = -1;
int     icon_drag_anchor_x, icon_drag_anchor_y;
int     icon_drag_off_x, icon_drag_off_y;
int     last_click_time = 0;
int     last_click_icon = -1;

bool    resize_candidate = FALSE;
int     resize_candidate_window = -1;
int     resize_edge = 0;
int     resize_anchor_x, resize_anchor_y;
int     resize_start_w, resize_start_h;

void gui_cancel_pointer_actions(void) {
    dragging = FALSE;
    resizing = FALSE;
    drag_candidate = FALSE;
    drag_candidate_window = -1;
    icon_dragging = FALSE;
    icon_drag_candidate = -1;
    resize_candidate = FALSE;
    resize_candidate_window = -1;
}
