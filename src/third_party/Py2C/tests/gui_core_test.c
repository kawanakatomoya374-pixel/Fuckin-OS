/* フレームバッファ描画コアの単体テスト（仮仕様の 1〜5 節）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui/python_code_to_c_gui.h"
#include "gui/python_code_to_c_gui_font8x16.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s\n", msg); failures++; } else { printf("ok   %s\n", msg); } } while (0)

static void test_resolutions(void) {
    static const uint32_t sizes[3][2] = { {1024u, 768u}, {640u, 480u}, {1920u, 1080u} };
    for (int i = 0; i < 3; i++) {
        P2C_Canvas c;
        int rc = p2c_fb_open_memory(&c, sizes[i][0], sizes[i][1], 0u);
        CHECK(rc == 0 && c.width == sizes[i][0] && c.height == sizes[i][1] && c.pitch == sizes[i][0] * 4u,
              "memory canvas opens at 1024x768, 640x480, and 1920x1080 with pitch = width*4");
        p2c_fb_close(&c);
    }
}

static void test_fill_and_clip(void) {
    P2C_Canvas c;
    CHECK(p2c_fb_open_memory(&c, 8u, 6u, 0u) == 0, "open 8x6 canvas");
    p2c_fb_fill_rect(&c, 2, 1, 3, 2, 0x00FF0000u);
    CHECK(p2c_fb_get_pixel(&c, 2, 1) == 0x00FF0000u && p2c_fb_get_pixel(&c, 4, 2) == 0x00FF0000u, "fill inside is painted");
    CHECK(p2c_fb_get_pixel(&c, 5, 1) == 0u && p2c_fb_get_pixel(&c, 2, 3) == 0u, "fill does not overrun its rectangle");
    p2c_fb_fill_rect(&c, -3, -2, 6, 4, 0x0000FF00u);
    CHECK(p2c_fb_get_pixel(&c, 0, 0) == 0x0000FF00u && p2c_fb_get_pixel(&c, 2, 1) == 0x0000FF00u && p2c_fb_get_pixel(&c, 3, 0) == 0u,
          "rectangle clipped at the top and left edges");
    p2c_fb_fill_rect(&c, 6, 4, 100, 100, 0x000000FFu);
    CHECK(p2c_fb_get_pixel(&c, 7, 5) == 0x000000FFu && p2c_fb_get_pixel(&c, 5, 4) == 0u, "rectangle clipped at the bottom and right edges");
    p2c_fb_fill_rect(&c, 1, 1, 0, 3, 0x00FFFFFFu);
    p2c_fb_fill_rect(&c, 1, 1, 3, -1, 0x00FFFFFFu);
    CHECK(p2c_fb_get_pixel(&c, 1, 1) == 0x0000FF00u, "zero or negative size paints nothing");
    p2c_fb_fill_rect(&c, 100, 100, 5, 5, 0x00FFFFFFu);
    CHECK(p2c_fb_get_pixel(&c, 7, 5) == 0x000000FFu, "rectangle entirely outside paints nothing");
    p2c_fb_put_pixel(&c, -1, 0, 0x00FFFFFFu);
    p2c_fb_put_pixel(&c, 8, 0, 0x00FFFFFFu);
    p2c_fb_put_pixel(&c, 0, 6, 0x00FFFFFFu);
    CHECK(p2c_fb_get_pixel(&c, 0, 0) == 0x0000FF00u, "out-of-range pixel writes are ignored");
    p2c_fb_put_pixel(&c, 7, 5, 0xAB123456u);
    CHECK(p2c_fb_get_pixel(&c, 7, 5) == 0x00123456u, "the top byte is masked off (XRGB8888)");
    p2c_fb_close(&c);
}

static void test_invalid_and_wrap(void) {
    P2C_Canvas c;
    CHECK(p2c_fb_open_memory(&c, 0u, 10u, 0u) < 0, "zero width is rejected");
    CHECK(p2c_fb_open_memory(&c, 10u, 0u, 0u) < 0, "zero height is rejected");
    CHECK(p2c_fb_open_memory(&c, 10u, 10u, 20u) < 0, "pitch smaller than a row is rejected");
    CHECK(p2c_fb_open_memory(&c, 10u, 10u, 42u) < 0, "pitch that is not a multiple of 4 is rejected");
    uint32_t *buf = (uint32_t*)calloc(1u, 4096u * 480u);   /* pitch * height バイト */
    CHECK(buf != NULL, "allocate padded framebuffer");
    if (buf) {
        CHECK(p2c_fb_wrap(&c, buf, 640u, 480u, 4096u) == 0 && c.pitch == 4096u, "wrap a 640x480 framebuffer with a 4096-byte pitch");
        p2c_fb_fill_rect(&c, 0, 0, 640, 480, 0x00102030u);
        p2c_fb_put_pixel(&c, 639, 479, 0x00405060u);
        CHECK(p2c_fb_get_pixel(&c, 639, 479) == 0x00405060u && p2c_fb_get_pixel(&c, 0, 0) == 0x00102030u,
              "pixels land at the right place when the pitch exceeds the row width");
        /* 行の余白部分には書かれていないこと */
        CHECK(((unsigned char*)buf)[640 * 4] == 0u && ((unsigned char*)buf)[640 * 4 + 1] == 0u, "padding bytes after each row are untouched");
        p2c_fb_close(&c);
        /* wrap した場合は close しても外部のバッファは解放しない */
        free(buf);
    }
    CHECK(p2c_fb_wrap(&c, NULL, 4u, 4u, 0u) < 0, "wrap rejects a null buffer");
}


