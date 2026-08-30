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

static inline void gui_boot_cli(void) { __asm__ __volatile__("cli"); }
static inline void gui_boot_sti(void) { __asm__ __volatile__("sti"); }
static inline void gui_boot_cpu_hlt(void) { __asm__ __volatile__("hlt"); }
static inline void gui_boot_cpu_pause(void) { __asm__ __volatile__("pause"); }
static int gui_boot_strlen(const char* s);

static void gui_boot_frame_delay(uint64_t elapsed_ticks) {
    /*
     * Soft delay so the animation keeps moving even if PIT/IRQ timing is
     * unstable in QEMU/firmware.
     */
    volatile uint32_t spins = 3000000u + (uint32_t)(elapsed_ticks & 63u) * 20000u;
    while (spins--) {
        gui_boot_cpu_pause();
    }
}

static uint32_t gui_boot_mix(uint32_t x);
static uint64_t gui_boot_seed(void);
static void gui_boot_text_frame(uint64_t elapsed_ticks, uint64_t total_ticks);
static void gui_boot_serial_frame(uint64_t elapsed_ticks, uint64_t total_ticks);

static bool gui_boot_animation_done = false;
static bool gui_boot_animation_started = false;
static int  gui_boot_animation_result = 0;  /* 0=running, 1=success, -1=error */
static gui_boot_phase_t gui_boot_current_phase = GUI_BOOT_PHASE_INIT;



gui_boot_phase_t gui_boot_get_current_phase(void) {
    return gui_boot_current_phase;
}

const char* gui_boot_get_phase_name(void) {
    switch (gui_boot_current_phase) {
        case GUI_BOOT_PHASE_INIT:      return "Initializing";
        case GUI_BOOT_PHASE_LOADING:   return "Loading";
        case GUI_BOOT_PHASE_TRANSITION:return "Transitioning to Desktop";
        case GUI_BOOT_PHASE_DESKTOP:   return "Desktop Ready";
        case GUI_BOOT_PHASE_COMPLETE:  return "Boot Complete";
        default:                        return "Unknown";
    }
}

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

static uint8_t gui_boot_triangle_u8(uint64_t tick, uint32_t period) {
    if (period == 0u) {
        return 0u;
    }
    uint32_t pos = (uint32_t)(tick % period);
    uint32_t half = period / 2u;
    if (half == 0u) {
        return 0u;
    }
    if (pos <= half) {
        return (uint8_t)((pos * 255u) / half);
    }
    return (uint8_t)(((period - pos) * 255u) / half);
}

static void gui_boot_init_stars(void) {
    if (gui_boot_stars_ready) {
        return;
    }

    uint32_t seed = (uint32_t)gui_boot_seed() ^ 0xBADC0DEu;
    int sw = SCREEN_W > 0 ? (int)SCREEN_W : 1;
    int sh = SCREEN_H > 0 ? (int)SCREEN_H : 1;

    for (int i = 0; i < GUI_BOOT_STAR_COUNT; ++i) {
        seed = gui_boot_mix(seed + (uint32_t)(i * 0x9E3779B9u + 0x13579BDFu));
        gui_boot_stars[i].x = (int)(seed % (uint32_t)sw);
        seed = gui_boot_mix(seed ^ 0xA5A5A5A5u ^ (uint32_t)(i * 131u));
        gui_boot_stars[i].y = (int)(seed % (uint32_t)sh);
        gui_boot_stars[i].layer = (uint8_t)(i % 3);
        gui_boot_stars[i].phase = (uint8_t)((seed >> 11) & 31u);
        gui_boot_stars[i].radius = (uint8_t)(1 + (seed & 1u) + (gui_boot_stars[i].layer == 0 ? 1u : 0u));
    }

    gui_boot_stars_ready = true;
}

static void gui_boot_draw_aurora(int cx, int cy, uint64_t elapsed_ticks) {
    int pulse = (int)gui_boot_triangle_u8(elapsed_ticks, 96u);
    int pulse2 = (int)gui_boot_triangle_u8(elapsed_ticks + 24u, 132u);
    int pulse3 = (int)gui_boot_triangle_u8(elapsed_ticks + 48u, 164u);

    vga_fill_circle(cx - 72, cy - 44, 78, rgb(10, 22, 46));
    vga_fill_circle(cx + 62, cy - 30, 70, rgb(12, 28, 56));
    vga_fill_circle(cx - 8, cy - 76, 56, rgb(16, 34, 68));

    vga_fill_circle(cx - 18, cy - 38 - pulse / 10, 50, rgb(18, 46, 90));
    vga_fill_circle(cx + 44, cy - 18 + pulse2 / 14, 44, rgb(26, 74, 138));
    vga_fill_circle(cx - 60 + pulse3 / 18, cy + 2, 36, rgb(20, 58, 106));
}

