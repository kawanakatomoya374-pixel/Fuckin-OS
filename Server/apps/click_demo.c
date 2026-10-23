/**
 * click_demo.c - the minimal end-to-end proof for cos_win_poll_mouse()
 * (SYS_WIN_POLL_MOUSE): create a real window, draw a rectangle, and
 * react to a real mouse click delivered across the ring3/ring0 syscall
 * boundary - the three steps agreed as the smallest meaningful milestone
 * before attempting to move any real app (File Manager, etc.) to a
 * genuine userspace process. See hello_app.c for the same milestone
 * proven for keyboard input; this is its mouse counterpart.
 *
 * WHAT IT DOES
 * ------------
 * Draws a button-sized rectangle. Clicking inside it toggles its color
 * and increments a counter drawn below it; clicking outside it (but
 * still inside the window) draws a small dot at the click position, to
 * prove the reported coordinates are real client-area coordinates and
 * not just "a click happened somewhere". Right-clicking exits, so the
 * demo can be closed without needing keyboard focus at all - this is
 * deliberately a mouse-only demo.
 */
#include "cos.h"

#define WIN_W 360
#define WIN_H 240
#define BG_COLOR     0x00202030u
#define FG_COLOR     0x00FFFFFFu
#define BTN_OFF      0x00406080u
#define BTN_ON       0x0060C0FFu
#define DOT_COLOR    0x00FF8040u

#define BTN_X 110
#define BTN_Y 90
#define BTN_W 140
#define BTN_H 60

static void draw_count(int64_t win, uint32_t count) {
    cos_win_fill(win, 16, 170, 328, 20, BG_COLOR);
    char digits[12];
    int n = 0;
    uint32_t v = count;
    if (v == 0) digits[n++] = '0';
    while (v > 0) { digits[n++] = (char)('0' + (v % 10)); v /= 10; }
    char line[48];
    int p = 0;
    const char *prefix = "button clicks: ";
    while (prefix[p]) { line[p] = prefix[p]; ++p; }
    for (int i = n - 1; i >= 0; --i) line[p++] = digits[i];
    line[p] = '\0';
    cos_win_text(win, 16, 170, FG_COLOR, BG_COLOR, line);
}

int main(void) {
    int64_t win = cos_win_create("Click Demo (.c-os, real ring3 mouse input)", WIN_W, WIN_H);
    if (win < 0) {
        cos_puts("[click_demo] window creation failed\n");
        return 1;
    }

    cos_win_fill(win, 0, 0, WIN_W, WIN_H, BG_COLOR);
    cos_win_text(win, 16, 16, FG_COLOR, BG_COLOR, "Left-click the button below.");
    cos_win_text(win, 16, 32, FG_COLOR, BG_COLOR, "Click elsewhere to mark that spot. Right-click exits.");
    cos_win_fill(win, BTN_X, BTN_Y, BTN_W, BTN_H, BTN_OFF);

    uint32_t count = 0;
    bool on = false;
    draw_count(win, count);

    for (;;) {
        cos_mouse_event_t ev;
        if (cos_win_poll_mouse(win, &ev)) {
            if (ev.button == COS_MOUSE_BTN_RIGHT) {
                break;
            }
            bool inside = ev.x >= BTN_X && ev.x < BTN_X + BTN_W &&
                          ev.y >= BTN_Y && ev.y < BTN_Y + BTN_H;
            if (inside) {
                on = !on;
                cos_win_fill(win, BTN_X, BTN_Y, BTN_W, BTN_H, on ? BTN_ON : BTN_OFF);
                ++count;
                draw_count(win, count);
            } else if (ev.x >= 0 && ev.x < WIN_W && ev.y >= 0 && ev.y < WIN_H) {
                /* A tiny filled square at the real click position -
                 * proves ev.x/ev.y are genuine client coordinates, not
                 * just a "something was clicked" flag. */
                cos_win_fill(win, ev.x - 2, ev.y - 2, 4, 4, DOT_COLOR);
            }
        }
        cos_sleep_ms(30);
    }

    cos_win_clear(win);
    cos_win_text(win, 16, 16, FG_COLOR, BG_COLOR, "Goodbye!");
    cos_sleep_ms(500);
    return 0;
}
