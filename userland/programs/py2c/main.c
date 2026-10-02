#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cos.h"
#include "cos_ui.h"
#include "core/python_code_to_c.h"

#define SRC_CAP 65536
#define OUT_CAP 131072
#define CONFIG_PATH "/desktop/py2c.conf"
static char source[SRC_CAP], output[OUT_CAP], path_buf[256] = "/desktop/untitled.py";
static size_t source_len; static int scroll, active_pane, dirty, settings_open;
static int ui_mode = 1; /* 1 = C-OS Studio, 0 = classic */
static char status[180] = "Ready - F5 / Ctrl+Enter converts Python to C";
static void set_status(const char *s) { snprintf(status, sizeof status, "%s", s); }
static void load_config(void) {
    FILE *f = fopen(CONFIG_PATH, "rb"); char b[16] = {0};
    if (f) { fread(b, 1, sizeof b - 1, f); fclose(f); if (b[0] == '0') ui_mode = 0; }
}
static void save_config(void) {
    FILE *f = fopen(CONFIG_PATH, "wb"); if (!f) return;
    fprintf(f, "%d\n", ui_mode); fclose(f);
}
static int load_file(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) { set_status("Could not open file"); return 0; }
    source_len = fread(source, 1, SRC_CAP - 1, f); fclose(f); source[source_len] = 0;
    snprintf(path_buf, sizeof path_buf, "%s", p); dirty = 0; scroll = 0; set_status("File opened"); return 1;
}
static void save_text(const char *p, const char *text, size_t n) {
    FILE *f = fopen(p, "wb"); if (!f) { set_status("Save failed"); return; }
    fwrite(text, 1, n, f); fclose(f); set_status("Generated C saved to /desktop/generated.c");
}
static void convert_source(void) {
    char *generated = NULL; P2C_Result r = python_to_c(source, NULL, &generated);
    if (r == P2C_OK && generated) {
        size_t n = strlen(generated); if (n >= OUT_CAP) n = OUT_CAP - 1;
        memcpy(output, generated, n); output[n] = 0; free(generated);
        set_status("Conversion succeeded - generated.c is ready"); active_pane = 1;
    } else {
        const char *e = p2c_last_error_details();
        snprintf(output, sizeof output, "Py2C conversion error (%s)\n%s", p2c_result_to_string(r), e ? e : "unknown error");
        set_status("Conversion failed - see Diagnostics"); active_pane = 2; if (generated) free(generated);
    }
}
static void insert_char(char c) { if (source_len + 1 < SRC_CAP) { source[source_len++] = c; source[source_len] = 0; dirty = 1; } }
static void backspace(void) { if (source_len) source[--source_len] = 0; dirty = 1; }
static void draw_lines(cui_canvas *c, cui_font *f, int x, int y, int w, int h, const char *text, uint32_t col, int first, int line_h) {
    int line = 0, shown = 0; const char *p = text;
    while (*p && shown * line_h < h) {
        const char *e = strchr(p, '\n'); int n = e ? (int)(e - p) : (int)strlen(p);
        if (line++ >= first) { char b[300]; if (n > 270) n = 270; memcpy(b, p, n); b[n] = 0; cui_text(c, f, x, y + shown * line_h, b, col); shown++; }
        p = e ? e + 1 : p + strlen(p);
    }
    (void)w;
}
static void draw_code(cui_canvas *c, cui_font *f, int x, int y, int w, int h, const char *text, uint32_t col, int first, int line_h) {
    cui_fill(c, x, y, 50, h, 0x181B22);
    int line = 0, shown = 0; const char *p = text;
    while (*p && shown * line_h < h) {
        const char *e = strchr(p, '\n'); int n = e ? (int)(e - p) : (int)strlen(p);
        if (line++ >= first) {
            char num[16], b[300]; snprintf(num, sizeof num, "%4d", line);
            if (n > 260) n = 260; memcpy(b, p, n); b[n] = 0;
            cui_text(c, f, x + 10, y + shown * line_h, num, 0x6B7385);
            cui_text(c, f, x + 58, y + shown * line_h, b, col); shown++;
        }
        p = e ? e + 1 : p + strlen(p);
    }
    (void)w;
}
static void btn(cui_canvas *c, cui_font *f, int x, int y, int w, const char *s, int primary) {
    cui_round_rect(c, x, y, w, 28, 5, primary ? 0x3978D4 : 0x303B4D); cui_text_center(c, f, x, y, w, 28, s, 0xEAF2FF);
}
static void draw_header(cui_canvas *c, cui_font *f, int w, int classic) {
    cui_fill(c, 0, 0, w, 25, 0x1B2230); cui_text(c, f, 14, 6, "File    Edit    View    Go    Run    Tools    Help", 0xE8EFF9);
    cui_text(c, f, w - 260, 6, classic ? "Py2C  /  Classic UI" : "Py2C  /  C-OS Studio UI", 0x76B8FF);
    cui_fill(c, 0, 25, w, 43, 0x202939);
    btn(c, f, 12, 33, 54, "New", 0); btn(c, f, 74, 33, 62, "Open", 0); btn(c, f, 144, 33, 62, "Save", 0);
    btn(c, f, 222, 33, 54, "Run", 0); btn(c, f, 284, 33, 78, "Convert", 1); btn(c, f, 370, 33, 70, "Save C", 0); btn(c, f, 448, 33, 78, "Settings", 0);
    cui_text(c, f, 550, 42, path_buf, 0x9BA8BB);
}
static void draw_classic(cui_canvas *c, cos_win_info_t *wi) {
    cui_font *f = cui_font_default(14), *fb = cui_font_bold(14); int w = wi->width, h = wi->height;
    draw_header(c, f, w, 1); int panel_y = h - 176, top = 68, bottom = panel_y - 6, split = w / 2;
    cui_fill(c, 0, top, split - 5, bottom - top, 0x202733); cui_fill(c, split, top, w - split, bottom - top, 0x1E2633);
    cui_fill(c, 0, top, split - 5, 32, 0x293447); cui_fill(c, split, top, w - split, 32, 0x293447);
    cui_text(c, fb, 18, top + 8, "Python source", 0xE8EFF9); cui_text(c, fb, split + 18, top + 8, active_pane == 2 ? "Diagnostics" : "Generated C", 0xE8EFF9);
    draw_code(c, f, 8, top + 36, split - 16, bottom - top - 40, source, 0xD7DAE0, scroll, 20);
    draw_code(c, f, split + 8, top + 36, w - split - 16, bottom - top - 40, output[0] ? output : "Press Convert or F5 to generate C", active_pane == 2 ? 0xF0616D : 0x2E9E6B, 0, 20);
    cui_fill(c, 0, panel_y, w, 176, 0x11161F); cui_fill(c, 0, panel_y, 132, 30, 0x3978D4);
    cui_text(c, f, 16, panel_y + 8, active_pane == 2 ? "Diagnostics" : "Output", 0xE8EFF9); cui_text(c, f, 155, panel_y + 8, "Terminal", 0x9BA8BB); cui_text(c, f, 250, panel_y + 8, "Problems", 0x9BA8BB);
    draw_lines(c, f, 18, panel_y + 42, w - 36, 105, status, 0x9BA8BB, 0, 20);
    cui_fill(c, 0, h - 26, w, 26, 0x28598E); cui_text(c, f, 14, h - 19, dirty ? "Python *" : "Python", 0xE8EFF9); cui_text(c, f, w - 280, h - 19, "F5  Ctrl+Enter  Ctrl+, Settings", 0xB4C5DC);
}
static void draw_studio(cui_canvas *c, cos_win_info_t *wi) {
    cui_font *f = cui_font_default(13), *fb = cui_font_bold(13); int w = wi->width, h = wi->height;
    draw_header(c, f, w, 0); int act = 40, side = 238, main = act + side, top = 68, panel_y = h - 184, split = main + (w - main) / 2;
    cui_fill(c, 0, top, act, panel_y - top, 0x12141A);
    cui_text(c, fb, 14, top + 20, "E", 0xFFFFFF); cui_text(c, fb, 14, top + 64, "S", 0x7C8496); cui_text(c, fb, 14, top + 108, "!", 0x7C8496); cui_text(c, fb, 14, top + 152, "C", 0x7C8496);
    cui_fill(c, act, top, side, panel_y - top, 0x161920); cui_text(c, fb, act + 16, top + 18, "PY2C WORKSPACE", 0xD7DAE0);
    cui_text(c, f, act + 18, top + 55, "EXPLORER", 0x7C8496); cui_text(c, f, act + 24, top + 84, "v  /desktop", 0xE2B96F); cui_text(c, f, act + 40, top + 112, "●  untitled.py", 0xD7DAE0); cui_text(c, f, act + 40, top + 140, "◇  generated.c", 0x7C8496); cui_text(c, f, act + 24, top + 184, "CONVERSION", 0x7C8496); cui_text(c, f, act + 40, top + 212, "F5  Python -> C", 0x5B8CFF); cui_text(c, f, act + 40, top + 240, "Ctrl+,  Settings", 0x7C8496);
    cui_fill(c, main, top, w - main, panel_y - top, 0x1B1E26); cui_fill(c, main, top, w - main, 30, 0x20242E); cui_fill(c, main, top, split - main, 2, 0x5B8CFF); cui_fill(c, split, top, w - split, 2, 0x5B8CFF);
    cui_text(c, f, main + 16, top + 8, dirty ? "untitled.py  *" : "untitled.py", 0xD7DAE0); cui_text(c, f, split + 16, top + 8, active_pane == 2 ? "Diagnostics" : "generated.c", 0xD7DAE0);
    draw_code(c, f, main, top + 31, split - main, panel_y - top - 31, source, 0xD7DAE0, scroll, 19);
    draw_code(c, f, split, top + 31, w - split, panel_y - top - 31, output[0] ? output : "Press Convert or F5 to generate C", active_pane == 2 ? 0xF0616D : 0x2E9E6B, 0, 19);
    cui_fill(c, 0, panel_y, w, 184, 0x171B22); cui_fill(c, 0, panel_y, w, 1, 0x292E3A); cui_fill(c, 0, panel_y + 1, w, 30, 0x21252F);
    cui_text(c, fb, 16, panel_y + 9, active_pane == 2 ? "PROBLEMS" : "OUTPUT", 0xFFFFFF); cui_text(c, fb, 102, panel_y + 9, "TERMINAL", 0x7C8496); cui_text(c, fb, 205, panel_y + 9, "DIAGNOSTICS", 0x7C8496);
    draw_lines(c, f, 18, panel_y + 45, w - 36, 112, status, 0x7C8496, 0, 19);
    cui_fill(c, 0, h - 26, w, 26, 0x5B8CFF); cui_text(c, f, 14, h - 19, dirty ? "Python *" : "Python", 0xFFFFFF); cui_text(c, f, w - 300, h - 19, "Studio UI  ·  F5  ·  Ctrl+, Settings", 0xD7E2FF);
}
static void draw_settings(cui_canvas *c, cos_win_info_t *wi) {
    int x = wi->width / 2 - 280, y = wi->height / 2 - 150; cui_fill_alpha(c, 0, 0, wi->width, wi->height, 0x000000, 145);
    cui_round_rect(c, x, y, 560, 300, 10, 0x273246); cui_round_rect_outline(c, x, y, 560, 300, 10, 1.5f, 0x5A78A8);
    cui_text(c, cui_font_bold(18), x + 28, y + 24, "Py2C Settings", 0xF0F5FF); cui_text(c, cui_font_default(13), x + 28, y + 60, "Interface layout", 0xAFC2DC);
    cui_round_rect(c, x + 28, y + 92, 504, 58, 6, ui_mode ? 0x3978D4 : 0x303B4D); cui_text(c, cui_font_bold(14), x + 48, y + 107, "C-OS Studio UI", 0xFFFFFF); cui_text(c, cui_font_default(12), x + 48, y + 129, "Explorer rail, editor split and bottom panel", 0xD5E3F6);
    cui_round_rect(c, x + 28, y + 162, 504, 58, 6, !ui_mode ? 0x3978D4 : 0x303B4D); cui_text(c, cui_font_bold(14), x + 48, y + 177, "Classic Py2C UI", 0xFFFFFF); cui_text(c, cui_font_default(12), x + 48, y + 199, "Traditional two-pane source / generated-C view", 0xD5E3F6);
    btn(c, cui_font_default(13), x + 408, y + 245, 100, "Close", 1);
}
static void frame(cui_canvas *c, cos_win_info_t *wi) {
    cui_fill(c, 0, 0, wi->width, wi->height, 0x151A24);
    if (ui_mode) draw_studio(c, wi); else draw_classic(c, wi);
    if (settings_open) draw_settings(c, wi);
}
int main(int argc, char **argv) {
    load_config(); if (argc > 1 && argv[1][0]) load_file(argv[1]); else source[0] = 0;
    cos_win_info_t wi; int64_t h = cos_win2_create("Py2C Studio", 1280, 800, &wi); if (h <= 0) return 1;
    cui_canvas c; cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride); frame(&c, &wi); cos_win2_present(h);
    for (;;) {
        cos_win_event_t ev; int r = cos_win2_wait(h, &ev, 120); if (r <= 0) continue;
        if (ev.type == COS_EV_CLOSE) break;
        int redraw = 1;
        if (ev.type == COS_EV_OPEN) { char p[256]; if (cos_win2_get_path(h, p, sizeof p)) load_file(p); }
        else if (ev.type == COS_EV_WHEEL) { scroll += ev.wheel > 0 ? -1 : 1; if (scroll < 0) scroll = 0; }
        else if (ev.type == COS_EV_KEY) {
            if ((ev.mods & COS_MOD_CTRL) && (ev.ascii == ',' || ev.ascii == '.')) settings_open = !settings_open;
            else if ((ev.mods & COS_MOD_CTRL) && (ev.ascii == 's' || ev.ascii == 'S')) save_text("/desktop/generated.c", output, strlen(output));
            else if ((ev.mods & COS_MOD_CTRL) && (ev.ascii == '\n' || ev.ascii == '\r')) convert_source();
            else if (ev.special == 0x3F || ev.ascii == 0x00) convert_source();
            else if (!settings_open && (ev.ascii == 8 || ev.ascii == 127)) backspace();
            else if (!settings_open && (ev.ascii >= 32 || ev.ascii == '\n' || ev.ascii == '\t')) insert_char(ev.ascii);
        } else if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) {
            if (settings_open) {
                int x = wi.width / 2 - 280, y = wi.height / 2 - 150;
                if (ev.x >= x + 28 && ev.x < x + 532 && ev.y >= y + 92 && ev.y < y + 150) { ui_mode = 1; save_config(); settings_open = 0; set_status("C-OS Studio UI selected"); }
                else if (ev.x >= x + 28 && ev.x < x + 532 && ev.y >= y + 162 && ev.y < y + 220) { ui_mode = 0; save_config(); settings_open = 0; set_status("Classic UI selected"); }
                else if (ev.x >= x + 400 && ev.y >= y + 235) settings_open = 0;
            } else if (ev.y >= 33 && ev.y < 62) {
                if (ev.x >= 284 && ev.x < 362) convert_source();
                else if (ev.x >= 370 && ev.x < 440) save_text("/desktop/generated.c", output, strlen(output));
                else if (ev.x >= 448 && ev.x < 530) settings_open = 1;
            }
        }
        if (redraw) { frame(&c, &wi); cos_win2_present(h); }
    }
    save_config(); cos_win2_close(h); return 0;
}
