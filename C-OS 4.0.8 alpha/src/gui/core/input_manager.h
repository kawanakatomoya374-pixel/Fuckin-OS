/**
 * input_manager.h - Input Management System Header
 * C-OS 4.0.8 alpha
 * 
 * Centralized input event handling and routing definitions
 */

#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include "types.h"
#include "keyboard.h"

// Input event types
typedef enum {
    INPUT_EVENT_MOUSE,
    INPUT_EVENT_KEYBOARD
} input_event_type_t;

// Mouse event structure (kept local to the input manager to avoid name clashes)
typedef struct {
    int x, y;
    uint8_t buttons;
    int dx, dy;
    int scroll;
} input_mouse_event_t;

// Keyboard event structure: reuse the canonical keyboard_event_t layout.
typedef keyboard_event_t input_keyboard_event_t;

// Input event structure
typedef struct {
    input_event_type_t type;
    union {
        input_mouse_event_t mouse;
        input_keyboard_event_t keyboard;
    } data;
} input_event_t;

// Input manager functions
void input_manager_init(void);
bool input_manager_push_event(const input_event_t* event);
bool input_manager_has_event(void);
input_event_t input_manager_get_event(void);
void input_manager_process_mouse_event(int x, int y, uint8_t buttons);
void input_manager_process_keyboard_event(uint8_t scancode, char ascii);
void input_manager_update(void);
bool input_manager_is_initialized(void);

#endif // INPUT_MANAGER_H
