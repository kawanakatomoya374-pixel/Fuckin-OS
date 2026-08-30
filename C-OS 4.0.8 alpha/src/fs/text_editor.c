/**
 * text_editor.c - External Text File Editor Module
 * C-OS 4.0.8 alpha
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

void* memset(void* ptr, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
size_t strlen(const char* s);
char* strncpy(char* dest, const char* src, size_t n);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
void serial_puts(const char* s);
void serial_puthex(uint64_t val);

#ifndef true
#define true 1
#define false 0
#endif

#define TEXT_EDITOR_MAX_FILES      32
#define TEXT_EDITOR_MAX_PATH_LEN   256
#define TEXT_EDITOR_MAX_CONTENT    (64 * 1024)

typedef struct {
    bool in_use;
    char path[TEXT_EDITOR_MAX_PATH_LEN];
    char* content;
    uint64_t content_size;
    uint64_t content_capacity;
    bool modified;
    bool is_new;
    uint64_t cursor_pos;
    uint64_t scroll_y;
    uint64_t scroll_x;
    uint64_t line_count;
    uint64_t current_line;
} text_editor_file_t;

#include "sync.h"
#include "task.h"

static text_editor_file_t editor_files[TEXT_EDITOR_MAX_FILES];
static char editor_storage[TEXT_EDITOR_MAX_FILES][TEXT_EDITOR_MAX_CONTENT + 1];
static bool text_editor_initialized = false;

/* Per-slot save state. text_editor_save() does a synchronous
 * storage_write_file() call, which for a real disk backend can block
 * for a while - long enough that doing it inline on the GUI thread
 * would freeze typing/scrolling in every other window until the write
 * finishes. text_editor_save_async() runs the existing save on its own
 * kernel thread instead. The mutex per slot prevents a second save (or
 * an edit that mutates file->content mid-write) from racing the
 * in-flight one; it does NOT lock out edits to *other* slots. */
static mutex_t editor_save_mutex[TEXT_EDITOR_MAX_FILES];
static volatile bool editor_saving[TEXT_EDITOR_MAX_FILES];
static bool editor_mutexes_ready = false;

static void editor_ensure_mutexes(void) {
    if (editor_mutexes_ready) return;
    for (int i = 0; i < TEXT_EDITOR_MAX_FILES; i++) {
        mutex_init(&editor_save_mutex[i]);
        editor_saving[i] = false;
    }
    editor_mutexes_ready = true;
}

static void text_editor_clear_slot(int i) {
    editor_files[i].in_use = false;
    editor_files[i].path[0] = '\0';
    editor_files[i].content = editor_storage[i];
    editor_files[i].content_size = 0;
    editor_files[i].content_capacity = TEXT_EDITOR_MAX_CONTENT;
    editor_files[i].modified = false;
    editor_files[i].is_new = true;
    editor_files[i].cursor_pos = 0;
    editor_files[i].scroll_y = 0;
    editor_files[i].scroll_x = 0;
    editor_files[i].line_count = 1;
    editor_files[i].current_line = 0;
    editor_storage[i][0] = '\0';
}

void text_editor_init(void) {
    serial_puts("[TEXT_EDITOR] init\n");
    for (int i = 0; i < TEXT_EDITOR_MAX_FILES; i++) {
        text_editor_clear_slot(i);
    }
    editor_ensure_mutexes();
    text_editor_initialized = true;
}

static int get_free_slot(void) {
    for (int i = 0; i < TEXT_EDITOR_MAX_FILES; i++) {
        if (!editor_files[i].in_use) return i;
    }
    return -1;
}

