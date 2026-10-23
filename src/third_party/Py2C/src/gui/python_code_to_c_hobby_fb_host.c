/* ホスト向けの p2c_hobby_fb_get: ホストには HobbyOS のフレームバッファが無いので、常に失敗を返す。
 * HobbyOS のビルドでは、この代わりにプラットフォームの実装を使う（この file は含めない）。 */
#include "gui/python_code_to_c_hobby_fb.h"

int p2c_hobby_fb_get(P2C_HobbyFbInfo *out) {
    (void)out;
    return -1;
}
