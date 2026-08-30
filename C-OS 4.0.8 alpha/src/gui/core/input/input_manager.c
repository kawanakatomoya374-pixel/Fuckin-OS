/**
 * input_manager.c - Input Management System
 * C-OS 4.0.8 alpha
 * 
 * Centralized input event handling and routing
 */

#include "input_manager.h"
#include "mouse.h"
#include "keyboard.h"
#include "memory.h"
#include "serial.h"

// Forward declarations for freestanding environment
void* memset(void* ptr, int value, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);

// Input event queue
#define INPUT_QUEUE_SIZE 64
static input_event_t input_queue[INPUT_QUEUE_SIZE];
static int input_queue_head = 0;
static int input_queue_tail = 0;
static bool input_manager_initialized = false;

// Initialize input manager
void input_manager_init(void) {
    serial_puts("[INPUT_MGR] Initializing input manager\n");
    
    // Clear input queue
    memset(input_queue, 0, sizeof(input_queue));
    input_queue_head = 0;
    input_queue_tail = 0;
    
    input_manager_initialized = true;
    serial_puts("[INPUT_MGR] Input manager initialized\n");
}

// Add event to queue
bool input_manager_push_event(const input_event_t* event) {
    if (!input_manager_initialized) {
        serial_puts("[INPUT_MGR] Input manager not initialized\n");
        return false;
    }
    
    // Check if queue is full
    int next_tail = (input_queue_tail + 1) % INPUT_QUEUE_SIZE;
    if (next_tail == input_queue_head) {
        serial_puts("[INPUT_MGR] Input queue full\n");
        return false;
    }
    
    // Add event to queue
    memcpy(&input_queue[input_queue_tail], event, sizeof(input_event_t));
    input_queue_tail = next_tail;
    
    return true;
}

// Get event from queue
bool input_manager_has_event(void) {
    return input_queue_head != input_queue_tail;
}

// Get next event from queue
input_event_t input_manager_get_event(void) {
    if (!input_manager_initialized || input_queue_head == input_queue_tail) {
        // Return empty event
        input_event_t empty = {0};
        return empty;
    }
    
    // Get event from queue
    input_event_t event = input_queue[input_queue_head];
    input_queue_head = (input_queue_head + 1) % INPUT_QUEUE_SIZE;
    
    return event;
}

// Process mouse events
void input_manager_process_mouse_event(int x, int y, uint8_t buttons) {
    input_event_t event = {
        .type = INPUT_EVENT_MOUSE,
        .data.mouse = {
            .x = x,
            .y = y,
            .buttons = buttons,
            .dx = 0,
            .dy = 0,
            .scroll = 0
        }
    };
    
    input_manager_push_event(&event);
}

// Process keyboard events
void input_manager_process_keyboard_event(uint8_t scancode, char ascii) {
    input_event_t event = {
        .type = INPUT_EVENT_KEYBOARD,
        .data.keyboard = {
            .scancode = (uint8_t)(scancode & 0x7F),
            .ascii = ascii,
            .modifiers = (keyboard_is_shift_pressed() ? KEYBOARD_MOD_SHIFT : 0) |
                         (keyboard_is_ctrl_pressed() ? KEYBOARD_MOD_CTRL : 0) |
                         (keyboard_is_alt_pressed() ? KEYBOARD_MOD_ALT : 0) |
                         (keyboard_is_caps_locked() ? KEYBOARD_MOD_CAPS : 0) |
                         (keyboard_is_num_locked() ? KEYBOARD_MOD_NUM : 0),
            .pressed = ((scancode & 0x80) == 0),
            .extended = false
        }
    };
    
    input_manager_push_event(&event);
}

// Update input manager (called from interrupt handlers)
void input_manager_update(void) {
    // Poll the canonical mouse state if available.
    static int last_mouse_x = 0;
    static int last_mouse_y = 0;
    static uint8_t last_mouse_buttons = 0;
    minimal_mouse_t* mouse_state = minimal_mouse_get_state();
    if (mouse_state) {
        uint8_t buttons = 0;
        if (mouse_state->left)   buttons |= 0x01;
        if (mouse_state->right)  buttons |= 0x02;
        if (mouse_state->middle) buttons |= 0x04;

        if (mouse_state->left_click) {
            buttons |= 0x01;
            serial_puts("[INPUT_MGR] Left click detected\n");
        }
        if (mouse_state->right_click) {
            buttons |= 0x02;
            serial_puts("[INPUT_MGR] Right click detected\n");
        }

        if ((int)mouse_state->x != last_mouse_x ||
            (int)mouse_state->y != last_mouse_y ||
            buttons != last_mouse_buttons ||
            mouse_state->wheel != 0) {
            input_event_t event = {
                .type = INPUT_EVENT_MOUSE,
                .data.mouse = {
                    .x = (int)mouse_state->x,
                    .y = (int)mouse_state->y,
                    .buttons = buttons,
                    .dx = (int)mouse_state->dx,
                    .dy = (int)mouse_state->dy,
                    .scroll = (int)mouse_state->wheel
                }
            };
            input_manager_push_event(&event);
            last_mouse_x = (int)mouse_state->x;
            last_mouse_y = (int)mouse_state->y;
            last_mouse_buttons = buttons;
        }

        if (mouse_state->left_click || mouse_state->right_click ||
            mouse_state->left_release || mouse_state->right_release) {
            minimal_mouse_clear_clicks();
        }
    }

    // Poll the keyboard state.
    while (keyboard_has_event()) {
        keyboard_event_t ev = keyboard_get_event();
        input_event_t event = {
            .type = INPUT_EVENT_KEYBOARD,
            .data.keyboard = {
                .scancode = ev.scancode,
                .ascii = ev.ascii,
                .modifiers = ev.modifiers,
                .pressed = ev.pressed ? true : false,
                .extended = ev.extended
            }
        };
        input_manager_push_event(&event);
    }
}

// Check if input manager is initialized
bool input_manager_is_initialized(void) {
    return input_manager_initialized;
}