static int find_by_path(const char* path) {
    if (!path) return -1;
    for (int i = 0; i < TEXT_EDITOR_MAX_FILES; i++) {
        if (editor_files[i].in_use && strcmp(editor_files[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static uint64_t count_lines(const char* content, uint64_t size) {
    if (!content || size == 0) return 1;
    uint64_t lines = 1;
    for (uint64_t i = 0; i < size; i++) {
        if (content[i] == '\n') lines++;
    }
    return lines;
}

int text_editor_open(const char* filepath) {
    if (!filepath || filepath[0] == 0) return -1;

    int existing = find_by_path(filepath);
    if (existing >= 0) return existing;

    int slot = get_free_slot();
    if (slot < 0) return -1;

    text_editor_file_t* file = &editor_files[slot];
    text_editor_clear_slot(slot);
    file->in_use = true;
    strncpy(file->path, filepath, TEXT_EDITOR_MAX_PATH_LEN - 1);
    file->path[TEXT_EDITOR_MAX_PATH_LEN - 1] = 0;
    file->content = editor_storage[slot];

    serial_puts("[TEXT_EDITOR] open: ");
    serial_puts(filepath);
    serial_puts("\n");

    extern bool storage_read_file(const char* fn, void* buf, uint64_t bsz, uint64_t* out);
    extern bool storage_file_exists(const char* fn);

    if (storage_file_exists(filepath)) {
        uint64_t read_size = 0;
        if (storage_read_file(filepath, file->content, file->content_capacity, &read_size)) {
            file->content_size = read_size;
            file->is_new = false;
        } else {
            file->content_size = 0;
            file->is_new = true;
        }
    } else {
        file->content_size = 0;
        file->is_new = true;
    }

    file->content[file->content_size] = 0;
    file->modified = false;
    file->cursor_pos = 0;
    file->scroll_y = 0;
    file->scroll_x = 0;
    file->line_count = count_lines(file->content, file->content_size);
    file->current_line = 0;

    return slot;
}

int text_editor_new(const char* name) {
    if (!name || name[0] == 0) return -1;

    int slot = get_free_slot();
    if (slot < 0) return -1;

    text_editor_file_t* file = &editor_files[slot];
    text_editor_clear_slot(slot);
    file->in_use = true;
    strncpy(file->path, name, TEXT_EDITOR_MAX_PATH_LEN - 1);
    file->path[TEXT_EDITOR_MAX_PATH_LEN - 1] = 0;
    file->content = editor_storage[slot];
    file->content[0] = 0;

    return slot;
}

bool text_editor_insert(int slot, const char* text) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use || !text || !file->content) return false;

    size_t text_len = strlen(text);
    if (file->content_size + text_len > file->content_capacity) return false;

    editor_ensure_mutexes();
    mutex_lock(&editor_save_mutex[slot]);

    for (uint64_t i = file->content_size; i > file->cursor_pos; i--) {
        file->content[i] = file->content[i - 1];
    }
    for (size_t i = 0; i < text_len; i++) {
        file->content[file->cursor_pos + i] = text[i];
    }

    file->content_size += text_len;
    file->content[file->content_size] = 0;
    file->modified = true;
    file->cursor_pos += text_len;
    file->line_count = count_lines(file->content, file->content_size);

    mutex_unlock(&editor_save_mutex[slot]);
    return true;
}

bool text_editor_backspace(int slot) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use || file->cursor_pos == 0 || !file->content) return false;

    editor_ensure_mutexes();
    mutex_lock(&editor_save_mutex[slot]);

    for (uint64_t i = file->cursor_pos - 1; i < file->content_size; i++) {
        file->content[i] = file->content[i + 1];
    }
    file->content_size--;
    file->modified = true;
    file->cursor_pos--;
    file->line_count = count_lines(file->content, file->content_size);

    mutex_unlock(&editor_save_mutex[slot]);
    return true;
}

bool text_editor_move_cursor(int slot, int direction) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use || !file->content) return false;

    switch (direction) {
        case 0:
            if (file->cursor_pos > 0) file->cursor_pos--;
            break;
        case 1:
            if (file->cursor_pos < file->content_size) file->cursor_pos++;
            break;
        case 2:
            if (file->cursor_pos > 0) {
                uint64_t ls = file->cursor_pos;
                while (ls > 0 && file->content[ls - 1] != '\n') ls--;
                if (ls > 0) {
                    ls--;
                    while (ls > 0 && file->content[ls - 1] != '\n') ls--;
                    file->cursor_pos = ls;
                }
            }
            if (file->scroll_y > 0) file->scroll_y--;
            break;
        case 3:
            {
                uint64_t nl = file->cursor_pos;
                while (nl < file->content_size && file->content[nl] != '\n') nl++;
                if (nl < file->content_size) {
                    file->cursor_pos = (nl + 1 < file->content_size) ? nl + 1 : file->content_size;
                }
            }
            file->scroll_y++;
            break;
        case 4:
            while (file->cursor_pos > 0 && file->content[file->cursor_pos - 1] != '\n') {
                file->cursor_pos--;
            }
            break;
        case 5:
            while (file->cursor_pos < file->content_size && file->content[file->cursor_pos] != '\n') {
                file->cursor_pos++;
            }
            break;
    }
    return true;
}

bool text_editor_save(int slot) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use || !file->content) return false;

    extern bool storage_write_file(const char* fn, const void* data, uint64_t size);

    editor_ensure_mutexes();
    mutex_lock(&editor_save_mutex[slot]);
    bool ok = storage_write_file(file->path, file->content, file->content_size);
    if (ok) {
        file->modified = false;
    }
    mutex_unlock(&editor_save_mutex[slot]);
    return ok;
}

typedef struct {
    int slot;
} editor_save_thread_arg_t;

static editor_save_thread_arg_t editor_save_args[TEXT_EDITOR_MAX_FILES];

static void editor_save_thread_entry(void* arg) {
    editor_save_thread_arg_t* a = (editor_save_thread_arg_t*)arg;
    int slot = a->slot;
    text_editor_save(slot);
    editor_saving[slot] = false;
}

