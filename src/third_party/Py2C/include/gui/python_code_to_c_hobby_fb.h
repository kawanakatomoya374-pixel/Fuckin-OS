/* HobbyOS 側のフレームバッファの接続点（仮仕様 docs/GUI_FRAMEBUFFER_SPEC_PROVISIONAL.md の 3 節）。
 * プラットフォーム（HobbyOS）が p2c_hobby_fb_get を実装する。Py2C は確保せず、そのメモリを包むだけ。 */
#ifndef PYTHON_CODE_TO_C_HOBBY_FB_H
#define PYTHON_CODE_TO_C_HOBBY_FB_H

#include <stdint.h>

typedef struct {
    uint32_t *pixels;   /* フレームバッファの先頭（XRGB8888） */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;    /* 1 行のバイト数（4 の倍数、4*width 以上） */
} P2C_HobbyFbInfo;

/* 0 で成功。失敗（フレームバッファが無いなど）は負の値。 */
int p2c_hobby_fb_get(P2C_HobbyFbInfo *out);

#endif