static void test_text(void) {
    P2C_Canvas c;
    CHECK(p2c_fb_open_memory(&c, 24u, 16u, 0u) == 0, "open 24x16 canvas for text");
    p2c_fb_fill_rect(&c, 0, 0, 24, 16, 0x00101010u);
    p2c_fb_draw_char(&c, 0, 0, 'A', 0x00FFFFFFu);
    const uint8_t *glyph = p2c_fb_font8x16[(int)('A' - P2C_GUI_FONT_FIRST)];
    int match = 1;
    for (uint32_t row = 0; row < 16u; row++)
        for (uint32_t col = 0; col < 8u; col++) {
            int on = (glyph[row] & (0x80u >> col)) != 0;
            uint32_t expect = on ? 0x00FFFFFFu : 0x00101010u;   /* 透明な背景は元の色のまま */
            if (p2c_fb_get_pixel(&c, col, row) != expect) match = 0;
        }
    CHECK(match, "character pixels equal the table bits, background untouched");
    p2c_fb_draw_text(&c, 8, 0, "B", 0x00FF0000u);
    CHECK(p2c_fb_get_pixel(&c, 8, 2) == 0x00FF0000u || p2c_fb_get_pixel(&c, 9, 2) == 0x00FF0000u,
          "second character starts 8 pixels to the right");
    P2C_Canvas q;
    p2c_fb_close(&c);
    CHECK(p2c_fb_open_memory(&c, 16u, 16u, 0u) == 0 && p2c_fb_open_memory(&q, 16u, 16u, 0u) == 0, "open two canvases");
    p2c_fb_draw_char(&c, 0, 0, (uint8_t)0xC3u, 0x00FFFFFFu);
    p2c_fb_draw_char(&q, 0, 0, '?', 0x00FFFFFFu);
    int same = 1;
    for (uint32_t y = 0; y < 16u; y++) for (uint32_t x = 0; x < 16u; x++)
        if (p2c_fb_get_pixel(&c, x, y) != p2c_fb_get_pixel(&q, x, y)) same = 0;
    CHECK(same, "non-ASCII byte is drawn as '?'");
    p2c_fb_draw_text(&q, -4, 9, "AB", 0x0000FF00u);
    p2c_fb_draw_text(&q, 14, -6, "Z", 0x0000FF00u);
    CHECK(1, "text partly outside the canvas is clipped without overrun");
    p2c_fb_close(&q);
    p2c_fb_close(&c);
}


static void test_widgets(void) {
    static const char *const labels[4][4] = {
        {"7", "8", "9", "/"}, {"4", "5", "6", "*"}, {"1", "2", "3", "-"}, {"0", "C", "=", "+"}
    };
    const char *flat[16];
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) flat[r * 4 + k] = labels[r][k];
    P2C_Button btn[16];
    p2c_fb_grid_layout(btn, 4, 4, 10, 20, 40, 30, 4, flat, 0x00FFFFFFu, 0x00303030u);
    CHECK(btn[0].x == 10 && btn[0].y == 20 && btn[5].x == 10 + 44 && btn[5].y == 20 + 34, "grid positions use cell size and gap");
    int hit_ok = 1;
    for (int i = 0; i < 16; i++) {
        int32_t cx = btn[i].x + btn[i].w / 2, cy = btn[i].y + btn[i].h / 2;
        if (p2c_fb_click_index(btn, 16, cx, cy) != i) hit_ok = 0;
    }
    CHECK(hit_ok, "the centre of every key hits that key");
    CHECK(p2c_fb_click_index(btn, 16, btn[0].x + btn[0].w, btn[0].y) == -1, "the gap between keys is not a hit");
    CHECK(p2c_fb_click_index(btn, 16, 0, 0) == -1, "a point outside the grid is not a hit");
    P2C_Button over[2] = { { 0, 0, 10, 10, "a", 0u, 0u }, { 5, 5, 10, 10, "b", 0u, 0u } };
    CHECK(p2c_fb_click_index(over, 2, 6, 6) == 1, "where keys overlap, the later key wins");
    CHECK(p2c_fb_button_contains(&btn[0], btn[0].x, btn[0].y) && !p2c_fb_button_contains(&btn[0], btn[0].x + btn[0].w, btn[0].y),
          "contains includes the top-left and excludes the right edge");

    P2C_Canvas c;
    CHECK(p2c_fb_open_memory(&c, 120u, 60u, 0u) == 0, "open canvas for a button");
    P2C_Button one = { 4, 4, 40, 24, "7", 0x00FFFFFFu, 0x00303030u };
    p2c_fb_button_draw(&c, &one);
    CHECK(p2c_fb_get_pixel(&c, 4, 4) == 0x00FFFFFFu && p2c_fb_get_pixel(&c, 43, 27) == 0x00FFFFFFu, "button border uses the text colour");
    CHECK(p2c_fb_get_pixel(&c, 5, 5) == 0x00303030u && p2c_fb_get_pixel(&c, 40, 6) == 0x00303030u, "button interior uses the background colour");
    int label_pixels = 0;
    for (uint32_t y = 8; y < 24; y++) for (uint32_t x = 16; x < 32; x++)
        if (p2c_fb_get_pixel(&c, x, y) == 0x00FFFFFFu) label_pixels++;
    CHECK(label_pixels > 0, "label is drawn inside the button");
    p2c_fb_close(&c);
}

int main(void) {
    test_resolutions();
    test_fill_and_clip();
    test_invalid_and_wrap();
    test_text();
    test_widgets();
    if (failures == 0) printf("gui_core_ok\n");
    return failures == 0 ? 0 : 1;
}
