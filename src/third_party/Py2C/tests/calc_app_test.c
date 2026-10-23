/* 電卓アプリの単体テスト: レイアウト（480p・1024x768・FHD）、クリック、キー入力、描画の書き出し。 */
#include <stdio.h>
#include <string.h>

#include "gui/python_code_to_c_calc_app.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s\n", msg); failures++; } else { printf("ok   %s\n", msg); } } while (0)

static void click_at(P2C_CalcApp *app, P2C_Button *b) {
    P2C_Event ev = { P2C_EV_MOUSE_DOWN, b->x + b->w / 2, b->y + b->h / 2, 0 };
    p2c_host_push(&app->host, ev);
    p2c_calc_app_pump(app);
}

static void key_at(P2C_CalcApp *app, char k) {
    P2C_Event ev = { P2C_EV_KEY, 0, 0, k };
    p2c_host_push(&app->host, ev);
    p2c_calc_app_pump(app);
}

static P2C_Button *find_key(P2C_CalcApp *app, const char *label) {
    for (int i = 0; i < 16; i++) if (strcmp(app->keys[i].label, label) == 0) return &app->keys[i];
    return NULL;
}

static int layout_fits(P2C_CalcApp *app) {
    int32_t W = (int32_t)app->host.canvas.width, H = (int32_t)app->host.canvas.height;
    for (int i = 0; i < 16; i++) {
        P2C_Button *b = &app->keys[i];
        if (b->x < 0 || b->y < 0 || b->x + b->w > W || b->y + b->h > H) return 0;
        for (int j = i + 1; j < 16; j++) {
            P2C_Button *o = &app->keys[j];
            int apart = b->x + b->w <= o->x || o->x + o->w <= b->x || b->y + b->h <= o->y || o->y + o->h <= b->y;
            if (!apart) return 0;
        }
    }
    return app->disp_x + app->disp_w <= W && app->disp_y + app->disp_h <= H;
}

static void test_layouts(void) {
    static const uint32_t sizes[3][2] = { {640u, 480u}, {1024u, 768u}, {1920u, 1080u} };
    for (int i = 0; i < 3; i++) {
        P2C_CalcApp app;
        int ok = p2c_calc_app_open(&app, sizes[i][0], sizes[i][1]) == 0 && layout_fits(&app);
        CHECK(ok, "keys and display fit without overlap at 640x480, 1024x768, and 1920x1080");
        p2c_calc_app_close(&app);
    }
}

static void test_clicks_and_keys(void) {
    P2C_CalcApp app;
    CHECK(p2c_calc_app_open(&app, 1024u, 768u) == 0, "open the default 1024x768 calculator");
    p2c_calc_app_render(&app);
    click_at(&app, find_key(&app, "7"));
    CHECK(strcmp(app.calc.entry.text, "7") == 0, "clicking 7 shows 7");
    click_at(&app, find_key(&app, "+"));
    click_at(&app, find_key(&app, "8"));
    click_at(&app, find_key(&app, "="));
    CHECK(strcmp(app.calc.entry.text, "15") == 0, "click 7 + 8 = shows 15");
    click_at(&app, find_key(&app, "C"));
    CHECK(app.calc.entry.len == 0u, "clicking C clears the display");
    key_at(&app, '2'); key_at(&app, '*'); key_at(&app, '3'); key_at(&app, '\n');
    CHECK(strcmp(app.calc.entry.text, "6") == 0, "keyboard 2*3 then Enter shows 6");
    key_at(&app, 27);
    CHECK(app.calc.entry.len == 0u, "Escape clears the display");
    key_at(&app, 'z');
    CHECK(app.calc.entry.len == 0u, "a key the calculator does not use is ignored");
    P2C_Event outside = { P2C_EV_MOUSE_DOWN, 0, 0, 0 };
    p2c_host_push(&app.host, outside);
    p2c_calc_app_pump(&app);
    CHECK(app.calc.entry.len == 0u, "a click outside every key does nothing");
    CHECK(p2c_host_present_ppm(&app.host, "build/calc_app_test.ppm") == 0, "write the screen as a PPM image");
    p2c_calc_app_close(&app);
}

static void test_ppm_file(void) {
    FILE *f = fopen("build/calc_app_test.ppm", "rb");
    CHECK(f != NULL, "PPM file exists");
    if (!f) return;
    char head[32] = {0};
    size_t n = fread(head, 1u, sizeof head - 1u, f);
    fseek(f, 0L, SEEK_END);
    long total = ftell(f);
    fclose(f);
    /* "P6\n1024 768\n255\n" は 16 バイト */
    int header_ok = n >= 16u && strncmp(head, "P6\n1024 768\n255\n", 16) == 0;
    long header_len = 16;
    CHECK(header_ok && total == header_len + 1024L * 768L * 3L, "PPM header and size match 1024x768 RGB");
}

int main(void) {
    test_layouts();
    test_clicks_and_keys();
    test_ppm_file();
    if (failures == 0) printf("calc_app_ok\n");
    return failures == 0 ? 0 : 1;
}
