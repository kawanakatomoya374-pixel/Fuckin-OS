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
/* kmalloc/kfree for the composited desktop-layer cache. Without this the
 * compiler implicitly declares them as returning int, which truncates the
 * returned pointer to 32 bits on x86-64. */
#include "memory.h"
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

/* TSC is used rather than get_timer_ticks() for these sub-phase counters:
 * the PIT tick is 1ms, and individual draw phases are well under that, so
 * millisecond deltas would quantise almost everything to zero. */
static inline uint64_t cos_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- Window chrome cache ---------------------------------------------- */

/* draw_window_frame() paints the window rect plus a drop shadow offset by up
 * to 8px down and right (four rects at i*2 for i=4..1). */
#define COS_CHROME_SHADOW_PAD 8
#define COS_CHROME_CACHE_MAX  6

typedef struct {
    uint32_t *pixels;
    int x, y, w, h;      /* captured region, including shadow padding */
    uint64_t sig;
    bool valid;
} cos_chrome_cache_entry_t;

static cos_chrome_cache_entry_t cos_chrome_cache[COS_CHROME_CACHE_MAX];
static bool cos_chrome_cache_usable_this_frame = false;

static inline void cos_chrome_rect(const window_t *w, int *x, int *y, int *cw, int *ch) {
    *x = w->x;
    *y = w->y;
    *cw = w->w + COS_CHROME_SHADOW_PAD;
    *ch = w->h + COS_CHROME_SHADOW_PAD;
}

/* Every input draw_window_frame() reads, for window idx. The three button
 * hover booleans are computed exactly as that function computes them, so a
 * hover highlight appearing or disappearing invalidates the cache; the raw
 * mouse position deliberately does not, or every mouse move would. */
static uint64_t cos_chrome_signature(int idx) {
    const window_t *w = &windows[idx];
    uint64_t sig = 1469598103934665603ULL;
    #define MIX(v) do { sig ^= (uint64_t)(v); sig *= 1099511628211ULL; } while (0)
    MIX(w->x); MIX(w->y); MIX(w->w); MIX(w->h);
    MIX(w->kind);
    MIX(idx == active_window ? 1 : 0);
    for (const char *t = w->title; *t; ++t) MIX((unsigned char)*t);
    int btn_y = w->y + 6;
    int close_x = w->x + w->w - 32;
    int max_x   = w->x + w->w - 62;
    int min_x   = w->x + w->w - 92;
    bool hov_min   = (mouse.x >= min_x   && mouse.x < min_x   + 24 && mouse.y >= btn_y && mouse.y < btn_y + 24);
    bool hov_max   = (mouse.x >= max_x   && mouse.x < max_x   + 24 && mouse.y >= btn_y && mouse.y < btn_y + 24);
    bool hov_close = (mouse.x >= close_x && mouse.x < close_x + 24 && mouse.y >= btn_y && mouse.y < btn_y + 24);
    MIX(hov_min ? 1 : 0); MIX(hov_max ? 1 : 0); MIX(hov_close ? 1 : 0);
    #undef MIX
    return sig;
}

/* True when no two visible windows' expanded chrome rects intersect. Called
 * once per frame, not once per window. */
static bool cos_chrome_no_overlap(void) {
    for (int a = 0; a < window_count; ++a) {
        const window_t *wa = &windows[a];
        if (!wa->visible || wa->minimized) continue;
        int ax, ay, aw, ah; cos_chrome_rect(wa, &ax, &ay, &aw, &ah);
        for (int b = a + 1; b < window_count; ++b) {
            const window_t *wb = &windows[b];
            if (!wb->visible || wb->minimized) continue;
            int bx, by, bw, bh; cos_chrome_rect(wb, &bx, &by, &bw, &bh);
            if (ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah) return false;
        }
    }
    return true;
}

static void cos_chrome_cache_invalidate_all(void) {
    for (int i = 0; i < COS_CHROME_CACHE_MAX; ++i) cos_chrome_cache[i].valid = false;
}

/* Returns true if the chrome for window idx was served from cache (caller
 * then skips draw_window_frame). Otherwise draws it and captures it. */
