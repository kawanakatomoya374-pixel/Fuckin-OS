/**
 * text_editor.h - External Text File Editor Module Header
 * C-OS 4.0.8 alpha
 */

#ifndef TEXT_EDITOR_H
#define TEXT_EDITOR_H

#include <stdbool.h>
#include <stdint.h>

/* Text editor limits */
#define TEXT_EDITOR_MAX_FILES      32
#define TEXT_EDITOR_MAX_PATH_LEN   256
#define TEXT_EDITOR_MAX_CONTENT    (64 * 1024)  // 64KB max file size

/* Text editor API */
void    text_editor_init(void);
void    text_editor_shutdown(void);

int     text_editor_open(const char* filepath);
int     text_editor_new(const char* name);
bool    text_editor_close(int slot);
bool    text_editor_save(int slot);
/* Non-blocking variant of text_editor_save(): the actual disk write
 * runs on its own kernel thread. Poll text_editor_is_saving(slot) to
 * know when it's done. Returns false immediately (nothing started) if
 * the slot is invalid/unused or a save for it is already in flight. */
bool    text_editor_save_async(int slot);
bool    text_editor_is_saving(int slot);

bool    text_editor_insert(int slot, const char* text);
bool    text_editor_insert_newline(int slot);
bool    text_editor_insert_tab(int slot);
bool    text_editor_backspace(int slot);
bool    text_editor_delete_forward(int slot);
bool    text_editor_move_cursor(int slot, int direction);  // 0=left, 1=right, 2=up, 3=down, 4=home, 5=end
bool    text_editor_goto_line(int slot, uint64_t line_no);

bool    text_editor_get_info(int slot, char* path_out, uint64_t* size_out, bool* modified_out);
const char* text_editor_get_content(int slot);
uint64_t text_editor_get_cursor(int slot);
void    text_editor_set_cursor(int slot, uint64_t pos);
void    text_editor_get_scroll(int slot, uint64_t* y_out, uint64_t* x_out);
void    text_editor_set_scroll(int slot, uint64_t y, uint64_t x);

bool    text_editor_load_external(int slot, const char* external_content, uint64_t size);
bool    text_editor_save_as(int slot, const char* filepath);
bool    text_editor_find(int slot, const char* needle, uint64_t start_pos, uint64_t* found_pos);
bool    text_editor_replace_all(int slot, const char* needle, const char* replacement, uint64_t* replaced_count);

int     text_editor_get_open_count(void);
bool    text_editor_has_unsaved(int slot);

/* Cursor direction constants */
#define CURSOR_LEFT   0
#define CURSOR_RIGHT  1
#define CURSOR_UP     2
#define CURSOR_DOWN   3
#define CURSOR_HOME   4
#define CURSOR_END    5

#endif /* TEXT_EDITOR_H */