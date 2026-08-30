#ifndef MOUSE_H
#define MOUSE_H

#include "types.h"
#include "vga.h"

/* Canonical mouse state used across the OS. */
typedef struct mouse_state_t {
    int64_t x, y;
    int64_t dx, dy;
    bool left, right, middle;
    bool left_click, right_click;
    bool left_release, right_release;
    int64_t wheel;
} mouse_state_t;

/* Compatibility alias kept for legacy code paths. */
typedef mouse_state_t minimal_mouse_t;

/* Shared instance owned by the input driver. */
extern mouse_state_t mouse;

void mouse_init(void);
void mouse_update(void);
mouse_state_t* mouse_get_state(void);
void mouse_clear_clicks(void);

void minimal_mouse_init(void);
void minimal_mouse_poll(void);
minimal_mouse_t* minimal_mouse_get_state(void);
void minimal_mouse_clear_clicks(void);
void minimal_mouse_interrupt_handler(void);

/* Feeds a translated USB HID boot-mouse report into the same shared
 * mouse state minimal_mouse_interrupt_handler() updates for PS/2 -
 * see mouse.c. buttons: bit0=left, bit1=right, bit2=middle. dx/dy are
 * relative movement (dy positive = down, matching typical USB HID
 * convention); wheel is relative scroll. */
void mouse_apply_usb_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);

#endif /* MOUSE_H */
