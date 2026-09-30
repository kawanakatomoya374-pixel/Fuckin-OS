#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"
#include "idt.h"

// Keyboard scancodes (US layout)
// Backward-compatible shorthand aliases used across the GUI layer.
#ifndef KEY_ESC
#define KEY_ESC KEYBOARD_SCANCODE_ESCAPE
#endif
#ifndef KEY_BACKSPACE
#define KEY_BACKSPACE KEYBOARD_SCANCODE_BACKSPACE
#endif
#ifndef KEY_ENTER
#define KEY_ENTER KEYBOARD_SCANCODE_ENTER
#endif
#ifndef KEY_TAB
#define KEY_TAB KEYBOARD_SCANCODE_TAB
#endif
#ifndef KEY_SPACE
#define KEY_SPACE KEYBOARD_SCANCODE_SPACE
#endif
#ifndef KEY_LSHIFT
#define KEY_LSHIFT KEYBOARD_SCANCODE_LSHIFT
#endif
#ifndef KEY_RSHIFT
#define KEY_RSHIFT KEYBOARD_SCANCODE_RSHIFT
#endif
#ifndef KEY_LCTRL
#define KEY_LCTRL KEYBOARD_SCANCODE_LCTRL
#endif
#ifndef KEY_LALT
#define KEY_LALT KEYBOARD_SCANCODE_LALT
#endif
#ifndef KEY_CAPSLOCK
#define KEY_CAPSLOCK KEYBOARD_SCANCODE_CAPSLOCK
#endif
#ifndef KEY_NUMLOCK
#define KEY_NUMLOCK KEYBOARD_SCANCODE_NUMLOCK
#endif
#ifndef KEY_UP
#define KEY_UP 0x48
#endif
#ifndef KEY_DOWN
#define KEY_DOWN 0x50
#endif
#ifndef KEY_LEFT
#define KEY_LEFT 0x4B
#endif
#ifndef KEY_RIGHT
#define KEY_RIGHT 0x4D
#endif

#define KEYBOARD_SCANCODE_ESCAPE  0x01
#define KEYBOARD_SCANCODE_1       0x02
#define KEYBOARD_SCANCODE_2       0x03
#define KEYBOARD_SCANCODE_3       0x04
#define KEYBOARD_SCANCODE_4       0x05
#define KEYBOARD_SCANCODE_5       0x06
#define KEYBOARD_SCANCODE_6       0x07
#define KEYBOARD_SCANCODE_7       0x08
#define KEYBOARD_SCANCODE_8       0x09
#define KEYBOARD_SCANCODE_9       0x0A
#define KEYBOARD_SCANCODE_0       0x0B
#define KEYBOARD_SCANCODE_MINUS   0x0C
#define KEYBOARD_SCANCODE_EQUALS  0x0D
#define KEYBOARD_SCANCODE_BACKSPACE 0x0E
#define KEYBOARD_SCANCODE_TAB     0x0F
#define KEYBOARD_SCANCODE_Q       0x10
#define KEYBOARD_SCANCODE_W       0x11
#define KEYBOARD_SCANCODE_E       0x12
#define KEYBOARD_SCANCODE_R       0x13
#define KEYBOARD_SCANCODE_T       0x14
#define KEYBOARD_SCANCODE_Y       0x15
#define KEYBOARD_SCANCODE_U       0x16
#define KEYBOARD_SCANCODE_I       0x17
#define KEYBOARD_SCANCODE_O       0x18
#define KEYBOARD_SCANCODE_P       0x19
#define KEYBOARD_SCANCODE_LBRACKET 0x1A
#define KEYBOARD_SCANCODE_RBRACKET 0x1B
#define KEYBOARD_SCANCODE_ENTER   0x1C
#define KEYBOARD_SCANCODE_LCTRL   0x1D
#define KEYBOARD_SCANCODE_RCTRL   0x1D  /* E0-prefixed */
#define KEYBOARD_SCANCODE_A       0x1E
#define KEYBOARD_SCANCODE_S       0x1F
#define KEYBOARD_SCANCODE_D       0x20
#define KEYBOARD_SCANCODE_F       0x21
#define KEYBOARD_SCANCODE_G       0x22
#define KEYBOARD_SCANCODE_H       0x23
#define KEYBOARD_SCANCODE_J       0x24
#define KEYBOARD_SCANCODE_K       0x25
#define KEYBOARD_SCANCODE_L       0x26
#define KEYBOARD_SCANCODE_SEMICOLON 0x27
#define KEYBOARD_SCANCODE_APOSTROPHE 0x28
#define KEYBOARD_SCANCODE_GRAVE   0x29
#define KEYBOARD_SCANCODE_LSHIFT  0x2A
#define KEYBOARD_SCANCODE_BACKSLASH 0x2B
#define KEYBOARD_SCANCODE_Z       0x2C
#define KEYBOARD_SCANCODE_X       0x2D
#define KEYBOARD_SCANCODE_C       0x2E
#define KEYBOARD_SCANCODE_V       0x2F
#define KEYBOARD_SCANCODE_B       0x30
#define KEYBOARD_SCANCODE_N       0x31
#define KEYBOARD_SCANCODE_M       0x32
#define KEYBOARD_SCANCODE_COMMA   0x33
#define KEYBOARD_SCANCODE_PERIOD  0x34
#define KEYBOARD_SCANCODE_SLASH   0x35
#define KEYBOARD_SCANCODE_RSHIFT  0x36
#define KEYBOARD_SCANCODE_MULTIPLY 0x37
#define KEYBOARD_SCANCODE_LALT    0x38
#define KEYBOARD_SCANCODE_RALT    0x38  /* E0-prefixed */
#define KEYBOARD_SCANCODE_SPACE   0x39
#define KEYBOARD_SCANCODE_CAPSLOCK 0x3A
#define KEYBOARD_SCANCODE_F1      0x3B
#define KEYBOARD_SCANCODE_F2      0x3C
#define KEYBOARD_SCANCODE_F3      0x3D
#define KEYBOARD_SCANCODE_F4      0x3E
#define KEYBOARD_SCANCODE_F5      0x3F
#define KEYBOARD_SCANCODE_F6      0x40
#define KEYBOARD_SCANCODE_F7      0x41
#define KEYBOARD_SCANCODE_F8      0x42
#define KEYBOARD_SCANCODE_F9      0x43
#define KEYBOARD_SCANCODE_F10     0x44
#define KEYBOARD_SCANCODE_F11     0x57
#define KEYBOARD_SCANCODE_F12     0x58
#define KEYBOARD_SCANCODE_NUMLOCK 0x45
#define KEYBOARD_SCANCODE_SCROLLLOCK 0x46

