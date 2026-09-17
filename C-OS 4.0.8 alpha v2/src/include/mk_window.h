/**
 * mk_window.h - Advanced Window System Header
 * C-OS 4.0.8 alpha Advanced Window Manager
 */
#ifndef MK_WINDOW_H
#define MK_WINDOW_H

#include "types.h"

// Window constants
#define MK_MAX_WINDOWS 64
#define MK_MAX_WINDOW_DECORATIONS 32
#define MK_MAX_WINDOW_CONTROLS 16
#define MK_MAX_WINDOW_THEMES 16

// Decoration types
#define MK_DECORATION_TYPE_STANDARD 1
#define MK_DECORATION_TYPE_MINIMAL 2
#define MK_DECORATION_TYPE_BORDERLESS 3

// Control types
#define MK_CONTROL_CLOSE 1
#define MK_CONTROL_MINIMIZE 2
#define MK_CONTROL_MAXIMIZE 3
#define MK_CONTROL_RESTORE 4

// Action types
#define MK_ACTION_NONE 0
#define MK_ACTION_CLOSE 1
#define MK_ACTION_MINIMIZE 2
#define MK_ACTION_MAXIMIZE 3
#define MK_ACTION_RESTORE 4

// Window functions
void mk_window_init(void);
uint64_t mk_window_create(const char* title, uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint64_t decoration_id);
int mk_window_show(uint64_t window_id);
int mk_window_hide(uint64_t window_id);
int mk_window_close(uint64_t window_id);
int mk_window_focus(uint64_t window_id);
int mk_window_unfocus(uint64_t window_id);
int mk_window_move(uint64_t window_id, uint64_t x, uint64_t y);
int mk_window_resize(uint64_t window_id, uint64_t width, uint64_t height);
int mk_window_maximize(uint64_t window_id);
int mk_window_minimize(uint64_t window_id);
int mk_window_restore(uint64_t window_id);
void mk_window_render_all(void);
void mk_window_handle_click(uint64_t x, uint64_t y);
void mk_window_generate_report(void);

#endif // MK_WINDOW_H
