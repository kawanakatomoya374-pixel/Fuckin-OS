/**
 * window_manager.h - Window Management System
 * C-OS 4.0.8 alpha
 * 
 * Separated window management from GUI core
 */

#ifndef WINDOW_MANAGER_H
#define WINDOW_MANAGER_H

#include "../include/types.h"

// Window states
typedef enum {
    WIN_STATE_NORMAL,
    WIN_STATE_MINIMIZED,
    WIN_STATE_MAXIMIZED,
    WIN_STATE_CLOSED
} window_state_t;

// Window types
typedef enum {
    WIN_TYPE_NORMAL,
    WIN_TYPE_MODAL,
    WIN_TYPE_DIALOG,
    WIN_TYPE_MENU
} window_type_t;

// Window structure
typedef struct window_t {
    uint64_t id;
    char title[MAX_TITLE_LENGTH];
    int x, y, w, h;
    window_state_t state;
    window_type_t type;
    bool visible;
    bool focused;
    
    // Drawing buffers
    uint64_t* content_buffer;
    
    // Z-order management
    struct window_t* next;
    struct window_t* prev;
} window_t;

// Window manager functions
void window_manager_init(void);
window_t* window_create(int x, int y, int w, int h, const char* title, window_type_t type);
void window_destroy(window_t* win);
void window_show(window_t* win);
void window_hide(window_t* win);
void window_focus(window_t* win);
void window_move(window_t* win, int x, int y);
void window_resize(window_t* win, int w, int h);
void window_minimize(window_t* win);
void window_maximize(window_t* win);
void window_restore(window_t* win);

window_t* window_get_focused(void);
window_t* window_get_at_position(int x, int y);
void window_bring_to_front(window_t* win);
void window_send_to_back(window_t* win);

#endif // WINDOW_MANAGER_H
