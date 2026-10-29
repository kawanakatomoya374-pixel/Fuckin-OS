#ifndef GUI_BOOT_VISIBLE_MIN_TICKS
#define GUI_BOOT_VISIBLE_MIN_TICKS 180
#endif

/**
 * boot_animation.c - GUI-owned C-OS boot animation
 *
 * This module keeps the visual startup sequence out of kernel orchestration
 * while preserving the strict "always run, always complete" behavior.
 */
#include "boot_animation.h"
#include "vga.h"
#include "serial.h"
#include "timer.h"
#include "../kernel/settings_manager.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;

extern uint64_t get_timer_ticks(void);





static int gui_boot_strlen(const char* s);




static bool gui_boot_animation_done = false;
static bool gui_boot_animation_started = false;
static int  gui_boot_animation_result = 0;  /* 0=running, 1=success, -1=error */
static gui_boot_phase_t gui_boot_current_phase = GUI_BOOT_PHASE_INIT;







typedef struct {
    int x;
    int y;
    uint8_t layer;
    uint8_t phase;
    uint8_t radius;
} gui_boot_star_t;

#define GUI_BOOT_STAR_COUNT 48
static gui_boot_star_t gui_boot_stars[GUI_BOOT_STAR_COUNT];
static bool gui_boot_stars_ready = false;









static bool gui_boot_graphics_ready(void) {
    return vga_has_display();
}

bool gui_boot_animation_completed(void) {
    /* The visible boot experience is now cos_bootsplash.c's ring-3 program (real cos_ui
     * vector graphics at the screen's own resolution instead of this file's kernel-side
     * VGA drawing - see gui_init()'s call to cos_bootsplash_launch()); this flag stays as
     * the mechanism, since gui_lifecycle.c and gui_render_loop.c already gate the
     * desktop's reveal on it and neither needed to change. gui_boot_animation_done itself
     * (this file's own flag) is deliberately NOT consulted here any more: it turns true
     * moments into boot, in gui_boot_animation_run() below, long before ring-3 or the
     * splash process exist - ORing it in would make this always true from the very start
     * and never actually wait for the splash. cos_bootsplash_done() alone is correct: it
     * reports true before anything launches it (nothing to wait for yet), false while it
     * is running, true once it exits. */
    extern bool cos_bootsplash_done(void);
    return cos_bootsplash_done();
}

static int gui_boot_strlen(const char* s) {
    int len = 0;
    if (!s) {
        return 0;
    }
    while (s[len] != '\0') {
        ++len;
    }
    return len;
}





static int gui_boot_center_x(const char* text, int scale) {
    (void)scale;
    int len = gui_boot_strlen(text);
    int text_w = len * vga_get_font_width();
    return ((int)SCREEN_W - text_w) / 2;
}

static void gui_boot_draw_centered_scaled(const char* text, int y, int scale, uint64_t fg, uint64_t glow) {
    if (!text) {
        return;
    }

    int prev_px = vga_get_font_pixel_width();
    vga_set_font_scale(scale);
    int x = gui_boot_center_x(text, scale);

    if (glow != fg) {
        vga_draw_string(x - 2, y - 1, text, glow, 0xFFFFFFFF);
        vga_draw_string(x + 2, y - 1, text, glow, 0xFFFFFFFF);
        vga_draw_string(x - 1, y + 1, text, glow, 0xFFFFFFFF);
        vga_draw_string(x + 1, y + 1, text, glow, 0xFFFFFFFF);
    }

    vga_draw_string(x, y, text, fg, 0xFFFFFFFF);
    vga_set_font_pixel_width(prev_px);
}



















#define GUI_BOOT_TEXT_COLS 80u
#define GUI_BOOT_TEXT_ROWS 25u
#define GUI_BOOT_TEXT_ATTR(fg, bg) (uint8_t)((((bg) & 0x0Fu) << 4) | ((fg) & 0x0Fu))

















/* ------------------------------------------------------------------ */
/* the REAL boot animation: one frame per actual init step               */
/* ------------------------------------------------------------------ */
/* Integer-only sin/cos (angle units are 0..255 per full circle, values
 * -127..127): this file's existing animation code (gui_boot_triangle_u8,
 * used throughout gui_boot_draw_scene above) is careful to avoid float/double
 * entirely, and this early in boot is not a place to be the first thing in
 * the kernel to find out whether SSE/FPU state is safe to touch - simple
 * integer arithmetic sidesteps the question rather than answering it. */
static const int8_t GUI_BOOT_SIN256[256] = {
    0, 3, 6, 9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46,
    49, 51, 54, 57, 60, 63, 65, 68, 71, 73, 76, 78, 81, 83, 85, 88,
    90, 92, 94, 96, 98, 100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116,
    117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127,
    127, 127, 127, 127, 126, 126, 126, 125, 125, 124, 123, 122, 122, 121, 120, 118,
    117, 116, 115, 113, 112, 111, 109, 107, 106, 104, 102, 100, 98, 96, 94, 92,
    90, 88, 85, 83, 81, 78, 76, 73, 71, 68, 65, 63, 60, 57, 54, 51,
    49, 46, 43, 40, 37, 34, 31, 28, 25, 22, 19, 16, 12, 9, 6, 3,
    0, -3, -6, -9, -12, -16, -19, -22, -25, -28, -31, -34, -37, -40, -43, -46,
    -49, -51, -54, -57, -60, -63, -65, -68, -71, -73, -76, -78, -81, -83, -85, -88,
    -90, -92, -94, -96, -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
    -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
    -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
    -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100, -98, -96, -94, -92,
    -90, -88, -85, -83, -81, -78, -76, -73, -71, -68, -65, -63, -60, -57, -54, -51,
    -49, -46, -43, -40, -37, -34, -31, -28, -25, -22, -19, -16, -12, -9, -6, -3,
};
static int gui_boot_sin256(int a) { return GUI_BOOT_SIN256[(unsigned)a & 255]; }
static int gui_boot_cos256(int a) { return GUI_BOOT_SIN256[((unsigned)a + 64) & 255]; }
static uint64_t gui_boot_lerp_color(uint64_t a, uint64_t b, uint8_t t) {
    uint8_t ar = (uint8_t)(a >> 16), ag = (uint8_t)(a >> 8), ab = (uint8_t)a;
    uint8_t br = (uint8_t)(b >> 16), bg = (uint8_t)(b >> 8), bb = (uint8_t)b;
    uint8_t r = (uint8_t)(ar + (((int)br - ar) * t) / 255);
    uint8_t g = (uint8_t)(ag + (((int)bg - ag) * t) / 255);
    uint8_t bl = (uint8_t)(ab + (((int)bb - ab) * t) / 255);
    return rgb(r, g, bl);
}

