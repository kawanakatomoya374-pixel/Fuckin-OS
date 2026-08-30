/**
 * gui_render_loop.c - GUIコア (メイン描画ループ・ウィンドウ生成/破棄・通知パネル)
 * gui.c (5,588行) から分割生成。詳細は gui_internal.h を参照。
 */

#include "gui.h"
#include "gui_internal.h"
#include "voxel_games_advanced.h"
#include "vga.h"
#include "gfx_blit.h"
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

#ifndef COS_BROWSER_FILE_SMOKE
#define COS_BROWSER_FILE_SMOKE 0
#endif

static void draw_cursor_sprite(int cx, int cy, uint32_t outline_color,
                               uint32_t fill_color, uint32_t shadow_color) {
    static const uint8_t shape[19][12] = {
        {1,1,0,0,0,0,0,0,0,0,0,0},{1,2,1,0,0,0,0,0,0,0,0,0},{1,2,2,1,0,0,0,0,0,0,0,0},{1,2,2,2,1,0,0,0,0,0,0,0},
        {1,2,2,2,2,1,0,0,0,0,0,0},{1,2,2,2,2,2,1,0,0,0,0,0},{1,2,2,2,2,2,2,1,0,0,0,0},{1,2,2,2,2,2,2,2,1,0,0,0},
        {1,2,2,2,2,2,2,2,2,1,0,0},{1,2,2,2,2,2,2,2,2,2,1,0},{1,2,2,2,2,2,1,1,1,1,1,1},{1,2,2,2,2,2,1,0,0,0,0,0},
        {1,2,2,1,2,2,2,1,0,0,0,0},{1,2,1,0,1,2,2,2,1,0,0,0},{1,1,0,0,1,2,2,2,1,0,0,0},{0,0,0,0,0,1,2,2,2,1,0,0},
        {0,0,0,0,0,1,2,2,2,1,0,0},{0,0,0,0,0,0,1,1,1,1,0,0}
    };
    if (!backbuffer) return;

    /* Build the shadow and main cursor as two tiny 12x18 sprite
     * surfaces (one pass over `shape`, instead of two full nested
     * pixel loops each calling vga_put_pixel with its own branching),
     * then composite each with a single colorkey blit. TRANSPARENT is
     * a sentinel that can never collide with a real cursor color -
     * rgb() only ever produces values <= 0x00FFFFFF. */
    const uint32_t TRANSPARENT = 0xFFFFFFFFu;
    uint32_t shadow_px[18 * 12];
    uint32_t main_px[18 * 12];
    for (int y = 0; y < 18; y++) {
        for (int x = 0; x < 12; x++) {
            uint8_t s = shape[y][x];
            shadow_px[y * 12 + x] = (s == 1 || s == 2) ? shadow_color : TRANSPARENT;
            main_px[y * 12 + x]   = (s == 1) ? outline_color : ((s == 2) ? fill_color : TRANSPARENT);
        }
    }

    gfx_surface_t dst = gfx_surface_make(backbuffer, (int)SCREEN_W, (int)SCREEN_H, (int)SCREEN_W);
    gfx_surface_t shadow_surf = gfx_surface_make(shadow_px, 12, 18, 12);
    gfx_surface_t main_surf   = gfx_surface_make(main_px, 12, 18, 12);

    gfx_blit(&dst, cx + 1, cy + 1, &shadow_surf, 0, 0, 12, 18, GFX_BLIT_COLORKEY, TRANSPARENT);
    gfx_blit(&dst, cx, cy, &main_surf, 0, 0, 12, 18, GFX_BLIT_COLORKEY, TRANSPARENT);
}

static void draw_cursor(void) {
    draw_cursor_sprite(mouse.x, mouse.y, (uint32_t)rgb(0, 0, 0),
                       (uint32_t)rgb(255, 255, 255), (uint32_t)rgb(30, 30, 30));
}

static void draw_multi_cursor(void) {
    int x = 0, y = 0;
    if (!gui_get_multi_cursor_enabled()) return;
    gui_multi_cursor_get_position(&x, &y);
    draw_cursor_sprite(x, y, (uint32_t)rgb(0, 72, 24),
                       (uint32_t)rgb(48, 255, 112), (uint32_t)rgb(0, 28, 10));
}

