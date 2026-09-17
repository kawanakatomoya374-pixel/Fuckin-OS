/*
 * python_ide_gui.h - C-OS Python IDE
 * Clean GUI front-end for MicroPython execution.
 */

#ifndef PYTHON_IDE_GUI_H
#define PYTHON_IDE_GUI_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "gui.h"
#include "vga.h"
#include "mouse.h"
#include "keyboard.h"
#include "fs.h"

#define IDE_MAX_FILE_SIZE   32768
#define IDE_MAX_LINE_COUNT  1024
#define IDE_MAX_LINE_LENGTH  256
#define IDE_CONSOLE_LINES   64
#define IDE_TAB_SIZE        4
#define IDE_MAX_TABS        4
#define IDE_TABBAR_H        (FONT_H + 8)

#define IDE_BG               rgb(30, 30, 40)
#define IDE_EDITOR_BG        rgb(40, 44, 52)
#define IDE_CONSOLE_BG       rgb(20, 25, 30)
#define IDE_STATUS_BG        rgb(24, 28, 36)
#define IDE_TOOLBAR_BG       rgb(46, 64, 90)
#define IDE_TOOLBAR_BTN      rgb(33, 46, 66)
#define IDE_TOOLBAR_BTN_HOT  rgb(54, 80, 120)
#define IDE_SYNTAX_KEYWORD   rgb(197, 134, 192)
#define IDE_SYNTAX_STRING    rgb(106, 153, 85)
#define IDE_SYNTAX_COMMENT   rgb(92, 99, 112)
#define IDE_SYNTAX_NUMBER    rgb(209, 154, 102)
#define IDE_SYNTAX_FUNCTION  rgb(97, 175, 239)
#define IDE_LINE_NUMBER      rgb(120, 126, 140)
#define IDE_CURSOR           rgb(255, 255, 255)
#define IDE_SELECTION        rgb(97, 175, 239)

typedef struct {
    char filename[128];
    char lines[IDE_MAX_LINE_COUNT][IDE_MAX_LINE_LENGTH];
    int line_count;
    int cursor_x;
    int cursor_y;
    int scroll_y;
    bool modified;
} python_ide_tab_t;

typedef struct {
    int x, y, w, h;
    bool visible;
    bool modified;
    char filename[128];

    char lines[IDE_MAX_LINE_COUNT][IDE_MAX_LINE_LENGTH];
    int line_count;
    int cursor_x;
    int cursor_y;
    int scroll_y;

    char console_lines[IDE_CONSOLE_LINES][512];
    int console_line_count;
    int console_scroll;

    bool repl_active;
    char repl_input[256];
    int repl_cursor_pos;

    bool executing;
    char status[256];

    int current_tab;
    int tab_count;
    python_ide_tab_t tabs[IDE_MAX_TABS];

    bool autosave_enabled;
    uint64_t autosave_next_tick;

    bool debug_mode;
    bool debug_paused;
    uint32_t breakpoint_line;
    char completion[128];
} python_ide_t;

void python_ide_init(void);
void python_ide_open(void);
void python_ide_close(void);
void python_ide_draw(void);
void python_ide_set_geometry(int x, int y, int w, int h);
void python_ide_handle_mouse(mouse_state_t mouse);
void python_ide_handle_click(int mx, int my);
void python_ide_scroll(int delta);
void python_ide_handle_keyboard(keyboard_event_t ev);
void python_ide_new_file(void);
void python_ide_open_file(void);
void python_ide_save_file(void);
void python_ide_execute_code(void);
void python_ide_debug_step(void);
void python_ide_debug_continue(void);
void python_ide_toggle_breakpoint(void);
void python_ide_add_console(const char* message);
void python_ide_draw_toolbar(void);
void python_ide_draw_editor(void);
void python_ide_draw_console(void);
void python_ide_draw_status_bar(void);

#endif /* PYTHON_IDE_GUI_H */
