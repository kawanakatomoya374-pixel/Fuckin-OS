/* HobbyOS のフレームバッファ接続の検査。プラットフォーム関数は、テスト内の偽物で置き換える。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui/python_code_to_c_calc_app.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s\n", msg); failures++; } else { printf("ok   %s\n", msg); } } while (0)

/* 偽のプラットフォーム: テストごとに設定する */
static P2C_HobbyFbInfo fake_info;
static int fake_result;
int p2c_hobby_fb_get(P2C_HobbyFbInfo *out) {
    if (fake_result != 0) return fake_result;
    *out = fake_info;
    return 0;
}

static void press(P2C_CalcApp *app, const char *label) {
    for (int i = 0; i < 16; i++) {
        if (strcmp(app->keys[i].label, label) == 0) {
            P2C_Event ev = { P2C_EV_MOUSE_DOWN, app->keys[i].x + app->keys[i].w / 2, app->keys[i].y + app->keys[i].h / 2, 0 };
            p2c_host_push(&app->host, ev);
            p2c_calc_app_pump(app);
            return;
        }
    }
}

static void run_resolution(uint32_t w, uint32_t h, uint32_t pitch, const char *name) {
    size_t words = (size_t)pitch / 4u * h;
    uint32_t *fb = (uint32_t*)malloc(words * sizeof(uint32_t));
    CHECK(fb != NULL, name);
    if (!fb) return;
    /* 行末の余白（画素の後ろ）を目印で埋める */
    for (size_t i = 0; i < words; i++) fb[i] = 0xDEADBEEFu;
    fake_info.pixels = fb;
    fake_info.width = w;
    fake_info.height = h;
    fake_info.pitch = pitch;
    fake_result = 0;

    P2C_CalcApp app;
    int ok = p2c_calc_app_open_framebuffer(&app) == 0;
    CHECK(ok && app.host.canvas.pixels == fb && app.host.canvas.owned == 0, "the calculator draws straight into the platform framebuffer (no copy)");
    p2c_calc_app_render(&app);
    press(&app, "5");
    press(&app, "*");
    press(&app, "4");
    press(&app, "=");
    CHECK(strcmp(app.calc.entry.text, "20") == 0, "keys work through the framebuffer");
    /* 行の余白（各行の幅の後ろ）は書かれていないこと */
    int pad_untouched = 1;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = w; x < pitch / 4u; x++)
            if (fb[(size_t)y * (pitch / 4u) + x] != 0xDEADBEEFu) pad_untouched = 0;
    CHECK(pad_untouched, "row padding after the visible width is untouched");
    /* 画面の背景が描かれていること（左上は背景色） */
    CHECK(fb[0] == 0x00101010u, "the top-left pixel carries the calculator background");
    p2c_calc_app_close(&app);
    /* close してもプラットフォームのメモリは解放されない（この後も使える） */
    fb[0] = 0x00ABCDEFu;
    CHECK(fb[0] == 0x00ABCDEFu, "closing does not free the platform framebuffer");
    free(fb);
}

static void test_failure(void) {
    fake_result = -1;
    P2C_CalcApp app;
    CHECK(p2c_calc_app_open_framebuffer(&app) < 0, "no framebuffer from the platform is reported as an error");
}

int main(void) {
    run_resolution(640u, 480u, 2560u, "480p framebuffer with 2560-byte pitch");
    run_resolution(1024u, 768u, 4096u, "1024x768 framebuffer with 4096-byte pitch");
    run_resolution(1920u, 1080u, 7680u, "FHD framebuffer with exact pitch");
    test_failure();
    if (failures == 0) printf("calc_hobby_fb_ok\n");
    return failures == 0 ? 0 : 1;
}