// Keyboard modifier flags
#define KEYBOARD_MOD_SHIFT   0x01
#define KEYBOARD_MOD_CTRL    0x02
#define KEYBOARD_MOD_ALT     0x04
#define KEYBOARD_MOD_CAPS    0x08
#define KEYBOARD_MOD_NUM     0x10

// Keyboard event structure
// Backward-compatible: the extra boolean fields let older code access
// modifier state directly while newer code can use the bitmask.
typedef struct {
    uint8_t scancode;
    char ascii;
    uint8_t modifiers;
    uint8_t pressed;  // 1 for press, 0 for release
    bool shift;
    bool ctrl;
    bool alt;
    bool extended;    // true for E0-prefixed scancodes (arrows, Insert, etc.)
} keyboard_event_t;

typedef keyboard_event_t key_event_t;

// Keyboard callback function type
typedef void (*keyboard_callback_t)(const keyboard_event_t* event);

// Keyboard functions
void keyboard_init(void);
void keyboard_irq_handler(struct regs *r);
void keyboard_interrupt_handler(void);
/* Feeds one already-translated PS/2 Set-1 key event into the same
 * press/release pipeline the real IRQ1 handler uses - see keyboard.c.
 * `extended` matches the E0-prefix flag for keys like arrows/RCtrl. */
void keyboard_inject_scancode(uint8_t code, bool pressed, bool extended);
bool keyboard_has_event(void);
keyboard_event_t keyboard_get_event(void);
char keyboard_get_char(void);
uint8_t keyboard_get_scancode(void);
char keyboard_get_ascii(void);
bool keyboard_has_key(void);
key_event_t keyboard_get_key(void);
void keyboard_flush(void);
void keyboard_poll(void);

void keyboard_install_callback(keyboard_callback_t callback);
void keyboard_remove_callback(keyboard_callback_t callback);

bool keyboard_is_shift_pressed(void);
bool keyboard_is_ctrl_pressed(void);
bool keyboard_is_alt_pressed(void);
bool keyboard_is_caps_locked(void);
bool keyboard_is_num_locked(void);

// Legacy shorthand aliases used by some modules
bool keyboard_shift(void);
bool keyboard_ctrl(void);
bool keyboard_alt(void);

/* === ブラウザURL編集用拡張機能 === */

/* URL編集モード設定 */
void keyboard_set_url_edit_mode(bool enabled);
bool keyboard_get_url_edit_mode(void);

/* URL編集時のバックスペース長押し検出 */
uint8_t keyboard_get_url_backspace_count(void);
void keyboard_reset_url_backspace_count(void);
bool keyboard_should_delete_all_url(void);
void keyboard_clear_delete_all_mode(void);

/* ブラウザ向け拡張関数 */
bool keyboard_is_url_edit_mode(void);
void keyboard_enter_url_edit_mode(void);
void keyboard_exit_url_edit_mode(void);
bool keyboard_is_delete_all_pending(void);
void keyboard_ack_delete_all(void);

/* キーリピート状態取得 */
bool keyboard_is_repeating(void);
uint8_t keyboard_get_repeat_scancode(void);

/* 拡張状態チェック関数 */
bool keyboard_check_backspace_hold(int threshold);
uint32_t keyboard_get_backspace_count(void);

/* キーリピート設定 */
void keyboard_set_repeat(bool enabled);
void keyboard_set_repeat_rate(uint16_t delay_ms, uint16_t rate_ms);

/* タイマーからの呼び出し（キーリピート処理用） */
void keyboard_timer_tick(uint32_t ticks);

#endif // KEYBOARD_H
