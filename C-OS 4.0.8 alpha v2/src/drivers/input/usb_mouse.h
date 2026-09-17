/**
 * usb_mouse.h - USB Mouse Driver Header
 */

#ifndef USB_MOUSE_H
#define USB_MOUSE_H

#include "types.h"

// USB Mouse state structure
typedef struct {
    int64_t x, y;
    uint8_t buttons;  // Bit 0=left, 1=right, 2=middle
    int8_t wheel;
} usb_mouse_state_t;

// Functions
int usb_mouse_init(void);
void usb_mouse_poll(void);
usb_mouse_state_t* usb_mouse_get_state(void);
void usb_mouse_clear_clicks(void);

#endif
