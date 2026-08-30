/**
 * gui_input.c - GUIコア (キーボード/ターミナル/テキストエディタ/IME 入力処理)
 * gui.c (5,588行) から分割生成。詳細は gui_internal.h を参照。
 */

#include "gui.h"
#include "gui_internal.h"
#include "voxel_games_advanced.h"
#include "vga.h"
#include "mouse.h"
#include "../drivers/input/mouse_minimal.h"
#include "keyboard.h"
#include "fs.h"
#include "../bios/bios.h"
#include "io.h"
#include "serial.h"
#include "timer.h"
#include "../apps/development/python_ide_gui.h"
#include "gui_render_engine.h"
#include "gui_utils.h"
#include "../apps/system/password_screen.h"
#include "../components/boot_animation.h"
#include "notification_center.h"
#include "theme_system.h"
#include <shell.h>
#include <string.h>
#include <stdio.h>

/* gui.c は他モジュール (gui_apps_common.c) にある公開版 gui_get_prompt_hostname()
 * とは別に、同名だが独立した static 版を意図的に保持していた。分割後に
 * 2ファイル (gui_settings.c と gui_input.c) から必要になるため、非static化
 * して衝突させるのではなく、この小さなヘルパーをここに複製しておく。 */
static const char* gui_input_local_prompt_hostname(void) {
    const char* hostname = config_get_string("system.hostname");
    return (hostname && hostname[0]) ? hostname : "cos";
}

static int gui_find_keyboard_target(void) {
    if (active_window >= 0 && active_window < window_count) {
        window_t* w = &windows[active_window];
        if (w->visible && !w->minimized) {
            return active_window;
        }
    }

    for (int i = window_count - 1; i >= 0; --i) {
        if (windows[i].visible && !windows[i].minimized) {
            return i;
        }
    }

    return -1;
}

// ============================================================
// Input & Update
// ============================================================
// ============================================================
// Keyboard input routing
// ============================================================

static bool keyboard_pending_valid = false;
static keyboard_event_t keyboard_pending_event;

/* Ctrl+P arms the Ctrl+P+O multi-cursor shortcut for a short interval. */
static bool multi_cursor_hotkey_armed = false;
static uint64_t multi_cursor_hotkey_tick = 0;

bool gui_pop_keyboard_event(keyboard_event_t* ev) {
    if (!ev) return false;
    if (keyboard_pending_valid) {
        *ev = keyboard_pending_event;
        keyboard_pending_valid = false;
        return true;
    }
    if (!keyboard_has_event()) {
        return false;
    }
    *ev = keyboard_get_event();
    return true;
}

static void gui_push_keyboard_event(const keyboard_event_t* ev) {
    if (!ev) return;
    keyboard_pending_event = *ev;
    keyboard_pending_valid = true;
}
static window_t* gui_terminal_output_window = NULL;
static bool gui_terminal_output_new_line = true;

static int gui_terminal_visible_lines(window_t* w) {
    if (!w) return 1;
    int content_h = w->h - TITLEBAR_H - 64;
    int lines = content_h / (FONT_H + 3);
    if (lines < 1) lines = 1;
    return lines;
}

int gui_terminal_max_scroll(window_t* w) {
    if (!w) return 0;
    int visible_lines = gui_terminal_visible_lines(w);
    int max_scroll = w->term_line_count - visible_lines;
    return max_scroll > 0 ? max_scroll : 0;
}

static void gui_terminal_scroll_if_needed(window_t* w) {
    if (!w) return;
    if (w->term_line_count < TERM_LINES) return;
    for (int i = 0; i < TERM_LINES - 1; ++i) {
        strncpy(w->term_lines[i], w->term_lines[i + 1], TERM_LINE_LEN - 1);
        w->term_lines[i][TERM_LINE_LEN - 1] = '\0';
    }
    w->term_lines[TERM_LINES - 1][0] = '\0';
    w->term_line_count = TERM_LINES - 1;
}

static void gui_terminal_begin_line(window_t* w) {
    if (!w) return;
    if (w->term_line_count == 0) {
        w->term_lines[0][0] = '\0';
        w->term_line_count = 1;
        return;
    }
    if (w->term_lines[w->term_line_count - 1][0] != '\0') {
        gui_terminal_scroll_if_needed(w);
        if (w->term_line_count < TERM_LINES) {
            w->term_lines[w->term_line_count][0] = '\0';
            w->term_line_count++;
        }
    }
}

static void gui_terminal_append_text(window_t* w, const char* text) {
    if (!w || !text) return;
    for (const char* p = text; *p; ++p) {
        char c = *p;
        if (c == '\r') continue;
        if (c == '\n') {
            gui_terminal_output_new_line = true;
            continue;
        }
        if (gui_terminal_output_new_line) {
            gui_terminal_begin_line(w);
            gui_terminal_output_new_line = false;
        }
        if (w->term_line_count == 0) {
            w->term_lines[0][0] = '\0';
            w->term_line_count = 1;
        }
        int idx = w->term_line_count - 1;
        size_t len = strlen(w->term_lines[idx]);
        if (len >= TERM_LINE_LEN - 1) {
            gui_terminal_begin_line(w);
            idx = w->term_line_count - 1;
            len = 0;
        }
        w->term_lines[idx][len] = c;
        w->term_lines[idx][len + 1] = '\0';
    }
}


static bool gui_terminal_command_allowed(const char* cmd) {
    if (!cmd) return false;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '\0') return true;

    char first[32];
    size_t i = 0;
    while (cmd[i] && cmd[i] != ' ' && cmd[i] != '\t' && i < sizeof(first) - 1) {
        first[i] = cmd[i];
        i++;
    }
    first[i] = '\0';

    static const char* const allowed[] = {
        "help", "clear", "cls", "ls", "cd", "pwd", "cat", "date", "time", "uname", "df", "ps", "echo",
        "history", "settings", "theme", "wallpaper", "autoscroll", "sysinfo", "version", "dir", "quit"
    };
    for (size_t j = 0; j < sizeof(allowed) / sizeof(allowed[0]); j++) {
        if (strcmp(first, allowed[j]) == 0) return true;
    }
    return false;
}

static void gui_terminal_output_callback(const char* text) {
    if (!gui_terminal_output_window || !text) return;
    gui_terminal_append_text(gui_terminal_output_window, text);
    if (gui_terminal_autoscroll) {
        gui_terminal_output_window->term_scroll = 0;
    }
}

static void gui_terminal_execute(window_t* w, const char* cmd) {
    if (!w || !cmd) return;

    if (gui_terminal_autoscroll) {
        w->term_scroll = 0;
    }

    if (!gui_terminal_command_allowed(cmd)) {
        if (w->term_line_count >= TERM_LINES) gui_terminal_scroll_if_needed(w);
        if (w->term_line_count < TERM_LINES) {
            char blocked[TERM_LINE_LEN];
            strncpy(blocked, "[Safe mode] Command blocked in GUI terminal: ", TERM_LINE_LEN - 1);
            blocked[TERM_LINE_LEN - 1] = '\0';
            strncat(blocked, cmd, TERM_LINE_LEN - strlen(blocked) - 1);
            strncpy(w->term_lines[w->term_line_count], blocked, TERM_LINE_LEN - 1);
            w->term_lines[w->term_line_count][TERM_LINE_LEN - 1] = '\0';
            w->term_line_count++;
        }
        gui_terminal_output_window = NULL;
        gui_terminal_output_new_line = true;
        return;
    }
    char prompt_line[TERM_LINE_LEN];
    strncpy(prompt_line, "user@", TERM_LINE_LEN - 1);
    prompt_line[TERM_LINE_LEN - 1] = '\0';
    strncat(prompt_line, gui_input_local_prompt_hostname(), TERM_LINE_LEN - strlen(prompt_line) - 1);
    strncat(prompt_line, ":", TERM_LINE_LEN - strlen(prompt_line) - 1);
    strncat(prompt_line, w->term_cwd, TERM_LINE_LEN - strlen(prompt_line) - 1);
    strncat(prompt_line, "$ ", TERM_LINE_LEN - strlen(prompt_line) - 1);
    strncat(prompt_line, cmd, TERM_LINE_LEN - strlen(prompt_line) - 1);
    if (w->term_line_count >= TERM_LINES) gui_terminal_scroll_if_needed(w);
    if (w->term_line_count < TERM_LINES) {
        strncpy(w->term_lines[w->term_line_count], prompt_line, TERM_LINE_LEN - 1);
        w->term_lines[w->term_line_count][TERM_LINE_LEN - 1] = '\0';
        w->term_line_count++;
    }
    gui_terminal_output_window = w;
    gui_terminal_output_new_line = true;
    shell_set_output_callback(gui_terminal_output_callback);
    char cmd_copy[TERM_LINE_LEN];
    strncpy(cmd_copy, cmd, TERM_LINE_LEN - 1);
    cmd_copy[TERM_LINE_LEN - 1] = '\0';
    shell_execute(cmd_copy, false);
    if (gui_terminal_autoscroll) {
        w->term_scroll = 0;
    }
    const char* cwd = shell_get_cwd();
    if (cwd) {
        strncpy(w->term_cwd, cwd, sizeof(w->term_cwd) - 1);
        w->term_cwd[sizeof(w->term_cwd) - 1] = '\0';
    }
    shell_set_output_callback(NULL);
    gui_terminal_output_window = NULL;
    gui_terminal_output_new_line = true;
}



