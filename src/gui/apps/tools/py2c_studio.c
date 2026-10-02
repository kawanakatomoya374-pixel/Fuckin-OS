#include "gui.h"
#include "vga.h"
#include "fs.h"
#include "string.h"
#include "memory.h"
#include "keyboard.h"
#include "gui_apps_common.h"

#define P2C_PREVIEW_MAX (64u * 1024u)
static char p2c_preview[P2C_PREVIEW_MAX];
static char p2c_error[512];
static bool p2c_has_output;
static int p2c_tab;
static const char* p2c_open_name = "main.py";

extern int p2c_cos_convert(const char*, char*, size_t, char*, size_t);
extern const char *p2c_cos_supported(void);
extern const char *fs_read_file_at(const char *path, const char *name);

static void p2c_notify(const char* en, const char* ja) {
    gui_notify(gui_text(en, ja), 2200);
}

static void p2c_convert(window_t* w) {
    if (!w) return;
    p2c_preview[0] = '\0';
    p2c_error[0] = '\0';
    int rc = p2c_cos_convert(w->text_buf, p2c_preview, sizeof(p2c_preview),
                             p2c_error, sizeof(p2c_error));
    p2c_has_output = (rc == 0);
    if (!p2c_has_output && !p2c_error[0]) {
        strncpy(p2c_error, "Py2C conversion failed", sizeof(p2c_error) - 1);
        p2c_error[sizeof(p2c_error) - 1] = '\0';
    }
    p2c_tab = 0;
    gui_request_redraw();
}

static void p2c_save_output(void) {
    if (!p2c_has_output || !p2c_preview[0]) {
        p2c_notify("Convert the source first", "先にPythonソースを変換してください");
        return;
    }
    if (fs_write_file_at("/desktop", "py2c_generated.c", p2c_preview,
                         (uint64_t)strlen(p2c_preview))) {
        p2c_notify("Generated C saved as /desktop/py2c_generated.c",
                   "生成Cを/desktop/py2c_generated.cへ保存しました");
    } else {
        p2c_notify("Could not save generated C", "生成Cの保存に失敗しました");
    }
}

static void p2c_save_source(window_t* w) {
    if (!w) return;
    const char* name = w->filename[0] ? w->filename : p2c_open_name;
    if (fs_write_file_at("/desktop", name, w->text_buf,
                         (uint64_t)strlen(w->text_buf))) {
        strncpy(w->filename, name, sizeof(w->filename) - 1);
        w->filename[sizeof(w->filename) - 1] = '\0';
        w->text_modified = FALSE;
        p2c_notify("Python source saved", "Pythonソースを保存しました");
    } else {
        p2c_notify("Could not save Python source", "Pythonソースの保存に失敗しました");
    }
}

static bool p2c_load_named(window_t* w, const char* name) {
    if (!w || !name) return false;
    const char* source = fs_read_file_at("/desktop", name);
    if (!source) return false;
    strncpy(w->text_buf, source, TEXT_BUF_SIZE - 1);
    w->text_buf[TEXT_BUF_SIZE - 1] = '\0';
    strncpy(w->filename, name, sizeof(w->filename) - 1);
    w->filename[sizeof(w->filename) - 1] = '\0';
    w->text_cursor = (int)strlen(w->text_buf);
    w->text_modified = FALSE;
    p2c_preview[0] = '\0'; p2c_error[0] = '\0'; p2c_has_output = false;
    return true;
}

static void p2c_open_source(window_t* w) {
    static const char* candidates[] = {"main.py", "test.py", "hello.py", "script.py"};
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (p2c_load_named(w, candidates[i])) {
            p2c_notify("Python source opened", "Pythonソースを開きました");
            gui_request_redraw();
            return;
        }
    }
    p2c_notify("No .py file found in /desktop", "/desktopに.pyファイルがありません");
}

static void p2c_new_source(window_t* w) {
    if (!w) return;
    w->text_buf[0] = '\0';
    w->filename[0] = '\0';
    w->text_cursor = 0;
    w->text_modified = TRUE;
    p2c_preview[0] = '\0'; p2c_error[0] = '\0'; p2c_has_output = false;
    p2c_notify("New Python source", "新しいPythonソースを作成しました");
    gui_request_redraw();
}