void gui_draw(void) {
    if (!gui_has_framebuffer()) {
        gui_sys.needs_redraw = false;
        return;
    }

    /* Ensure desktop is drawn after boot animation completes */
    static bool desktop_drawn_once = false;

    /* Check if boot animation is done */
    bool boot_done = gui_boot_animation_completed();

    /* On first draw after boot animation, ensure everything is visible */
    if (boot_done && !desktop_drawn_once) {
        serial_puts("[GUI] Boot complete - drawing desktop\n");
        /* Force redraw of all elements */
        gui_sys.needs_redraw = true;
        desktop_drawn_once = true;
    }
    
    draw_wallpaper();
    draw_desktop_icons();
    for (int i = 0; i < window_count; i++) {
        window_t* w = &windows[i]; if (!w->visible || w->minimized) continue;
        draw_window_frame(i); gui_set_clip(w->x, w->y, w->w, w->h);
        if (w->kind == WIN_FILE_MGR) draw_file_manager(i);
        else if (w->kind == WIN_TEXT_EDITOR) draw_text_editor(i);
        else if (w->kind == WIN_TERMINAL) draw_terminal(i);
        else if (w->kind == WIN_SETTINGS) draw_settings(i);
        else if (w->kind == WIN_CALC) draw_calculator(i);
        else if (w->kind == WIN_CALC_GRAPH) draw_calc_graph(i);
        else if (w->kind == WIN_SHEET) draw_sheet_app(i);
        else if (w->kind == WIN_BROWSER)    draw_browser_app(i);
        else if (w->kind == WIN_HTTP_DOWNLOADER) http_downloader_draw(i);
        else if (w->kind == WIN_PAINT)      draw_paint_app(i);
        else if (w->kind == WIN_TASK_MGR)   draw_task_manager(i);
        else if (w->kind == WIN_CLOCK)      draw_clock_app(i);
        else if (w->kind == WIN_SYSINFO)    draw_sysinfo_app(i);
        else if (w->kind == WIN_STORAGE)    draw_storage_app(i);
        else if (w->kind == WIN_MEMORY_MGR) draw_memory_manager_app(i);
        else if (w->kind == WIN_ABOUT)      draw_about(i);
        else if (w->kind == WIN_MUSIC)      draw_music_player(i);
        else if (w->kind == WIN_JPEG)       draw_jpeg_viewer(i);
        else if (w->kind == WIN_VOXEL_GAME)  voxel_games_draw(i);
        else if (w->kind == WIN_TINYGL_VIEWER) tinygl_viewer_draw(i);
        else if (w->kind == WIN_2DGAMES) games2d_draw(i);
        else if (w->kind == WIN_PYTHON_IDE) { python_ide_set_geometry(w->x, w->y, w->w, w->h); python_ide_draw(); }
        gui_reset_clip();
    }
    gui_draw_notifications_panel();
    draw_taskbar();
    gui_draw_ctx_menu_panel();
    draw_cursor();
    draw_multi_cursor();
}

void gui_set_clip(int x, int y, int w, int h) { gui_clip_x = x; gui_clip_y = y; gui_clip_w = w; gui_clip_h = h; gui_clip_enabled = TRUE; }
void gui_reset_clip(void) { gui_clip_enabled = FALSE; }

// Poll PS/2 mouse directly (fallback when interrupts don't work)
static uint8_t poll_mouse_cycle = 0;
static uint8_t poll_mouse_packet[3];

void poll_mouse(void) {
    // Drain a few packets per call so the cursor keeps up with bursty input.
    for (int iter = 0; iter < 6; iter++) {
        uint8_t status = inb(0x64);
        if (!(status & 0x01)) return;  // No data available
        if (!(status & 0x20)) {
            // Leave keyboard data for the keyboard driver.
            return;
        }

        uint8_t data = inb(0x60);

        // Parse 3-byte mouse packet
        poll_mouse_packet[poll_mouse_cycle] = data;
        poll_mouse_cycle++;

        if (poll_mouse_cycle == 3) {
            poll_mouse_cycle = 0;

            uint8_t b0 = poll_mouse_packet[0];
            int8_t b1 = (int8_t)poll_mouse_packet[1];
            int8_t b2 = (int8_t)poll_mouse_packet[2];

            // Check for valid packet (bit 3 of first byte should be 1)
            if (!(b0 & 0x08)) continue;

            // Update mouse position using configurable sensitivity
            extern int mouse_sensitivity;
            int move_x = b1 * mouse_sensitivity / 2;
            int move_y = b2 * mouse_sensitivity / 2;
            mouse.x += move_x;
            mouse.y -= move_y;  // Y is inverted in PS/2

            // Bounds checking
            if (mouse.x < 0) mouse.x = 0;
            if (mouse.x >= (int64_t)SCREEN_W) mouse.x = SCREEN_W - 1;
            if (mouse.y < 0) mouse.y = 0;
            if (mouse.y >= (int64_t)SCREEN_H) mouse.y = SCREEN_H - 1;

            // Update button states
            bool left = (b0 & 0x01) != 0;
            bool right = (b0 & 0x02) != 0;
            bool middle = (b0 & 0x04) != 0;

            mouse.left_click |= left && !mouse.left;
            mouse.right_click |= right && !mouse.right;
            mouse.left_release |= !left && mouse.left;
            mouse.right_release |= !right && mouse.right;
            mouse.left = left;
            mouse.right = right;
            mouse.middle = middle;
        }
    }
}