/* Called from kernel.c after each major subsystem finishes initializing -
 * vga_init, smp/scheduler/task_init, ipc_init, rtc_init, pci_init, usb_init,
 * net_init, fs_init, permission_manager_init, keyboard_init - so what is on
 * screen changes because something really did just finish loading, not on a
 * timer counting down to a fixed, made-up duration. Preemptive multitasking
 * (scheduler_set_preemption(1)/scheduler_start(), kernel.c) is not enabled
 * until the very end of kernel_main - before that, nothing else can run
 * concurrently with kernel_main no matter how it's launched, ring-3 program
 * or otherwise - so this has to be kernel-side code called synchronously
 * between steps, the same way kernel_draw_status_screen's text ever was.
 * The ring-3 splash (userland/programs/bootsplash, cos_bootsplash_launch())
 * still runs afterward, once gui_init() and then the scheduler are up - a
 * closing flourish once the system genuinely has nothing meaningful left
 * queued to report, not a stand-in for progress that already happened. */
static bool     gui_boot_progress_started = false;
static uint64_t gui_boot_progress_t0;

void gui_boot_progress(const char *label) {
    if (!gui_boot_graphics_ready()) { if (label) { serial_puts("[BOOT] "); serial_puts(label); serial_puts("\n"); } return; }
    if (!gui_boot_progress_started) { gui_boot_progress_started = true; gui_boot_progress_t0 = get_timer_ticks(); }
    uint64_t elapsed = get_timer_ticks() - gui_boot_progress_t0;

    const uint64_t BG_TOP = rgb(0x14, 0x18, 0x26), BG_BOTTOM = rgb(0x0B, 0x0E, 0x16);
    const uint64_t ACCENT = rgb(0x4D, 0x9F, 0xFF), ACCENT_DIM = rgb(0x2C, 0x3B, 0x57);
    const uint64_t TEXT = rgb(0xF0, 0xF3, 0xFA), MUTED = rgb(0x7C, 0x85, 0x98);

    int W = (int)SCREEN_W, H = (int)SCREEN_H;
    for (int y = 0; y < H; y += 2) {
        int t = H > 1 ? (y * 255) / (H - 1) : 0;
        uint64_t row = gui_boot_lerp_color(BG_TOP, BG_BOTTOM, (uint8_t)t);
        vga_fill_rect(0, y, W, 2, row);
    }

    int cx = W / 2, cy = H / 2 - 60, mr = 46;
    vga_draw_circle(cx, cy, mr, ACCENT_DIM);
    vga_draw_circle(cx, cy, mr - 1, ACCENT_DIM);
    int spin = (int)((elapsed % 1600u) * 256u / 1600u);              /* 0..255, one lap every 1.6s of real elapsed time */
    for (int k = 0; k < 3; ++k) {
        int a0 = spin + k * 85;                                      /* 3 arcs, 120 degrees (85/256) apart */
        for (int s = 0; s < 14; ++s) {
            int a = a0 + s * 2;
            int px = cx + (mr * gui_boot_cos256(a)) / 127;
            int py = cy + (mr * gui_boot_sin256(a)) / 127;
            vga_fill_circle(px, py, 3, ACCENT);
        }
    }

    gui_boot_draw_centered_scaled("C-OS", cy + 90, 3, TEXT, TEXT);
    vga_draw_string(gui_boot_center_x("4.1.0", 1), cy + 150, "4.1.0", MUTED, 0xFFFFFFFF);

    int dot_y = cy + 210;
    for (int i = 0; i < 3; ++i) {
        int phase = (int)((elapsed % 1200u) * 256u / 1200u) - i * 57;
        int glow = (gui_boot_sin256(phase) + 127) * 255 / 254;
        if (glow < 0) glow = 0; if (glow > 255) glow = 255;
        uint64_t col = gui_boot_lerp_color(ACCENT_DIM, ACCENT, (uint8_t)glow);
        vga_fill_circle(cx - 22 + i * 22, dot_y, 4, col);
    }

    const char *l = label ? label : "starting up";
    vga_draw_string(gui_boot_center_x(l, 1), H - 40, l, MUTED, 0xFFFFFFFF);

    vga_wait_vblank();
    vga_flip();
    if (label) { serial_puts("[BOOT] "); serial_puts(label); serial_puts("\n"); }
}


/* External initialization - call once before boot sequence */




/* Check if boot animation is still running */


/* Get animation result: 0=running, 1=success, -1=error */


/* Force complete animation (for error recovery) */


/* Reset animation state for reboot */


