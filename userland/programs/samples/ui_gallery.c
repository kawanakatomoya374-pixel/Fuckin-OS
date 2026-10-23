#include <stdio.h>
#include "cos.h"
#include "cos_ui.h"
int main(void) {
    cos_win_info_t wi;
    int64_t h = cos_win2_create("cos_ui gallery", 520, 330, &wi);
    if (h <= 0) return 1;
    cui_canvas c; cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    cui_fill(&c, 0, 0, c.w, c.h, cui_rgb(246, 247, 250));
    cui_vgradient(&c, 0, 0, c.w, 64, cui_rgb(58, 96, 196), cui_rgb(92, 140, 232));
    cui_text(&c, cui_font_bold(22), 20, 12, "Anti-aliased UI on C-OS", 0xFFFFFF);
    cui_text(&c, cui_font_default(13), 22, 40, "TrueType text (stb_truetype) \xE2\x80\x94 Inter", cui_rgb(225, 234, 255));
    int y = 80;
    int sizes[] = { 11, 13, 16, 20 };
    for (int i = 0; i < 4; ++i) {
        char buf[64]; snprintf(buf, sizeof buf, "%dpx  The quick brown fox jumps over 0123", sizes[i]);
        cui_text(&c, cui_font_default(sizes[i]), 20, y, buf, cui_rgb(30, 34, 44));
        y += cui_font_height(cui_font_default(sizes[i])) + 2;
    }
    /* buttons */
    cui_round_rect(&c, 20, 200, 120, 36, 10, cui_rgb(58, 110, 230));
    cui_text_center(&c, cui_font_bold(14), 20, 200, 120, 36, "Primary", 0xFFFFFF);
    cui_round_rect(&c, 152, 200, 120, 36, 10, cui_rgb(228, 231, 238));
    cui_round_rect_outline(&c, 152, 200, 120, 36, 10, 1.0f, cui_rgb(190, 196, 210));
    cui_text_center(&c, cui_font_default(14), 152, 200, 120, 36, "Secondary", cui_rgb(40, 44, 56));
    /* circles, ring, play triangle, lines */
    cui_circle(&c, 320, 218, 18, cui_rgb(58, 110, 230));
    cui_triangle(&c, 314, 208, 314, 228, 330, 218, 0xFFFFFF);
    cui_ring(&c, 370, 218, 18, 3, cui_rgb(230, 80, 90));
    for (int i = 0; i < 6; ++i) cui_line(&c, 410, 250, 410 + 90 * (i + 1) / 6.0f, 200, 1.5f, cui_rgb(40, 150, 110));
    /* translucent overlay + slider */
    cui_round_rect(&c, 20, 260, 480, 8, 4, cui_rgb(214, 218, 228));
    cui_round_rect(&c, 20, 260, 300, 8, 4, cui_rgb(58, 110, 230));
    cui_circle(&c, 320, 264, 9, 0xFFFFFF);
    cui_ring(&c, 320, 264, 9, 1.2f, cui_rgb(58, 110, 230));
    cui_round_rect_alpha(&c, 300, 285, 200, 34, 8, cui_rgb(20, 24, 32), 170);
    cui_text_fit(&c, cui_font_default(13), 310, 294, 180, "A very long track title that must be truncated", 0xFFFFFF);
    cos_win2_present(h);
    printf("[uigallery] drawn\n");
    cos_win_event_t ev;
    for (int t = 0; t < 3000; ++t) { if (cos_win2_wait(h, &ev, 10) == 1 && ev.type == COS_EV_CLOSE) break; }
    cos_win2_close(h);
    return 0;
}