// Duplicate gui_update function removed - implementation exists in first gui_update function
// void gui_update(void) {
//     // Process mouse input and update GUI state
//     gui_handle_input();
//     
//     gui_draw(); 
//     
//     // Flip to display - shows complete frame at once (no flicker!)
//     vga_flip();
//     
//     // Clear transient mouse click states after processing
//     mouse.left_click = mouse.right_click = FALSE;
//     
//     // Also clear clicks in the minimal mouse driver to prevent stuck menus
//     minimal_mouse_clear_clicks();
//     
//     // Small delay to limit frame rate (about 60fps for smooth display)
//     for (volatile int i = 0; i < 2500; i++);
// }

window_t* gui_open_window(int kind, const char* title, int x, int y, int w, int h) {
    if (kind == WIN_PYTHON_IDE) {
        int existing = gui_find_window(WIN_PYTHON_IDE);
        if (existing >= 0) {
            gui_bring_to_front(existing);
            return &windows[existing];
        }
    }
    if (window_count >= MAX_WINDOWS) return NULL;

    gui_clamp_window_geometry(&x, &y, &w, &h);

    window_t* win = &windows[window_count++];
    memset(win, 0, sizeof(*win));

    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->kind = kind;
    win->visible = TRUE;
    win->focused = TRUE;
    win->minimized = FALSE;
    win->maximized = FALSE;

    if (title) {
        strncpy(win->title, title, 63);
        win->title[63] = '\0';
    } else {
        win->title[0] = '\0';
    }
    win->text_buf[0] = 0;
    win->term_line_count = 0;
    win->scroll_y = 0;

    if (kind == WIN_TEXT_EDITOR) {
        strcpy(win->text_buf, gui_text("Welcome to C-OS 4.0.8 alpha Text Editor!\nStart typing here...\n",
                                      "C-OS 4.0.8 alpha テキストエディターへようこそ。\nここから入力を開始できます。\n"));
    }
    if (kind == WIN_SETTINGS) {
        win->settings_tab = 1;
        win->settings_theme_idx = gui_get_theme_idx();
        win->wallpaper_idx = current_wallpaper;
        win->dark_mode = FALSE;
        win->settings_scroll = 0;
        win->settings_search[0] = 0;
        win->settings_search_active = FALSE;
    }
    if (kind == WIN_TERMINAL) {
        strncpy(win->term_cwd, "/", sizeof(win->term_cwd) - 1);
        win->term_cwd[sizeof(win->term_cwd) - 1] = '\0';
        strncpy(win->term_lines[0], "C-OS 4.0.8 alpha Terminal v1.0 - Type 'help' for commands", 127);
        win->term_lines[0][127] = '\0';
        win->term_line_count = 1;
        win->term_hist_count = 0;
        win->term_hist_pos = -1;
    }
    if (kind == WIN_CALC || kind == WIN_CALC_GRAPH) {
        memset(win->calc_display, 0, sizeof(win->calc_display));
        memset(win->calc_expr, 0, sizeof(win->calc_expr));
        memset(win->calc_steps, 0, sizeof(win->calc_steps));
        memset(win->calc_status, 0, sizeof(win->calc_status));
        win->calc_display[0] = '0';
        win->calc_display[1] = 0;
        win->calc_mode = (kind == WIN_CALC_GRAPH) ? 2 : 0;
        win->calc_topic_idx = 0;
        win->calc_angle_deg = TRUE;
        win->calc_initialized = FALSE;
        win->calc_clear_next = TRUE;
    }
    if (kind == WIN_SHEET) {
        memset(win->sheet_cells, 0, sizeof(win->sheet_cells));
        win->sheet_sel_row = 0;
        win->sheet_sel_col = 0;
        win->sheet_scroll_row = 0;
        win->sheet_scroll_col = 0;
        win->sheet_editing = FALSE;
        win->sheet_edit_buf[0] = 0;
        win->sheet_status[0] = 0;
        win->sheet_initialized = TRUE;
    }
    if (kind == WIN_PYTHON_IDE) {
        python_ide_init();
    }
    if (kind == WIN_HTTP_DOWNLOADER) {
        http_downloader_init(win);
    }
    if (kind == WIN_BROWSER) {
        /* A compact local NetSurf start page.  All destinations use absolute
         * HTTPS URLs, so each underlined label is a real upstream NetSurf
         * anchor, not a C-OS-specific shortcut or retired renderer path. */
        const char* start_url =
            "data:text/html,%3Cmeta%20charset=utf-8%3E"
            "%3Ch1%3EC-OS%20NetSurf%20Start%3C/h1%3E"
            "%3Cp%3E%3Ca%20href=https://www.google.com/%3EGoogle%3C/a%3E"
            "%20%7C%20%3Ca%20href=https://www.wikipedia.org/%3EWikipedia%3C/a%3E"
            "%20%7C%20%3Ca%20href=https://github.com/%3EGitHub%3C/a%3E"
            "%20%7C%20%3Ca%20href=https://www.pixiv.net/%3EPixiv%3C/a%3E%3C/p%3E"
            "%3Cp%3EReal%20NetSurf%203.11%20HTTPS%20links%3C/p%3E";
        const char* start_title = title ? title : gui_text("NetSurf 3.11", "NetSurf 3.11");
        if (title && strcmp(title, "Home") == 0) {
            start_title = gui_text("NetSurf 3.11", "NetSurf 3.11");
        }
        strncpy(win->browser_url, start_url, sizeof(win->browser_url) - 1);
        win->browser_url[sizeof(win->browser_url) - 1] = '\0';
        strncpy(win->browser_title, start_title, sizeof(win->browser_title) - 1);
        win->browser_title[sizeof(win->browser_title) - 1] = '\0';
        win->browser_scroll = 0;
        win->browser_url_focus = 0;
        win->browser_url_selected = FALSE;
        win->browser_url_cursor = 0;
        win->browser_search_text[0] = '\0';
        win->browser_search_focus = 0;
        win->browser_search_selected = FALSE;
        win->browser_search_cursor = 0;
        /* Paint the chrome first; NetSurf and the initial document start on
         * the next frame instead of blocking window creation. */
        win->browser_initial_load_pending = TRUE;
    }

    (void)gui_load_window_state_snapshot(win);
    if (win->title[0] == '\0' && title) {
        strncpy(win->title, title, 63);
        win->title[63] = '\0';
    }

    win->visible = TRUE;
    win->focused = TRUE;
    win->minimized = FALSE;
    win->maximized = FALSE;
    for (int i = 0; i < window_count - 1; ++i) {
        windows[i].focused = FALSE;
    }
    active_window = window_count - 1;
    /* A newly-created window may be opened before the next GUI cadence.
     * Invalidate immediately so the browser chrome and its asynchronous
     * initial document are painted instead of leaving the old desktop frame
     * on screen until an unrelated input event occurs. */
    gui_sys.needs_redraw = true;
    return win;
}