static bool text_editor_has_selection(window_t* w) {
    return w && w->text_sel_start >= 0 && w->text_sel_end >= 0 && w->text_sel_start != w->text_sel_end;
}

static void text_editor_clear_selection(window_t* w) {
    if (!w) return;
    w->text_sel_start = w->text_cursor;
    w->text_sel_end = w->text_cursor;
}

static void text_editor_select_all(window_t* w) {
    if (!w) return;
    int len = (int)strlen(w->text_buf);
    w->text_sel_start = 0;
    w->text_sel_end = len;
    w->text_cursor = len;
}

/* Called before any edit actually mutates text_buf. If more than
 * 500ms have passed since the last edit, this is treated as the
 * start of a new "run" (a burst of typing, a paste, a delete) and the
 * pre-edit state is snapshotted into undo_buf - so Ctrl+Z undoes the
 * whole run, not one keystroke at a time. Continuing the same run
 * (fast typing) does not re-snapshot, so undo lands before the run
 * started rather than one character back. */
static void text_editor_maybe_snapshot(window_t* w) {
    if (!w) return;
    uint64_t now = get_timer_ticks();
    if (now - w->last_edit_tick > 500) {
        memcpy(w->undo_buf, w->text_buf, TEXT_BUF_SIZE);
        w->undo_cursor = w->text_cursor;
        w->undo_valid = true;
    }
    w->last_edit_tick = now;
}

/* Ctrl+Z/Ctrl+Y: swaps the current and saved states, so pressing it
 * again reverses itself (undo, then redo, then undo, ...). */
static void text_editor_toggle_undo(window_t* w) {
    if (!w || !w->undo_valid) return;
    static char swap_tmp[TEXT_BUF_SIZE];
    memcpy(swap_tmp, w->text_buf, TEXT_BUF_SIZE);
    memcpy(w->text_buf, w->undo_buf, TEXT_BUF_SIZE);
    memcpy(w->undo_buf, swap_tmp, TEXT_BUF_SIZE);
    int tmp_cursor = w->text_cursor;
    w->text_cursor = w->undo_cursor;
    w->undo_cursor = tmp_cursor;
    w->text_modified = TRUE;
    text_editor_clear_selection(w);
    /* Starts a fresh run on the next edit rather than folding it into
     * whatever run was active before the swap. */
    w->last_edit_tick = get_timer_ticks();
}

static void text_editor_delete_range(window_t* w, int start, int end) {
    if (!w) return;
    int len = (int)strlen(w->text_buf);
    if (start < 0) start = 0;
    if (end < start) end = start;
    if (end > len) end = len;
    if (start >= end) return;
    text_editor_maybe_snapshot(w);
    for (int i = start; i <= len - (end - start); ++i) {
        w->text_buf[i] = w->text_buf[i + (end - start)];
    }
    w->text_cursor = start;
    w->text_modified = TRUE;
    text_editor_clear_selection(w);
}

static void text_editor_insert_text(window_t* w, const char* text) {
    if (!w || !text || !text[0]) return;
    if (text_editor_has_selection(w)) {
        int sel_start = w->text_sel_start < w->text_sel_end ? w->text_sel_start : w->text_sel_end;
        int sel_end = w->text_sel_start < w->text_sel_end ? w->text_sel_end : w->text_sel_start;
        text_editor_delete_range(w, sel_start, sel_end);
    } else {
        text_editor_maybe_snapshot(w);
    }
    int len = (int)strlen(w->text_buf);
    int add = (int)strlen(text);
    if (add <= 0 || len + add >= TEXT_BUF_SIZE) return;
    for (int i = len; i >= w->text_cursor; --i) {
        w->text_buf[i + add] = w->text_buf[i];
    }
    for (int i = 0; i < add; ++i) {
        w->text_buf[w->text_cursor + i] = text[i];
    }
    w->text_cursor += add;
    w->text_modified = TRUE;
    text_editor_clear_selection(w);
}

static bool text_editor_save_current(window_t* w) {
    if (!w || w->filename[0] == '\0') return false;
    int len = (int)strlen(w->text_buf);
    char parent[256];
    char leaf[256];
    gui_split_path(w->filename, parent, sizeof(parent), leaf, sizeof(leaf));
    if (!leaf[0]) return false;
    if (fs_write_file_at(parent, leaf, w->text_buf, (uint64_t)len)) {
        w->text_modified = FALSE;
        gui_refresh_file_managers_for_path(w->filename);
        return true;
    }
    return false;
}

static bool text_editor_reload_current(window_t* w) {
    if (!w || w->filename[0] == '\0') return false;
    const char* text = fs_read_file_at((char*)0, w->filename);
    if (!text || !fs_looks_like_text(text, (uint64_t)strlen(text))) {
        return false;
    }
    strncpy(w->text_buf, text, TEXT_BUF_SIZE - 1);
    w->text_buf[TEXT_BUF_SIZE - 1] = '\0';
    w->text_cursor = (int)strlen(w->text_buf);
    w->scroll_y = 0;
    w->scroll_x = 0;
    w->text_sel_start = 0;
    w->text_sel_end = 0;
    w->text_modified = FALSE;
    return true;
}

static void text_editor_new_document(window_t* w) {
    if (!w) return;
    w->text_buf[0] = '\0';
    w->text_cursor = 0;
    w->text_sel_start = 0;
    w->text_sel_end = 0;
    w->scroll_y = 0;
    w->scroll_x = 0;
    w->text_modified = FALSE;
    w->filename[0] = '\0';
}

static void text_editor_clear_buffer(window_t* w) {
    if (!w) return;
    w->text_buf[0] = '\0';
    w->text_cursor = 0;
    w->text_sel_start = 0;
    w->text_sel_end = 0;
    w->scroll_y = 0;
    w->scroll_x = 0;
    w->text_modified = TRUE;
}




/* Japanese IME helpers: romaji -> hiragana */

typedef struct {
    const char* romaji;
    const char* kana;
} ime_map_t;

static char g_ime_pending[32] = {0};
static int  g_ime_pending_len = 0;

