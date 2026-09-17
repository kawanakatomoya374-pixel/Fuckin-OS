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


/* Discard queued commands for a window the caller owns. */
void cos_app_window_clear(int64_t handle, uint32_t caller_pid);

/* Called from the GUI render loop (owner thread only). */
void cos_app_window_draw(int window_index);

/* Drop surfaces belonging to an exiting process. */
void cos_app_window_release_for_pid(uint32_t pid);

#endif /* COS_APP_WINDOW_H */
