/* ホスト向けの GUI バックエンド。
 * - 描画先は描画コア（P2C_Canvas）。XRGB8888、pitch はバイト単位。
 * - 画面への出力は、PPM 画像への書き出し（テスト・確認用、どの環境でも動く）と、
 *   SDL2 のウィンドウ（P2C_HAVE_SDL2 を定義した場合のみ。ここではコンパイル未確認）。
 * - 入力イベントは外部から流し込むキューで受ける（ウィンドウの入力もここへ入れる）。 */
#ifndef PYTHON_CODE_TO_C_HOST_H
#define PYTHON_CODE_TO_C_HOST_H

#include <stdint.h>

#include "gui/python_code_to_c_gui.h"

typedef enum {
    P2C_EV_NONE = 0,
    P2C_EV_MOUSE_DOWN = 1,   /* x, y は画面座標 */
    P2C_EV_KEY = 2           /* key は文字（0 なら key_code を見る） */
} P2C_EventType;

typedef struct {
    P2C_EventType type;
    int32_t x, y;
    char key;
} P2C_Event;

#define P2C_HOST_QUEUE_CAPACITY 64

typedef struct {
    P2C_Canvas canvas;
    P2C_Event  queue[P2C_HOST_QUEUE_CAPACITY];
    unsigned   head, count;
} P2C_Host;

/* 描画先を用意する（解像度は仮仕様のとおり。既定は 1024x768）。 */
int  p2c_host_open(P2C_Host *h, uint32_t width, uint32_t height);
/* 既存のフレームバッファを包む（確保しない。close しても解放しない）。 */
int  p2c_host_wrap(P2C_Host *h, uint32_t *pixels, uint32_t width, uint32_t height, uint32_t pitch);
void p2c_host_close(P2C_Host *h);
/* イベントを 1 件積む。満杯なら 0（捨てる）。 */
int  p2c_host_push(P2C_Host *h, P2C_Event ev);
/* イベントを 1 件取り出す。無ければ 0。 */
int  p2c_host_poll(P2C_Host *h, P2C_Event *out);
/* 描画内容を PPM（P6、バイナリ）として書き出す。成功は 0。 */
int  p2c_host_present_ppm(const P2C_Host *h, const char *path);

#endif
