/*
 * python_ide_gui.c - C-OS Python IDE
 * Modern editor + console front-end for MicroPython.
 */

#include "python_ide_gui.h"
#include "../../kernel/ai/micropython_integration.h"
#include "../../kernel/ai/micropython_interface.h"
#include "../../kernel/drivers/http.h"
#include "serial.h"
#include <string.h>
#include <stdio.h>

extern uint64_t get_timer_ticks(void);

#ifndef KEY_DELETE
#define KEY_DELETE 0x53
#endif
#ifndef KEY_HOME
#define KEY_HOME 0x47
#endif
#ifndef KEY_END
#define KEY_END 0x4F
#endif
#ifndef KEY_PGUP
#define KEY_PGUP 0x49
#endif
#ifndef KEY_PGDN
#define KEY_PGDN 0x51
#endif
#ifndef KEY_F5
#define KEY_F5 0x3F
#endif
#ifndef KEY_F6
#define KEY_F6 0x40
#endif
#ifndef KEY_F7
#define KEY_F7 0x41
#endif
#ifndef KEY_F9
#define KEY_F9 0x43
#endif


#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

#define IDE_TOOLBAR_H   (FONT_H + 14)
#define IDE_STATUS_H    (FONT_H + 8)
#define IDE_CONSOLE_H   (FONT_H * 6 + 26)
#define IDE_MARGIN      10
#define IDE_GUTTER_W    (FONT_W * 5 + 16)
#define IDE_BUTTON_H    (FONT_H + 8)
#define IDE_BUTTON_PAD  14
#define IDE_TABBAR_H    (FONT_H + 8)

typedef struct {
    const char* label;
    int x, y, w, h;
} ide_button_t;

static python_ide_t ide;
static bool ide_initialized = false;
static bool backend_initialized = false;
static ide_button_t toolbar_buttons[10];
static ide_button_t tab_buttons[IDE_MAX_TABS];
static char tab_button_labels[IDE_MAX_TABS][128];
static int ide_mouse_x = -1;
static int ide_mouse_y = -1;

/* Previously this only wrote to python_ide_win_x/y/w/h, four globals that
 * nothing ever read - ide.x/y/w/h (what python_ide_draw() and every hit-test
 * in this file actually use) stayed hardcoded at the values set once in
 * python_ide_init(). That's why the window frame could be dragged/resized by
 * the window manager while the IDE's own content stayed glued in place: the
 * "window" and the "UI" were tracking two completely separate positions. */
void python_ide_set_geometry(int x, int y, int w, int h) {
    ide.x = x;
    ide.y = y;
    ide.w = w;
    ide.h = h;
}

static void ide_set_text(char* dst, size_t dst_sz, const char* src);
static void ide_append_text(char* dst, size_t dst_sz, const char* src);
static void ide_clear_buffer(char* dst, size_t dst_sz);
static void ide_push_console(const char* message);
static void ide_ensure_visible(void);
static void ide_set_current_line(const char* line);
static void ide_shift_lines_down(int from);
static void ide_split_current_line(void);
static void ide_join_with_previous_line(void);
static void ide_delete_at_cursor(void);
static void ide_insert_char(char c);
static void ide_insert_text(const char* text);
static void ide_execute_text(const char* code);
static void ide_rebuild_completion_hint(void);
static void ide_run_repl_command(void);
static void ide_load_text_from_buffer(const char* data, size_t len);
static void ide_load_defaults(void);
static int ide_current_line_len(void);
static void ide_snapshot_current_tab(void);
static void ide_restore_tab(int idx);
static void ide_action_new(void);
static void ide_action_open(void);
static void ide_action_save(void);
static void ide_action_run(void);
static void ide_action_toggle_repl(void);
static int ide_clamp_int(int value, int min_value, int max_value);
static void ide_draw_text_clipped(int x, int y, const char* text, int max_px, uint64_t fg, uint64_t bg);
static void ide_truncate_label(char* dst, size_t dst_sz, const char* src, int max_px);

