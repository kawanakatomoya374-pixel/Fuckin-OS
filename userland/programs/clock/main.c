/* clock.c-os - the Clock app, as a real ring-3 program.
 *
 * A faithful, higher-resolution redraw of the old kernel-space analog clock
 * (src/gui/apps/clock/gui_apps_clock.c) using cos_ui's anti-aliased vector
 * primitives and the Inter font instead of hand-plotted VGA pixels and the
 * fixed-width bitmap font - glowing rings, tick marks, three hands, a digital
 * readout and the date, redrawn every second from cos_rtc_now(). No polling
 * loop burning CPU for no reason: it waits on cos_win2_wait() with a timeout
 * just under a second, so a tick and a repaint happen right as the second
 * actually changes rather than on some arbitrary faster interval.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "cos.h"
#include "cos_ui.h"

int main(void) {
    int32_t sw = 560, sh = 620;
    cos_win_info_t wi;
    int64_t h = cos_win2_create("Clock", sw, sh, &wi);
    if (h <= 0) return 1;
    cui_canvas c;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);

    cui_font *f_num   = cui_font_bold(15);
    cui_font *f_digit = cui_font_bold(28);
    cui_font *f_date  = cui_font_default(15);

    const uint32_t BG_TOP = 0x141826, BG_BOTTOM = 0x0B0E16;
    const uint32_t FACE = 0x171B2A, RING1 = 0x24304A, RING2 = 0x2F4468, RING3 = 0x3E5D8C;
    const uint32_t TICK_MAJOR = 0xD8E4FF, TICK_MINOR = 0x4A5872;
    const uint32_t HOUR_HAND = 0xF0F3FA, MIN_HAND = 0x6FB4FF, SEC_HAND = 0xFF5C5C;
    const uint32_t TEXT = 0xE8ECF5, MUTED = 0x8891A5;
    static const char *const HLBL[12] = { "12","1","2","3","4","5","6","7","8","9","10","11" };

    int last_sec = -1;
    for (;;) {
        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, 250);
        if (r > 0 && ev.type == COS_EV_CLOSE) break;

        cos_datetime_t t;
        cos_rtc_now(&t);
        if ((int)t.second == last_sec) continue;
        last_sec = (int)t.second;

        cui_vgradient(&c, 0, 0, c.w, c.h, BG_TOP, BG_BOTTOM);

        int cx = c.w / 2, cy = c.h / 2 - 28;
        int r_outer = (c.w < c.h ? c.w : c.h) / 2 - 44;
        if (r_outer < 90) r_outer = 90;

        cui_ring(&c, (float)cx, (float)cy, (float)(r_outer + 10), 2.0f, RING1);
        cui_ring(&c, (float)cx, (float)cy, (float)(r_outer + 6), 2.0f, RING2);
        cui_circle(&c, (float)cx, (float)cy, (float)r_outer, FACE);
        cui_ring(&c, (float)cx, (float)cy, (float)r_outer, 2.0f, RING3);

        for (int i = 0; i < 60; ++i) {
            double a = (i * 6 - 90) * 3.14159265 / 180.0;
            bool major = (i % 5) == 0;
            int inner = r_outer - (major ? 16 : 8), outer = r_outer - 4;
            float x1 = (float)(cx + inner * cos(a)), y1 = (float)(cy + inner * sin(a));
            float x2 = (float)(cx + outer * cos(a)), y2 = (float)(cy + outer * sin(a));
            cui_line(&c, x1, y1, x2, y2, major ? 3.0f : 1.5f, major ? TICK_MAJOR : TICK_MINOR);
        }
        for (int i = 0; i < 12; ++i) {
            double a = (i * 30 - 90) * 3.14159265 / 180.0;
            int nr = r_outer - 34;
            int tx = cx + (int)(nr * cos(a)), ty = cy + (int)(nr * sin(a));
            cui_text_center(&c, f_num, tx - 16, ty - 9, 32, 18, HLBL[i], TICK_MAJOR);
        }

        double hour_a = ((t.hour % 12) * 30 + t.minute / 2.0 - 90) * 3.14159265 / 180.0;
        double min_a  = (t.minute * 6 + t.second / 10.0 - 90) * 3.14159265 / 180.0;
        double sec_a  = (t.second * 6 - 90) * 3.14159265 / 180.0;
        int hlen = r_outer * 52 / 100, mlen = r_outer * 78 / 100, slen_ = r_outer * 88 / 100;

        cui_line(&c, (float)cx, (float)cy, (float)(cx + hlen * cos(hour_a)), (float)(cy + hlen * sin(hour_a)), 6.0f, HOUR_HAND);
        cui_line(&c, (float)cx, (float)cy, (float)(cx + mlen * cos(min_a)), (float)(cy + mlen * sin(min_a)), 4.0f, MIN_HAND);
        cui_line(&c, (float)(cx - (slen_ / 6) * cos(sec_a)), (float)(cy - (slen_ / 6) * sin(sec_a)),
                     (float)(cx + slen_ * cos(sec_a)), (float)(cy + slen_ * sin(sec_a)), 1.6f, SEC_HAND);
        cui_circle(&c, (float)cx, (float)cy, 7.0f, TEXT);
        cui_circle(&c, (float)cx, (float)cy, 3.2f, SEC_HAND);

        char digital[16], datebuf[32];
        snprintf(digital, sizeof digital, "%02d:%02d:%02d", t.hour, t.minute, t.second);
        snprintf(datebuf, sizeof datebuf, "%04d-%02d-%02d", t.year, t.month, t.day);
        cui_text_center(&c, f_digit, 0, cy + r_outer + 30, c.w, 34, digital, TEXT);
        cui_text_center(&c, f_date, 0, cy + r_outer + 68, c.w, 22, datebuf, MUTED);

        cos_win2_present(h);
    }
    cos_win2_close(h);
    return 0;
}
