/**
 * hello_app.c - the first end-to-end demonstration of a REAL, standalone
 * .c-os program owning its own desktop window: drawn by the desktop's
 * own compositor (WIN_COS_APP, src/kernel/cos_app_window.c), interactive
 * via real keyboard input, running as its own isolated ring3 process -
 * not a function baked into the kernel and dispatched by window kind the
 * way Calculator/About/every other built-in app still is.
 *
 * The kernel side of this (five SYS_WIN_* syscalls) and the userland
 * wrappers (cos_win_create/fill/text/clear/poll_key in
 * userland/include/cos.h) both already existed; what was missing was any
 * real program actually using them. This is that program, and the
 * pattern any future app conversion (see Server/README.md) can copy.
 *
 * WHAT IT DOES
 * ------------
 * Opens a small window, draws a static greeting, then loops: on each
 * printable keypress, draws that character large in the middle of the
 * window and increments a counter shown in the corner. This exercises
 * BOTH halves of the app-window surface - drawing (fill_rect + text) and
 * input (poll_key) - not just a static display, since a real interactive
 * app needs both to be useful for anything.
 *
 * cos_win_poll_key() is non-blocking, so the loop pairs it with
 * cos_sleep_ms() for a modest idle cadence - spinning as fast as
 * possible between checks would burn CPU for no benefit, since a human
 * typing cannot produce events faster than this polls for them.
 */
#include "cos.h"

#define BG_COLOR    0x00202030u
#define FG_COLOR    0x00FFFFFFu
#define ACCENT      0x0060A0FFu

int main(void)
{
    int64_t win = cos_win_create("Hello App (.c-os)", 360, 220);
    if (win < 0) {
        cos_puts("[hello_app] window creation failed\n");
        return 1;
    }

    cos_win_fill(win, 0, 0, 360, 220, BG_COLOR);
    cos_win_text(win, 16, 16, FG_COLOR, BG_COLOR,
                "Hello from a real .c-os process!");
    cos_win_text(win, 16, 36, FG_COLOR, BG_COLOR,
                "Type a key - this window is live.");

    uint32_t count = 0;
    char last = ' ';

    for (;;) {
        cos_key_event_t ev;
        if (cos_win_poll_key(win, &ev)) {
            if (ev.ascii >= 32 && ev.ascii < 127) {
                last = ev.ascii;
                ++count;
            } else if (ev.special == COS_KEY_ESC) {
                break;
            }

            /* Redraw only the two lines that actually change - the
             * static greeting above is drawn once and never touched
             * again, matching how a real app should avoid re-queuing
             * work for content that has not changed. */
            cos_win_fill(win, 16, 70, 328, 110, BG_COLOR);

            char big[2] = { last, '\0' };
            cos_win_text(win, 160, 90, ACCENT, BG_COLOR, big);

            char line[48];
            /* No snprintf in this tiny libc surface - build the decimal
             * count by hand rather than pull one in for a single
             * counter display. */
            {
                char digits[12];
                int n = 0;
                uint32_t v = count;
                if (v == 0) digits[n++] = '0';
                while (v > 0) { digits[n++] = (char)('0' + (v % 10)); v /= 10; }
                int p = 0;
                const char *prefix = "keys pressed: ";
                while (prefix[p]) { line[p] = prefix[p]; ++p; }
                for (int i = n - 1; i >= 0; --i) line[p++] = digits[i];
                line[p] = '\0';
            }
            cos_win_text(win, 16, 150, FG_COLOR, BG_COLOR, line);
        }
        cos_sleep_ms(30);
    }

    cos_win_clear(win);
    cos_win_text(win, 16, 16, FG_COLOR, BG_COLOR, "Goodbye!");
    cos_sleep_ms(500);
    return 0;
}