static void ide_set_text(char* dst, size_t dst_sz, const char* src) {
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (i + 1 < dst_sz && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void ide_append_text(char* dst, size_t dst_sz, const char* src) {
    if (!dst || !src || dst_sz == 0) return;
    size_t len = strlen(dst);
    size_t i = 0;
    while (len + i + 1 < dst_sz && src[i]) {
        dst[len + i] = src[i];
        ++i;
    }
    dst[len + i] = '\0';
}

static void ide_clear_buffer(char* dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
}

static int ide_clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static void ide_draw_text_clipped(int x, int y, const char* text, int max_px, uint64_t fg, uint64_t bg) {
    if (!text || max_px <= 0) return;
    int max_chars = max_px / FONT_W;
    if (max_chars <= 0) return;
    char buf[256];
    size_t len = strlen(text);
    if ((int)len <= max_chars) {
        vga_draw_string(x, y, text, fg, bg);
        return;
    }
    if (max_chars < 4) {
        strcpy(buf, "...");
    } else {
        size_t keep = (size_t)(max_chars - 3);
        if (keep > sizeof(buf) - 4) keep = sizeof(buf) - 4;
        memcpy(buf, text, keep);
        memcpy(buf + keep, "...", 4);
    }
    vga_draw_string(x, y, buf, fg, bg);
}

static void ide_truncate_label(char* dst, size_t dst_sz, const char* src, int max_px) {
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    int max_chars = max_px / FONT_W;
    if (max_chars <= 0) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if ((int)len <= max_chars) {
        ide_set_text(dst, dst_sz, src);
        return;
    }
    if (dst_sz < 4) {
        size_t keep = dst_sz - 1;
        memcpy(dst, src, keep);
        dst[keep] = '\0';
        return;
    }
    size_t keep = (size_t)(max_chars > 3 ? max_chars - 3 : 0);
    if (keep > dst_sz - 4) keep = dst_sz - 4;
    memcpy(dst, src, keep);
    memcpy(dst + keep, "...", 4);
}

static const char* const ide_completion_words[] = {
    "and","as","assert","break","class","continue","def","del","elif","else","except","False","finally",
    "for","from","global","if","import","in","is","lambda","len","list","not","None","or","pass",
    "print","range","return","str","True","try","while","with","yield"
};

static bool ide_is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void ide_build_code_buffer(char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < ide.line_count; ++i) {
        size_t len = strlen(ide.lines[i]);
        if (used + len + 2 >= out_sz) break;
        memcpy(out + used, ide.lines[i], len);
        used += len;
        if (i < ide.line_count - 1) out[used++] = '\n';
        out[used] = '\0';
    }
}

static void ide_update_completion(void) {
    ide.completion[0] = '\0';
    if (ide.cursor_y < 0 || ide.cursor_y >= ide.line_count) return;
    const char* line = ide.lines[ide.cursor_y];
    int pos = ide.cursor_x;
    int len = (int)strlen(line);
    if (pos > len) pos = len;
    int start = pos;
    while (start > 0 && ide_is_ident_char(line[start - 1])) start--;
    int prefix_len = pos - start;
    if (prefix_len <= 0) return;
    char prefix[64];
    if (prefix_len >= (int)sizeof(prefix)) prefix_len = (int)sizeof(prefix) - 1;
    memcpy(prefix, &line[start], (size_t)prefix_len);
    prefix[prefix_len] = '\0';
    for (size_t i = 0; i < sizeof(ide_completion_words)/sizeof(ide_completion_words[0]); ++i) {
        if (strncmp(ide_completion_words[i], prefix, (size_t)prefix_len) == 0) {
            ide_set_text(ide.completion, sizeof(ide.completion), ide_completion_words[i]);
            return;
        }
    }
}

static void ide_rebuild_completion_hint(void) {
    ide_update_completion();
}

static void ide_apply_completion(void) {
    if (ide.completion[0] == '\0' || ide.cursor_y < 0 || ide.cursor_y >= ide.line_count) return;
    char* line = ide.lines[ide.cursor_y];
    int pos = ide.cursor_x;
    int len = (int)strlen(line);
    if (pos > len) pos = len;
    int start = pos;
    while (start > 0 && ide_is_ident_char(line[start - 1])) start--;
    int comp_len = (int)strlen(ide.completion);
    if (len - (pos - start) + comp_len >= IDE_MAX_LINE_LENGTH) return;
    memmove(&line[start + comp_len], &line[pos], (size_t)(len - pos + 1));
    memcpy(&line[start], ide.completion, (size_t)comp_len);
    ide.cursor_x = start + comp_len;
    ide.modified = true;
    ide_snapshot_current_tab();
}

static void ide_autosave_tick(void) {
    if (!ide.autosave_enabled || !ide.modified) return;
    uint64_t now = get_timer_ticks();
    if (ide.autosave_next_tick != 0 && now < ide.autosave_next_tick) return;
    ide.autosave_next_tick = now + 250;
    ide_snapshot_current_tab();
    python_ide_save_file();
}

static void ide_debug_load(void) {
    char code[IDE_MAX_FILE_SIZE];
    ide_build_code_buffer(code, sizeof(code));
    if (micropython_debug_load_source(code, ide.filename) == 0) {
        ide.debug_paused = false;
        ide_set_text(ide.status, sizeof(ide.status), "Debug loaded");
        ide_push_console("Debug session loaded.");
    } else {
        ide_push_console("Debug load failed.");
        ide_set_text(ide.status, sizeof(ide.status), "Debug load failed");
    }
}

static void ide_debug_step_internal(void) {
    char output[1024]; output[0] = '\0';
    if (!micropython_debug_is_loaded()) ide_debug_load();
    if (micropython_debug_step(output, sizeof(output)) == 0) {
        if (output[0]) ide_push_console(output);
        ide.debug_paused = micropython_debug_is_paused();
        if (ide.debug_paused) snprintf(ide.status, sizeof(ide.status), "Paused at line %u", (unsigned)micropython_debug_current_line());
    } else if (output[0]) {
        ide_push_console(output);
    }
}

static void ide_debug_continue_internal(void) {
    char output[1024]; output[0] = '\0';
    if (!micropython_debug_is_loaded()) ide_debug_load();
    if (micropython_debug_continue(output, sizeof(output)) == 0) {
        if (output[0]) ide_push_console(output);
        ide.debug_paused = micropython_debug_is_paused();
        if (!ide.debug_paused) ide_set_text(ide.status, sizeof(ide.status), "Debug finished");
    } else if (output[0]) {
        ide_push_console(output);
    }
}

static void ide_toggle_breakpoint_internal(void) {
    uint32_t line = (uint32_t)(ide.cursor_y + 1);
    if (line == 0) return;
    if (ide.breakpoint_line == line) {
        micropython_debug_remove_breakpoint(line);
        ide.breakpoint_line = 0;
        ide_push_console("Breakpoint cleared.");
    } else {
        micropython_debug_add_breakpoint(line);
        ide.breakpoint_line = line;
        ide_push_console("Breakpoint set.");
    }
}

static void ide_switch_tab(int idx) {
    if (idx < 0 || idx >= ide.tab_count || idx == ide.current_tab) return;
    ide_snapshot_current_tab();
    ide.current_tab = idx;
    ide_restore_tab(idx);
    snprintf(ide.status, sizeof(ide.status), "Tab %d/%d", idx + 1, ide.tab_count);
}
static void ide_push_console(const char* message) {
    if (!message) return;
    if (ide.console_line_count < IDE_CONSOLE_LINES) {
        ide_set_text(ide.console_lines[ide.console_line_count], sizeof(ide.console_lines[0]), message);
        ide.console_line_count++;
    } else {
        for (int i = 1; i < IDE_CONSOLE_LINES; ++i) {
            ide_set_text(ide.console_lines[i - 1], sizeof(ide.console_lines[0]), ide.console_lines[i]);
        }
        ide_set_text(ide.console_lines[IDE_CONSOLE_LINES - 1], sizeof(ide.console_lines[0]), message);
    }
    if (ide.console_line_count > 0) {
        ide.console_scroll = MAX(0, ide.console_line_count - 6);
    }
}

void python_ide_add_console(const char* message) {
    ide_push_console(message);
}

static void ide_load_defaults(void) {
    ide_clear_buffer(ide.lines[0], sizeof(ide.lines[0]));
    ide_clear_buffer(ide.lines[1], sizeof(ide.lines[1]));
    ide_clear_buffer(ide.lines[2], sizeof(ide.lines[2]));
    ide_clear_buffer(ide.lines[3], sizeof(ide.lines[3]));
    ide_clear_buffer(ide.lines[4], sizeof(ide.lines[4]));
    ide_set_text(ide.lines[0], sizeof(ide.lines[0]), "# C-OS Python IDE");
    ide_set_text(ide.lines[1], sizeof(ide.lines[1]), "print('Hello from MicroPython')");
    ide_set_text(ide.lines[2], sizeof(ide.lines[2]), "");
    ide_set_text(ide.lines[3], sizeof(ide.lines[3]), "for i in range(3):");
    ide_set_text(ide.lines[4], sizeof(ide.lines[4]), "    print(i)");
    ide.line_count = 5;
    ide.cursor_x = 0;
    ide.cursor_y = 0;
    ide.scroll_y = 0;
    ide.modified = false;
}

/* Performance/memory fix: this used to unconditionally copy all
 * IDE_MAX_LINE_COUNT (1024) line buffers on every single keystroke
 * (ide_insert_char/ide_delete_at_cursor/ide_apply_completion all call this),
 * even though a real script is almost always a handful of lines. That made
 * every keypress do O(1024) strlen+copy work instead of O(line_count).
 * We now only copy the lines that actually exist, and clear just the
 * (small) leftover range from the previous snapshot so stale tab data can't
 * survive a shrink (e.g. after deleting lines then switching tabs). */
static void ide_snapshot_current_tab(void) {
    if (ide.current_tab < 0 || ide.current_tab >= IDE_MAX_TABS) return;
    python_ide_tab_t* tab = &ide.tabs[ide.current_tab];
    ide_set_text(tab->filename, sizeof(tab->filename), ide.filename);
    int old_count = tab->line_count;
    int new_count = ide.line_count;
    if (new_count > IDE_MAX_LINE_COUNT) new_count = IDE_MAX_LINE_COUNT;
    tab->line_count = new_count;
    tab->cursor_x = ide.cursor_x;
    tab->cursor_y = ide.cursor_y;
    tab->scroll_y = ide.scroll_y;
    tab->modified = ide.modified;
    for (int i = 0; i < new_count; ++i) {
        ide_set_text(tab->lines[i], sizeof(tab->lines[i]), ide.lines[i]);
    }
    for (int i = new_count; i < old_count && i < IDE_MAX_LINE_COUNT; ++i) {
        tab->lines[i][0] = '\0';
    }
}

static void ide_restore_tab(int idx) {
    if (idx < 0 || idx >= IDE_MAX_TABS) return;
    python_ide_tab_t* tab = &ide.tabs[idx];
    ide_set_text(ide.filename, sizeof(ide.filename), tab->filename[0] ? tab->filename : "untitled.py");
    ide.line_count = tab->line_count > 0 ? tab->line_count : 1;
    if (ide.line_count > IDE_MAX_LINE_COUNT) ide.line_count = IDE_MAX_LINE_COUNT;
    for (int i = 0; i < ide.line_count; ++i) {
        ide_set_text(ide.lines[i], sizeof(ide.lines[i]), tab->lines[i]);
    }
    for (int i = ide.line_count; i < IDE_MAX_LINE_COUNT; ++i) {
        ide.lines[i][0] = '\0';
    }
    if (ide.line_count == 0) {
        ide.lines[0][0] = '\0';
        ide.line_count = 1;
    }
    ide.cursor_x = tab->cursor_x;
    ide.cursor_y = tab->cursor_y;
    ide.scroll_y = tab->scroll_y;
    ide.modified = tab->modified;
    if (ide.cursor_y < 0) ide.cursor_y = 0;
    if (ide.cursor_y >= ide.line_count) ide.cursor_y = ide.line_count - 1;
    if (ide.cursor_x < 0) ide.cursor_x = 0;
    int len = (ide.cursor_y >= 0 && ide.cursor_y < ide.line_count) ? (int)strlen(ide.lines[ide.cursor_y]) : 0;
    if (ide.cursor_x > len) ide.cursor_x = len;
    if (ide.scroll_y < 0) ide.scroll_y = 0;
}

void python_ide_init(void) {
    if (!ide_initialized) {
        memset(&ide, 0, sizeof(ide));
        ide.x = 64;
        ide.y = 40;
        ide.w = 960;
        ide.h = 680;
        ide.visible = true;
        ide.modified = false;
        ide.executing = false;
        ide.repl_active = false;
        ide.repl_cursor_pos = 0;
        ide.console_scroll = 0;
        ide.console_line_count = 0;
        ide.current_tab = 0;
        ide.tab_count = 1;
        ide.autosave_enabled = true;
        ide.autosave_next_tick = 0;
        ide.debug_paused = false;
        ide.breakpoint_line = 0;
        ide_set_text(ide.filename, sizeof(ide.filename), "untitled.py");
        ide_set_text(ide.tabs[0].filename, sizeof(ide.tabs[0].filename), ide.filename);
        ide_load_defaults();
        ide_snapshot_current_tab();
        ide_set_text(ide.status, sizeof(ide.status), "Ready");
        ide_initialized = true;
    }

    if (ide.console_line_count == 0) {
        ide_push_console("Python IDE ready.");
        ide_push_console("MicroPython backend will execute code from the editor.");
    }

    if (!backend_initialized) {
        if (micropython_integration_init() == 0) {
            ide_push_console("MicroPython integration initialized.");
            backend_initialized = true;
        } else {
            ide_push_console("MicroPython integration failed to initialize.");
        }
    }
}

static int ide_current_line_len(void) {
    if (ide.cursor_y < 0 || ide.cursor_y >= ide.line_count) return 0;
    return (int)strlen(ide.lines[ide.cursor_y]);
}

static void ide_trim_line(int line_idx) {
    if (line_idx < 0 || line_idx >= ide.line_count) return;
    ide.lines[line_idx][IDE_MAX_LINE_LENGTH - 1] = '\0';
}

static void ide_set_current_line(const char* line) {
    if (ide.cursor_y < 0 || ide.cursor_y >= IDE_MAX_LINE_COUNT) return;
    ide_set_text(ide.lines[ide.cursor_y], sizeof(ide.lines[0]), line ? line : "");
    ide_trim_line(ide.cursor_y);
}

static void ide_shift_lines_down(int from) {
    if (from < 0 || from >= IDE_MAX_LINE_COUNT - 1) return;
    if (ide.line_count >= IDE_MAX_LINE_COUNT) return;
    for (int i = ide.line_count; i > from; --i) {
        ide_set_text(ide.lines[i], sizeof(ide.lines[0]), ide.lines[i - 1]);
    }
    ide.line_count++;
}

static void ide_ensure_visible(void) {
    int visible_editor_lines = ((ide.h - IDE_TOOLBAR_H - IDE_CONSOLE_H - IDE_STATUS_H - 4) / (FONT_H + 6));
    if (visible_editor_lines < 1) visible_editor_lines = 1;
    if (ide.cursor_y < ide.scroll_y) ide.scroll_y = ide.cursor_y;
    if (ide.cursor_y >= ide.scroll_y + visible_editor_lines) ide.scroll_y = ide.cursor_y - visible_editor_lines + 1;
    if (ide.scroll_y < 0) ide.scroll_y = 0;
    if (ide.scroll_y > ide.line_count - 1) ide.scroll_y = MAX(0, ide.line_count - 1);
}

static void ide_join_with_previous_line(void) {
    if (ide.cursor_y <= 0 || ide.cursor_y >= ide.line_count) return;
    int prev = ide.cursor_y - 1;
    int prev_len = (int)strlen(ide.lines[prev]);
    int cur_len = (int)strlen(ide.lines[ide.cursor_y]);
    if (prev_len + cur_len >= IDE_MAX_LINE_LENGTH) return;

    memcpy(ide.lines[prev] + prev_len, ide.lines[ide.cursor_y], (size_t)cur_len + 1);
    for (int i = ide.cursor_y; i < ide.line_count - 1; ++i) {
        ide_set_text(ide.lines[i], sizeof(ide.lines[0]), ide.lines[i + 1]);
    }
    ide.line_count--;
    ide.cursor_y = prev;
    ide.cursor_x = prev_len;
    ide.modified = true;
}

static void ide_split_current_line(void) {
    if (ide.line_count >= IDE_MAX_LINE_COUNT) return;
    if (ide.cursor_y < 0 || ide.cursor_y >= ide.line_count) return;

    char current[IDE_MAX_LINE_LENGTH];
    ide_set_text(current, sizeof(current), ide.lines[ide.cursor_y]);
    int len = (int)strlen(current);
    int pos = ide.cursor_x;
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;

    char left[IDE_MAX_LINE_LENGTH];
    char right[IDE_MAX_LINE_LENGTH];
    left[0] = '\0';
    right[0] = '\0';

    memcpy(left, current, (size_t)pos);
    left[pos] = '\0';
    ide_set_text(right, sizeof(right), current + pos);

    ide_set_text(ide.lines[ide.cursor_y], sizeof(ide.lines[0]), left);
    ide_shift_lines_down(ide.cursor_y + 1);
    ide_set_text(ide.lines[ide.cursor_y + 1], sizeof(ide.lines[0]), right);

    ide.cursor_y++;
    ide.cursor_x = 0;
    ide.modified = true;
    ide_snapshot_current_tab();
}

static void ide_delete_at_cursor(void) {
    if (ide.cursor_y < 0 || ide.cursor_y >= ide.line_count) return;
    int len = (int)strlen(ide.lines[ide.cursor_y]);
    if (ide.cursor_x < len) {
        memmove(&ide.lines[ide.cursor_y][ide.cursor_x], &ide.lines[ide.cursor_y][ide.cursor_x + 1], (size_t)(len - ide.cursor_x));
        ide.modified = true;
        ide_snapshot_current_tab();
        return;
    }
    if (ide.cursor_y < ide.line_count - 1) {
        int next_len = (int)strlen(ide.lines[ide.cursor_y + 1]);
        if (len + next_len < IDE_MAX_LINE_LENGTH) {
            memcpy(ide.lines[ide.cursor_y] + len, ide.lines[ide.cursor_y + 1], (size_t)next_len + 1);
            for (int i = ide.cursor_y + 1; i < ide.line_count - 1; ++i) {
                ide_set_text(ide.lines[i], sizeof(ide.lines[0]), ide.lines[i + 1]);
            }
            ide.line_count--;
            ide.modified = true;
        }
    }
}

static void ide_join_left_char(void) {
    if (ide.cursor_y < 0 || ide.cursor_y >= ide.line_count) return;
    if (ide.cursor_x > 0) {
        int len = (int)strlen(ide.lines[ide.cursor_y]);
        memmove(&ide.lines[ide.cursor_y][ide.cursor_x - 1], &ide.lines[ide.cursor_y][ide.cursor_x], (size_t)(len - ide.cursor_x + 1));
        ide.cursor_x--;
        ide.modified = true;
        return;
    }
    ide_join_with_previous_line();
}

static void ide_insert_text(const char* text) {
    if (!text) return;
    for (const char* p = text; *p; ++p) ide_insert_char(*p);
}

static void ide_insert_char(char c) {
    if (ide.cursor_y < 0 || ide.cursor_y >= ide.line_count) return;
    if (c == '\t') {
        for (int i = 0; i < IDE_TAB_SIZE; ++i) ide_insert_char(' ');
        return;
    }

    char* line = ide.lines[ide.cursor_y];
    int len = (int)strlen(line);
    if (len + 1 >= IDE_MAX_LINE_LENGTH) return;
    if (ide.cursor_x < 0) ide.cursor_x = 0;
    if (ide.cursor_x > len) ide.cursor_x = len;

    memmove(&line[ide.cursor_x + 1], &line[ide.cursor_x], (size_t)(len - ide.cursor_x + 1));
    line[ide.cursor_x] = c;
    ide.cursor_x++;
    ide.modified = true;
    ide_snapshot_current_tab();
}

static void ide_execute_text(const char* code) {
    char output[1024];
    output[0] = '\0';
    ide.executing = true;
    ide_set_text(ide.status, sizeof(ide.status), "Running...");
    if (micropython_execute_string(code, output, sizeof(output)) == 0) {
        if (output[0]) {
            ide_push_console(output);
        } else {
            ide_push_console("Execution complete.");
        }
        ide_set_text(ide.status, sizeof(ide.status), "Ready");
    } else {
        if (output[0]) {
            ide_push_console(output);
        } else {
            ide_push_console("Execution failed.");
        }
        ide_set_text(ide.status, sizeof(ide.status), "Execution error");
    }
    ide.executing = false;
}

static void ide_run_repl_command(void) {
    if (ide.repl_input[0] == '\0') return;
    ide_push_console(">>>");
    ide_push_console(ide.repl_input);
    ide_execute_text(ide.repl_input);
    ide_clear_buffer(ide.repl_input, sizeof(ide.repl_input));
    ide.repl_cursor_pos = 0;
}

void python_ide_new_file(void) {
    ide_snapshot_current_tab();
    if (ide.tab_count < IDE_MAX_TABS) {
        ide.current_tab = ide.tab_count;
        ide.tab_count++;
    } else {
        /* Bug fix: previously fell through here and reused the current
         * tab slot, silently discarding whatever was open in it. Refuse
         * instead of destroying the user's work. */
        ide_set_text(ide.status, sizeof(ide.status), "Max tabs reached");
        ide_push_console("Cannot create a new file: maximum of 4 tabs is already open.");
        return;
    }
    char name[128];
    snprintf(name, sizeof(name), "untitled_%d.py", ide.current_tab + 1);
    ide_load_defaults();
    ide_set_text(ide.filename, sizeof(ide.filename), name);
    ide_set_text(ide.tabs[ide.current_tab].filename, sizeof(ide.tabs[ide.current_tab].filename), name);
    ide_snapshot_current_tab();
    ide_set_text(ide.status, sizeof(ide.status), "New file");
    ide_push_console("Created new script.");
}

void python_ide_open_file(void) {
    /* Bug fix: this used to always re-read ide.filename from disk and
     * overwrite the live buffer, even when the current tab had unsaved
     * edits (ide.modified). Since this IDE's "Open" reloads the file
     * that's already associated with the tab (there is no file picker),
     * that silently destroyed unsaved work whenever it was pressed twice
     * by habit, or via the Ctrl+O shortcut. Refuse instead, the same way
     * python_ide_new_file() already refuses rather than clobbering data. */
    if (ide.modified) {
        ide_set_text(ide.status, sizeof(ide.status), "Unsaved changes");
        ide_push_console("Open cancelled: save or discard your changes first (unsaved edits would be lost).");
        return;
    }
    const char* data = fs_read_file(ide.filename);
    if (!data) {
        ide_set_text(ide.status, sizeof(ide.status), "Open failed");
        ide_push_console("Open failed: file not found.");
        return;
    }
    ide_set_text(ide.tabs[ide.current_tab].filename, sizeof(ide.tabs[ide.current_tab].filename), ide.filename);

    ide.line_count = 0;
    ide.cursor_x = 0;
    ide.cursor_y = 0;
    ide.scroll_y = 0;

    const char* p = data;
    while (*p && ide.line_count < IDE_MAX_LINE_COUNT) {
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= IDE_MAX_LINE_LENGTH) len = IDE_MAX_LINE_LENGTH - 1;
        memcpy(ide.lines[ide.line_count], p, len);
        ide.lines[ide.line_count][len] = '\0';
        if (len > 0 && ide.lines[ide.line_count][len - 1] == '\r') {
            ide.lines[ide.line_count][len - 1] = '\0';
        }
        ide.line_count++;
        if (!eol) break;
        p = eol + 1;
    }

    if (ide.line_count == 0) {
        ide.line_count = 1;
        ide.lines[0][0] = '\0';
    }

    ide.modified = false;
    ide_set_text(ide.status, sizeof(ide.status), "File opened");
    ide_push_console("Loaded file into editor.");
}