static bool cos_chrome_cache_render_window(int idx) {
    if (!cos_chrome_cache_usable_this_frame) return false;
    if (idx < 0 || idx >= COS_CHROME_CACHE_MAX) return false;

    const window_t *w = &windows[idx];
    int cx, cy, cw, ch;
    cos_chrome_rect(w, &cx, &cy, &cw, &ch);
    if (cw <= 0 || ch <= 0) return false;
    /* Only cache fully on-screen chrome: a partially off-screen region would
     * be clipped on capture and would not blit back correctly. */
    if (cx < 0 || cy < 0 || cx + cw > (int)SCREEN_W || cy + ch > (int)SCREEN_H) return false;

    cos_chrome_cache_entry_t *e = &cos_chrome_cache[idx];
    uint64_t sig = cos_chrome_signature(idx);

    if (e->valid && e->sig == sig && e->pixels != NULL &&
        e->x == cx && e->y == cy && e->w == cw && e->h == ch) {
        vga_copy_rect_strided(cx, cy, cw, ch, e->pixels, cw);
        return true;
    }

    if (e->pixels != NULL && (e->w != cw || e->h != ch)) {
        kfree(e->pixels);
        e->pixels = NULL;
    }
    if (e->pixels == NULL) {
        e->pixels = (uint32_t *)kmalloc((size_t)cw * (size_t)ch * sizeof(uint32_t));
        if (e->pixels == NULL) { e->valid = false; return false; }
    }

    draw_window_frame(idx);
    vga_read_rect(cx, cy, cw, ch, e->pixels, cw);
    e->x = cx; e->y = cy; e->w = cw; e->h = ch;
    e->sig = sig;
    e->valid = true;
    return true; /* chrome is now on screen; caller must not draw it again */
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
    
    /* Phase instrumentation inside the scene build. gui_draw() averages 10ms
     * of a 34-41ms frame and unconditionally repaints wallpaper, every icon,
     * every window and the taskbar whenever anything changed - including a
     * one-pixel mouse move. Attribute it before optimising it. */
    uint64_t cos_p0 = cos_rdtsc();
    /* Composited desktop-layer cache.
     *
     * Measured with TSC counters (kcycles per frame, averaged over 1200
     * frames): wallpaper 6,427 - desktop icons 24,938 - windows 46,065 -
     * taskbar/cursor 4,605. The icons alone were 30% of the whole scene,
     * despite being static artwork: every frame re-rendered, per icon, a
     * rounded-rect drop shadow, a vertical-gradient rounded fill, a rounded
     * stroke, a highlight line and the glyph art - all with per-pixel corner
     * math - and then re-rendered the wallpaper gradient underneath it.
     * None of that changes between frames unless the wallpaper, the icon
     * set, the icon size or which icon is hovered changes.
     *
     * So wallpaper+icons are composited once into an offscreen layer and
     * blitted back on subsequent frames, turning ~31,000 kcycles of drawing
     * into one RAM-to-RAM block copy. Correctness comes from the signature
     * below: any input the layer depends on is folded into it, and any
     * change re-renders. The layer is captured from the backbuffer after a
     * normal draw, so it is by construction identical to what the old code
     * produced - this memoises the existing renderer rather than
     * reimplementing it. */
    {
        static uint32_t *s_layer = NULL;
        static uint64_t s_layer_sig = 0;
        static int s_layer_w = 0, s_layer_h = 0;

        int layer_h = (int)SCREEN_H - TASKBAR_H;
        if (layer_h < 0) layer_h = 0;

        /* Signature: every input the composited layer depends on. */
        uint64_t sig = 1469598103934665603ULL;
        #define COS_SIG_MIX(v) do { sig ^= (uint64_t)(v); sig *= 1099511628211ULL; } while (0)
        COS_SIG_MIX(SCREEN_W); COS_SIG_MIX(layer_h);
        COS_SIG_MIX(current_wallpaper);
        COS_SIG_MIX(gui_wallpaper_image_loaded ? 1 : 0);
        COS_SIG_MIX(gui_get_desktop_icon_render_size());
        COS_SIG_MIX(desktop_icon_count);
        for (int i = 0; i < desktop_icon_count; i++) {
            const desktop_icon_t *ic = &desktop_icons[i];
            COS_SIG_MIX(ic->x); COS_SIG_MIX(ic->y);
            COS_SIG_MIX(ic->win_kind);
            COS_SIG_MIX(ic->selected ? 1 : 0);
            COS_SIG_MIX(ic->is_file ? 1 : 0);
            COS_SIG_MIX(ic->is_dynamic ? 1 : 0);
            /* Hover is per-icon and changes the artwork, so it belongs in
             * the signature - but only the hover *state*, not the raw mouse
             * position, otherwise every mouse move would invalidate. */
            int box = gui_get_desktop_icon_render_size();
            int icon_h = box + 22;
            bool hov = (mouse.x >= ic->x && mouse.x < ic->x + box &&
                        mouse.y >= ic->y && mouse.y < ic->y + icon_h);
            COS_SIG_MIX(hov ? 1 : 0);
            for (const char *L = ic->label; *L; ++L) COS_SIG_MIX((unsigned char)*L);
        }
        #undef COS_SIG_MIX

        if (s_layer != NULL && (s_layer_w != (int)SCREEN_W || s_layer_h != layer_h)) {
            kfree(s_layer);
            s_layer = NULL;
        }
        if (s_layer == NULL && SCREEN_W > 0 && layer_h > 0) {
            s_layer = (uint32_t *)kmalloc((size_t)SCREEN_W * (size_t)layer_h * sizeof(uint32_t));
            s_layer_w = (int)SCREEN_W;
            s_layer_h = layer_h;
            s_layer_sig = 0; /* force a render into the fresh buffer */
        }

        if (s_layer != NULL && s_layer_sig == sig) {
            vga_copy_rect_strided(0, 0, s_layer_w, s_layer_h, s_layer, s_layer_w);
        } else {
            draw_wallpaper();
            draw_desktop_icons();
            if (s_layer != NULL) {
                vga_read_rect(0, 0, s_layer_w, s_layer_h, s_layer, s_layer_w);
                s_layer_sig = sig;
            }
        }
    }
    uint64_t cos_p1 = cos_rdtsc();
    uint64_t cos_p2 = cos_rdtsc();
    uint64_t cos_frame_cycles = 0;
    /* Decide once per frame whether the chrome cache may be used at all. */
    cos_chrome_cache_usable_this_frame = cos_chrome_no_overlap();
    if (!cos_chrome_cache_usable_this_frame) cos_chrome_cache_invalidate_all();
    for (int i = 0; i < window_count; i++) {
        window_t* w = &windows[i]; if (!w->visible || w->minimized) continue;
        /* Window chrome cache.
         *
         * Measured: draw_window_frame() was 43,494 of 59,391 kcycles per
         * frame - 73% of all window rendering, and the single largest cost
         * in the scene. It repaints four offset shadow rects, a per-scanline
         * body gradient, rounded fill and stroke, a per-scanline title-bar
         * gradient, the title string and three rounded buttons, every frame,
         * for every visible window. All of it is a pure function of the
         * window's geometry, title, focus and which button (if any) the
         * mouse is over - none of which changes on a typical frame.
         *
         * Like the desktop layer, the cached pixels are captured from the
         * backbuffer after a normal draw, so they are identical by
         * construction to what the old path produced.
         *
         * The correctness hazard here (absent for the desktop layer) is that
         * chrome is NOT composited over a fixed background: the drop shadow
         * is offset down-right and the rounded corners leave the extreme
         * corner pixels showing whatever is underneath. If windows overlap,
         * what is underneath includes another window's *client* area, which
         * is app-drawn and can change every frame - so a captured chrome
         * region could go stale. Rather than try to track that, the cache is
         * simply disabled for any frame in which the expanded chrome rects of
         * two visible windows intersect. Non-overlapping windows (the common
         * case, and always true for a single window) still get the benefit,
         * and overlapping ones fall back to the exact original code path. */
        uint64_t cos_wf0 = cos_rdtsc();
        if (!cos_chrome_cache_render_window(i)) {
            draw_window_frame(i);
        }
        uint64_t cos_wf1 = cos_rdtsc();
        cos_frame_cycles += cos_wf1 - cos_wf0;
        gui_set_clip(w->x, w->y, w->w, w->h);
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
        else if (w->kind == WIN_COS_APP) { extern void cos_app_window_draw(int); cos_app_window_draw(i); }
        else if (w->kind == WIN_PYTHON_IDE) { python_ide_set_geometry(w->x, w->y, w->w, w->h); python_ide_draw(); }
        gui_reset_clip();
    }
    uint64_t cos_p3 = cos_rdtsc();
    {
        static uint64_t fn = 0, ftot = 0;
        ++fn; ftot += cos_frame_cycles;
        if ((fn % 200ULL) == 0ULL) {
            serial_puts("[GFXPERF] window_chrome_avg=");
            serial_putdec((ftot / fn) / 1000ULL);
            serial_puts(" kcycles_per_frame\n");
        }
    }
    gui_draw_notifications_panel();
    draw_taskbar();
    gui_draw_ctx_menu_panel();
    draw_cursor();
    draw_multi_cursor();
    {
        uint64_t cos_p4 = cos_rdtsc();
        static uint64_t n = 0, wall = 0, icons = 0, wins = 0, chrome = 0;
        ++n;
        wall   += cos_p1 - cos_p0;
        icons  += cos_p2 - cos_p1;
        wins   += cos_p3 - cos_p2;
        chrome += cos_p4 - cos_p3;
        if ((n % 200ULL) == 0ULL) {
            serial_puts("[GFXPERF] gui_draw n=");
            serial_putdec(n);
            serial_puts(" wallpaper_avg=");
            serial_putdec((wall / n) / 1000ULL);
            serial_puts(" icons_avg=");
            serial_putdec((icons / n) / 1000ULL);
            serial_puts(" windows_avg=");
            serial_putdec((wins / n) / 1000ULL);
            serial_puts(" chrome_avg=");
            serial_putdec((chrome / n) / 1000ULL);
            serial_puts(" kcycles_per_frame\n");
        }
    }
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
    if (kind == WIN_TCC_IDE) {
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