static void gui_boot_draw_badge(uint64_t elapsed_ticks) {
    int badge_x = 24;
    int badge_y = 22;
    int badge_w = 170;
    int badge_h = 30;

    vga_fill_rounded_rect(badge_x + 2, badge_y + 3, badge_w, badge_h, 10, 0x00000000);
    vga_fill_rounded_rect(badge_x, badge_y, badge_w, badge_h, 10, rgb(10, 18, 32));
    vga_draw_rounded_rect(badge_x, badge_y, badge_w, badge_h, 10, rgb(88, 136, 196));

    int pulse = 4 + (int)(gui_boot_triangle_u8(elapsed_ticks, 40u) / 32u);
    vga_fill_circle(badge_x + 15, badge_y + 15, pulse, rgb(78, 182, 255));
    vga_fill_circle(badge_x + 15, badge_y + 15, 2, rgb(238, 248, 255));
    vga_fill_rect(badge_x + 28, badge_y + 6, 1, badge_h - 12, rgb(55, 90, 132));

    // Center text in badge (using actual font width)
    const char* badge_text = settings_get_os_name();
    int text_len = gui_boot_strlen(badge_text);
    int text_w = text_len * vga_get_font_width();
    int text_x = badge_x + (badge_w - text_w) / 2;
    int text_y = badge_y + 9;
    vga_draw_string(text_x, text_y, badge_text, rgb(228, 240, 252), rgb(10, 18, 32));
}

static bool gui_boot_graphics_ready(void) {
    return framebuffer != NULL && SCREEN_W > 0 && SCREEN_H > 0;
}