void gui_close_window(int idx) {
    if (idx < 0 || idx >= window_count) return;

    if (windows[idx].kind == WIN_VOXEL_GAME) voxel_games_save_window_state(&windows[idx]);
    (void)gui_save_window_state_snapshot(&windows[idx]);
    for (int i = idx; i < window_count - 1; i++) windows[i] = windows[i + 1];

    window_count--;
    if (window_count > 0) {
        memset(&windows[window_count], 0, sizeof(windows[window_count]));
        active_window = window_count - 1;
        windows[active_window].focused = true;
    } else {
        active_window = -1;
    }
    gui_request_redraw();
}

// Duplicate gui_create_window function removed

void gui_bring_to_front(int idx) {
    if (idx < 0 || idx >= window_count) return;

    if (idx != window_count - 1) {
        window_t tmp = windows[idx];
        for (int i = idx; i < window_count - 1; i++) windows[i] = windows[i + 1];
        windows[window_count - 1] = tmp;
    }

    for (int i = 0; i < window_count; ++i) {
        windows[i].focused = false;
    }
    windows[window_count - 1].focused = true;
    active_window = window_count - 1;
    gui_request_redraw();
}

/* FIX: Added missing GUI functions referenced by gui_apps.c */
int gui_find_window(int kind) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i].visible && windows[i].kind == kind) return i;
    }
    return -1;
}
// Duplicate gui_focus_window function removed - already defined above
void gui_minimize_window(int idx) {
    if (idx < 0 || idx >= window_count) return;
    windows[idx].minimized = TRUE;
    if (windows[idx].kind == WIN_VOXEL_GAME) voxel_games_save_window_state(&windows[idx]);
    (void)gui_save_window_state_snapshot(&windows[idx]);
    gui_request_redraw();
}
void gui_maximize_window(int idx) {
    if (idx < 0 || idx >= window_count) return;
    if (!windows[idx].maximized) {
        windows[idx].restore_x = windows[idx].x;
        windows[idx].restore_y = windows[idx].y;
        windows[idx].restore_w = windows[idx].w;
        windows[idx].restore_h = windows[idx].h;
        windows[idx].x = 0;
        windows[idx].y = 0;
        windows[idx].w = (int)SCREEN_W;
        windows[idx].h = (int)SCREEN_H - TASKBAR_H;
        windows[idx].maximized = TRUE;
        (void)gui_save_window_state_snapshot(&windows[idx]);
    }
    gui_request_redraw();
}
void gui_restore_window(int idx) {
    if (idx < 0 || idx >= window_count) return;
    if (windows[idx].maximized) {
        windows[idx].x = windows[idx].restore_x;
        windows[idx].y = windows[idx].restore_y;
        windows[idx].w = windows[idx].restore_w;
        windows[idx].h = windows[idx].restore_h;
        windows[idx].maximized = FALSE;
    }
    windows[idx].minimized = FALSE;
    (void)gui_save_window_state_snapshot(&windows[idx]);
    gui_request_redraw();
}
/* ---- Notification system ---- */
notif_t notifications[MAX_NOTIFS];
int     notif_count = 0;


