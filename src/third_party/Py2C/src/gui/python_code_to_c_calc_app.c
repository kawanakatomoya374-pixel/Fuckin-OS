#include <string.h>

#include "gui/python_code_to_c_calc_app.h"

static const char *const p2c_calc_labels[16] = {
    "7", "8", "9", "/",
    "4", "5", "6", "*",
    "1", "2", "3", "-",
    "0", "C", "=", "+"
};

#define P2C_CALC_BG 0x00101010u
#define P2C_CALC_FG 0x00F0F0F0u
#define P2C_CALC_KEY_BG 0x00303840u

int p2c_calc_app_open(P2C_CalcApp *app, uint32_t width, uint32_t height) {
    if (!app) return -1;
    if (p2c_host_open(&app->host, width, height) != 0) return -1;
    p2c_calc_init(&app->calc);
    p2c_calc_app_layout(app);
    return 0;
}

int p2c_calc_app_open_framebuffer(P2C_CalcApp *app) {
    if (!app) return -1;
    P2C_HobbyFbInfo info;
    if (p2c_hobby_fb_get(&info) != 0) return -1;
    if (p2c_host_wrap(&app->host, info.pixels, info.width, info.height, info.pitch) != 0) return -1;
    p2c_calc_init(&app->calc);
    p2c_calc_app_layout(app);
    return 0;
}

void p2c_calc_app_close(P2C_CalcApp *app) {
    if (!app) return;
    p2c_host_close(&app->host);
}

void p2c_calc_app_layout(P2C_CalcApp *app) {
    if (!app) return;
    int32_t W = (int32_t)app->host.canvas.width;
    int32_t H = (int32_t)app->host.canvas.height;
    /* 余白は画面の 1/16（最低 4 画素）、キーは 4 列 4 行が収まる大きさにする */
    int32_t margin = W / 16 < 4 ? 4 : W / 16;
    int32_t gap = margin / 2 < 2 ? 2 : margin / 2;
    int32_t cell_w = (W - 2 * margin - 3 * gap) / 4;
    int32_t disp_h = H / 6 < 24 ? 24 : H / 6;
    int32_t cell_h = (H - 3 * margin - disp_h - 3 * gap) / 4;
    if (cell_w < 8) cell_w = 8;
    if (cell_h < 8) cell_h = 8;
    app->disp_x = margin;
    app->disp_y = margin;
    app->disp_w = 4 * cell_w + 3 * gap;
    app->disp_h = disp_h;
    int32_t y0 = margin + disp_h + margin;
    for (int r = 0; r < 4; r++) {
        for (int k = 0; k < 4; k++) {
            P2C_Button *b = &app->keys[r * 4 + k];
            b->x = margin + (int32_t)k * (cell_w + gap);
            b->y = y0 + (int32_t)r * (cell_h + gap);
            b->w = cell_w;
            b->h = cell_h;
            b->label = p2c_calc_labels[r * 4 + k];
            b->fg = P2C_CALC_FG;
            b->bg = P2C_CALC_KEY_BG;
        }
    }
}

void p2c_calc_app_render(P2C_CalcApp *app) {
    if (!app) return;
    P2C_Canvas *cv = &app->host.canvas;
    p2c_fb_fill_rect(cv, 0, 0, (int32_t)cv->width, (int32_t)cv->height, P2C_CALC_BG);
    p2c_entry_draw(cv, app->disp_x, app->disp_y, app->disp_w, app->disp_h, &app->calc.entry, P2C_CALC_FG, P2C_CALC_BG);
    for (int i = 0; i < 16; i++) p2c_fb_button_draw(cv, &app->keys[i]);
}

void p2c_calc_app_press_label(P2C_CalcApp *app, const char *label) {
    if (!app || !label || !label[0]) return;
    p2c_calc_press(&app->calc, label);
}

/* キーボードの文字を、電卓のキー名に変える。対応しない文字は 0 を返す（無視）。 */
static char p2c_calc_key_name(char key, char out[2]) {
    out[1] = '\0';
    if ((key >= '0' && key <= '9') || key == '+' || key == '-' || key == '*' || key == '/' ||
        key == '(' || key == ')' || key == '.') {
        out[0] = key;
        return 1;
    }
    if (key == '\n' || key == '\r' || key == '=') { out[0] = '='; return 1; }
    if (key == '\b') { out[0] = 'B'; return 1; }
    if (key == 'c' || key == 'C' || key == 27) { out[0] = 'C'; return 1; }
    return 0;
}

int p2c_calc_app_pump(P2C_CalcApp *app) {
    if (!app) return 0;
    int handled = 0;
    P2C_Event ev;
    while (p2c_host_poll(&app->host, &ev)) {
        handled++;
        if (ev.type == P2C_EV_MOUSE_DOWN) {
            int idx = p2c_fb_click_index(app->keys, 16, ev.x, ev.y);
            if (idx >= 0) p2c_calc_app_press_label(app, app->keys[idx].label);
        } else if (ev.type == P2C_EV_KEY) {
            char name[2];
            if (p2c_calc_key_name(ev.key, name)) p2c_calc_app_press_label(app, name);
        }
    }
    if (handled > 0) p2c_calc_app_render(app);
    return handled;
}
