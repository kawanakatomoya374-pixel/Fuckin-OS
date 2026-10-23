/* 電卓の部品: 入力欄、四則演算の式評価（eval を使わない）、キー押下の処理。
 * 数値は double。整数の結果は整数として、小数は最大 9 桁で表示する（libm に依存しない）。 */
#ifndef PYTHON_CODE_TO_C_CALC_H
#define PYTHON_CODE_TO_C_CALC_H

#include "common/python_code_to_c_common.h"

#include "gui/python_code_to_c_gui.h"

#define P2C_ENTRY_CAPACITY 63

typedef struct {
    char     text[P2C_ENTRY_CAPACITY + 1];
    uint32_t len;
} P2C_Entry;

void p2c_entry_clear(P2C_Entry *e);
int  p2c_entry_insert(P2C_Entry *e, char ch);   /* 容量を超えたら 0 を返して無視 */
void p2c_entry_backspace(P2C_Entry *e);
void p2c_entry_set(P2C_Entry *e, const char *s);
/* 入力欄を枠付きで描く（文字は左詰め、枠の内側に背景色を塗る）。 */
void p2c_entry_draw(P2C_Canvas *c, int32_t x, int32_t y, int32_t w, int32_t h,
                    const P2C_Entry *e, uint32_t fg, uint32_t bg);

/* 式を評価する。成功は 0（*out に結果）、誤りは負の値。
 * 対応: 数値（小数点可）、+ - * /、単項のマイナス、括弧、空白。0 除算は誤り。 */
int p2c_calc_eval(const char *expr, double *out);
/* 数値を表示用の文字列にする。成功は 0。範囲外（1e15 以上）は負の値。 */
int p2c_calc_format(double value, char *out, size_t cap);

/* 電卓の状態。 */
typedef struct {
    P2C_Entry entry;
    int       error;        /* 直前の「=」が誤りだったら 1（次のキーで入力を消す） */
} P2C_Calc;

void p2c_calc_init(P2C_Calc *calc);
/* キーを 1 回押したときの処理。key は 0〜9、小数点、括弧、四則演算の記号、
 * "C"（全消去）、"="（計算）、"B"（1 文字消去）。 */
void p2c_calc_press(P2C_Calc *calc, const char *key);

#endif