void python_ide_save_file(void) {
    char buffer[IDE_MAX_FILE_SIZE];
    buffer[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < ide.line_count; ++i) {
        size_t line_len = strlen(ide.lines[i]);
        if (used + line_len + 2 >= sizeof(buffer)) break;
        memcpy(buffer + used, ide.lines[i], line_len);
        used += line_len;
        if (i < ide.line_count - 1) buffer[used++] = '\n';
        buffer[used] = '\0';
    }

    if (!fs_write_file(ide.filename, buffer, (uint64_t)used)) {
        if (!fs_create_file(ide.filename) || !fs_write_file(ide.filename, buffer, (uint64_t)used)) {
            ide_set_text(ide.status, sizeof(ide.status), "Save failed");
            ide_push_console("Save failed.");
            return;
        }
    }

    ide.modified = false;
    ide_set_text(ide.status, sizeof(ide.status), "Saved");
    ide_push_console("Saved file.");
}

static void ide_run_editor_buffer(void) {
    char code[IDE_MAX_FILE_SIZE];
    size_t used = 0;
    code[0] = '\0';
    for (int i = 0; i < ide.line_count; ++i) {
        size_t line_len = strlen(ide.lines[i]);
        if (used + line_len + 2 >= sizeof(code)) break;
        memcpy(code + used, ide.lines[i], line_len);
        used += line_len;
        code[used++] = '\n';
        code[used] = '\0';
    }
    ide_execute_text(code);
}

