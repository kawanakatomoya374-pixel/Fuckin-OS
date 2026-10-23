/* フレームバッファ GUI の描画コア（仮仕様は docs/GUI_FRAMEBUFFER_SPEC_PROVISIONAL.md）。
 * 32 ビット XRGB8888（0x00RRGGBB）、pitch はバイト単位、原点は左上。 */
#ifndef PYTHON_CODE_TO_C_FB_GUI_H
#define PYTHON_CODE_TO_C_FB_GUI_H

#include "common/python_code_to_c_common.h"

#define P2C_GUI_DEFAULT_WIDTH  1024u
#define P2C_GUI_DEFAULT_HEIGHT 768u

typedef struct {
    uint32_t *pixels;   /* 描画先（行の先頭は pitch バイトごと） */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;    /* 1 行のバイト数（4 * width 以上） */
    int       owned;    /* 1 なら内部で確保したメモリ（p2c_fb_close で解放） */
} P2C_Canvas;

/* 幅・高さ・pitch（0 なら width*4）を指定して描画先を作る。失敗・不正値は負の値。 */
int p2c_fb_open_memory(P2C_Canvas *c, uint32_t width, uint32_t height, uint32_t pitch);
/* 既存のフレームバッファ（HobbyOS など）を包む。不正値は負の値。 */
int p2c_fb_wrap(P2C_Canvas *c, uint32_t *pixels, uint32_t width, uint32_t height, uint32_t pitch);
void p2c_fb_close(P2C_Canvas *c);

/* クリップ付きの描画。範囲外は黙って切り取る。 */
void p2c_fb_put_pixel(P2C_Canvas *c, int32_t x, int32_t y, uint32_t color);
void p2c_fb_fill_rect(P2C_Canvas *c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
uint32_t p2c_fb_get_pixel(const P2C_Canvas *c, uint32_t x, uint32_t y);

/* 8x16 の仮フォントで 1 文字を描く（背景は描かない＝透明）。ASCII 以外は '?' で描く。 */
void p2c_fb_draw_char(P2C_Canvas *c, int32_t x, int32_t y, uint8_t ch, uint32_t color);
/* 文字列を左から描く（改行は扱わない。1 文字 8 画素ずつ進む）。 */
void p2c_fb_draw_text(P2C_Canvas *c, int32_t x, int32_t y, const char *text, uint32_t color);
#define P2C_GUI_CHAR_WIDTH  8
#define P2C_GUI_CHAR_HEIGHT 16


/* ── ウィジェット（最小版）: ボタンと格子配置 ── */
typedef struct {
    int32_t  x, y, w, h;   /* 位置と大きさ（画素） */
    const char *label;     /* 表示する文字列（ASCII） */
    uint32_t fg, bg;       /* 文字色と背景色 */
} P2C_Button;

/* 1 個のボタンを描く（枠は背景色と同じ塗りの中央に 1 画素の縁、文字は中央に置く）。 */
void p2c_fb_button_draw(P2C_Canvas *c, const P2C_Button *b);
/* 画素 (px, py) がボタンの内側にあるか。 */
int p2c_fb_button_contains(const P2C_Button *b, int32_t px, int32_t py);
/* 押された位置にあるボタンの添字を返す（重なりは後の方を優先）。無ければ -1。 */
int p2c_fb_click_index(const P2C_Button *buttons, int count, int32_t px, int32_t py);
/* 格子配置: rows x cols のボタンを、左上 (x0, y0) から、セル幅・高さ・間隔で並べる。
 * out は rows*cols 個分の領域。ラベルは labels（行優先、rows*cols 個）。 */
void p2c_fb_grid_layout(P2C_Button *out, int rows, int cols, int32_t x0, int32_t y0,
                         int32_t cell_w, int32_t cell_h, int32_t gap,
                         const char *const *labels, uint32_t fg, uint32_t bg);

#endif