void gui_draw_notifications_panel(void)
{
    /* Use the new notification center to draw notifications.
     * Expiry/dismissal now happens on notification_gc_thread's own
     * schedule (see notification_center.c), not here - this function
     * only reads and draws. Bracketing with begin/end_read keeps the
     * GC thread from mutating the array mid-iteration if it gets
     * scheduled in between. */
    notification_center_begin_read();

    int count = 0;
    notification_t* list = notification_get_all(&count);
    if (!list || count == 0) {
        notification_center_end_read();
        return;
    }

    int nx = (int)SCREEN_W - 320;
    int ny = 16;

    for (int i = 0; i < count; i++) {
        notification_t* n = &list[i];

        uint64_t bg_col, border_col;
        if      (n->type == NOTIFY_TYPE_ERROR)   { bg_col = rgb(180, 30, 20); border_col = rgb(220, 80, 70); }
        else if (n->type == NOTIFY_TYPE_WARNING) { bg_col = rgb(140, 100, 0); border_col = rgb(220, 180, 0); }
        else if (n->type == NOTIFY_TYPE_SUCCESS) { bg_col = rgb(30, 140, 40); border_col = rgb(80, 220, 100); }
        else                                     { bg_col = rgb(30,  45,  75); border_col = rgb(80, 130, 200); }

        vga_fill_rounded_rect(nx + 3, ny + 3, 300, 52, 6, rgb(10, 10, 20));
        vga_fill_rounded_rect(nx, ny, 300, 52, 6, bg_col);
        vga_draw_rounded_rect(nx, ny, 300, 52, 6, border_col);
        
        const char* icon = (n->type == NOTIFY_TYPE_ERROR) ? "!" : (n->type == NOTIFY_TYPE_WARNING) ? "?" : "i";
        vga_fill_circle(nx + 22, ny + 26, 10, border_col);
        vga_draw_string(nx + 18, ny + 20, icon, rgb(255,255,255), 0xFFFFFFFF);
        
        vga_draw_string(nx + 40, ny + 10, n->title, rgb(180, 210, 255), 0xFFFFFFFF);
        
        char mbuf[36];
        strncpy(mbuf, n->message, 35);
        mbuf[35] = '\0';
        vga_draw_string(nx + 40, ny + 28, mbuf, rgb(220, 230, 245), 0xFFFFFFFF);

        ny += 60;
    }

    notification_center_end_read();
}
/* draw_window_frame is static in gui.c but called from gui_apps.c.
   Provide a public wrapper. */
void draw_window_frame_public(int idx) {
    draw_window_frame(idx);
}