static const ime_map_t g_ime_map[] = {
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"},
    {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"},
    {"cha", "ちゃ"}, {"chu", "ちゅ"}, {"cho", "ちょ"},
    {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
    {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"},
    {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"},
    {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
    {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"},
    {"ja", "じゃ"}, {"ju", "じゅ"}, {"jo", "じょ"},
    {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
    {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"},

    {"a", "あ"}, {"i", "い"}, {"u", "う"}, {"e", "え"}, {"o", "お"},
    {"ka", "か"}, {"ki", "き"}, {"ku", "く"}, {"ke", "け"}, {"ko", "こ"},
    {"sa", "さ"}, {"shi", "し"}, {"su", "す"}, {"se", "せ"}, {"so", "そ"},
    {"ta", "た"}, {"chi", "ち"}, {"tsu", "つ"}, {"te", "て"}, {"to", "と"},
    {"na", "な"}, {"ni", "に"}, {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"},
    {"ha", "は"}, {"hi", "ひ"}, {"fu", "ふ"}, {"he", "へ"}, {"ho", "ほ"},
    {"ma", "ま"}, {"mi", "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"},
    {"ya", "や"}, {"yu", "ゆ"}, {"yo", "よ"},
    {"ra", "ら"}, {"ri", "り"}, {"ru", "る"}, {"re", "れ"}, {"ro", "ろ"},
    {"wa", "わ"}, {"wo", "を"},
    {"ga", "が"}, {"gi", "ぎ"}, {"gu", "ぐ"}, {"ge", "げ"}, {"go", "ご"},
    {"za", "ざ"}, {"ji", "じ"}, {"zu", "ず"}, {"ze", "ぜ"}, {"zo", "ぞ"},
    {"da", "だ"}, {"de", "で"}, {"do", "ど"},
    {"ba", "ば"}, {"bi", "び"}, {"bu", "ぶ"}, {"be", "べ"}, {"bo", "ぼ"},
    {"pa", "ぱ"}, {"pi", "ぴ"}, {"pu", "ぷ"}, {"pe", "ぺ"}, {"po", "ぽ"},
    {"fa", "ふぁ"}, {"fi", "ふぃ"}, {"fe", "ふぇ"}, {"fo", "ふぉ"},
    {"va", "ゔぁ"}, {"vi", "ゔぃ"}, {"vu", "ゔ"}, {"ve", "ゔぇ"}, {"vo", "ゔぉ"},

    {"tya", "ちゃ"}, {"tyu", "ちゅ"}, {"tyo", "ちょ"},
    {"xya", "ゃ"}, {"xyu", "ゅ"}, {"xyo", "ょ"},
    {"xa", "ぁ"}, {"xi", "ぃ"}, {"xu", "ぅ"}, {"xe", "ぇ"}, {"xo", "ぉ"},
    {"xtsu", "っ"}, {"ltsu", "っ"},
};

static bool gui_ime_is_vowel(char c) {
    return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o';
}

static bool gui_ime_is_consonant(char c) {
    return (c >= 'a' && c <= 'z') && !gui_ime_is_vowel(c) && c != 'n';
}

static bool gui_ime_has_prefix(const char* s, int len) {
    if (!s || len <= 0) return false;
    for (size_t i = 0; i < sizeof(g_ime_map) / sizeof(g_ime_map[0]); ++i) {
        int ml = (int)strlen(g_ime_map[i].romaji);
        if (ml >= len && strncmp(g_ime_map[i].romaji, s, (size_t)len) == 0) return true;
    }
    return false;
}

static int gui_ime_match_exact(const char* s, int len, int* consumed) {
    if (!s || len <= 0) return -1;
    int best = -1;
    int best_len = 0;
    for (size_t i = 0; i < sizeof(g_ime_map) / sizeof(g_ime_map[0]); ++i) {
        int ml = (int)strlen(g_ime_map[i].romaji);
        if (ml == len && strncmp(g_ime_map[i].romaji, s, (size_t)len) == 0 && ml > best_len) {
            best = (int)i;
            best_len = ml;
        }
    }
    if (consumed) *consumed = best_len;
    return best;
}

static bool gui_ime_append(char* out_text, size_t out_text_size, const char* add) {
    if (!out_text || !add || !add[0] || out_text_size == 0) return false;
    size_t have = strlen(out_text);
    size_t need = strlen(add);
    if (have + need >= out_text_size) return false;
    memcpy(out_text + have, add, need + 1);
    return true;
}

void gui_ime_reset(void) {
    g_ime_pending[0] = '\0';
    g_ime_pending_len = 0;
}

bool gui_ime_is_active(void) {
    return gui_get_language_idx() == 1;
}

static bool gui_ime_flush_internal(char* out_text, size_t out_text_size, bool raw_fallback) {
    bool produced = false;
    if (!out_text || out_text_size == 0) return false;
    while (g_ime_pending_len > 0) {
        int consumed = 0;
        int idx = gui_ime_match_exact(g_ime_pending, g_ime_pending_len, &consumed);
        if (idx >= 0 && consumed > 0) {
            if (!gui_ime_append(out_text, out_text_size, g_ime_map[idx].kana)) return produced;
            memmove(g_ime_pending, g_ime_pending + consumed, (size_t)g_ime_pending_len - (size_t)consumed);
            g_ime_pending_len -= consumed;
            g_ime_pending[g_ime_pending_len] = '\0';
            produced = true;
            continue;
        }

        /* Double consonants create a small tsu before the remaining
         * consonant starts the next syllable: gakkou -> がっこう. */
        if (g_ime_pending_len >= 2 &&
            g_ime_pending[0] == g_ime_pending[1] &&
            gui_ime_is_consonant(g_ime_pending[0])) {
            if (!gui_ime_append(out_text, out_text_size, "っ")) return produced;
            memmove(g_ime_pending, g_ime_pending + 1, (size_t)g_ime_pending_len);
            g_ime_pending_len--;
            g_ime_pending[g_ime_pending_len] = '\0';
            produced = true;
            continue;
        }

        if (g_ime_pending_len >= 2 && g_ime_pending[0] == 'n') {
            char nxt = g_ime_pending[1];
            if (nxt == '\'' || (!gui_ime_is_vowel(nxt) && nxt != 'y')) {
                if (!gui_ime_append(out_text, out_text_size, "ん")) return produced;
                int skip = (nxt == '\'') ? 2 : 1;
                memmove(g_ime_pending, g_ime_pending + skip,
                        (size_t)g_ime_pending_len - (size_t)skip + 1);
                g_ime_pending_len -= skip;
                g_ime_pending[g_ime_pending_len] = '\0';
                produced = true;
                continue;
            }
        }

        /* A trailing n is the common standalone ん, not a literal n.
         * This must precede the prefix test because `n` itself is also the
         * beginning of na/ni/nu/ne/no. */
        if (raw_fallback && g_ime_pending_len == 1 && g_ime_pending[0] == 'n') {
            if (!gui_ime_append(out_text, out_text_size, "ん")) return produced;
            g_ime_pending_len = 0;
            g_ime_pending[0] = '\0';
            produced = true;
            continue;
        }

        if (gui_ime_has_prefix(g_ime_pending, g_ime_pending_len)) break;
        if (!raw_fallback) break;

        char raw[2] = { g_ime_pending[0], '\0' };
        if (!gui_ime_append(out_text, out_text_size, raw)) return produced;
        memmove(g_ime_pending, g_ime_pending + 1, (size_t)g_ime_pending_len - 1);
        g_ime_pending_len--;
        g_ime_pending[g_ime_pending_len] = '\0';
        produced = true;
    }
    return produced;
}

bool gui_ime_translate_key(const keyboard_event_t* ev, char* out_text, size_t out_text_size, gui_ime_action_t* action) {
    if (!out_text || out_text_size == 0) return false;
    out_text[0] = '\0';
    if (action) *action = GUI_IME_ACTION_NONE;
    if (!ev || !ev->pressed || !gui_ime_is_active()) return false;
    if (ev->ctrl || ev->alt) return false;

    char ascii = ev->ascii;
    if (ev->scancode == KEY_BACKSPACE || ascii == '\b') {
        if (g_ime_pending_len > 0) {
            g_ime_pending[--g_ime_pending_len] = '\0';
            return true;
        }
        if (action) *action = GUI_IME_ACTION_BACKSPACE;
        return true;
    }
    if (ev->scancode == KEY_ENTER || ascii == '\n' || ascii == '\r') {
        gui_ime_flush_internal(out_text, out_text_size, true);
        if (action) *action = GUI_IME_ACTION_ENTER;
        gui_ime_reset();
        return true;
    }
    if (ev->scancode == KEY_TAB || ascii == '\t') {
        gui_ime_flush_internal(out_text, out_text_size, true);
        if (action) *action = GUI_IME_ACTION_TAB;
        gui_ime_reset();
        return true;
    }
    if (ascii == ' ') {
        gui_ime_flush_internal(out_text, out_text_size, true);
        gui_ime_append(out_text, out_text_size, " ");
        gui_ime_reset();
        return true;
    }

    if (ascii >= 33 && ascii <= 126) {
        char c = ascii;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || c == '\'') {
            if (g_ime_pending_len + 1 < (int)sizeof(g_ime_pending)) {
                g_ime_pending[g_ime_pending_len++] = c;
                g_ime_pending[g_ime_pending_len] = '\0';
                gui_ime_flush_internal(out_text, out_text_size, false);
            }
            return true;
        }
        gui_ime_flush_internal(out_text, out_text_size, true);
        size_t len = strlen(out_text);
        if (len + 1 < out_text_size) {
            out_text[len] = ascii;
            out_text[len + 1] = '\0';
        }
        gui_ime_reset();
        return true;
    }
    return false;
}

static void handle_keyboard_for_window(int idx) {
    window_t* w = &windows[idx];
    for (;;) {
        keyboard_event_t ev;
        if (!gui_pop_keyboard_event(&ev)) break;
        if (!ev.pressed) continue;

        char ascii = ev.ascii;
        bool ctrl = ev.ctrl || ((ev.modifiers & KEYBOARD_MOD_CTRL) != 0);
        uint8_t key = ev.scancode;
        gui_ime_action_t ime_action = GUI_IME_ACTION_NONE;
        char ime_text[64];
        bool ime_handled = gui_ime_translate_key(&ev, ime_text, sizeof(ime_text), &ime_action);

        if (w->kind == WIN_TEXT_EDITOR) {
            /* --- Save As プロンプト (無題ドキュメントの保存先入力) ---
             * 致命的バグ修正: 以前は filename が空の新規文書で保存を押すと
             * "No file open" と表示されるだけで、入力内容が保存されずに
             * 失われていた。fm_action/fm_input (このウィンドウ種別では
             * 未使用のフィールド) を流用してファイル名入力欄を表示し、
             * 実際に保存できるようにする。 */
            if (w->fm_action == 1) {
                if (key == KEY_ENTER) {
                    if (w->fm_input[0]) {
                        char full[256];
                        if (w->fm_input[0] == '/') {
                            gui_copy_cstr(full, sizeof(full), w->fm_input);
                        } else {
                            gui_copy_cstr(full, sizeof(full), "/desktop/");
                            strncat(full, w->fm_input, sizeof(full) - strlen(full) - 1);
                        }
                        int len = (int)strlen(w->text_buf);
                        char parent[256]; char leaf[256];
                        gui_split_path(full, parent, sizeof(parent), leaf, sizeof(leaf));
                        if (leaf[0] && fs_write_file_at(parent, leaf, w->text_buf, (uint64_t)len)) {
                            gui_copy_cstr(w->filename, sizeof(w->filename), full);
                            w->text_modified = FALSE;
                            gui_refresh_file_managers_for_path(w->filename);
                            w->fm_action = 0;
                            gui_notify(gui_text("File saved", "ファイルを保存しました"), 2000);
                        } else {
                            gui_notify(gui_text("Save failed", "保存に失敗しました"), 2000);
                        }
                    }
                } else if (key == KEY_ESC) {
                    w->fm_action = 0;
                    gui_notify_simple(gui_text("Save cancelled", "保存をキャンセルしました"));
                } else if (key == KEY_BACKSPACE) {
                    size_t l = strlen(w->fm_input);
                    if (l > 0) w->fm_input[l - 1] = '\0';
                } else if (ascii >= 32 && ascii < 127) {
                    size_t l = strlen(w->fm_input);
                    if (l + 1 < sizeof(w->fm_input)) { w->fm_input[l] = ascii; w->fm_input[l + 1] = '\0'; }
                }
                continue;
            }
            if (ctrl && (ascii == 'n' || ascii == 'N')) {
                text_editor_new_document(w);
                gui_notify_simple("New document");
            } else if (ctrl && (ascii == 'o' || ascii == 'O')) {
                if (w->filename[0] != '\0') {
                    if (text_editor_reload_current(w)) gui_notify_simple("Reloaded file");
                    else gui_notify("Reload failed", 2000);
                } else {
                    text_editor_new_document(w);
                    gui_notify_simple("New document");
                }
            } else if (ctrl && (ascii == 's' || ascii == 'S')) {
                if (w->filename[0] != '\0') {
                    if (text_editor_save_current(w)) gui_notify(gui_text("File saved", "ファイルを保存しました"), 2000);
                    else gui_notify(gui_text("Save failed", "保存に失敗しました"), 2000);
                } else {
                    w->fm_action = 1;
                    w->fm_input[0] = '\0';
                    gui_notify_simple(gui_text("Enter a filename and press Enter", "ファイル名を入力しEnterを押してください"));
                }
            } else if (ctrl && (ascii == 'a' || ascii == 'A')) {
                text_editor_select_all(w); gui_notify_simple("Selected all");
            } else if (ctrl && (ascii == 'z' || ascii == 'Z' || ascii == 'y' || ascii == 'Y')) {
                if (w->undo_valid) {
                    text_editor_toggle_undo(w);
                    gui_notify_simple(gui_text("Undo", "元に戻す"));
                } else {
                    gui_notify(gui_text("Nothing to undo", "取り消せる操作がありません"), 1500);
                }
            } else if (ctrl && (ascii == 'c' || ascii == 'C')) {
                if (text_editor_has_selection(w)) {
                    static char sel_buf[TEXT_BUF_SIZE];
                    int s = w->text_sel_start < w->text_sel_end ? w->text_sel_start : w->text_sel_end;
                    int e = w->text_sel_start < w->text_sel_end ? w->text_sel_end : w->text_sel_start;
                    int n = e - s;
                    if (n >= (int)sizeof(sel_buf)) n = (int)sizeof(sel_buf) - 1;
                    memcpy(sel_buf, w->text_buf + s, n);
                    sel_buf[n] = '\0';
                    gui_clipboard_set_text(sel_buf);
                    gui_notify_simple(gui_text("Text copied", "コピーしました"));
                } else {
                    gui_notify(gui_text("Nothing selected", "選択されていません"), 1500);
                }
            } else if (ctrl && (ascii == 'x' || ascii == 'X')) {
                if (text_editor_has_selection(w)) {
                    static char sel_buf[TEXT_BUF_SIZE];
                    int s = w->text_sel_start < w->text_sel_end ? w->text_sel_start : w->text_sel_end;
                    int e = w->text_sel_start < w->text_sel_end ? w->text_sel_end : w->text_sel_start;
                    int n = e - s;
                    if (n >= (int)sizeof(sel_buf)) n = (int)sizeof(sel_buf) - 1;
                    memcpy(sel_buf, w->text_buf + s, n);
                    sel_buf[n] = '\0';
                    gui_clipboard_set_text(sel_buf);
                    text_editor_delete_range(w, s, e);
                    gui_notify_simple(gui_text("Text cut", "切り取りました"));
                } else {
                    gui_notify(gui_text("Nothing selected", "選択されていません"), 1500);
                }
            } else if (ctrl && (ascii == 'v' || ascii == 'V')) {
                const char* clip = gui_clipboard_get_text(); if (clip && clip[0]) { text_editor_insert_text(w, clip); gui_notify_simple("Text pasted"); }
            } else if (ctrl && (ascii == 'f' || ascii == 'F')) {
                const char* clip = gui_clipboard_get_text(); if (clip && clip[0]) {
                    int found = gui_find_text(w->text_buf + w->text_cursor, (int)strlen(w->text_buf) - w->text_cursor, clip);
                    if (found >= 0) { w->text_cursor += found; text_editor_clear_selection(w); gui_notify_simple("Match found"); }
                    else gui_notify("No match", 1500);
                } else gui_notify("Clipboard empty", 1500);
            } else if (ctrl && (ascii == 'h' || ascii == 'H')) {
                const char* clip = gui_clipboard_get_text();
                if (clip && clip[0]) {
                    char needle[TEXT_BUF_SIZE / 2];
                    char replacement[TEXT_BUF_SIZE / 2];
                    int ni = 0, ri = 0; int mode = 0;
                    for (int i = 0; clip[i]; ++i) {
                        char ch = clip[i];
                        if (mode == 0 && (ch == '|')) { mode = 1; continue; }
                        if (mode == 0 && ni < (int)sizeof(needle) - 1) needle[ni++] = ch;
                        else if (mode == 1 && ri < (int)sizeof(replacement) - 1) replacement[ri++] = ch;
                    }
                    needle[ni] = '\0'; replacement[ri] = '\0';
                    int size = (int)strlen(w->text_buf);
                    if (needle[0] && gui_replace_all_text(w->text_buf, TEXT_BUF_SIZE, &size, needle, replacement)) {
                        w->text_cursor = size; text_editor_clear_selection(w); w->text_modified = TRUE; gui_notify_simple("Replaced text");
                    } else gui_notify("Replace failed", 1500);
                }
            } else if (ime_handled) {
                if (ime_action == GUI_IME_ACTION_BACKSPACE) {
                    if (text_editor_has_selection(w)) {
                        int s = w->text_sel_start < w->text_sel_end ? w->text_sel_start : w->text_sel_end;
                        int e = w->text_sel_start < w->text_sel_end ? w->text_sel_end : w->text_sel_start;
                        text_editor_delete_range(w, s, e);
                    } else {
                        int l = (int)strlen(w->text_buf);
                        if (l > 0 && w->text_cursor > 0) {
                            text_editor_maybe_snapshot(w);
                            int pos = gui_utf8_prev_char_start(w->text_buf, w->text_cursor);
                            int move = w->text_cursor - pos;
                            for (int i = pos; i <= l; ++i) w->text_buf[i] = w->text_buf[i + move];
                            w->text_cursor = pos; w->text_modified = TRUE; text_editor_clear_selection(w);
                        }
                    }
                } else if (ime_action == GUI_IME_ACTION_ENTER) {
                    if (ime_text[0]) text_editor_insert_text(w, ime_text);
                    text_editor_insert_text(w, "\n");
                } else if (ime_action == GUI_IME_ACTION_TAB) {
                    if (ime_text[0]) text_editor_insert_text(w, ime_text);
                    text_editor_insert_text(w, "    ");
                } else if (ime_text[0]) {
                    text_editor_insert_text(w, ime_text);
                }
            } else if (key == KEY_BACKSPACE) {
                if (text_editor_has_selection(w)) {
                    int s = w->text_sel_start < w->text_sel_end ? w->text_sel_start : w->text_sel_end;
                    int e = w->text_sel_start < w->text_sel_end ? w->text_sel_end : w->text_sel_start;
                    text_editor_delete_range(w, s, e);
                } else {
                    int l = (int)strlen(w->text_buf);
                    if (l > 0 && w->text_cursor > 0) {
                        text_editor_maybe_snapshot(w);
                        int pos = gui_utf8_prev_char_start(w->text_buf, w->text_cursor);
                        int move = w->text_cursor - pos;
                        for (int i = pos; i <= l; ++i) w->text_buf[i] = w->text_buf[i + move];
                        w->text_cursor = pos; w->text_modified = TRUE; text_editor_clear_selection(w);
                    }
                }
            } else if (key == KEY_DELETE) {
                if (text_editor_has_selection(w)) {
                    int s = w->text_sel_start < w->text_sel_end ? w->text_sel_start : w->text_sel_end;
                    int e = w->text_sel_start < w->text_sel_end ? w->text_sel_end : w->text_sel_start;
                    text_editor_delete_range(w, s, e);
                } else {
                    int l = (int)strlen(w->text_buf);
                    if (w->text_cursor < l) {
                        text_editor_maybe_snapshot(w);
                        unsigned char lead = (unsigned char)w->text_buf[w->text_cursor];
                        int step = 1;
                        if ((lead & 0x80u) == 0x00u) step = 1;
                        else if ((lead & 0xE0u) == 0xC0u) step = 2;
                        else if ((lead & 0xF0u) == 0xE0u) step = 3;
                        else if ((lead & 0xF8u) == 0xF0u) step = 4;
                        for (int i = w->text_cursor; i <= l; ++i) w->text_buf[i] = w->text_buf[i + step];
                        w->text_modified = TRUE; text_editor_clear_selection(w);
                    }
                }
            } else if (key == KEY_ENTER) {
                text_editor_insert_text(w, "\n");
            } else if (key == KEY_TAB) {
                text_editor_insert_text(w, "    ");
            } else if (key == KEY_LEFT) {
                if (w->text_cursor > 0) w->text_cursor = gui_utf8_prev_char_start(w->text_buf, w->text_cursor);
                text_editor_clear_selection(w);
            } else if (key == KEY_RIGHT) {
                int l = (int)strlen(w->text_buf);
                if (w->text_cursor < l) {
                    unsigned char lead = (unsigned char)w->text_buf[w->text_cursor];
                    int step = 1;
                    if ((lead & 0x80u) == 0x00u) step = 1;
                    else if ((lead & 0xE0u) == 0xC0u) step = 2;
                    else if ((lead & 0xF0u) == 0xE0u) step = 3;
                    else if ((lead & 0xF8u) == 0xF0u) step = 4;
                    w->text_cursor += step; if (w->text_cursor > l) w->text_cursor = l;
                }
                text_editor_clear_selection(w);
            } else if (key == KEY_UP) {
                if (w->scroll_y > 0) w->scroll_y--;
            } else if (key == KEY_DOWN) {
                w->scroll_y++;
            } else if (key == KEY_HOME) {
                w->text_cursor = 0; text_editor_clear_selection(w);
            } else if (key == KEY_END) {
                w->text_cursor = (int)strlen(w->text_buf); text_editor_clear_selection(w);
            } else if (!ctrl && ascii >= 32 && ascii < 127) {
                char tmp[2] = { ascii, '\0' }; text_editor_insert_text(w, tmp);
            }
        } else if (w->kind == WIN_VOXEL_GAME) {
            voxel_games_handle_key(idx, &ev);
        } else if (w->kind == WIN_TINYGL_VIEWER) {
            tinygl_viewer_handle_key(idx, &ev);
        } else if (w->kind == WIN_2DGAMES) {
            games2d_handle_key(idx, &ev);
        } else if (w->kind == WIN_FILE_MGR) {
            handle_file_manager_key(idx, key, ascii, ctrl);
        } else if (w->kind == WIN_BROWSER) {
            handle_browser_key(idx, ascii, key, ctrl);
        } else if (w->kind == WIN_HTTP_DOWNLOADER) {
            http_downloader_handle_key(idx, ascii, key, ctrl);
            return;
        } else if (w->kind == WIN_SETTINGS) {
            handle_settings_key(idx, ascii, key, ctrl);
        } else if (w->kind == WIN_TERMINAL) {
            int visible_lines = gui_terminal_visible_lines(w);
            int max_scroll = gui_terminal_max_scroll(w);
            if (ime_handled) {
                if (ime_action == GUI_IME_ACTION_BACKSPACE) {
                    int l = (int)strlen(w->term_input);
                    if (l > 0) {
                        int pos = gui_utf8_prev_char_start(w->term_input, l);
                        int step = l - pos;
                        for (int i = pos; i <= l; ++i) w->term_input[i] = w->term_input[i + step];
                    }
                } else if (ime_action == GUI_IME_ACTION_ENTER) {
                    if (w->term_hist_count < TERM_HISTORY) {
                        strncpy(w->term_history[w->term_hist_count], w->term_input, TERM_LINE_LEN - 1);
                        w->term_history[w->term_hist_count][TERM_LINE_LEN - 1] = '\0';
                        w->term_hist_count++;
                    }
                    w->term_hist_pos = -1;
                    if (ime_text[0]) {
                        int l = (int)strlen(w->term_input);
                        int add = (int)strlen(ime_text);
                        if (l + add < TERM_LINE_LEN - 1) {
                            for (int i = l; i >= 0; --i) w->term_input[i + add] = w->term_input[i];
                            for (int i = 0; i < add; ++i) w->term_input[i] = ime_text[i];
                        }
                    }
                    gui_terminal_execute(w, w->term_input);
                    w->term_input[0] = 0;
                } else if (ime_action == GUI_IME_ACTION_TAB) {
                    int l = (int)strlen(w->term_input);
                    if (l < TERM_LINE_LEN - 2) { w->term_input[l] = '	'; w->term_input[l+1] = 0; }
                } else if (ime_text[0]) {
                    int l = (int)strlen(w->term_input);
                    int add = (int)strlen(ime_text);
                    if (l + add < TERM_LINE_LEN - 1) {
                        for (int i = l; i >= 0; --i) w->term_input[i + add] = w->term_input[i];
                        for (int i = 0; i < add; ++i) w->term_input[i] = ime_text[i];
                    }
                }
            } else if (key == KEY_BACKSPACE) {
                int l = (int)strlen(w->term_input);
                if (l > 0) {
                    int pos = gui_utf8_prev_char_start(w->term_input, l);
                    int step = l - pos;
                    for (int i = pos; i <= l; ++i) w->term_input[i] = w->term_input[i + step];
                }
            } else if (key == KEY_ENTER) {
                if (w->term_hist_count < TERM_HISTORY) {
                    strncpy(w->term_history[w->term_hist_count], w->term_input, TERM_LINE_LEN - 1);
                    w->term_history[w->term_hist_count][TERM_LINE_LEN - 1] = '\0';
                    w->term_hist_count++;
                }
                w->term_hist_pos = -1;
                gui_terminal_execute(w, w->term_input);
                w->term_input[0] = 0;
            } else if (ctrl && key == KEY_UP) {
                if (w->term_scroll < max_scroll) w->term_scroll++;
            } else if (ctrl && key == KEY_DOWN) {
                if (w->term_scroll > 0) w->term_scroll--;
            } else if (key == KEY_PAGEUP) {
                w->term_scroll += visible_lines; if (w->term_scroll > max_scroll) w->term_scroll = max_scroll;
            } else if (key == KEY_PAGEDOWN) {
                if (w->term_scroll > visible_lines) w->term_scroll -= visible_lines; else w->term_scroll = 0;
            } else if (key == KEY_HOME) {
                w->term_scroll = max_scroll;
            } else if (key == KEY_END) {
                w->term_scroll = 0;
            } else if (key == KEY_UP && w->term_hist_count > 0) {
                if (w->term_hist_pos < w->term_hist_count - 1) w->term_hist_pos++;
                int hi = w->term_hist_count - 1 - w->term_hist_pos;
                if (hi >= 0 && hi < w->term_hist_count) { strncpy(w->term_input, w->term_history[hi], TERM_LINE_LEN - 1); w->term_input[TERM_LINE_LEN - 1] = '\0'; }
            } else if (key == KEY_DOWN) {
                if (w->term_hist_pos > 0) {
                    w->term_hist_pos--;
                    int hi = w->term_hist_count - 1 - w->term_hist_pos;
                    if (hi >= 0 && hi < w->term_hist_count) { strncpy(w->term_input, w->term_history[hi], TERM_LINE_LEN - 1); w->term_input[TERM_LINE_LEN - 1] = '\0'; }
                } else { w->term_hist_pos = -1; w->term_input[0] = 0; }
            } else if (ascii >= 32 && ascii < 127) {
                int l = (int)strlen(w->term_input);
                if (l < TERM_LINE_LEN - 2) { w->term_input[l] = ascii; w->term_input[l+1] = 0; }
            }
        } else if (w->kind == WIN_CALC) {
            handle_calculator_key(idx, ascii, key, ctrl);
            if (ev.extended && key == KEY_UP) {
                if (w->scroll_y > 0) w->scroll_y--;
            } else if (ev.extended && key == KEY_DOWN) {
                if (w->scroll_y < 7) w->scroll_y++;
            }
        } else if (w->kind == WIN_SHEET) {
            handle_sheet_key(idx, key, ascii, ctrl);
        } else if (w->kind == WIN_PYTHON_IDE) {
            python_ide_handle_keyboard(ev);
        }
    }
}
void gui_handle_input(void) {
    int mx = mouse.x, my = mouse.y, taskbar_y = (int)SCREEN_H - TASKBAR_H;
    static int last_mouse_x = -1;
    static int last_mouse_y = -1;
    static uint8_t last_mouse_buttons = 0xFF;
    uint8_t mouse_buttons = (mouse.left ? 0x01 : 0) |
                            (mouse.right ? 0x02 : 0) |
                            (mouse.middle ? 0x04 : 0);
    bool left_edge = (mouse_buttons & 0x01) && !(last_mouse_buttons & 0x01);
    bool right_edge = (mouse_buttons & 0x02) && !(last_mouse_buttons & 0x02);
    bool left_release = !(mouse_buttons & 0x01) && (last_mouse_buttons & 0x01);
    bool right_release = !(mouse_buttons & 0x02) && (last_mouse_buttons & 0x02);

    // Check click flags from mouse_state directly
    minimal_mouse_t* ms = minimal_mouse_get_state();
    bool left_click = ms->left_click || left_edge;
    bool right_click = ms->right_click || right_edge;

    /* A keyboard-driven click is translated into the exact same left/right
     * dispatch path as a physical pointer click. It intentionally has no
     * drag/release state: the secondary pointer performs discrete actions. */
    int multi_click_x = 0, multi_click_y = 0;
    bool multi_click_right = false;
    if (gui_multi_cursor_take_click(&multi_click_x, &multi_click_y, &multi_click_right)) {
        mx = multi_click_x;
        my = multi_click_y;
        if (multi_click_right) right_click = true;
        else left_click = true;
    }

    bool mouse_changed = (mx != last_mouse_x) || (my != last_mouse_y) ||
                         (mouse_buttons != last_mouse_buttons) ||
                         left_click || right_click ||
                         left_release || right_release;
    if (mouse_changed || keyboard_has_event()) {
        gui_request_redraw();
    }

    /* Deferred desktop-state restore: let the first desktop frame appear, then
     * attempt the snapshot load on a later GUI tick. */
    gui_process_pending_desktop_layout_load();
    
    // Keep the keyboard queue intact for focused windows. Enter-as-click
    // emulation used to consume all key events here and starve apps of input.

    if (ctx_menu.visible && ctx_menu.context_type == 2 && keyboard_has_event()) {
        handle_start_menu_keyboard();
    }

    if (left_click) {
        if (ctx_menu.visible) { handle_context_menu_click(mx, my); goto gui_handle_input_finish; }
        if (my >= taskbar_y + 4 && my < taskbar_y + 36 && mx >= 8 && mx < 94) {
            if (ctx_menu.visible && ctx_menu.context_type == 2) {
                ctx_menu.visible = FALSE;
                submenu_close();
                start_menu_clear_search();
            } else {
                open_start_menu(8, taskbar_y - 220);
            }
            goto gui_handle_input_finish;
        }
        // Taskbar window buttons: click to restore/focus minimized windows
        if (my >= taskbar_y) {
            /* C-OS 4.0.8 alpha: dock quick-launch icons (NetSurf,
             * Calculator) at the right side of the taskbar.
             * Layout mirrors draw_taskbar()'s dock_x = SCREEN_W - 320,
             * each button 36px wide, 30px drawn. */
            int dock_base_x = (int)SCREEN_W - 320;
            if (dock_base_x < 56) dock_base_x = 56;
            /* NS button */
            if (mx >= dock_base_x && mx < dock_base_x + 30 &&
                my >= taskbar_y + 4 && my < taskbar_y + 36) {
                int ex = gui_find_window(WIN_BROWSER);
                if (ex >= 0) {
                    if (windows[ex].minimized) windows[ex].minimized = FALSE;
                    gui_bring_to_front(ex);
                } else {
                    gui_open_window(WIN_BROWSER, gui_text("NetSurf", "NetSurf"),
                                    96, 64, 1160, 760);
                }
                goto gui_handle_input_finish;
            }
            /* CA button */
            if (mx >= dock_base_x + 36 && mx < dock_base_x + 66 &&
                my >= taskbar_y + 4 && my < taskbar_y + 36) {
                int ex = gui_find_window(WIN_CALC);
                if (ex >= 0) {
                    if (windows[ex].minimized) windows[ex].minimized = FALSE;
                    gui_bring_to_front(ex);
                } else {
                    gui_open_window(WIN_CALC, gui_text("Calculator", "電卓"),
                                    200, 100, 400, 500);
                }
                goto gui_handle_input_finish;
            }
            /* Icon-only taskbar buttons: mirrors draw_taskbar()'s
             * window-icon row - start button ends at x=94, buttons
             * begin at x=110, each a 46px-wide slot. Must skip
             * invisible windows exactly like the render loop does,
             * or this cursor drifts out of sync with what's drawn. */
            int bx2 = 110;
            for (int i = 0; i < window_count; i++) {
                if (!windows[i].visible) continue;
                if (mx >= bx2 && mx < bx2 + 46) {
                    if (windows[i].minimized) {
                        windows[i].minimized = FALSE;
                    }
                    gui_bring_to_front(i);
                    goto gui_handle_input_finish;
                }
                bx2 += 46;
            }
        }
        // Windows must be checked before desktop icons so icons hidden behind
        // an app window do not receive clicks or start dragging.
        for (int i = window_count - 1; i >= 0; i--) {
            window_t* w = &windows[i];
            if (!w->visible || w->minimized) continue;
            if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
                /* Capture geometry before reordering the z-stack. gui_bring_to_front()
                 * mutates the window array, so any pointer/index we keep must be
                 * refreshed after the focus change.
                 */
                int wx = w->x;
                int wy = w->y;
                int ww = w->w;

                // Check close button first (top-right corner of title bar).
                // Must match draw_window_frame()'s close_x/btn_y/size exactly
                // (w->x+w->w-32, w->y+6, 24x24) or clicks on part of the
                // visible button silently miss.
                int bx = wx + ww - 32, by = wy + 6;
                if (mx >= bx && mx < bx + 24 && my >= by && my < by + 24) {
                    gui_close_window(i);
                    goto gui_handle_input_finish;
                }

                gui_bring_to_front(i);
                i = window_count - 1;
                w = &windows[i];

                // Title bar drag is armed on press and starts after a small move threshold.
                if (my >= wy && my < wy + TITLEBAR_H) {
                    drag_candidate = TRUE;
                    drag_candidate_window = i;
                    drag_anchor_x = mx;
                    drag_anchor_y = my;
                    drag_off_x = mx - wx;
                    drag_off_y = my - wy;
                    goto gui_handle_input_finish;
                }

                // Resize grips along the right/bottom edges and the
                // bottom-right corner (both flags set). Kept below the
                // titlebar row so they never overlap the window-control
                // buttons there. Same press-then-threshold arming as the
                // titlebar drag above.
                {
                    const int grip = 8;
                    int wh = w->h;
                    bool on_right = !w->maximized && (mx >= wx + ww - grip && mx < wx + ww) && (my >= wy + TITLEBAR_H && my < wy + wh);
                    bool on_bottom = !w->maximized && (my >= wy + wh - grip && my < wy + wh) && (mx >= wx && mx < wx + ww);
                    if (on_right || on_bottom) {
                        resize_candidate = TRUE;
                        resize_candidate_window = i;
                        resize_edge = (on_right ? 1 : 0) | (on_bottom ? 2 : 0);
                        resize_anchor_x = mx;
                        resize_anchor_y = my;
                        resize_start_w = ww;
                        resize_start_h = wh;
                        goto gui_handle_input_finish;
                    }
                }

                // Per-app click handling
                if (w->kind == WIN_VOXEL_GAME) {
                    voxel_games_handle_click(i, mx, my);
                    goto gui_handle_input_finish;
                } else if (w->kind == WIN_TINYGL_VIEWER) {
                    tinygl_viewer_handle_click(i, mx, my);
                    goto gui_handle_input_finish;
                } else if (w->kind == WIN_2DGAMES) {
                    games2d_handle_click(i, mx, my);
                    goto gui_handle_input_finish;
                } else if (w->kind == WIN_CALC) {
                    handle_calculator_click(i, mx, my);
                    goto gui_handle_input_finish;
                } else if (w->kind == WIN_TEXT_EDITOR) {
                    /* Toolbar button clicks */
                    int ty2 = wy + TITLEBAR_H + 10;
                    if (mx >= wx+8 && mx < wx+56 && my >= ty2 && my < ty2 + 22) {
                        text_editor_new_document(w);
                        gui_notify_simple("New document");
                    } else if (mx >= wx+64 && mx < wx+124 && my >= ty2 && my < ty2 + 22) {
                        if (w->filename[0] != '\0') {
                            if (text_editor_reload_current(w)) {
                                gui_notify_simple("Reloaded file");
                            } else {
                                gui_notify("Reload failed", 2000);
                            }
                        } else {
                            gui_notify("No file open", 2000);
                        }
                    } else if (mx >= wx+130 && mx < wx+182 && my >= ty2 && my < ty2 + 22) {
                        if (w->filename[0] != '\0') {
                            if (text_editor_save_current(w)) {
                                gui_notify(gui_text("File saved", "ファイルを保存しました"), 2000);
                            } else {
                                gui_notify(gui_text("Save failed", "保存に失敗しました"), 2000);
                            }
                        } else {
                            w->fm_action = 1;
                            w->fm_input[0] = '\0';
                            gui_notify_simple(gui_text("Enter a filename and press Enter", "ファイル名を入力しEnterを押してください"));
                        }
                    } else if (mx >= wx+188 && mx < wx+276 && my >= ty2 && my < ty2 + 22) {
                        w->text_sel_start = 0;
                        w->text_sel_end = (int)strlen(w->text_buf);
                        w->text_cursor = w->text_sel_end;
                        gui_notify_simple("Selected all");
                    }
                } else if (w->kind == WIN_MUSIC) {
                    music_player_handle_click(i, mx, my);
                } else if (w->kind == WIN_FILE_MGR) {
                    handle_file_manager_click(i, mx, my);
                } else if (w->kind == WIN_BROWSER) {
                    handle_browser_click(i, mx, my);
                } else if (w->kind == WIN_HTTP_DOWNLOADER) {
                    http_downloader_handle_click(i, mx, my);
                } else if (w->kind == WIN_SHEET) {
                    handle_sheet_click(i, mx, my);
                } else if (w->kind == WIN_SETTINGS) {
                    handle_settings_click(i, mx, my);
                } else if (w->kind == WIN_PAINT) {
                    handle_paint_mouse(i, mx, my, true);
                } else if (w->kind == WIN_PYTHON_IDE) {
                    python_ide_handle_click(mx, my);
                }
                goto gui_handle_input_finish;
            }
        }
        // Desktop icons: double-click to open, drag snaps to the desktop grid.
        // Only when no visible app window is covering the click point.
        int icon_box = gui_get_desktop_icon_render_size();
        for (int i = 0; i < desktop_icon_count; i++) {
            desktop_icon_t* ic = &desktop_icons[i];
            int icon_h = icon_box + 22;
            if (mx >= ic->x && mx < ic->x + icon_box && my >= ic->y && my < ic->y + icon_h) {
                extern uint64_t get_timer_ticks(void);
                int current_time = get_timer_ticks();
                if (last_click_icon == i && (current_time - last_click_time) < 500) {
                    int existing = gui_find_window(ic->win_kind);
                    if (existing >= 0) {
                        gui_bring_to_front(existing);
                    } else {
                        int open_w = 700, open_h = 500, open_x = 100 + i * 30, open_y = 80 + i * 20;
                        if (ic->win_kind == WIN_VOXEL_GAME) {
                            open_w = 1120;
                            open_h = (int)SCREEN_H - 40;
                            open_x = 80;
                            open_y = 60;
                        }
                        gui_open_window(ic->win_kind, ic->label, open_x, open_y, open_w, open_h);
                    }
                    last_click_icon = -1;
                    last_click_time = 0;
                    icon_drag_candidate = -1;
                    icon_dragging = FALSE;
                    goto gui_handle_input_finish;
                }
                last_click_icon = i;
                last_click_time = current_time;
                drag_candidate = FALSE;
                drag_candidate_window = -1;
                icon_drag_candidate = i;
                icon_dragging = FALSE;
                icon_drag_anchor_x = mx;
                icon_drag_anchor_y = my;
                icon_drag_off_x = mx - ic->x;
                icon_drag_off_y = my - ic->y;
                goto gui_handle_input_finish;
            }
        }
    }
    if (right_click) {
        if (my >= taskbar_y) {
            open_taskbar_context_menu(mx, my);
            goto gui_handle_input_finish;
        }
        for (int i = window_count - 1; i >= 0; i--) {
            if (mx >= windows[i].x && mx < windows[i].x + windows[i].w && my >= windows[i].y && my < windows[i].y + windows[i].h) {
                if (windows[i].kind == WIN_VOXEL_GAME) {
                    goto gui_handle_input_finish;
                }

                /* The enhanced file manager owns a file-aware context menu
                 * (open, rename, copy, paste, delete, properties).  The old
                 * global branch below intercepted every right click first,
                 * making that menu impossible to open.  Focus the file
                 * manager, then hand the event to its dedicated handler. */
                if (windows[i].kind == WIN_FILE_MGR) {
                    gui_bring_to_front(i);
                    handle_file_manager_click(window_count - 1, mx, my);
                    goto gui_handle_input_finish;
                }
                if (windows[i].kind == WIN_BROWSER) {
                    gui_bring_to_front(i);
                    handle_browser_right_click(window_count - 1, mx, my);
                    goto gui_handle_input_finish;
                }

                gui_bring_to_front(i);
                open_window_context_menu(mx, my, window_count - 1);
                goto gui_handle_input_finish;
            }
        }
        int target_icon = -1;
        int icon_box = gui_get_desktop_icon_render_size();
        for (int i = 0; i < desktop_icon_count; ++i) {
            desktop_icon_t* ic = &desktop_icons[i];
            if (mx >= ic->x && mx < ic->x + icon_box && my >= ic->y && my < ic->y + icon_box + 22) {
                target_icon = i;
                break;
            }
        }
        open_desktop_context_menu(mx, my, target_icon);
    }
    if (left_release || right_release) {
        if ((dragging || resizing) && active_window >= 0 && active_window < window_count) {
            (void)gui_save_window_state_snapshot(&windows[active_window]);
        }
        if (icon_dragging && icon_drag_candidate >= 0 && icon_drag_candidate < desktop_icon_count) {
            desktop_icon_t* source = &desktop_icons[icon_drag_candidate];
            int drop_target = -1;
            int icon_box = gui_get_desktop_icon_render_size();

            /* A drop onto an actual desktop folder moves the underlying file.
             * Reordering remains the fallback for app icons, missing paths,
             * or drops on empty desktop space. */
            if (source->is_dynamic && source->is_file && source->path[0]) {
                for (int di = 0; di < desktop_icon_count; ++di) {
                    desktop_icon_t* target = &desktop_icons[di];
                    if (di == icon_drag_candidate || target->is_file || !target->path[0]) continue;
                    if (mx >= target->x && mx < target->x + icon_box &&
                        my >= target->y && my < target->y + icon_box + 22) {
                        fs_entry_t* destination = fs_find(target->path);
                        if (destination && destination->is_dir) {
                            drop_target = di;
                            break;
                        }
                    }
                }
            }

            if (drop_target >= 0) {
                const char* leaf = strrchr(source->path, '/');
                char destination_path[256];
                size_t base_len = strlen(desktop_icons[drop_target].path);
                if (leaf != NULL) leaf++; else leaf = source->path;
                if (leaf[0] && base_len + 1 + strlen(leaf) < sizeof(destination_path)) {
                    memcpy(destination_path, desktop_icons[drop_target].path, base_len);
                    destination_path[base_len] = '/';
                    strcpy(destination_path + base_len + 1, leaf);
                    if (fs_rename(source->path, destination_path) == 0) {
                        gui_refresh_desktop_icons();
                        gui_notify_simple(gui_text("File moved", "ファイルを移動しました"));
                    } else {
                        gui_notify_simple(gui_text("File move failed", "ファイルを移動できませんでした"));
                    }
                }
            } else {
                int snap_x = source->x;
                int snap_y = source->y;
                gui_snap_desktop_icon_position(&snap_x, &snap_y, icon_drag_candidate);
                source->x = snap_x;
                source->y = snap_y;
                gui_snapshot_save_desktop();
            }
        }
        dragging = FALSE;
        resizing = FALSE;
        drag_candidate = FALSE;
        drag_candidate_window = -1;
        icon_dragging = FALSE;
        icon_drag_candidate = -1;
        resize_candidate = FALSE;
        resize_candidate_window = -1;
        gui_request_redraw();
    }
    if (drag_candidate && mouse.left && drag_candidate_window >= 0 && drag_candidate_window < window_count) {
        int dx = mx - drag_anchor_x;
        int dy = my - drag_anchor_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx >= gui_mouse_drag_threshold || dy >= gui_mouse_drag_threshold) {
            dragging = TRUE;
            active_window = drag_candidate_window;
        }
    }
    if (icon_drag_candidate >= 0 && mouse.left && icon_drag_candidate < desktop_icon_count) {
        int dx = mx - icon_drag_anchor_x;
        int dy = my - icon_drag_anchor_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (!icon_dragging && (dx >= gui_mouse_drag_threshold || dy >= gui_mouse_drag_threshold)) {
            icon_dragging = TRUE;
            /* Clear stale double-click tracking once the icon is actually moved. */
            last_click_icon = -1;
            last_click_time = 0;
        }
        if (icon_dragging) {
            int nx = mx - icon_drag_off_x;
            int ny = my - icon_drag_off_y;
            gui_snap_desktop_icon_position(&nx, &ny, icon_drag_candidate);
            if (desktop_icons[icon_drag_candidate].x != nx || desktop_icons[icon_drag_candidate].y != ny) {
                desktop_icons[icon_drag_candidate].x = nx;
                desktop_icons[icon_drag_candidate].y = ny;
                gui_request_redraw();
            }
        }
    }
    if (dragging && mouse.left && active_window >= 0 && active_window < window_count) {
        window_t* moving = &windows[active_window];
        int next_x = mx - drag_off_x;
        int next_y = my - drag_off_y;
        /* This used to mutate coordinates without invalidating the compositor.
         * A moved window could therefore remain visually frozen until an
         * unrelated 30Hz/idle redraw. Request a frame only when its rectangle
         * actually changed, letting the lifecycle coalesce high-rate IRQ input. */
        if (moving->x != next_x || moving->y != next_y) {
            moving->x = next_x;
            moving->y = next_y;
            gui_request_redraw();
        }
    }

    if (resize_candidate && mouse.left && resize_candidate_window >= 0 && resize_candidate_window < window_count) {
        int dx = mx - resize_anchor_x;
        int dy = my - resize_anchor_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx >= gui_mouse_drag_threshold || dy >= gui_mouse_drag_threshold) {
            resizing = TRUE;
            active_window = resize_candidate_window;
        }
    }
    if (resizing && mouse.left && resize_candidate_window >= 0 && resize_candidate_window < window_count) {
        window_t* rw = &windows[resize_candidate_window];
        const int min_w = 240, min_h = 160;
        int dx = mx - resize_anchor_x;
        int dy = my - resize_anchor_y;
        if (resize_edge & 1) {
            int new_w = resize_start_w + dx;
            if (new_w < min_w) new_w = min_w;
            rw->w = new_w;
        }
        if (resize_edge & 2) {
            int new_h = resize_start_h + dy;
            if (new_h < min_h) new_h = min_h;
            rw->h = new_h;
        }
        gui_request_redraw();
    }

    if (active_window >= 0 && active_window < window_count && windows[active_window].kind == WIN_PAINT) {
        handle_paint_mouse(active_window, mx, my, mouse.left);
    }

gui_handle_input_finish:
    // Clear click flags after processing
    minimal_mouse_clear_clicks();
    
    // Update mouse movement tracking after click handling so keyboard focus
    // changes caused by a click are visible before key events are routed.
    last_mouse_x = mx;
    last_mouse_y = my;
    last_mouse_buttons = mouse_buttons;

    /* Global keyboard actions are consumed before the focused application.
     * The PS/2 protocol does not expose a dedicated Fn modifier on ordinary
     * hardware. When firmware maps Fn to F12, F12 is therefore the portable
     * Fn-compatible right-click trigger; an Alt press remains left-click. */
    for (;;) {
        keyboard_event_t peek;
        if (!gui_pop_keyboard_event(&peek)) break;
        if (peek.pressed) {
            uint64_t now = get_timer_ticks();
            if (multi_cursor_hotkey_armed && now - multi_cursor_hotkey_tick > 800u) {
                multi_cursor_hotkey_armed = false;
            }

            if (peek.ctrl && (peek.scancode == KEYBOARD_SCANCODE_P ||
                              peek.ascii == 'p' || peek.ascii == 'P')) {
                multi_cursor_hotkey_armed = true;
                multi_cursor_hotkey_tick = now;
                continue;
            }
            if (peek.ctrl && multi_cursor_hotkey_armed &&
                (peek.scancode == KEYBOARD_SCANCODE_O ||
                 peek.ascii == 'o' || peek.ascii == 'O')) {
                multi_cursor_hotkey_armed = false;
                gui_toggle_multi_cursor_mode();
                gui_notify(gui_get_multi_cursor_enabled()
                               ? gui_text("Multi-cursor mode enabled", "マルチカーソルモードを有効にしました")
                               : gui_text("Multi-cursor mode disabled", "マルチカーソルモードを無効にしました"),
                           1800);
                continue;
            }

            /* Ctrl+B: open or focus the standard NetSurf browser. This gives
             * keyboard-only and virtual-machine users a reliable route to the
             * browser without changing ordinary desktop click rules. */
            if (peek.ctrl &&
                (peek.scancode == KEYBOARD_SCANCODE_B ||
                 peek.ascii == 'b' || peek.ascii == 'B' || peek.ascii == '\x02')) {
                int existing = gui_find_window(WIN_BROWSER);
                if (existing >= 0) {
                    if (windows[existing].minimized) windows[existing].minimized = FALSE;
                    gui_bring_to_front(existing);
                } else {
                    gui_open_window(WIN_BROWSER, gui_text("NetSurf", "NetSurf"),
                                    96, 64, 1160, 760);
                }
                gui_notify(gui_text("NetSurf opened", "NetSurfを開きました"), 1200);
                continue;
            }

            if (gui_get_multi_cursor_enabled()) {
                if (!peek.ctrl && !peek.alt && peek.extended && peek.scancode == KEY_LEFT) {
                    gui_multi_cursor_move(-16, 0); continue;
                }
                if (!peek.ctrl && !peek.alt && peek.extended && peek.scancode == KEY_RIGHT) {
                    gui_multi_cursor_move(16, 0); continue;
                }
                if (!peek.ctrl && !peek.alt && peek.extended && peek.scancode == KEY_UP) {
                    gui_multi_cursor_move(0, -16); continue;
                }
                if (!peek.ctrl && !peek.alt && peek.extended && peek.scancode == KEY_DOWN) {
                    gui_multi_cursor_move(0, 16); continue;
                }
                if (peek.scancode == KEY_LALT || peek.scancode == KEYBOARD_SCANCODE_RALT) {
                    gui_multi_cursor_request_click(false); continue;
                }
                if (peek.scancode == KEYBOARD_SCANCODE_F12) {
                    gui_multi_cursor_request_click(true); continue;
                }
            }
        }

        // Alt+Tab: switch to the next window. windows[] is already kept in
        // Z-order by gui_bring_to_front() (focused window always last), so
        // repeatedly promoting the window just below the top cycles through
        // every open window over repeated presses - no separate MRU list.
        if (peek.pressed && peek.scancode == KEY_TAB && peek.alt) {
            if (window_count >= 2) {
                int target = -1;
                for (int i = window_count - 2; i >= 0; i--) {
                    if (windows[i].visible) { target = i; break; }
                }
                if (target >= 0) {
                    if (windows[target].minimized) gui_restore_window(target);
                    gui_bring_to_front(target);
                }
            }
            continue;
        }
        gui_push_keyboard_event(&peek);
        break;
    }

    // Route keyboard to the focused window. If no window has focus yet,
    // keep draining the queue so it does not overflow while idle.
    int keyboard_target = gui_find_keyboard_target();
    if (keyboard_target >= 0) {
        handle_keyboard_for_window(keyboard_target);
    } else {
        while (keyboard_has_event()) {
            (void)keyboard_get_event();
        }
    }
}