/* Non-blocking save: returns immediately, does the actual
 * storage_write_file() on a dedicated kernel thread. Returns false
 * without doing anything if the slot is invalid, unused, or a save for
 * this slot is already in flight. Poll text_editor_is_saving(slot) to
 * find out when it's done; text_editor_has_unsaved(slot) reflects the
 * result once it clears. */
bool text_editor_save_async(int slot) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return false;
    if (!editor_files[slot].in_use) return false;

    editor_ensure_mutexes();
    if (editor_saving[slot]) return false;

    editor_saving[slot] = true;
    editor_save_args[slot].slot = slot;
    if (!thread_create_kernel("editor_save", (void*)editor_save_thread_entry, &editor_save_args[slot])) {
        editor_saving[slot] = false;
        return false;
    }
    return true;
}

bool text_editor_is_saving(int slot) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return false;
    return editor_saving[slot];
}

bool text_editor_close(int slot) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use) return false;

    if (file->modified) text_editor_save(slot);

    text_editor_clear_slot(slot);
    return true;
}

bool text_editor_get_info(int slot, char* path_out, uint64_t* size_out, bool* modified_out) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use) return false;

    if (path_out) strncpy(path_out, file->path, TEXT_EDITOR_MAX_PATH_LEN - 1);
    if (size_out) *size_out = file->content_size;
    if (modified_out) *modified_out = file->modified;

    return true;
}

const char* text_editor_get_content(int slot) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return NULL;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use) return NULL;
    return file->content;
}

uint64_t text_editor_get_cursor(int slot) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return 0;
    return editor_files[slot].cursor_pos;
}

void text_editor_set_cursor(int slot, uint64_t pos) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return;
    text_editor_file_t* file = &editor_files[slot];
    if (file->in_use && pos <= file->content_size) {
        file->cursor_pos = pos;
    }
}

void text_editor_get_scroll(int slot, uint64_t* y_out, uint64_t* x_out) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use) return;
    if (y_out) *y_out = file->scroll_y;
    if (x_out) *x_out = file->scroll_x;
}

void text_editor_set_scroll(int slot, uint64_t y, uint64_t x) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return;
    text_editor_file_t* file = &editor_files[slot];
    if (file->in_use) {
        file->scroll_y = y;
        file->scroll_x = x;
    }
}

bool text_editor_load_external(int slot, const char* external_content, uint64_t size) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use || !external_content || size == 0 || !file->content) return false;
    if (size > file->content_capacity) size = file->content_capacity;

    editor_ensure_mutexes();
    mutex_lock(&editor_save_mutex[slot]);

    for (uint64_t i = 0; i < size; i++) {
        file->content[i] = external_content[i];
    }
    file->content_size = size;
    file->content[file->content_size] = 0;
    file->modified = false;
    file->line_count = count_lines(file->content, file->content_size);

    mutex_unlock(&editor_save_mutex[slot]);
    return true;
}

void text_editor_shutdown(void) {
    for (int i = 0; i < TEXT_EDITOR_MAX_FILES; i++) {
        if (editor_files[i].in_use) text_editor_close(i);
    }
    text_editor_initialized = false;
}

int text_editor_get_open_count(void) {
    int count = 0;
    for (int i = 0; i < TEXT_EDITOR_MAX_FILES; i++) {
        if (editor_files[i].in_use) count++;
    }
    return count;
}

bool text_editor_has_unsaved(int slot) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use) return false;
    return file->modified;
}

/* Extended editor features ------------------------------------------------- */
static uint64_t te_count_lines_from_buf(const char* buf, uint64_t size) {
    if (!buf || size == 0) return 1;
    uint64_t lines = 1;
    for (uint64_t i = 0; i < size; ++i) {
        if (buf[i] == '\n') lines++;
    }
    return lines;
}

static uint64_t te_find_substring(const char* haystack, uint64_t hay_len, const char* needle) {
    if (!haystack || !needle || needle[0] == '\0') return (uint64_t)-1;
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > hay_len) return (uint64_t)-1;
    for (uint64_t i = 0; i + needle_len <= hay_len; ++i) {
        size_t j = 0;
        for (; j < needle_len; ++j) {
            if (haystack[i + j] != needle[j]) break;
        }
        if (j == needle_len) return i;
    }
    return (uint64_t)-1;
}