bool gui_boot_animation_completed(void) {
    return gui_boot_animation_done;
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

static uint32_t gui_boot_mix(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static uint64_t gui_boot_seed(void) {
    uint32_t a = (uint32_t)get_timer_ticks();
    uint32_t b = (uint32_t)(SCREEN_W * 131u + SCREEN_H * 17u);
    uint32_t c = 0xC05405u;
    return (uint64_t)gui_boot_mix(a ^ gui_boot_mix(b ^ c));
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

    int prev_scale = vga_get_font_scale();
    vga_set_font_scale(scale);
    int x = gui_boot_center_x(text, scale);

    if (glow != fg) {
        vga_draw_string(x - 2, y - 1, text, glow, 0xFFFFFFFF);
        vga_draw_string(x + 2, y - 1, text, glow, 0xFFFFFFFF);
        vga_draw_string(x - 1, y + 1, text, glow, 0xFFFFFFFF);
        vga_draw_string(x + 1, y + 1, text, glow, 0xFFFFFFFF);
    }

    vga_draw_string(x, y, text, fg, 0xFFFFFFFF);
    vga_set_font_scale(prev_scale);
}

static void gui_boot_draw_window_pane(int x, int y, int w, int h, uint64_t fill, uint64_t border) {
    vga_fill_rounded_rect(x + 2, y + 2, w, h, 7, 0x00000000);
    vga_fill_rounded_rect(x, y, w, h, 7, fill);
    vga_draw_rounded_rect(x, y, w, h, 7, border);
}

static void gui_boot_draw_starfield(int frame) {
    gui_boot_init_stars();

    for (int i = 0; i < GUI_BOOT_STAR_COUNT; ++i) {
        const gui_boot_star_t* star = &gui_boot_stars[i];
        int drift_x = frame * (1 + (int)star->layer) * (star->layer == 0 ? 1 : 2);
        int drift_y = frame * (star->layer == 0 ? 1 : 0);
        int x = (star->x + drift_x + (int)star->phase) % (int)SCREEN_W;
        int y = (star->y + drift_y / 3) % (int)SCREEN_H;
        if (x < 0) x += (int)SCREEN_W;
        if (y < 0) y += (int)SCREEN_H;

        int tw = (int)((frame + star->phase) & 3);
        uint64_t color;
        if (star->layer == 0) {
            color = (tw >= 2) ? rgb(248, 252, 255) : rgb(212, 232, 255);
        } else if (star->layer == 1) {
            color = (tw == 3) ? rgb(170, 214, 255) : rgb(124, 180, 240);
        } else {
            color = (tw >= 2) ? rgb(92, 134, 192) : rgb(70, 104, 156);
        }

        int r = star->radius;
        vga_fill_circle(x, y, r, color);
        if (star->layer == 0) {
            vga_put_pixel(x + 1, y, color);
            vga_put_pixel(x, y + 1, color);
        } else if (star->layer == 1 && (tw & 1)) {
            vga_put_pixel(x + 1, y, color);
        }
    }
}

static void gui_boot_draw_orbit_ring(int cx, int cy, int radius_x, int radius_y, int frame, uint64_t color, int speed) {
    static const int8_t wave[32] = {
         0,  6, 12, 18, 23, 27, 30, 31,
        32, 31, 30, 27, 23, 18, 12,  6,
         0, -6,-12,-18,-23,-27,-30,-31,
       -32,-31,-30,-27,-23,-18,-12, -6
    };

    for (int i = 0; i < 6; ++i) {
        int phase = (frame * speed + i * 5) & 31;
        int x = cx + (wave[phase] * radius_x) / 32;
        int y = cy + (wave[(phase + 8) & 31] * radius_y) / 32;
        vga_fill_circle(x, y, 3 + ((i + frame) & 1), color);
    }
    vga_draw_circle(cx, cy, radius_x, 0x00466EA5);
    vga_draw_circle(cx, cy, radius_x + 1, 0x00121C2C);
}

static void gui_boot_draw_glass_card(int cx, int cy, uint64_t elapsed_ticks) {
    int card_w = 266;
    int card_h = 172;
    int left = cx - card_w / 2;
    int top = cy - card_h / 2;

    vga_fill_rounded_rect(left + 5, top + 7, card_w, card_h, 28, 0x00000000);
    vga_fill_rounded_rect(left, top, card_w, card_h, 28, 0x000A1222);
    vga_draw_rounded_rect(left, top, card_w, card_h, 28, 0x006088BE);

    vga_fill_rounded_rect(left + 3, top + 3, card_w - 6, card_h - 6, 24, 0x00101A30);
    vga_fill_rounded_rect(left + 5, top + 5, card_w - 10, card_h - 10, 22, 0x0014223C);

    /* Soft highlight sweep across the card. */
    int sweep_w = 22;
    int sweep_travel = card_w - 40 - sweep_w;
    if (sweep_travel < 1) {
        sweep_travel = 1;
    }
    int sweep_x = left + 20 + (int)((elapsed_ticks * 5u) % (uint64_t)sweep_travel);
    vga_fill_rounded_rect(sweep_x, top + 10, sweep_w, card_h - 20, 10, 0x00BEE4FF);
    vga_fill_rounded_rect(sweep_x + sweep_w, top + 10, 6, card_h - 20, 10, 0x00589CEE);

    /* Inner accents and a small "window stack" motif. */
    vga_fill_rect(left + 30, top + 24, 64, 4, 0x00ECF6FF);
    vga_fill_rect(left + 30, top + 34, 94, 4, 0x009ECEFF);
    vga_fill_rect(left + 30, top + 44, 80, 4, 0x006EA8F6);
    vga_fill_rect(left + 30, top + 54, 42, 4, 0x00ECF6FF);

    /* Floating glass panes. */
    int pane_w = 64;
    int pane_h = 44;
    int gap = 11;
    int skew = (int)((elapsed_ticks / 10u) % 5u) - 2;

    int x1 = left + 18;
    int y1 = top + 82 + skew;
    int x2 = x1 + pane_w + gap;
    int y2 = y1 - 4;
    int y3 = y1 + pane_h + gap;
    int y4 = y3 + 4;

    gui_boot_draw_window_pane(x1, y1, pane_w, pane_h, rgb(54, 130, 236), rgb(178, 220, 255));
    gui_boot_draw_window_pane(x2, y2, pane_w, pane_h, rgb(32, 108, 222), rgb(156, 202, 250));
    gui_boot_draw_window_pane(x1 - 2, y3, pane_w, pane_h, rgb(82, 156, 248), rgb(194, 234, 255));
    gui_boot_draw_window_pane(x2 + 2, y4, pane_w, pane_h, rgb(42, 118, 224), rgb(168, 210, 252));
}

static void gui_boot_draw_particle_ring(int cx, int cy, int frame) {
    /* Tiny sparkles orbiting the center to keep the screen feeling alive. */
    static const int8_t orbit[32] = {
         0,  8, 15, 22, 28, 33, 36, 38,
        39, 38, 36, 33, 28, 22, 15,  8,
         0, -8,-15,-22,-28,-33,-36,-38,
       -39,-38,-36,-33,-28,-22,-15, -8
    };

    for (int i = 0; i < 8; ++i) {
        int phase = (frame * 2 + i * 4) & 31;
        int x = cx + (orbit[phase] * (66 + (i % 3) * 8)) / 39;
        int y = cy + (orbit[(phase + 8) & 31] * (34 + (i % 2) * 6)) / 39;
        uint64_t color = (i & 1) ? rgb(84, 172, 255) : rgb(255, 184, 78);
        vga_fill_circle(x, y, 3, color);
    }

    gui_boot_draw_orbit_ring(cx, cy, 78, 30, frame, rgb(70, 110, 165), 2);
    gui_boot_draw_orbit_ring(cx, cy, 112, 46, frame + 12, rgb(28, 45, 74), 1);
}

static uint64_t gui_boot_eased_progress(uint64_t elapsed_ticks, uint64_t total_ticks) {
    if (total_ticks == 0) {
        return 0;
    }
    if (elapsed_ticks >= total_ticks) {
        return 100;
    }

    /* Smooth in/out so the bar feels less linear and more polished. */
    uint64_t pct = (elapsed_ticks * 100u) / total_ticks;
    if (pct < 20u) {
        return (pct * pct) / 20u;
    }
    if (pct > 80u) {
        uint64_t tail = 100u - pct;
        return 100u - ((tail * tail) / 20u);
    }
    return pct;
}

static void gui_boot_draw_progress(uint64_t elapsed_ticks, uint64_t total_ticks) {
    int bar_w = (int)SCREEN_W / 2;
    if (bar_w < 320) {
        bar_w = 320;
    }
    if (bar_w > (int)SCREEN_W - 96) {
        bar_w = (int)SCREEN_W - 96;
    }

    int bar_x = ((int)SCREEN_W - bar_w) / 2;
    int bar_y = (int)SCREEN_H - 94;
    int bar_h = 18;

    uint64_t fill_pct = gui_boot_eased_progress(elapsed_ticks, total_ticks);
    uint64_t fill_w = ((uint64_t)bar_w * fill_pct) / 100u;
    if (fill_w > (uint64_t)bar_w) {
        fill_w = (uint64_t)bar_w;
    }

    /* Shadow and shell. */
    vga_fill_rounded_rect(bar_x + 2, bar_y + 4, bar_w, bar_h, 9, 0x00000000);
    vga_fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, 9, 0x000C121E);
    vga_draw_rounded_rect(bar_x, bar_y, bar_w, bar_h, 9, 0x005E88BC);

    if (fill_w > 0) {
        int inner_w = (int)fill_w - 4;
        if (inner_w < 1) {
            inner_w = 1;
        }
        vga_fill_rounded_rect(bar_x + 2, bar_y + 2, inner_w, bar_h - 4, 7, rgb(34, 88, 166));

        int grad_w = (int)fill_w - 6;
        if (grad_w < 1) {
            grad_w = 1;
        }
        vga_fill_rect(bar_x + 3, bar_y + 3, grad_w, bar_h - 6, rgb(74, 150, 244));
        if (grad_w > 8) {
            vga_fill_rect(bar_x + 3, bar_y + 3, grad_w / 2, bar_h - 6, rgb(170, 230, 255));
        }
    }

    if (fill_w > 16) {
        int shine_w = 36;
        int travel = bar_w - shine_w - 8;
        if (travel < 1) {
            travel = 1;
        }
        int shine_x = bar_x + 4 + (int)((elapsed_ticks * 9u) % (uint64_t)travel);
        if (shine_x > bar_x + (int)fill_w - shine_w) {
            shine_x = bar_x + (int)fill_w - shine_w;
        }
        if (shine_x < bar_x + 4) {
            shine_x = bar_x + 4;
        }
        vga_fill_rounded_rect(shine_x, bar_y + 3, shine_w, bar_h - 6, 6, rgb(235, 247, 255));
    }

    /*
     * Subtle reflection under the bar.
     * Keep it faint and short so the bar still reads as glass, not chrome.
     */
    if (fill_w > 10) {
        int reflect_w = (int)fill_w - 12;
        if (reflect_w < 12) {
            reflect_w = 12;
        }
        if (reflect_w > bar_w - 12) {
            reflect_w = bar_w - 12;
        }
        int reflect_x = bar_x + 6;
        int reflect_y = bar_y + bar_h + 2;

        vga_fill_rounded_rect(reflect_x, reflect_y, reflect_w, 4, 2, rgb(80, 145, 202));
        vga_fill_rounded_rect(reflect_x + 3, reflect_y + 1, reflect_w - 6, 2, 1, rgb(162, 221, 255));
        if (reflect_w > 18) {
            vga_fill_rect(reflect_x + 8, reflect_y + 3, reflect_w - 16, 1, rgb(46, 88, 132));
        }
    }

    /* Caption and percentage. */
    vga_draw_string(bar_x, bar_y - 22, "Booting with style", rgb(184, 208, 232), rgb(5, 10, 22));

    char pct_text[16];
    int pct = (int)fill_pct;
    int idx = 0;
    if (pct >= 100) {
        pct_text[idx++] = '1';
        pct_text[idx++] = '0';
        pct_text[idx++] = '0';
    } else {
        if (pct >= 10) {
            pct_text[idx++] = (char)('0' + (pct / 10));
        }
        pct_text[idx++] = (char)('0' + (pct % 10));
    }
    pct_text[idx++] = '%';
    pct_text[idx] = '\0';

    int pct_x = bar_x + bar_w - gui_boot_strlen(pct_text) * FONT_W;
    vga_draw_string(pct_x, bar_y - 22, pct_text, rgb(184, 208, 232), rgb(5, 10, 22));
}

static void gui_boot_draw_status_line(uint64_t elapsed_ticks, uint64_t total_ticks) {
    const char* phase = "Preparing a richer desktop";
    if (total_ticks > 0) {
        uint64_t pct = (elapsed_ticks * 100) / total_ticks;
        if (pct > 90) {
            phase = "Final polish";
        } else if (pct > 65) {
            phase = "Aligning the interface";
        } else if (pct > 35) {
            phase = "Loading visuals and input";
        } else {
            phase = "Assembling the startup scene";
        }
    }

    int x = gui_boot_center_x(phase, 1);
    vga_draw_string(x, (int)SCREEN_H - 122, phase, rgb(228, 238, 248), rgb(4, 10, 22));
}

static void gui_boot_serial_frame(uint64_t elapsed_ticks, uint64_t total_ticks) {
    static const char spinner[4] = {'|', '/', '-', '\\'};
    uint64_t percent = total_ticks ? (elapsed_ticks * 100u) / total_ticks : 100u;
    uint64_t filled = percent / 5u; /* 20 cells */

    serial_puts("\r[BOOT] ");
    serial_putc(spinner[(elapsed_ticks / 6u) & 3u]);
    serial_puts(" ");
    serial_putdec(percent);
    serial_puts("% [");

    for (uint64_t i = 0; i < 20u; ++i) {
        serial_putc(i < filled ? '#' : '.');
    }

    serial_puts("] "); serial_puts(settings_get_os_name());
}

#define GUI_BOOT_TEXT_COLS 80u
#define GUI_BOOT_TEXT_ROWS 25u
#define GUI_BOOT_TEXT_ATTR(fg, bg) (uint8_t)((((bg) & 0x0Fu) << 4) | ((fg) & 0x0Fu))

static volatile uint16_t* gui_boot_text_vram(void) {
    return (volatile uint16_t*)(uintptr_t)0xB8000;
}

static void gui_boot_text_put_cell(unsigned row, unsigned col, char ch, uint8_t attr) {
    if (row >= GUI_BOOT_TEXT_ROWS || col >= GUI_BOOT_TEXT_COLS) {
        return;
    }
    gui_boot_text_vram()[row * GUI_BOOT_TEXT_COLS + col] = (uint16_t)(unsigned char)ch | ((uint16_t)attr << 8);
}

static void gui_boot_text_write(unsigned row, unsigned col, const char* text, uint8_t attr) {
    if (!text || row >= GUI_BOOT_TEXT_ROWS || col >= GUI_BOOT_TEXT_COLS) {
        return;
    }
    volatile uint16_t* vram = gui_boot_text_vram();
    unsigned x = col;
    while (*text && x < GUI_BOOT_TEXT_COLS) {
        vram[row * GUI_BOOT_TEXT_COLS + x] = (uint16_t)(unsigned char)(*text) | ((uint16_t)attr << 8);
        ++text;
        ++x;
    }
}

static void gui_boot_text_clear(uint8_t attr) {
    volatile uint16_t* vram = gui_boot_text_vram();
    uint16_t cell = (uint16_t)' ' | ((uint16_t)attr << 8);
    for (unsigned i = 0; i < GUI_BOOT_TEXT_COLS * GUI_BOOT_TEXT_ROWS; ++i) {
        vram[i] = cell;
    }
}

static void gui_boot_text_bar(unsigned row, unsigned col, unsigned width, uint64_t filled, uint8_t fg, uint8_t bg) {
    if (width == 0 || row >= GUI_BOOT_TEXT_ROWS || col >= GUI_BOOT_TEXT_COLS) {
        return;
    }
    if (col + width > GUI_BOOT_TEXT_COLS) {
        width = GUI_BOOT_TEXT_COLS - col;
    }
    uint8_t attr = GUI_BOOT_TEXT_ATTR(fg, bg);
    for (unsigned i = 0; i < width; ++i) {
        gui_boot_text_put_cell(row, col + i, i < filled ? '#' : '.', attr);
    }
}

static void gui_boot_text_frame(uint64_t elapsed_ticks, uint64_t total_ticks) {
    static const char spinner[4] = {'|', '/', '-', '\\'};
    uint64_t percent = total_ticks ? (elapsed_ticks * 100u) / total_ticks : 100u;
    uint64_t filled = percent / 5u;
    if (filled > 20u) {
        filled = 20u;
    }

    gui_boot_text_clear(GUI_BOOT_TEXT_ATTR(15, 1));
    gui_boot_text_write(3, 29, settings_get_os_name(), GUI_BOOT_TEXT_ATTR(14, 1));
    gui_boot_text_write(5, 20, "Boot animation is starting...", GUI_BOOT_TEXT_ATTR(15, 1));
    gui_boot_text_write(7, 20, "Loading visuals and input", GUI_BOOT_TEXT_ATTR(11, 1));

    gui_boot_text_write(10, 20, "[", GUI_BOOT_TEXT_ATTR(15, 1));
    gui_boot_text_bar(10, 21, 20u, filled, 15, 1);
    gui_boot_text_write(10, 41, "]", GUI_BOOT_TEXT_ATTR(15, 1));

    char pctbuf[16];
    pctbuf[0] = (char)('0' + (int)((percent / 100u) % 10u));
    pctbuf[1] = (char)('0' + (int)((percent / 10u) % 10u));
    pctbuf[2] = (char)('0' + (int)(percent % 10u));
    pctbuf[3] = '%';
    pctbuf[4] = 0;
    gui_boot_text_write(10, 45, pctbuf, GUI_BOOT_TEXT_ATTR(15, 1));

    gui_boot_text_write(12, 20, "Status: ", GUI_BOOT_TEXT_ATTR(15, 1));
    gui_boot_text_put_cell(12, 28, spinner[(elapsed_ticks / 6u) & 3u], GUI_BOOT_TEXT_ATTR(14, 1));
    gui_boot_text_write(12, 30, "Animating", GUI_BOOT_TEXT_ATTR(15, 1));

    if (percent < 35u) {
        gui_boot_text_write(14, 20, "Assembling the startup scene", GUI_BOOT_TEXT_ATTR(15, 1));
    } else if (percent < 65u) {
        gui_boot_text_write(14, 20, "Loading visuals and input", GUI_BOOT_TEXT_ATTR(15, 1));
    } else if (percent < 90u) {
        gui_boot_text_write(14, 20, "Aligning the interface", GUI_BOOT_TEXT_ATTR(15, 1));
    } else {
        gui_boot_text_write(14, 20, "Final polish", GUI_BOOT_TEXT_ATTR(15, 1));
    }

    gui_boot_text_write(20, 20, "This screen is shown even when framebuffer setup fails.", GUI_BOOT_TEXT_ATTR(7, 1));
}

static void gui_boot_draw_simple_splash(void) {
    if (!gui_boot_graphics_ready()) {
        gui_boot_text_frame(0, 420u);
        gui_boot_serial_frame(0, GUI_BOOT_VISIBLE_MIN_TICKS);
        return;
    }

    vga_clear(rgb(4, 8, 18));
    vga_fill_rect(0, 0, (int)SCREEN_W, 64, rgb(24, 44, 86));
    vga_fill_rect(0, 64, (int)SCREEN_W, 2, rgb(255, 255, 255));
    vga_fill_rect(0, (int)SCREEN_H - 48, (int)SCREEN_W, 48, rgb(8, 14, 28));

    gui_boot_draw_centered_scaled(settings_get_os_name(), (int)SCREEN_H / 2 - 56, 4, rgb(120, 196, 255), rgb(20, 44, 88));
    gui_boot_draw_centered_scaled("Booting...", (int)SCREEN_H / 2 + 6, 1, rgb(238, 246, 255), rgb(18, 34, 58));
    vga_draw_string(gui_boot_center_x("Framebuffer startup sequence", 1), (int)SCREEN_H / 2 + 34,
                    "Framebuffer startup sequence", rgb(170, 206, 244), rgb(4, 8, 18));

    int bar_w = (int)SCREEN_W - 160;
    if (bar_w < 240) {
        bar_w = 240;
    }
    int bar_x = ((int)SCREEN_W - bar_w) / 2;
    int bar_y = (int)SCREEN_H / 2 + 76;
    vga_fill_rounded_rect(bar_x, bar_y, bar_w, 18, 8, rgb(11, 19, 34));
    vga_draw_rounded_rect(bar_x, bar_y, bar_w, 18, 8, rgb(92, 140, 196));
    vga_fill_rounded_rect(bar_x + 3, bar_y + 3, bar_w - 6, 12, 6, rgb(42, 92, 170));
    vga_fill_rect(bar_x + 8, bar_y + 5, bar_w / 3, 8, rgb(178, 228, 255));

    vga_wait_vblank();
    vga_flip();
}

static void gui_boot_draw_scene(uint64_t elapsed_ticks, uint64_t total_ticks, bool always_visible) {
    if (!gui_boot_graphics_ready()) {
        return;
    }

    /* Background layers: base tone + ambient glow. */
    vga_clear(rgb(5, 10, 22));
    vga_fill_rect(0, 0, (int)SCREEN_W, (int)SCREEN_H, rgb(5, 10, 22));

    int cx = (int)SCREEN_W / 2;
    int cy = (int)SCREEN_H / 2 - 18;
    int title_shift = 8 - (int)gui_boot_triangle_u8(elapsed_ticks, 96u) / 32;

    gui_boot_draw_aurora(cx, cy, elapsed_ticks);

    vga_fill_circle(cx, cy - 10, 170, rgb(10, 18, 34));
    vga_fill_circle(cx, cy - 10, 130, rgb(14, 26, 50));
    vga_fill_circle(cx, cy - 10, 92, rgb(18, 34, 66));
    vga_fill_circle(cx, cy - 10, 56, rgb(24, 48, 92));

    /* Soft horizon bands to add depth. */
    for (int y = 0; y < (int)SCREEN_H; y += 16) {
        uint8_t band = (uint8_t)(10 + (y / 16));
        vga_fill_rect(0, y, (int)SCREEN_W, 1, rgb((uint8_t)(band / 2), (uint8_t)(12 + band / 3), (uint8_t)(24 + band)));
    }

    /* Fine scanline texture. */
    for (int y = 0; y < (int)SCREEN_H; y += 4) {
        if (((y / 4) & 1) == 0) {
            vga_fill_rect(0, y, (int)SCREEN_W, 1, rgb(6, 12, 24));
        }
    }

    gui_boot_draw_badge(elapsed_ticks);
    gui_boot_draw_starfield((int)elapsed_ticks);
    gui_boot_draw_particle_ring(cx, cy, (int)elapsed_ticks);
    gui_boot_draw_glass_card(cx, cy, elapsed_ticks);

    int logo_shift = (int)((elapsed_ticks / 12u) % 7u) - 3 + title_shift / 4;
    int title_y = cy + 82 + logo_shift;
    gui_boot_draw_centered_scaled(settings_get_os_name(), title_y, 3, rgb(114, 190, 255), rgb(20, 50, 96));
    gui_boot_draw_centered_scaled("Welcome to C-OS", title_y + 38, 1, rgb(234, 244, 254), rgb(22, 40, 64));
    vga_draw_string(gui_boot_center_x("A polished startup sequence for C-OS", 1), title_y + 60,
                    "A polished startup sequence for C-OS", rgb(156, 186, 226), rgb(6, 12, 24));

    gui_boot_draw_progress(elapsed_ticks, total_ticks);
    (void)always_visible;
    gui_boot_draw_status_line(elapsed_ticks, total_ticks);

    int scan_y = (int)((elapsed_ticks * 3u) % (uint64_t)(SCREEN_H ? SCREEN_H : 1));
    vga_fill_rect(0, scan_y, (int)SCREEN_W, 1, rgb(134, 214, 255));
    if (scan_y + 1 < (int)SCREEN_H) {
        vga_fill_rect(0, scan_y + 1, (int)SCREEN_W, 1, rgb(34, 78, 126));
    }

    /* Edge vignette made from simple strips for a more cinematic feel. */
    vga_fill_rect(0, 0, (int)SCREEN_W, 10, rgb(2, 4, 10));
    vga_fill_rect(0, (int)SCREEN_H - 10, (int)SCREEN_W, 10, rgb(2, 4, 10));
    vga_fill_rect(0, 0, 10, (int)SCREEN_H, rgb(2, 4, 10));
    vga_fill_rect((int)SCREEN_W - 10, 0, 10, (int)SCREEN_H, rgb(2, 4, 10));
}

#define GUI_BOOT_INITIAL_HOLD_TICKS 25u
#define GUI_BOOT_FINAL_HOLD_TICKS 20u

/* External initialization - call once before boot sequence */
void gui_boot_animation_init(void) {
    if (gui_boot_animation_started) return;
    gui_boot_animation_started = true;
    gui_boot_animation_done = false;
    gui_boot_animation_result = 0;
    gui_boot_current_phase = GUI_BOOT_PHASE_INIT;
    gui_boot_init_stars();
    serial_puts("[BOOT] Boot animation module initialized\n");
}

void gui_boot_animation_run(void) {
    /* Boot animation is a required part of startup and is always executed. */
    uint64_t seed = gui_boot_seed();
    uint64_t total_ticks = 2000u + (seed % 3001u);  /* guaranteed 2000..5000 ticks (2-5 seconds at 1000 Hz) */
    if (total_ticks < GUI_BOOT_VISIBLE_MIN_TICKS) {
        total_ticks = GUI_BOOT_VISIBLE_MIN_TICKS;
    }
    if (total_ticks > 5000u) {
        total_ticks = 5000u;
    }

    gui_boot_sti();

    /*
     * Always show a very simple splash first. This keeps the startup
     * experience visible even if the richer scene is later obscured by
     * platform-specific framebuffer quirks.
     */
    serial_puts("[BOOT] Showing early splash screen\n");
    if (gui_boot_graphics_ready()) {
        gui_boot_draw_simple_splash();
    } else {
        gui_boot_text_frame(0, 420u);
        gui_boot_serial_frame(0, GUI_BOOT_VISIBLE_MIN_TICKS);
    }
    timer_wait(60u);

    /*
     * Hold the first rich frame long enough for humans to notice it, even on
     * very fast boots where the rest of startup completes quickly.
     */
    if (gui_boot_graphics_ready()) {
        gui_boot_draw_scene(0, total_ticks, true);
        vga_wait_vblank();
        vga_flip();
    } else {
        gui_boot_text_frame(0, total_ticks);
        gui_boot_serial_frame(0, total_ticks);
    }
    timer_wait(GUI_BOOT_INITIAL_HOLD_TICKS);

    uint64_t start = get_timer_ticks();
    while (1) {
        uint64_t elapsed = get_timer_ticks() - start;
        if (elapsed >= total_ticks) {
            elapsed = total_ticks;
        }

        if (gui_boot_graphics_ready()) {
            gui_boot_draw_scene(elapsed, total_ticks, true);
            vga_wait_vblank();
            vga_flip();
        } else {
            gui_boot_text_frame(elapsed, total_ticks);
            gui_boot_serial_frame(elapsed, total_ticks);
        }

        if (elapsed >= total_ticks) {
            break;
        }

        gui_boot_frame_delay(elapsed);
    }

    if (gui_boot_graphics_ready()) {
        vga_fill_rect(0, 0, (int)SCREEN_W, (int)SCREEN_H, rgb(5, 10, 22));
        gui_boot_draw_centered_scaled("C-OS", (int)SCREEN_H / 2 - 40, 3, rgb(104, 182, 255), rgb(20, 50, 96));
        gui_boot_draw_centered_scaled("Welcome to C-OS", (int)SCREEN_H / 2 + 16, 1, rgb(230, 240, 252), rgb(22, 40, 64));
        vga_draw_string(gui_boot_center_x("Launching the desktop...", 1), (int)SCREEN_H / 2 + 50,
                        "Launching the desktop...", rgb(162, 196, 238), rgb(5, 10, 22));
        vga_draw_string(24, (int)SCREEN_H - 100,
                        "Startup sequence complete",
                        rgb(182, 214, 250), rgb(5, 10, 22));
        vga_wait_vblank();
        vga_flip();
        timer_wait(GUI_BOOT_FINAL_HOLD_TICKS);
    } else {
        gui_boot_text_frame(total_ticks, total_ticks);
        gui_boot_serial_frame(total_ticks, total_ticks);
        serial_puts("\r[BOOT] "); serial_puts(settings_get_os_name()); serial_puts(" complete\n");
    }
    gui_boot_animation_done = true;
    gui_boot_animation_result = 1;
    gui_boot_current_phase = GUI_BOOT_PHASE_COMPLETE;
    serial_puts("[BOOT] Boot animation completed successfully\n");
}

/* Check if boot animation is still running */
bool gui_boot_is_animating(void) {
    return !gui_boot_animation_done;
}

/* Get animation result: 0=running, 1=success, -1=error */
int gui_boot_get_result(void) {
    return gui_boot_animation_result;
}

/* Force complete animation (for error recovery) */
void gui_boot_force_complete(void) {
    if (!gui_boot_animation_done) {
        serial_puts("[BOOT] Forcing boot animation completion\n");
        gui_boot_animation_done = true;
        gui_boot_animation_result = 1;
        gui_boot_current_phase = GUI_BOOT_PHASE_COMPLETE;
    }
}

/* Reset animation state for reboot */
void gui_boot_animation_reset(void) {
    gui_boot_animation_done = false;
    gui_boot_animation_started = false;
    gui_boot_animation_result = 0;
    gui_boot_current_phase = GUI_BOOT_PHASE_INIT;
    gui_boot_stars_ready = false;
}