void py2c_studio_init(window_t* w) {
    if (!w) return;
    /* Do not inject a fake program into the editor.  The editor starts empty;
     * Open loads a real file from /desktop and New creates a real document. */
    if (!w->text_buf[0]) w->text_cursor = 0;
    w->text_modified = FALSE;
    p2c_preview[0] = '\0'; p2c_error[0] = '\0'; p2c_has_output = false; p2c_tab = 0;
}

static void p2c_button(int x, int y, int width, const char* label, bool primary) {
    uint64_t fill = primary ? rgb(38, 104, 190) : rgb(38, 52, 70);
    uint64_t border = primary ? rgb(104, 184, 255) : rgb(84, 108, 140);
    vga_fill_rect(x, y, width, 24, fill);
    vga_draw_rect(x, y, width, 24, border);
    vga_draw_string(x + 8, y + 7, label, rgb(235, 242, 252), fill);
}

static void p2c_draw_lines(int x, int y, int width, int height, const char* text,
                           uint64_t fg, uint64_t bg) {
    vga_fill_rect(x, y, width, height, bg);
    vga_draw_rect(x, y, width, height, rgb(70, 84, 108));
    int line = 0; const char* p = text ? text : "";
    int max = (height - 14) / (FONT_H + 2); if (max < 1) max = 1;
    while (*p && line < max) {
        char buf[256]; int n = 0;
        while (*p && *p != '\n' && n < (int)sizeof(buf) - 1) buf[n++] = *p++;
        buf[n] = '\0';
        vga_draw_string(x + 8, y + 7 + line * (FONT_H + 2), buf, fg, bg);
        if (*p == '\n') p++;
        line++;
    }
    if (!text || !text[0]) vga_draw_string(x + 8, y + 8, "(empty)", rgb(130,140,155), bg);
}

