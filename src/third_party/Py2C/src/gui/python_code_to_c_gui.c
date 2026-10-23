#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <stdlib.h>
#endif

#include "gui/python_code_to_c_gui.h"
#include "gui/python_code_to_c_gui_font8x16.h"

static int p2c_fb_check_dims(uint32_t width, uint32_t height, uint32_t pitch) {
    if (width == 0u || height == 0u) return -1;
    if (width > 16384u || height > 16384u) return -1;
    if (pitch < width * 4u) return -1;          /* 行は少なくとも画素分のバイトが必要 */
    if (pitch % 4u != 0u) return -1;            /* 32 ビット画素の境界 */
    return 0;
}

int p2c_fb_open_memory(P2C_Canvas *c, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!c) return -1;
    if (pitch == 0u) pitch = width * 4u;
    if (p2c_fb_check_dims(width, height, pitch) != 0) return -1;
#ifdef PYTHON_CODE_TO_C_NO_STDLIB
    (void)pitch;
    return -1;   /* フリースタンディングではヒープを使わない（描画先は p2c_fb_wrap で包む） */
#else
    size_t bytes = (size_t)pitch * (size_t)height;
    uint32_t *px = (uint32_t*)calloc(1u, bytes);
    if (!px) return -1;
    c->pixels = px;
    c->width = width;
    c->height = height;
    c->pitch = pitch;
    c->owned = 1;
    return 0;
#endif
}

int p2c_fb_wrap(P2C_Canvas *c, uint32_t *pixels, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!c || !pixels) return -1;
    if (p2c_fb_check_dims(width, height, pitch) != 0) return -1;
    c->pixels = pixels;
    c->width = width;
    c->height = height;
    c->pitch = pitch;
    c->owned = 0;
    return 0;
}

void p2c_fb_close(P2C_Canvas *c) {
    if (!c) return;
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
    if (c->owned) free(c->pixels);
#endif
    c->pixels = NULL;
    c->width = 0u;
    c->height = 0u;
    c->pitch = 0u;
    c->owned = 0;
}

/* 行の先頭（pitch は 4 の倍数と検査済みなので、32 ビット配列の添字で求める。キャストは使わない） */
static uint32_t *p2c_fb_row(const P2C_Canvas *c, uint32_t y) {
    return c->pixels + (size_t)y * (c->pitch / 4u);
}

void p2c_fb_put_pixel(P2C_Canvas *c, int32_t x, int32_t y, uint32_t color) {
    if (!c || !c->pixels) return;
    if (x < 0 || y < 0) return;
    if ((uint32_t)x >= c->width || (uint32_t)y >= c->height) return;
    p2c_fb_row(c, (uint32_t)y)[x] = color & 0x00FFFFFFu;
}

uint32_t p2c_fb_get_pixel(const P2C_Canvas *c, uint32_t x, uint32_t y) {
    if (!c || !c->pixels || x >= c->width || y >= c->height) return 0u;
    return p2c_fb_row(c, y)[x];
}

void p2c_fb_fill_rect(P2C_Canvas *c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!c || !c->pixels || w <= 0 || h <= 0) return;
    /* クリップ矩形 [0, width) x [0, height) との共通部分を求める */
    int64_t x0 = x, y0 = y, x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int64_t)c->width) x1 = (int64_t)c->width;
    if (y1 > (int64_t)c->height) y1 = (int64_t)c->height;
    if (x0 >= x1 || y0 >= y1) return;
    uint32_t px = color & 0x00FFFFFFu;
    for (int64_t j = y0; j < y1; j++) {
        uint32_t *row = p2c_fb_row(c, (uint32_t)j);
        for (int64_t i = x0; i < x1; i++) row[i] = px;
    }
}

void p2c_fb_draw_char(P2C_Canvas *c, int32_t x, int32_t y, uint8_t ch, uint32_t color) {
    if (!c || !c->pixels) return;
    uint8_t index = (ch >= P2C_GUI_FONT_FIRST && ch < P2C_GUI_FONT_FIRST + P2C_GUI_FONT_COUNT)
                        ? (uint8_t)(ch - P2C_GUI_FONT_FIRST)
                        : (uint8_t)('?' - P2C_GUI_FONT_FIRST);
    for (int32_t row = 0; row < P2C_GUI_CHAR_HEIGHT; row++) {
        uint8_t bits = p2c_fb_font8x16[index][row];
        for (int32_t col = 0; col < P2C_GUI_CHAR_WIDTH; col++) {
            if (bits & (uint8_t)(0x80u >> col)) p2c_fb_put_pixel(c, x + col, y + row, color);
        }
    }
}

void p2c_fb_draw_text(P2C_Canvas *c, int32_t x, int32_t y, const char *text, uint32_t color) {
    if (!c || !c->pixels || !text) return;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        p2c_fb_draw_char(c, x, y, *p, color);
        x += P2C_GUI_CHAR_WIDTH;
    }
}

void p2c_fb_button_draw(P2C_Canvas *c, const P2C_Button *b) {
    if (!c || !c->pixels || !b || b->w <= 0 || b->h <= 0) return;
    p2c_fb_fill_rect(c, b->x, b->y, b->w, b->h, b->bg);
    /* 1 画素の縁（文字色で囲む） */
    p2c_fb_fill_rect(c, b->x, b->y, b->w, 1, b->fg);
    p2c_fb_fill_rect(c, b->x, b->y + b->h - 1, b->w, 1, b->fg);
    p2c_fb_fill_rect(c, b->x, b->y, 1, b->h, b->fg);
    p2c_fb_fill_rect(c, b->x + b->w - 1, b->y, 1, b->h, b->fg);
    if (b->label) {
        int32_t len = 0;
        while (b->label[len]) len++;
        int32_t tw = len * P2C_GUI_CHAR_WIDTH;
        int32_t tx = b->x + (b->w - tw) / 2;
        int32_t ty = b->y + (b->h - P2C_GUI_CHAR_HEIGHT) / 2;
        /* 文字は背景色の上に描く（透明だと、ボタンの地の色の上に残るため塗り直す） */
        p2c_fb_fill_rect(c, tx, ty, tw, P2C_GUI_CHAR_HEIGHT, b->bg);
        p2c_fb_draw_text(c, tx, ty, b->label, b->fg);
    }
}

int p2c_fb_button_contains(const P2C_Button *b, int32_t px, int32_t py) {
    if (!b || b->w <= 0 || b->h <= 0) return 0;
    return px >= b->x && px < b->x + b->w && py >= b->y && py < b->y + b->h;
}

int p2c_fb_click_index(const P2C_Button *buttons, int count, int32_t px, int32_t py) {
    for (int i = count - 1; i >= 0; i--) {
        if (p2c_fb_button_contains(&buttons[i], px, py)) return i;
    }
    return -1;
}

void p2c_fb_grid_layout(P2C_Button *out, int rows, int cols, int32_t x0, int32_t y0,
                         int32_t cell_w, int32_t cell_h, int32_t gap,
                         const char *const *labels, uint32_t fg, uint32_t bg) {
    if (!out || rows <= 0 || cols <= 0) return;
    for (int r = 0; r < rows; r++) {
        for (int k = 0; k < cols; k++) {
            P2C_Button *b = &out[r * cols + k];
            b->x = x0 + (int32_t)k * (cell_w + gap);
            b->y = y0 + (int32_t)r * (cell_h + gap);
            b->w = cell_w;
            b->h = cell_h;
            b->label = labels ? labels[r * cols + k] : NULL;
            b->fg = fg;
            b->bg = bg;
        }
    }
}