static bool te_expand_or_shrink(text_editor_file_t* file, uint64_t pos, uint64_t match_len, const char* replacement) {
    if (!file || !file->content || !replacement) return false;
    size_t repl_len = strlen(replacement);
    if (pos > file->content_size || match_len > file->content_size - pos) return false;
    if (file->content_size - match_len + repl_len > file->content_capacity) return false;

    if (repl_len != match_len) {
        if (repl_len > match_len) {
            uint64_t delta = (uint64_t)(repl_len - match_len);
            for (uint64_t i = file->content_size; i > pos + match_len; --i) {
                file->content[i + delta - 1] = file->content[i - 1];
            }
        } else {
            uint64_t delta = match_len - (uint64_t)repl_len;
            for (uint64_t i = pos + match_len; i <= file->content_size; ++i) {
                file->content[i - delta] = file->content[i];
            }
        }
    }

    for (size_t i = 0; i < repl_len; ++i) {
        file->content[pos + i] = replacement[i];
    }
    file->content_size = file->content_size - match_len + (uint64_t)repl_len;
    file->content[file->content_size] = '\0';
    file->cursor_pos = pos + (uint64_t)repl_len;
    file->line_count = te_count_lines_from_buf(file->content, file->content_size);
    file->modified = true;
    return true;
}

bool text_editor_insert_newline(int slot) {
    return text_editor_insert(slot, "\n");
}

bool text_editor_insert_tab(int slot) {
    return text_editor_insert(slot, "\t");
}

bool text_editor_delete_forward(int slot) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES) return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use || !file->content || file->cursor_pos >= file->content_size) return false;

    editor_ensure_mutexes();
    mutex_lock(&editor_save_mutex[slot]);

    for (uint64_t i = file->cursor_pos; i < file->content_size; ++i) {
        file->content[i] = file->content[i + 1];
    }
    if (file->content_size > 0) file->content_size--;
    file->content[file->content_size] = '\0';
    file->line_count = te_count_lines_from_buf(file->content, file->content_size);
    file->modified = true;

    mutex_unlock(&editor_save_mutex[slot]);
    return true;
}

bool text_editor_goto_line(int slot, uint64_t line_no) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES || line_no == 0) return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use || !file->content) return false;

    uint64_t line = 1;
    uint64_t pos = 0;
    while (pos < file->content_size && line < line_no) {
        if (file->content[pos++] == '\n') line++;
    }
    if (line != line_no) return false;
    file->cursor_pos = pos;
    return true;
}

bool text_editor_find(int slot, const char* needle, uint64_t start_pos, uint64_t* found_pos) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES || !needle || needle[0] == '\0') return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use || !file->content) return false;
    if (start_pos > file->content_size) start_pos = file->content_size;

    uint64_t hit = te_find_substring(file->content + start_pos, file->content_size - start_pos, needle);
    if (hit == (uint64_t)-1) return false;
    if (found_pos) *found_pos = start_pos + hit;
    return true;
}

bool text_editor_replace_all(int slot, const char* needle, const char* replacement, uint64_t* replaced_count) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES || !needle || !replacement || needle[0] == '\0') return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use || !file->content) return false;

    uint64_t count = 0;
    uint64_t pos = 0;
    size_t needle_len = strlen(needle);
    size_t repl_len = strlen(replacement);
    if (repl_len > needle_len && file->content_size + (repl_len - needle_len) > file->content_capacity) return false;

    editor_ensure_mutexes();
    mutex_lock(&editor_save_mutex[slot]);

    while (pos <= file->content_size) {
        uint64_t hit = te_find_substring(file->content + pos, file->content_size - pos, needle);
        if (hit == (uint64_t)-1) break;
        hit += pos;
        if (!te_expand_or_shrink(file, hit, (uint64_t)needle_len, replacement)) {
            mutex_unlock(&editor_save_mutex[slot]);
            return false;
        }
        count++;
        pos = hit + (uint64_t)repl_len;
        if (repl_len == 0 && pos < file->content_size) pos++;
    }

    mutex_unlock(&editor_save_mutex[slot]);
    if (replaced_count) *replaced_count = count;
    return true;
}

bool text_editor_save_as(int slot, const char* filepath) {
    if (slot < 0 || slot >= TEXT_EDITOR_MAX_FILES || !filepath || filepath[0] == '\0') return false;
    text_editor_file_t* file = &editor_files[slot];
    if (!file->in_use || !file->content) return false;

    char old_path[TEXT_EDITOR_MAX_PATH_LEN];
    strncpy(old_path, file->path, TEXT_EDITOR_MAX_PATH_LEN - 1);
    old_path[TEXT_EDITOR_MAX_PATH_LEN - 1] = '\0';

    strncpy(file->path, filepath, TEXT_EDITOR_MAX_PATH_LEN - 1);
    file->path[TEXT_EDITOR_MAX_PATH_LEN - 1] = '\0';
    if (text_editor_save(slot)) return true;

    strncpy(file->path, old_path, TEXT_EDITOR_MAX_PATH_LEN - 1);
    file->path[TEXT_EDITOR_MAX_PATH_LEN - 1] = '\0';
    return false;
}
