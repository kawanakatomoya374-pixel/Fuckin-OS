/* notify.c-os - notification toasts, as a ring-3 program.
 *
 * Shell layer 4: a small colour-keyed surface in the top-right corner, drawn by the compositor only while the kernel's
 * notification list is non-empty (the kernel owns the list and expires entries). This program lays the toasts out.
 * Without it the kernel draws them itself.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "cos.h"
#include "cos_ui.h"

static cui_canvas c;

/* chamfered rectangle: plain fills, so nothing anti-aliased meets the key colour */
static void chip(int x, int y, int w, int h, int r, uint32_t col)
{
    cui_fill(&c, x + r, y, w - 2 * r, h, col);
    cui_fill(&c, x, y + r, w, h - 2 * r, col);
    cui_fill(&c, x + 1, y + 1, w - 2, h - 2, col);
}

int main(void)
{
    cos_win_info_t wi;
    int64_t win = cos_shell_create(4, &wi);
    if (win <= 0) return 1;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    cui_font *ft = cui_font_bold(13), *fm = cui_font_default(12);
    cos_notif_t cur[COS_NOTIF_MAX], now[COS_NOTIF_MAX];
    int ncur = -1;
    for (;;) {
        cos_win_event_t ev;
        cos_win2_wait(win, &ev, 100);
        int n = cos_notif_list(now, COS_NOTIF_MAX);
        if (n < 0) n = 0;
        if (n == ncur && memcmp(now, cur, (size_t)n * sizeof(cos_notif_t)) == 0) continue;
        ncur = n;
        memcpy(cur, now, (size_t)n * sizeof(cos_notif_t));
        cui_fill(&c, 0, 0, c.w, c.h, COS_DESKTOP_KEY);
        for (int i = 0; i < n; i++) {
            uint32_t bg, bd;
            switch (cur[i].type) {
            case 2:  bg = 0xB41E14; bd = 0xDC5046; break;     /* error */
            case 1:  bg = 0x8C6400; bd = 0xDCB400; break;     /* warning */
            case 3:  bg = 0x1E8C28; bd = 0x50DC64; break;     /* success */
            default: bg = 0x1E2D4B; bd = 0x5082C8; break;
            }
            int y = i * 60;
            chip(0, y, 300, 52, 6, bd);
            chip(1, y + 1, 298, 50, 5, bg);
            cui_circle(&c, 22, y + 26, 10, bd);
            cui_text_center(&c, ft, 12, y + 16, 20, 20, cur[i].type == 2 ? "!" : cur[i].type == 1 ? "?" : "i", 0xFFFFFF);
            cui_text_fit(&c, ft, 40, y + 8, 250, cur[i].title, 0xB4D2FF);
            cui_text_fit(&c, fm, 40, y + 28, 250, cur[i].message, 0xDCE6F5);
        }
        cos_win2_present(win);
    }
}
