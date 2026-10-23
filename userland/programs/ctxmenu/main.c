/* ctxmenu.c-os - context menu renderer, as a ring-3 program.
 *
 * Shell layer 5: a screen-sized, colour-keyed surface the compositor draws over everything but the cursor, only while a
 * context menu is open. The kernel still owns the menu's content, hit-testing and actions - clicks never come here; this
 * program only draws the description (cos_ctx_list) the kernel publishes, geometry and hover included. Icons are drawn by
 * the compositor at the positions given with cos_shell_icons() (kind 1000 + icon id). Each frame carries the snapshot's seq
 * as its tag: the kernel switches from its own painter to this frame once the tag matches the menu that is open.
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
    int64_t win = cos_shell_create(5, &wi);
    if (win <= 0) return 1;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    cui_font *f = cui_font_default(13), *fb = cui_font_bold(13);
    static cos_ctxsnap_t s, last;
    int32_t last_seq = 0;
    uint8_t last_hover[48];
    memset(last_hover, 0xFF, sizeof last_hover);

    for (;;) {
        cos_win_event_t ev;
        cos_win2_wait(win, &ev, 40);
        if (!cos_ctx_list(&s) || s.n < 0) { last_seq = 0; continue; }
        uint8_t hv[48];
        for (int i = 0; i < 32; i++) hv[i] = s.it[i].hover;
        for (int i = 0; i < 16; i++) hv[32 + i] = s.sm[i].hover;
        if (s.seq == last_seq && memcmp(hv, last_hover, sizeof hv) == 0) continue;
        last_seq = s.seq; memcpy(last_hover, hv, sizeof hv);

        cui_fill(&c, 0, 0, c.w, c.h, COS_DESKTOP_KEY);
        cos_shell_icon_t ic[COS_SHELL_MAX_ICONS];
        int ni = 0;
        chip(s.x + 4, s.y + 5, s.w, s.h, 8, 0x0A101C);
        chip(s.x, s.y, s.w, s.h, 8, 0xBCCAE2);
        chip(s.x + 1, s.y + 1, s.w - 2, s.h - 2, 7, 0xFAFCFF);
        cui_fill(&c, s.x + 2, s.y + 1, s.w - 4, 27, 0xECF2FB);
        cui_fill(&c, s.x + 1, s.y + 27, s.w - 2, 1, 0xCEDBEE);
        cui_text_fit(&c, fb, s.x + 8, s.y + 6, s.w - 16, s.header, 0x243656);
        for (int i = 0; i < s.n; i++) {
            if (s.it[i].sep) { cui_fill(&c, s.x + 12, s.it[i].y + s.it[i].h / 2, s.w - 24, 1, 0xDFE7F2); continue; }
            bool hov = s.it[i].hover && s.it[i].enabled;
            if (hov) chip(s.x + 6, s.it[i].y + 2, s.w - 12, s.it[i].h - 4, 5, 0xDEEAFC);
            if (ni < COS_SHELL_MAX_ICONS) ic[ni++] = (cos_shell_icon_t){ 1000 + s.it[i].icon, s.x + 9, s.it[i].y + 6, 16 };
            cui_text_fit(&c, f, s.x + 30, s.it[i].y + 8, s.w - 70, s.it[i].label, !s.it[i].enabled ? 0xA8B0BC : hov ? 0x1A325C : 0x2A3A54);
            if (s.it[i].sub) cui_text(&c, f, s.x + s.w - 22, s.it[i].y + 8, ">", hov ? 0x5A8CDC : 0x7A96BE);
            if (s.it[i].checked) chip(s.x + s.w - 36, s.it[i].y + 12, 8, 8, 2, 0x3C965A);
        }
        if (s.sm_visible) {
            chip(s.sm_x + 4, s.sm_y + 5, s.sm_w, s.sm_h, 8, 0x0A101C);
            chip(s.sm_x, s.sm_y, s.sm_w, s.sm_h, 8, 0xBCCAE2);
            chip(s.sm_x + 1, s.sm_y + 1, s.sm_w - 2, s.sm_h - 2, 7, 0xFCFDFF);
            cui_fill(&c, s.sm_x + 2, s.sm_y + 1, s.sm_w - 4, 23, 0xECF2FB);
            cui_text_fit(&c, fb, s.sm_x + 10, s.sm_y + 5, s.sm_w - 20, s.sm_title, 0x243656);
            for (int i = 0; i < s.sm_n; i++) {
                bool hov = s.sm[i].hover;
                if (hov) chip(s.sm_x + 4, s.sm[i].y + 2, s.sm_w - 8, s.sm[i].h - 4, 5, 0xDEEAFC);
                cui_text_fit(&c, f, s.sm_x + 12, s.sm[i].y + 8, s.sm_w - 24, s.sm[i].label, hov ? 0x1A325C : 0x2A3A54);
            }
        }
        cos_shell_icons(5, ic, ni, (uint32_t)s.seq);
        cos_win2_present(win);
    }
}