void draw_py2c_studio(int idx) {
    window_t* w = &windows[idx];
    int x = w->x + 8, y = w->y + TITLEBAR_H + 8;
    int ww = w->w - 16, hh = w->h - TITLEBAR_H - 16;
    uint64_t bg = rgb(18, 22, 30), panel = rgb(28, 34, 45);
    uint64_t fg = rgb(225, 235, 248), muted = rgb(160,175,195);
    uint64_t line = rgb(61, 72, 91), active = rgb(45, 105, 180);
    vga_fill_rect(x, y, ww, hh, bg);

    /* This mirrors the actual C-OS Studio frame: menu row, compact toolbar,
     * activity rail, tabbed editor, output panel and status bar. */
    int menu_y = y, tool_y = y + 23, top = y + 57;
    vga_fill_rect(x, menu_y, ww, 22, rgb(24, 29, 38));
    vga_draw_string(x + 12, menu_y + 7, "File", fg, rgb(24,29,38));
    vga_draw_string(x + 54, menu_y + 7, "Edit", muted, rgb(24,29,38));
    vga_draw_string(x + 98, menu_y + 7, "View", muted, rgb(24,29,38));
    vga_draw_string(x + 146, menu_y + 7, "Go", muted, rgb(24,29,38));
    vga_draw_string(x + 184, menu_y + 7, "Run", muted, rgb(24,29,38));
    vga_draw_string(x + 232, menu_y + 7, "Tools", muted, rgb(24,29,38));
    vga_draw_string(x + 288, menu_y + 7, "Help", muted, rgb(24,29,38));
    vga_draw_string(x + ww - 260, menu_y + 7, "Py2C / C-OS", rgb(117,190,255), rgb(24,29,38));

    vga_fill_rect(x, tool_y, ww, 34, rgb(31, 38, 50));
    p2c_button(x + 48, tool_y + 5, 44, "New", false);
    p2c_button(x + 98, tool_y + 5, 52, "Open", false);
    p2c_button(x + 156, tool_y + 5, 52, "Save", false);
    p2c_button(x + 228, tool_y + 5, 56, "Run", false);
    p2c_button(x + 290, tool_y + 5, 62, "Convert", true);
    p2c_button(x + 358, tool_y + 5, 60, "Save C", false);
    vga_draw_string(x + 432, tool_y + 12, w->filename[0] ? w->filename : "Untitled Python", muted, rgb(31,38,50));

    int panel_y = y + hh - 128, editor_h = panel_y - top;
    vga_fill_rect(x, top, 38, editor_h, rgb(24,29,38));
    vga_draw_rect(x, top, 38, editor_h, line);
    vga_draw_string(x + 12, top + 10, "<", rgb(110,180,240), rgb(24,29,38));
    vga_draw_string(x + 12, top + 42, "P", rgb(110,180,240), rgb(24,29,38));
    vga_draw_string(x + 12, top + 74, "C", rgb(110,180,240), rgb(24,29,38));
    int left = (ww * 47) / 100, sx = x + 46, source_w = left - 46;
    p2c_draw_lines(sx, top, source_w, editor_h, w->text_buf, fg, panel);
    vga_draw_string(sx + 8, top - 14, w->filename[0] ? w->filename : "untitled.py", fg, bg);
    int rx = x + left + 10, rw = ww - left - 10;
    const char* right = p2c_has_output ? p2c_preview : (p2c_error[0] ? p2c_error : "Press Convert or F5 to generate C");
    p2c_draw_lines(rx, top, rw, editor_h, right, p2c_has_output ? rgb(180,235,180) : rgb(255,190,130), panel);
    vga_draw_string(rx + 8, top - 14, p2c_has_output ? "generated.c" : "Py2C output", fg, bg);

    vga_fill_rect(x, panel_y, ww, 128, rgb(22,27,35));
    vga_draw_rect(x, panel_y, ww, 128, line);
    vga_fill_rect(x, panel_y, 110, 24, active);
    vga_draw_string(x + 12, panel_y + 8, p2c_tab == 1 ? "Problems" : "Output", fg, active);
    vga_draw_string(x + 126, panel_y + 8, "Terminal", muted, rgb(22,27,35));
    vga_draw_string(x + 226, panel_y + 8, "Diagnostics", muted, rgb(22,27,35));
    p2c_draw_lines(x + 8, panel_y + 30, ww - 16, 90,
                   p2c_tab == 1 ? p2c_cos_supported() : (p2c_has_output ? "Py2C: conversion succeeded\nGenerated C is ready to save." : "Py2C: idle\nF5 / Ctrl+Enter  Convert Python to C"),
                   muted, rgb(22,27,35));
    vga_fill_rect(x, y + hh - 20, ww, 20, rgb(38, 78, 122));
    vga_draw_string(x + 10, y + hh - 14, "Py2C", fg, rgb(38,78,122));
    vga_draw_string(x + 72, y + hh - 14, "UTF-8", muted, rgb(38,78,122));
    vga_draw_string(x + ww - 210, y + hh - 14, "Ctrl+S Save  F5 Convert", muted, rgb(38,78,122));
}

void py2c_studio_handle_click(int idx, int mx, int my) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    int x = w->x + 8, ty = w->y + TITLEBAR_H + 23;
    if (my < ty || my >= ty + 34) return;
    if (mx >= x + 48 && mx < x + 92) p2c_new_source(w);
    else if (mx >= x + 98 && mx < x + 150) p2c_open_source(w);
    else if (mx >= x + 156 && mx < x + 208) p2c_save_source(w);
    else if (mx >= x + 228 && mx < x + 284) p2c_convert(w);
    else if (mx >= x + 290 && mx < x + 352) p2c_convert(w);
    else if (mx >= x + 358 && mx < x + 418) p2c_save_output();
}

void py2c_studio_handle_key(int idx, char ascii, int scancode, bool ctrl) {
    window_t* w = &windows[idx];
    if (ctrl && (ascii == 's' || ascii == 'S')) { p2c_save_output(); return; }
    if (ctrl && (ascii == 'o' || ascii == 'O')) { p2c_open_source(w); return; }
    if (ctrl && (ascii == 'n' || ascii == 'N')) { p2c_new_source(w); return; }
    if (ctrl && (ascii == ' ' || ascii == 't' || ascii == 'T')) { p2c_tab = !p2c_tab; gui_request_redraw(); return; }
    if ((ctrl && (ascii == '\n' || ascii == '\r')) || scancode == KEY_F5) { p2c_convert(w); return; }
    int saved = w->kind; w->kind = WIN_TEXT_EDITOR;
    handle_text_editor_key(idx, ascii, scancode);
    w->kind = saved;
}