void python_ide_execute_code(void) {
    ide_run_editor_buffer();
}

static void ide_action_new(void) { python_ide_new_file(); }
static void ide_action_open(void) { python_ide_open_file(); }
static void ide_action_save(void) { python_ide_save_file(); }
static void ide_action_run(void) { python_ide_execute_code(); }
static void ide_action_toggle_repl(void) {
    ide.repl_active = !ide.repl_active;
    ide.repl_cursor_pos = (int)strlen(ide.repl_input);
    ide_set_text(ide.status, sizeof(ide.status), ide.repl_active ? "REPL mode" : "Editor mode");
    ide_push_console(ide.repl_active ? "REPL enabled." : "REPL disabled.");
}

static bool ide_is_url_like(const char* s) {
    if (!s || !s[0]) return false;
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

static void ide_normalize_url(const char* raw, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!raw || !raw[0]) {
        strncpy(out, "http://example.com/", out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    if (ide_is_url_like(raw)) {
        strncpy(out, raw, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    snprintf(out, out_sz, "http://%s", raw);
}

static void ide_load_text_from_buffer(const char* data, size_t len) {
    ide.line_count = 0;
    ide.cursor_x = 0;
    ide.cursor_y = 0;
    ide.scroll_y = 0;
    if (!data || len == 0) {
        ide.line_count = 1;
        ide.lines[0][0] = '\0';
        ide.modified = false;
        return;
    }
    const char* p = data;
    const char* end = data + len;
    while (p < end && ide.line_count < IDE_MAX_LINE_COUNT) {
        const char* eol = NULL;
        for (const char* q = p; q < end; ++q) { if (*q == '\n') { eol = q; break; } }
        size_t line_len = eol ? (size_t)(eol - p) : (size_t)(end - p);
        if (line_len >= IDE_MAX_LINE_LENGTH) line_len = IDE_MAX_LINE_LENGTH - 1;
        memcpy(ide.lines[ide.line_count], p, line_len);
        ide.lines[ide.line_count][line_len] = '\0';
        if (line_len > 0 && ide.lines[ide.line_count][line_len - 1] == '\r') {
            ide.lines[ide.line_count][line_len - 1] = '\0';
        }
        ide.line_count++;
        if (!eol) break;
        p = eol + 1;
    }
    if (ide.line_count == 0) {
        ide.line_count = 1;
        ide.lines[0][0] = '\0';
    }
    ide.modified = false;
}

static void ide_set_filename_from_url(const char* url) {
    char name[128];
    strncpy(name, "downloaded.py", sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    if (url && url[0]) {
        const char* slash = strrchr(url, '/');
        const char* start = slash ? slash + 1 : url;
        size_t n = 0;
        while (start[n] && start[n] != '?' && start[n] != '#') n++;
        if (n > 0) {
            if (n >= sizeof(name)) n = sizeof(name) - 1;
            memcpy(name, start, n);
            name[n] = '\0';
        }
    }
    if (name[0] == '\0') {
        strncpy(name, "downloaded.py", sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
    if (!strchr(name, '.')) {
        size_t l = strlen(name);
        if (l + 3 < sizeof(name)) strcat(name, ".py");
    }
    ide_set_text(ide.filename, sizeof(ide.filename), name);
}

static void ide_fetch_remote_source(void) {
    const char* candidate = gui_clipboard_get_text();
    if (!candidate || !candidate[0] || !ide_is_url_like(candidate)) {
        candidate = ide.filename;
    }
    if (!candidate || !candidate[0]) {
        ide_set_text(ide.status, sizeof(ide.status), "Fetch failed");
        ide_push_console("Fetch failed: no URL available.");
        return;
    }

    char url[HTTP_MAX_URL];
    char current[HTTP_MAX_URL];
    ide_normalize_url(candidate, url, sizeof(url));
    strncpy(current, url, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    http_client_t* http = http_create();
    if (!http) {
        ide_set_text(ide.status, sizeof(ide.status), "HTTP init failed");
        ide_push_console("HTTP client initialization failed.");
        return;
    }

    int status = 0;
    int ret = -1;
    for (int redirects = 0; redirects < 4; ++redirects) {
        ret = http_get(http, current);
        status = http_status_code(http);
        if (ret != 0) break;
        if (status == HTTP_FOUND) {
            const char* location = http_get_header(http, "Location");
            if (location && location[0]) {
                ide_normalize_url(location, current, sizeof(current));
                continue;
            }
        }
        break;
    }

    if (ret != 0 || status != HTTP_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Fetch failed: HTTP %d", status);
        ide_set_text(ide.status, sizeof(ide.status), msg);
        ide_push_console(msg);
        http_destroy(http);
        return;
    }

    const char* body = http_response_body(http);
    uint64_t len = http_response_length(http);
    if (!body || len == 0) {
        ide_set_text(ide.status, sizeof(ide.status), "Empty response");
        ide_push_console("Fetch complete, but response was empty.");
        http_destroy(http);
        return;
    }

    ide_load_text_from_buffer(body, (size_t)len);
    ide_set_filename_from_url(current);
    ide_set_text(ide.status, sizeof(ide.status), "Fetched remote source");
    char msg[128];
    snprintf(msg, sizeof(msg), "Fetched %llu bytes from URL.", (unsigned long long)len);
    ide_push_console(msg);
    http_destroy(http);
}
static void ide_update_toolbar_layout(void) {
    const char* labels[] = { "New", "Open", "Save", "Run", "Step", "Cont", "BP", "Dbg", "REPL", "Clear" };
    int count = 10;
    int x = ide.x + IDE_MARGIN;
    int y = ide.y + 6;
    int available = ide.w - IDE_MARGIN * 2;
    int gap = 8;
    int min_w = 50;
    int remaining = available - gap * (count - 1);
    if (remaining < count * min_w) {
        min_w = MAX(34, remaining / count);
    }
    for (int i = 0; i < count; ++i) {
        int label_w = (int)strlen(labels[i]) * FONT_W;
        int w = MAX(min_w, label_w + IDE_BUTTON_PAD * 2);
        toolbar_buttons[i].label = labels[i];
        toolbar_buttons[i].x = x;
        toolbar_buttons[i].y = y;
        toolbar_buttons[i].w = w;
        toolbar_buttons[i].h = IDE_BUTTON_H;
        x += w + gap;
    }
}

static void ide_update_tab_layout(void) {
    int x = ide.x + IDE_MARGIN;
    int y = ide.y + IDE_TOOLBAR_H + 1;
    int count = ide.tab_count;
    if (count < 1) return;
    int available = ide.w - IDE_MARGIN * 2;
    int gap = 6;
    int max_each = (available - gap * (count - 1)) / count;
    if (max_each < 90) max_each = 90;
    if (max_each > 180) max_each = 180;
    for (int i = 0; i < count && i < IDE_MAX_TABS; ++i) {
        const char* label = ide.tabs[i].filename[0] ? ide.tabs[i].filename : "untitled.py";
        ide_truncate_label(tab_button_labels[i], sizeof(tab_button_labels[i]), label, max_each - 16);
        tab_buttons[i].label = tab_button_labels[i];
        tab_buttons[i].x = x;
        tab_buttons[i].y = y;
        tab_buttons[i].w = max_each;
        tab_buttons[i].h = IDE_TABBAR_H - 2;
        x += tab_buttons[i].w + gap;
    }
}

void python_ide_draw_tabs(void) {
    ide_update_tab_layout();
    vga_fill_rect(ide.x, ide.y + IDE_TOOLBAR_H, ide.w, IDE_TABBAR_H, rgb(34, 38, 48));
    for (int i = 0; i < ide.tab_count && i < IDE_MAX_TABS; ++i) {
        bool hot = (ide_mouse_x >= tab_buttons[i].x && ide_mouse_x < tab_buttons[i].x + tab_buttons[i].w &&
                    ide_mouse_y >= tab_buttons[i].y && ide_mouse_y < tab_buttons[i].y + tab_buttons[i].h);
        uint64_t bg = (i == ide.current_tab) ? IDE_TOOLBAR_BTN_HOT : (hot ? rgb(60, 86, 128) : IDE_TOOLBAR_BTN);
        vga_fill_rect(tab_buttons[i].x, tab_buttons[i].y, tab_buttons[i].w, tab_buttons[i].h, bg);
        vga_draw_rect(tab_buttons[i].x, tab_buttons[i].y, tab_buttons[i].w, tab_buttons[i].h, IDE_SELECTION);
        ide_draw_text_clipped(tab_buttons[i].x + 8, tab_buttons[i].y + 1, tab_buttons[i].label,
                              tab_buttons[i].w - 16, COLOR_WHITE, bg);
    }
}

void python_ide_draw_toolbar(void) {
    ide_update_toolbar_layout();
    int toolbar_x = ide.x;
    int toolbar_y = ide.y;
    int toolbar_w = ide.w;
    vga_fill_rect(toolbar_x, toolbar_y, toolbar_w, IDE_TOOLBAR_H, IDE_TOOLBAR_BG);

    for (int i = 0; i < 10; ++i) {
        bool hot = (ide_mouse_x >= toolbar_buttons[i].x && ide_mouse_x < toolbar_buttons[i].x + toolbar_buttons[i].w &&
                    ide_mouse_y >= toolbar_buttons[i].y && ide_mouse_y < toolbar_buttons[i].y + toolbar_buttons[i].h);
        uint64_t color = hot ? IDE_TOOLBAR_BTN_HOT : IDE_TOOLBAR_BTN;
        vga_fill_rect(toolbar_buttons[i].x, toolbar_buttons[i].y, toolbar_buttons[i].w, toolbar_buttons[i].h, color);
        vga_draw_rect(toolbar_buttons[i].x, toolbar_buttons[i].y, toolbar_buttons[i].w, toolbar_buttons[i].h, IDE_SELECTION);
        int text_w = (int)strlen(toolbar_buttons[i].label) * FONT_W;
        vga_draw_string(toolbar_buttons[i].x + (toolbar_buttons[i].w - text_w) / 2,
                        toolbar_buttons[i].y + 1,
                        toolbar_buttons[i].label, COLOR_WHITE, color);
    }

    char title[200];
    snprintf(title, sizeof(title), "%s%s", ide.filename, ide.modified ? " *" : "");
    int title_x = toolbar_buttons[9].x + toolbar_buttons[9].w + 16;
    if (title_x < ide.x + ide.w - 20) {
        ide_draw_text_clipped(title_x, toolbar_y + 2, title, ide.x + ide.w - 20 - title_x, COLOR_WHITE, IDE_TOOLBAR_BG);
    }
}

static void ide_draw_line_token(int x, int y, const char* token, bool in_string, bool in_comment) {
    if (!token || !token[0]) return;
    uint64_t color = IDE_SYNTAX_FUNCTION;
    if (in_comment) color = IDE_SYNTAX_COMMENT;
    else if (in_string) color = IDE_SYNTAX_STRING;
    else if (strcmp(token, "def") == 0 || strcmp(token, "class") == 0 ||
             strcmp(token, "if") == 0 || strcmp(token, "else") == 0 ||
             strcmp(token, "elif") == 0 || strcmp(token, "for") == 0 ||
             strcmp(token, "while") == 0 || strcmp(token, "return") == 0 ||
             strcmp(token, "import") == 0 || strcmp(token, "from") == 0 ||
             strcmp(token, "try") == 0 || strcmp(token, "except") == 0 ||
             strcmp(token, "with") == 0 || strcmp(token, "as") == 0 ||
             strcmp(token, "print") == 0 || strcmp(token, "True") == 0 ||
             strcmp(token, "False") == 0 || strcmp(token, "None") == 0) {
        color = IDE_SYNTAX_KEYWORD;
    } else {
        bool is_num = true;
        for (const char* p = token; *p; ++p) {
            if ((*p < '0' || *p > '9') && *p != '.') { is_num = false; break; }
        }
        if (is_num) color = IDE_SYNTAX_NUMBER;
    }
    vga_draw_string(x, y, token, color, IDE_EDITOR_BG);
}

void python_ide_draw_editor(void) {
    int x = ide.x + IDE_MARGIN;
    int y = ide.y + IDE_TOOLBAR_H + IDE_TABBAR_H + 4;
    int w = ide.w - IDE_MARGIN * 2;
    int h = ide.h - IDE_TOOLBAR_H - IDE_TABBAR_H - IDE_CONSOLE_H - IDE_STATUS_H - 18;
    if (h < FONT_H * 6) h = FONT_H * 6;
    if (w < 120) return;

    vga_fill_rect(x, y, w, h, IDE_EDITOR_BG);
    vga_draw_rect(x, y, w, h, IDE_SELECTION);

    int visible = h / (FONT_H + 6);
    if (visible < 1) visible = 1;

    if (ide.cursor_y < ide.scroll_y) ide.scroll_y = ide.cursor_y;
    if (ide.cursor_y >= ide.scroll_y + visible) ide.scroll_y = ide.cursor_y - visible + 1;
    if (ide.scroll_y < 0) ide.scroll_y = 0;

    int start = ide.scroll_y;
    int end = MIN(ide.line_count, start + visible);
    int line_h = FONT_H + 6;

    for (int line = start; line < end; ++line) {
        int ly = y + 4 + (line - start) * line_h;
        char num[8];
        snprintf(num, sizeof(num), "%d", line + 1);
        vga_fill_rect(x + 1, ly - 1, IDE_GUTTER_W - 2, line_h, rgb(34, 38, 46));
        vga_draw_string(x + 6, ly, num, IDE_LINE_NUMBER, rgb(34, 38, 46));
        if (line == ide.cursor_y) {
            vga_fill_rect(x + IDE_GUTTER_W, ly - 1, w - IDE_GUTTER_W, line_h, rgb(46, 52, 62));
        }

        const char* text = ide.lines[line];
        int tx = x + IDE_GUTTER_W;
        bool in_string = false;
        bool in_comment = false;
        char token[64];
        int token_len = 0;
        for (int i = 0; text[i] && tx < x + w - 4; ++i) {
            char c = text[i];
            if (in_comment) {
                char s[2] = { c, '\0' };
                vga_draw_string(tx, ly, s, IDE_SYNTAX_COMMENT, IDE_EDITOR_BG);
                tx += FONT_W;
                continue;
            }
            if (c == '#') {
                if (token_len > 0) {
                    token[token_len] = '\0';
                    ide_draw_line_token(tx - token_len * FONT_W, ly, token, in_string, false);
                    token_len = 0;
                }
                in_comment = true;
                char s[2] = { c, '\0' };
                vga_draw_string(tx, ly, s, IDE_SYNTAX_COMMENT, IDE_EDITOR_BG);
                tx += FONT_W;
                continue;
            }
            if (c == '\'' || c == '"') {
                if (token_len > 0) {
                    token[token_len] = '\0';
                    ide_draw_line_token(tx - token_len * FONT_W, ly, token, in_string, false);
                    token_len = 0;
                }
                in_string = !in_string;
                char s[2] = { c, '\0' };
                vga_draw_string(tx, ly, s, IDE_SYNTAX_STRING, IDE_EDITOR_BG);
                tx += FONT_W;
                continue;
            }
            if (!in_string && (c == ' ' || c == '\t' || c == '(' || c == ')' || c == ',' || c == ':' || c == '+' || c == '-' || c == '=' || c == '[' || c == ']' || c == '{' || c == '}')) {
                if (token_len > 0) {
                    token[token_len] = '\0';
                    ide_draw_line_token(tx - token_len * FONT_W, ly, token, false, false);
                    token_len = 0;
                }
                char s[2] = { c, '\0' };
                vga_draw_string(tx, ly, s, COLOR_WHITE, IDE_EDITOR_BG);
                tx += FONT_W;
                continue;
            }
            if (token_len < (int)sizeof(token) - 1) token[token_len++] = c;
        }
        if (token_len > 0) {
            token[token_len] = '\0';
            ide_draw_line_token(tx - token_len * FONT_W, ly, token, in_string, in_comment);
        }

        if (line == ide.cursor_y) {
            int cursor_x = x + IDE_GUTTER_W + ide.cursor_x * FONT_W;
            vga_draw_line(cursor_x, ly - 1, cursor_x, ly + FONT_H, IDE_CURSOR);
        }
    }

    if (ide.repl_active) {
        int repl_y = y + h - FONT_H - 2;
        vga_fill_rect(x + 1, repl_y - 2, w - 2, FONT_H + 4, IDE_CONSOLE_BG);
        vga_draw_string(x + 6, repl_y, "REPL >", IDE_SELECTION, IDE_CONSOLE_BG);
        vga_draw_string(x + 6 + 6 * FONT_W, repl_y, ide.repl_input, COLOR_WHITE, IDE_CONSOLE_BG);
        vga_draw_line(x + 6 + 6 * FONT_W + ide.repl_cursor_pos * FONT_W, repl_y - 1,
                      x + 6 + 6 * FONT_W + ide.repl_cursor_pos * FONT_W, repl_y + FONT_H, IDE_CURSOR);
    }
}

void python_ide_draw_console(void) {
    int x = ide.x + IDE_MARGIN;
    int y = ide.y + ide.h - IDE_CONSOLE_H - IDE_STATUS_H - 8;
    int w = ide.w - IDE_MARGIN * 2;
    int h = IDE_CONSOLE_H;
    if (w < 120 || h < 50) return;

    vga_fill_rect(x, y, w, h, IDE_CONSOLE_BG);
    vga_draw_rect(x, y, w, h, IDE_SELECTION);
    vga_draw_string(x + 8, y + 4, "Console", IDE_SELECTION, IDE_CONSOLE_BG);

    int lines_visible = (h - FONT_H - 10) / (FONT_H + 4);
    if (lines_visible < 1) lines_visible = 1;
    int start = ide.console_scroll;
    if (start < 0) start = 0;
    if (start > ide.console_line_count - 1) start = MAX(0, ide.console_line_count - 1);
    int end = MIN(ide.console_line_count, start + lines_visible);

    for (int i = start; i < end; ++i) {
        int ly = y + FONT_H + 10 + (i - start) * (FONT_H + 4);
        vga_draw_string(x + 8, ly, ide.console_lines[i], COLOR_WHITE, IDE_CONSOLE_BG);
    }
}

void python_ide_draw_status_bar(void) {
    int x = ide.x;
    int y = ide.y + ide.h - IDE_STATUS_H;
    vga_fill_rect(x, y, ide.w, IDE_STATUS_H, IDE_STATUS_BG);
    vga_draw_rect(x, y, ide.w, IDE_STATUS_H, IDE_SELECTION);

    char left[256];
    snprintf(left, sizeof(left), "%s%s  |  Tab %d/%d  |  Ln %d, Col %d  |  %s",
             ide.filename,
             ide.modified ? " *" : "",
             ide.current_tab + 1,
             ide.tab_count,
             ide.cursor_y + 1,
             ide.cursor_x + 1,
             ide.status[0] ? ide.status : "Ready");
    vga_draw_string(x + 8, y + 2, left, COLOR_WHITE, IDE_STATUS_BG);

    const char* mode = ide.repl_active ? "REPL" : "Editor";
    vga_draw_string(x + ide.w - 140, y + 2, mode, IDE_SELECTION, IDE_STATUS_BG);
}

void python_ide_draw(void) {
    if (!ide.visible) return;
    ide_autosave_tick();
    ide_update_completion();
    vga_fill_rect(ide.x, ide.y, ide.w, ide.h, IDE_BG);
    python_ide_draw_toolbar();
    python_ide_draw_tabs();
    python_ide_draw_editor();
    python_ide_draw_console();
    python_ide_draw_status_bar();
}

static void ide_click_toolbar(int mx, int my) {
    for (int i = 0; i < 10; ++i) {
        if (mx >= toolbar_buttons[i].x && mx < toolbar_buttons[i].x + toolbar_buttons[i].w &&
            my >= toolbar_buttons[i].y && my < toolbar_buttons[i].y + toolbar_buttons[i].h) {
            switch (i) {
                case 0: ide_action_new(); break;
                case 1: ide_action_open(); break;
                case 2: ide_action_save(); break;
                case 3: ide_action_run(); break;
                case 4: ide_debug_step_internal(); break;
                case 5: ide_debug_continue_internal(); break;
                case 6: ide_toggle_breakpoint_internal(); break;
                case 7: ide.debug_paused = !ide.debug_paused; if (ide.debug_paused) ide_debug_load(); break;
                case 8: ide_action_toggle_repl(); break;
                case 9:
                    ide_clear_buffer(ide.console_lines[0], sizeof(ide.console_lines[0]));
                    ide.console_line_count = 0;
                    ide.console_scroll = 0;
                    ide_push_console("Console cleared.");
                    break;
            }
            return;
        }
    }
}

void python_ide_handle_click(int mx, int my) {
    if (!ide.visible) return;
    ide_update_toolbar_layout();
    if (my >= ide.y && my < ide.y + IDE_TOOLBAR_H) {
        ide_click_toolbar(mx, my);
        return;
    }
    if (my >= ide.y + IDE_TOOLBAR_H && my < ide.y + IDE_TOOLBAR_H + IDE_TABBAR_H) {
        ide_update_tab_layout();
        for (int i = 0; i < ide.tab_count && i < IDE_MAX_TABS; ++i) {
            if (mx >= tab_buttons[i].x && mx < tab_buttons[i].x + tab_buttons[i].w) {
                ide_switch_tab(i);
                return;
            }
        }
        return;
    }

    int editor_x = ide.x + IDE_MARGIN;
    int editor_y = ide.y + IDE_TOOLBAR_H + 4;
    int editor_w = ide.w - IDE_MARGIN * 2;
    int editor_h = ide.h - IDE_TOOLBAR_H - IDE_CONSOLE_H - IDE_STATUS_H - 18;
    if (editor_h < FONT_H * 6) editor_h = FONT_H * 6;
    if (mx >= editor_x && mx < editor_x + editor_w && my >= editor_y && my < editor_y + editor_h) {
        int line_h = FONT_H + 6;
        int line = ide.scroll_y + (my - editor_y - 4) / line_h;
        if (line < 0) line = 0;
        if (line >= ide.line_count) line = ide.line_count - 1;
        if (line >= 0) {
            ide.cursor_y = line;
            int rel = mx - (editor_x + IDE_GUTTER_W);
            if (rel < 0) rel = 0;
            ide.cursor_x = rel / FONT_W;
            int len = ide_current_line_len();
            if (ide.cursor_x > len) ide.cursor_x = len;
            ide_ensure_visible();
        }
    }
}

void python_ide_handle_mouse(mouse_state_t mouse) {
    ide_mouse_x = (int)mouse.x;
    ide_mouse_y = (int)mouse.y;
    if (!mouse.left_click) return;
    python_ide_handle_click((int)mouse.x, (int)mouse.y);
}

void python_ide_scroll(int delta) {
    if (!ide.visible) return;
    ide.scroll_y += delta;
    if (ide.scroll_y < 0) ide.scroll_y = 0;
    int max_scroll = ide.line_count - 1;
    if (max_scroll < 0) max_scroll = 0;
    if (ide.scroll_y > max_scroll) ide.scroll_y = max_scroll;
}

void python_ide_handle_keyboard(keyboard_event_t ev) {
    if (!ide.visible || !ev.pressed) return;

    if (ide.repl_active) {
        if (ev.ctrl && ev.scancode == KEYBOARD_SCANCODE_R) { ide_action_toggle_repl(); return; }
        if (ev.scancode == KEY_ESC) { ide_action_toggle_repl(); return; }
        if (ev.scancode == KEY_BACKSPACE) {
            if (ide.repl_cursor_pos > 0) {
                memmove(&ide.repl_input[ide.repl_cursor_pos - 1], &ide.repl_input[ide.repl_cursor_pos], strlen(&ide.repl_input[ide.repl_cursor_pos]) + 1);
                ide.repl_cursor_pos--;
            }
            return;
        }
        if (ev.scancode == KEY_ENTER) {
            ide_run_repl_command();
            return;
        }
        if (ev.ascii >= 32 && ev.ascii <= 126) {
            int len = (int)strlen(ide.repl_input);
            if (len + 1 < (int)sizeof(ide.repl_input)) {
                memmove(&ide.repl_input[ide.repl_cursor_pos + 1], &ide.repl_input[ide.repl_cursor_pos], (size_t)(len - ide.repl_cursor_pos + 1));
                ide.repl_input[ide.repl_cursor_pos] = ev.ascii;
                ide.repl_cursor_pos++;
            }
        }
        return;
    }

    if (ev.ctrl) {
        switch (ev.scancode) {
            case KEYBOARD_SCANCODE_N: ide_action_new(); return;
            case KEYBOARD_SCANCODE_O: ide_action_open(); return;
            case KEYBOARD_SCANCODE_S: ide_action_save(); return;
            case KEYBOARD_SCANCODE_R: ide_action_run(); return;
            case KEYBOARD_SCANCODE_T: ide_action_toggle_repl(); return;
            case KEYBOARD_SCANCODE_U: ide_fetch_remote_source(); return;
            case KEYBOARD_SCANCODE_L:
                ide.console_line_count = 0;
                ide.console_scroll = 0;
                ide_push_console("Console cleared.");
                return;
            default: break;
        }
    }

    switch (ev.scancode) {
        case KEY_ESC:
            python_ide_close();
            return;
        case KEY_LEFT:
            if (ide.cursor_x > 0) ide.cursor_x--;
            else if (ide.cursor_y > 0) { ide.cursor_y--; ide.cursor_x = (int)strlen(ide.lines[ide.cursor_y]); }
            ide_ensure_visible();
            return;
        case KEY_RIGHT: {
            int len = ide_current_line_len();
            if (ide.cursor_x < len) ide.cursor_x++;
            else if (ide.cursor_y < ide.line_count - 1) { ide.cursor_y++; ide.cursor_x = 0; }
            ide_ensure_visible();
            return;
        }
        case KEY_UP:
            if (ide.cursor_y > 0) ide.cursor_y--;
            if (ide.cursor_x > ide_current_line_len()) ide.cursor_x = ide_current_line_len();
            ide_ensure_visible();
            return;
        case KEY_DOWN:
            if (ide.cursor_y < ide.line_count - 1) ide.cursor_y++;
            if (ide.cursor_x > ide_current_line_len()) ide.cursor_x = ide_current_line_len();
            ide_ensure_visible();
            return;
        case KEY_HOME:
            ide.cursor_x = 0;
            return;
        case KEY_END:
            ide.cursor_x = ide_current_line_len();
            return;
        case KEY_BACKSPACE:
            ide_join_left_char();
            return;
        case KEY_DELETE:
            ide_delete_at_cursor();
            return;
        case KEY_ENTER:
            ide_split_current_line();
            return;
        case KEY_TAB:
            if (ide.completion[0]) ide_apply_completion(); else ide_insert_text("    ");
            return;
        case KEY_F5:
            ide_action_run();
            return;
        case KEY_F6:
            ide_debug_step_internal();
            return;
        case KEY_F7:
            ide_debug_continue_internal();
            return;
        case KEY_F9:
            ide_toggle_breakpoint_internal();
            return;
        default:
            break;
    }

    if (ev.ctrl && ev.scancode == KEYBOARD_SCANCODE_SPACE) {
        ide_apply_completion();
        return;
    }
    if (ev.ascii >= 32 && ev.ascii <= 126) {
        ide_insert_char(ev.ascii);
        ide_rebuild_completion_hint();
        ide_ensure_visible();
        ide_snapshot_current_tab();
    }
}

void python_ide_open(void) {
    python_ide_init();
    ide.visible = true;
    ide_set_text(ide.status, sizeof(ide.status), "Ready");
}

void python_ide_close(void) {
    ide.visible = false;
    ide_set_text(ide.status, sizeof(ide.status), "Closed");
}


/* ---- Public API wrappers declared in python_ide_gui.h but missing above ---- */

void python_ide_debug_step(void) {
    ide_debug_step_internal();
}

void python_ide_debug_continue(void) {
    ide_debug_continue_internal();
}

void python_ide_toggle_breakpoint(void) {
    ide_toggle_breakpoint_internal();
}
