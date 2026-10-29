/* bootsplash.c-os - the C-OS boot splash, as a real ring-3 program.
 *
 * Launched once by the kernel right after the window system comes up (see
 * src/kernel/cos_bootsplash.c), drawn with the same vector toolkit (cos_ui) every other
 * C-OS program uses rather than hand-plotted kernel-side pixels - real anti-aliased text,
 * a gradient, a smooth animation, at the screen's own resolution instead of a fixed
 * low-res VGA mode. It runs for a fixed short duration and then exits on its own; the
 * kernel treats that exit as "boot animation complete" and reveals the desktop
 * (gui_boot_animation_completed(), unchanged from the caller's point of view).
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include "cos.h"
#include "cos_ui.h"

#define DURATION_MS   2200
#define FADE_MS        350

static uint32_t mix(uint32_t a, uint32_t b, int t) { return cui_mix(a, b, t < 0 ? 0 : t > 255 ? 255 : t); }
static const uint8_t splash_glyphs[37][7] = {
 {0x1E,0x21,0x21,0x3F,0x21,0x21,0x21},{0x3E,0x21,0x21,0x3E,0x21,0x21,0x3E},{0x1E,0x21,0x20,0x20,0x20,0x21,0x1E},{0x3E,0x21,0x21,0x21,0x21,0x21,0x3E},{0x3F,0x20,0x20,0x3E,0x20,0x20,0x3F},{0x3F,0x20,0x20,0x3E,0x20,0x20,0x20},{0x1E,0x21,0x20,0x2F,0x21,0x21,0x1E},{0x21,0x21,0x21,0x3F,0x21,0x21,0x21},{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},{0x01,0x01,0x01,0x01,0x21,0x21,0x1E},{0x21,0x22,0x24,0x38,0x24,0x22,0x21},{0x20,0x20,0x20,0x20,0x20,0x20,0x3F},{0x21,0x33,0x2D,0x21,0x21,0x21,0x21},{0x21,0x31,0x29,0x25,0x23,0x21,0x21},{0x1E,0x21,0x21,0x21,0x21,0x21,0x1E},{0x3E,0x21,0x21,0x3E,0x20,0x20,0x20},{0x1E,0x21,0x21,0x21,0x25,0x22,0x1D},{0x3E,0x21,0x21,0x3E,0x24,0x22,0x21},{0x1F,0x20,0x20,0x1E,0x01,0x01,0x3E},{0x3F,0x04,0x04,0x04,0x04,0x04,0x04},{0x21,0x21,0x21,0x21,0x21,0x21,0x1E},{0x21,0x21,0x21,0x21,0x21,0x12,0x0C},{0x21,0x21,0x21,0x21,0x2D,0x33,0x21},{0x21,0x21,0x12,0x0C,0x12,0x21,0x21},{0x21,0x21,0x12,0x0C,0x04,0x04,0x04},{0x3F,0x02,0x04,0x08,0x10,0x20,0x3F},
 {0x1E,0x23,0x25,0x29,0x31,0x21,0x1E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x1F},{0x1E,0x21,0x01,0x06,0x18,0x20,0x3F},{0x3E,0x01,0x01,0x1E,0x01,0x01,0x3E},{0x02,0x06,0x0A,0x12,0x3F,0x02,0x02},{0x3F,0x20,0x20,0x3E,0x01,0x01,0x3E},{0x1E,0x20,0x20,0x3E,0x21,0x21,0x1E},{0x3F,0x01,0x02,0x04,0x08,0x08,0x08},{0x1E,0x21,0x21,0x1E,0x21,0x21,0x1E},{0x1E,0x21,0x21,0x1F,0x01,0x01,0x1E},{0,0,0,0,0,0,0}
};
static int splash_index(char ch) { if (ch >= 'A' && ch <= 'Z') return ch-'A'; if (ch >= '0' && ch <= '9') return 26+ch-'0'; return 36; }
static int splash_text_width(const char *s, int scale) { int n=0; while (s && *s++) ++n; return n ? n*6*scale-scale : 0; }
static void splash_text(cui_canvas *c, int x, int y, int scale, const char *s, uint32_t color, int alpha) {
    if (!c || !s || scale < 1) return;
    for (; *s; ++s, x += 6*scale) { const uint8_t *rows=splash_glyphs[splash_index(*s)];
        for (int row=0; row<7; ++row) for (int col=0; col<6; ++col)
            if (rows[row] & (1u << (5-col))) cui_fill_alpha(c,x+col*scale,y+row*scale,scale,scale,color,alpha);
    }
}

int main(void) {
    int32_t sw = 1000, sh = 680;
    cos_win2_screen_size(&sw, &sh);
    if (sw > 1000) sw = 1000;
    if (sh > 680) sh = 680;
    if (sw < 480) sw = 480;
    if (sh < 320) sh = 320;

    cos_win_info_t wi;
    int64_t h = cos_win2_create("C-OS", sw, sh, &wi);
    if (h <= 0) return 1;
    cui_canvas c;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);

    const uint32_t BG_TOP    = 0x141826;
    const uint32_t BG_BOTTOM = 0x0B0E16;
    const uint32_t ACCENT    = 0x4D9FFF;
    const uint32_t ACCENT_DIM= 0x2C3B57;
    const uint32_t TEXT      = 0xF0F3FA;
    const uint32_t MUTED     = 0x7C8598;


    int cx = c.w / 2, cy = c.h / 2;
    uint64_t t0 = cos_time_ms();

    while (1) {
        uint64_t now = cos_time_ms();
        uint64_t elapsed = now - t0;
        if (elapsed > DURATION_MS) break;

        cos_win_event_t ev;
        if (cos_win2_wait(h, &ev, 16) > 0 && ev.type == COS_EV_CLOSE) { cos_win2_close(h); return 0; }

        /* fade in, hold, fade out - alpha 0..255 across the whole scene */
        int alpha = 255;
        if (elapsed < FADE_MS) alpha = (int)(elapsed * 255 / FADE_MS);
        else if (elapsed > DURATION_MS - FADE_MS) alpha = (int)((DURATION_MS - elapsed) * 255 / FADE_MS);

        cui_vgradient(&c, 0, 0, c.w, c.h, BG_TOP, BG_BOTTOM);

        /* the mark: an open ring with a short accent arc "breaking" it, standing in for a
         * logo without needing an image asset - a simple, deliberate geometric identity
         * rather than a placeholder rectangle. */
        float mr = 46.0f;
        int mark_y = cy - 96;
        cui_ring(&c, (float)cx, (float)mark_y, mr, 7.0f, mix(BG_TOP, ACCENT_DIM, alpha));
        double spin = (double)(now % 1600) / 1600.0 * 6.2831853;
        for (int k = 0; k < 3; ++k) {
            double a0 = spin + k * 2.0943951;                 /* 3 short arcs, 120 degrees apart, slowly orbiting */
            for (int s = 0; s < 14; ++s) {
                double a = a0 + s * 0.045;
                float px = (float)cx + mr * (float)cos(a);
                float py = (float)mark_y + mr * (float)sin(a);
                cui_circle(&c, px, py, 3.4f, mix(BG_TOP, ACCENT, alpha));
            }
        }

        splash_text(&c, cx - splash_text_width("C-OS", 6)/2, cy - 30, 6, "C-OS", TEXT, alpha);
        splash_text(&c, cx - splash_text_width("4.1.0", 2)/2, cy + 30, 2, "4.1.0", MUTED, alpha);

        /* three softly pulsing dots beneath, a quiet "still working" cue rather than a
         * determinate progress bar this program has no real progress to report for */
        int dot_y = cy + 96;
        for (int i = 0; i < 3; ++i) {
            double phase = (double)(now % 1200) / 1200.0 * 6.2831853 - i * 1.4;
            int glow = (int)((sin(phase) * 0.5 + 0.5) * 255.0);
            uint32_t col = mix(ACCENT_DIM, ACCENT, glow);
            cui_circle(&c, (float)(cx - 22 + i * 22), (float)dot_y, 4.0f, mix(BG_TOP, col, alpha));
        }

        splash_text(&c, 18, c.h - 24, 2, "STARTING UP", MUTED, alpha * 3 / 4);

        cos_win2_present(h);
    }

    cos_win2_close(h);
    return 0;
}
