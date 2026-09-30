/**
 * cos_app_window.h - window surface for ring3 ".c-os" programs.
 *
 * Syscalls append draw commands; the GUI owner thread replays them.
 * See cos_app_window.c for why this is a queue rather than direct
 * drawing.
 */
#ifndef COS_APP_WINDOW_H
#define COS_APP_WINDOW_H

#include <stdint.h>
#include <stdbool.h>

/* Keyboard input for a .c-os window.
 *
 * `special` distinguishes non-printable keys the app is likely to care
 * about (Enter to submit, Backspace to edit) from an ordinary printable
 * character in `ascii`. This is NOT a general scancode/modifier
 * passthrough - a deliberately small, stable surface matching the same
 * "custom ABI, not a raw hardware mirror" approach used for every other
 * syscall input structure in this codebase (COS_O_* open flags, etc.). */
#define COS_KEY_NONE      0
#define COS_KEY_ENTER     1
#define COS_KEY_BACKSPACE 2
#define COS_KEY_ESC       3
#define COS_KEY_UP        4
#define COS_KEY_DOWN      5
#define COS_KEY_LEFT      6
#define COS_KEY_RIGHT     7
#define COS_KEY_TAB       8
#define COS_KEY_DELETE    9
/* Navigation and function keys. Added so an editor can bind Home/End,
 * PageUp/PageDown and F1..F12 (F5 = run). Home/End/PageUp/PageDown are the
 * extended (E0-prefixed) keys only - the keypad 7/1/9/3 keys keep producing
 * their digits. F1..F12 are COS_KEY_F1 + n - 1. */
#define COS_KEY_HOME      10
#define COS_KEY_END       11
#define COS_KEY_PGUP      12
#define COS_KEY_PGDN      13
#define COS_KEY_INSERT    14
#define COS_KEY_F1        16
#define COS_KEY_F12       27

/* Modifier bits, deliberately a SEPARATE small set from this codebase's
 * internal KEYBOARD_MOD_* (keyboard.h) rather than the same bit values
 * reused directly - the same "custom ABI, not a raw hardware/internal
 * mirror" principle used for COS_O_* open flags and everywhere else in
 * this syscall layer. An internal renumbering of KEYBOARD_MOD_* must not
 * silently change what a compiled .c-os binary observes. */
#define COS_MOD_SHIFT 0x01u
#define COS_MOD_CTRL  0x02u
#define COS_MOD_ALT   0x04u

typedef struct {
    char    ascii;
    uint8_t special;
    uint8_t modifiers;   /* COS_MOD_* bitmask, valid for EVERY event -
                          * e.g. Ctrl+C arrives as ascii='c' (or the raw
                          * control-code byte the keyboard driver already
                          * produces - not reinterpreted here), special=
                          * COS_KEY_NONE, modifiers=COS_MOD_CTRL, so a
                          * program can distinguish plain 'c' from Ctrl+C
                          * without needing separate key names for every
                          * modified combination. */
} cos_app_key_event_t;

/* Mouse input for a .c-os window - one discrete event per button press
 * (the down edge only, like a real click - not continuous position
 * tracking or button-held state), in the SAME client-area coordinate
 * space cos_app_window_fill_rect()/draw_text() already use, so a program
 * that just drew a button at (x,y,w,h) can hit-test a click against
 * those exact same numbers with no chrome-offset math of its own. A
 * SEPARATE small bitmask from this codebase's internal mouse button
 * encoding, for the same reason COS_MOD_ and COS_KEY_ constants are
 * separate from their internal equivalents - see cos_app_key_event_t
 * above. */
#define COS_MOUSE_BTN_LEFT   0x01u
#define COS_MOUSE_BTN_RIGHT  0x02u
#define COS_MOUSE_BTN_MIDDLE 0x04u

typedef struct {
    int32_t x, y;
    uint8_t button;   /* exactly one COS_MOUSE_BTN_* bit - which button triggered this event */
} cos_app_mouse_event_t;

/* Returns a 1-based handle, or -1. */
int64_t cos_app_window_create(const char *title, int32_t width, int32_t height,
                              uint32_t owner_pid);

/* Queue a filled rectangle in client coordinates. Rejects handles the
 * caller does not own. */
bool cos_app_window_fill_rect(int64_t handle, int32_t x, int32_t y,
                              int32_t w, int32_t h, uint32_t color,
                              uint32_t caller_pid);

/* Queue a text string in client coordinates. `text` is copied
 * immediately (not retained by reference), truncated to
 * COS_APP_MAX_TEXT_LEN. Rejects handles the caller does not own. */
