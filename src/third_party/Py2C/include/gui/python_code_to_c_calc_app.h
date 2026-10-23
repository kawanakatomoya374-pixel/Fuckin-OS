/* 電卓アプリ（レイアウト、描画、イベントの処理）。ホストでも HobbyOS でも、同じ処理を使う。 */
#ifndef PYTHON_CODE_TO_C_CALC_APP_H
#define PYTHON_CODE_TO_C_CALC_APP_H

#include "gui/python_code_to_c_calc.h"
#include "gui/python_code_to_c_host.h"
#include "gui/python_code_to_c_hobby_fb.h"

typedef struct {
    P2C_Host   host;          /* 描画先と入力キュー */
    P2C_Calc   calc;          /* 入力欄と計算の状態 */
    P2C_Button keys[16];      /* 4 x 4 のキー（行優先） */
    int32_t    disp_x, disp_y, disp_w, disp_h;   /* 表示欄の位置と大きさ */
} P2C_CalcApp;

int  p2c_calc_app_open(P2C_CalcApp *app, uint32_t width, uint32_t height);
/* HobbyOS のフレームバッファ（p2c_hobby_fb_get）を包んで開く。確保は行わない。 */
int  p2c_calc_app_open_framebuffer(P2C_CalcApp *app);
void p2c_calc_app_close(P2C_CalcApp *app);
/* 画面の大きさに合わせて、表示欄とキーを配置する。 */
void p2c_calc_app_layout(P2C_CalcApp *app);
/* 画面全体を描く。 */
void p2c_calc_app_render(P2C_CalcApp *app);
/* 届いているイベントを処理し、描き直す。処理したイベントの数を返す。 */
int  p2c_calc_app_pump(P2C_CalcApp *app);
/* 1 つのキー名（"7"、"+"、"="、"C" など）を押したのと同じ処理をする。 */
void p2c_calc_app_press_label(P2C_CalcApp *app, const char *label);

#endif