bool cos_app_window_draw_text(int64_t handle, int32_t x, int32_t y,
                              uint32_t fg, uint32_t bg,
                              const char *text, uint32_t len,
                              uint32_t caller_pid);

/* Keyboard input for a .c-os window - see cos_app_key_event_t and the
 * COS_KEY_* constants in cos_app_window.c. Called from gui_input.c (GUI
 * owner thread) when the window has keyboard focus; drained by ring3 via
 * SYS_WIN_POLL_KEY. Non-opaque here only to the extent gui_input.c needs
 * it - the struct itself is defined in the .c file since nothing outside
 * these two call sites needs its layout. */
void cos_app_window_push_key(void *win_ptr, char ascii, uint8_t special, uint8_t modifiers);

/* Pops one queued key event for a window the caller owns. Returns false
 * if the queue is empty (not an error - just nothing typed yet) or the
 * handle is invalid/not owned by caller_pid. */
bool cos_app_window_poll_key(int64_t handle, uint32_t caller_pid,
                             cos_app_key_event_t *out);

/* Mouse counterpart of push_key/poll_key above - called from
 * gui_input.c's WIN_COS_APP click dispatch (GUI owner thread) and
 * drained by ring3 via SYS_WIN_POLL_MOUSE. x/y are already client-area
 * coordinates by the time this is called (the chrome-offset
 * subtraction happens once, at the call site, not per-poll). */
void cos_app_window_push_mouse(void *win_ptr, int32_t x, int32_t y, uint8_t button);
bool cos_app_window_poll_mouse(int64_t handle, uint32_t caller_pid,
                               cos_app_mouse_event_t *out);


/* Discard queued commands for a window the caller owns. */
void cos_app_window_clear(int64_t handle, uint32_t caller_pid);

/* Called from the GUI render loop (owner thread only). */
void cos_app_window_draw(int window_index);

/* Drop surfaces belonging to an exiting process. */
void cos_app_window_release_for_pid(uint32_t pid);


/* ---- v2 (SYS_WIN2_*): shared pixel buffer + unified events ----------
 * Layouts are ABI: mirrored in userland/include/cos.h. */
#define COS_EV_NONE        0
#define COS_EV_KEY         1   /* ascii/special/mods */
#define COS_EV_MOUSE_MOVE  2   /* x,y (client coords), buttons = held mask */
#define COS_EV_MOUSE_DOWN  3   /* button = the one pressed */
#define COS_EV_MOUSE_UP    4
#define COS_EV_WHEEL       5   /* wheel = +up / -down */
#define COS_EV_CLOSE       6   /* user pressed the close button; window stays until the app closes it */
#define COS_EV_OPEN        7   /* the OS asks the app to open a file; fetch it with cos_win2_get_path() */
#define COS_EV_DROP        8   /* a file was dropped on the window at x,y; path via cos_win2_get_path() */
typedef struct {
    uint32_t type;
    int32_t  x, y;
    uint8_t  button, buttons, mods, special;
    int32_t  wheel;
    char     ascii;
    uint8_t  pad[3];
} cos_win_event_t;

typedef struct {
    int64_t  handle;
    uint64_t pixels;     /* user address of the 0x00RRGGBB buffer */
    int32_t  width, height, stride;   /* stride in pixels */
    int32_t  reserved;
} cos_win_info_t;

int64_t cos_app_window_create_v2(const char *title, int32_t w, int32_t h, uint32_t owner_pid, cos_win_info_t *out);
bool    cos_app_window_present(int64_t handle, uint32_t caller_pid);
int     cos_app_window_poll_event(int64_t handle, uint32_t caller_pid, cos_win_event_t *out);
bool    cos_app_window_set_title(int64_t handle, uint32_t caller_pid, const char *title);
bool    cos_app_window_close(int64_t handle, uint32_t caller_pid);
/* GUI-thread hooks */
void    cos_app_window_mouse_state(void *win_ptr, int32_t client_x, int32_t client_y, uint8_t buttons, int32_t wheel, bool inside);
bool    cos_app_window_user_close(void *win_ptr);
void    cos_app_window_gui_tick(void);
bool    cos_app_window_deliver_path(uint32_t pid, const char *path, uint32_t ev_type, int32_t x, int32_t y);
bool    cos_app_window_deliver_drop_uid(uint32_t win_uid, const char *path, int32_t x, int32_t y);
int     cos_app_window_get_path(int64_t handle, uint32_t caller_pid, char *out, uint64_t cap);   /* true = handled (app gets COS_EV_CLOSE, window kept) */

#endif /* COS_APP_WINDOW_H */
