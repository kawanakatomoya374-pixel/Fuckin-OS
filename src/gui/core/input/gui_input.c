/**
 * gui_input.c - GUIコア (キーボード/ターミナル/テキストエディタ/IME 入力処理)
 * gui.c (5,588行) から分割生成。詳細は gui_internal.h を参照。
 */

#include "gui.h"
#include "gui_internal.h"
#include "cos_shell.h"
#include "../../../kernel/cos_app_window.h"
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
#include "gui_render_engine.h"
#include "gui_utils.h"
#include "../apps/system/password_screen.h"
#include "../components/boot_animation.h"
#include "notification_center.h"
#include "../screenshot.h"
#include "theme_system.h"
#include <shell.h>
#include <string.h>
#include <stdio.h>

/* gui.c は他モジュール (gui_apps_common.c) にある公開版 gui_get_prompt_hostname()
 * とは別に、同名だが独立した static 版を意図的に保持していた。分割後に
 * 2ファイル (gui_settings.c と gui_input.c) から必要になるため、非static化
 * して衝突させるのではなく、この小さなヘルパーをここに複製しておく。 */


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

/* Is this the window the keyboard goes to? (SYS_WIN2_INPUT only reports keys to the focused window.) */
bool gui_window_is_keyboard_focus(const void *win_ptr) {
    int t = gui_find_keyboard_target();
    return t >= 0 && win_ptr == (const void *)&windows[t];
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


























/* Called before any edit actually mutates text_buf. If more than
 * 500ms have passed since the last edit, this is treated as the
 * start of a new "run" (a burst of typing, a paste, a delete) and the
 * pre-edit state is snapshotted into undo_buf - so Ctrl+Z undoes the
 * whole run, not one keystroke at a time. Continuing the same run
 * (fast typing) does not re-snapshot, so undo lands before the run
 * started rather than one character back. */


/* Ctrl+Z/Ctrl+Y: swaps the current and saved states, so pressing it
 * again reverses itself (undo, then redo, then undo, ...). */

















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

        if (w->kind == WIN_BROWSER) {
            handle_browser_key(idx, ascii, key, ctrl);
        }      else if (w->kind == WIN_COS_APP) {
            /* Forward to the ring3 program's own input queue instead of
             * interpreting the key here - this window kind has no
             * kernel-side notion of what the keystroke means (a text
             * field, a game control, anything the program decides), so
             * the kernel's only job is to deliver it faithfully. Maps to
             * the same small COS_KEY_* ABI documented in
             * cos_app_window.h; anything not specifically named there
             * and not an ordinary printable ASCII character is dropped
             * (not queued) - a function key or other key with no ASCII
             * payload and no COS_KEY_* meaning is not something this
             * small surface represents yet.
             *
             * Modifiers are reported on EVERY event via a separate
             * bitmask (COS_MOD_*), not folded into `special` or `ascii`:
             * the keyboard driver already hands ascii through
             * unconverted for a Ctrl-held key (confirmed by reading
             * keyboard.c - there is no Ctrl-to-control-code
             * transformation anywhere in it), so Ctrl+C arrives here as
             * ascii='c' with COS_MOD_CTRL set, not as a synthesized
             * control byte - simpler for a ring3 program to interpret
             * however it wants, and does not silently invent a
             * translation the driver itself does not perform. */
            extern void cos_app_window_push_key(void *win_ptr, char ascii,
                                                uint8_t special, uint8_t cos_mods);
            uint8_t cos_mods = 0;
            if (ev.shift || (ev.modifiers & KEYBOARD_MOD_SHIFT)) cos_mods |= COS_MOD_SHIFT;
            if (ctrl) cos_mods |= COS_MOD_CTRL;
            if (ev.alt || (ev.modifiers & KEYBOARD_MOD_ALT)) cos_mods |= COS_MOD_ALT;

            if (key == KEY_ENTER) {
                cos_app_window_push_key(w, 0, COS_KEY_ENTER, cos_mods);
            } else if (key == KEY_BACKSPACE) {
                cos_app_window_push_key(w, 0, COS_KEY_BACKSPACE, cos_mods);
            } else if (key == KEY_ESC) {
                cos_app_window_push_key(w, 0, COS_KEY_ESC, cos_mods);
            } else if (key == KEY_TAB) {
                cos_app_window_push_key(w, 0, COS_KEY_TAB, cos_mods);
            } else if (key == KEY_DELETE && ev.extended) {
                cos_app_window_push_key(w, 0, COS_KEY_DELETE, cos_mods);
            } else if (ev.extended && key == KEY_UP) {
                cos_app_window_push_key(w, 0, COS_KEY_UP, cos_mods);
            } else if (ev.extended && key == KEY_DOWN) {
                cos_app_window_push_key(w, 0, COS_KEY_DOWN, cos_mods);
            } else if (ev.extended && key == KEY_LEFT) {
                cos_app_window_push_key(w, 0, COS_KEY_LEFT, cos_mods);
            } else if (ev.extended && key == KEY_RIGHT) {
                cos_app_window_push_key(w, 0, COS_KEY_RIGHT, cos_mods);
            } else if (ev.extended && key == 0x47) {
                cos_app_window_push_key(w, 0, COS_KEY_HOME, cos_mods);
            } else if (ev.extended && key == 0x4F) {
                cos_app_window_push_key(w, 0, COS_KEY_END, cos_mods);
            } else if (ev.extended && key == 0x49) {
                cos_app_window_push_key(w, 0, COS_KEY_PGUP, cos_mods);
            } else if (ev.extended && key == 0x51) {
                cos_app_window_push_key(w, 0, COS_KEY_PGDN, cos_mods);
            } else if (ev.extended && key == 0x52) {
                cos_app_window_push_key(w, 0, COS_KEY_INSERT, cos_mods);
            } else if (!ev.extended && key >= 0x3B && key <= 0x44) {          /* F1..F10 */
                cos_app_window_push_key(w, 0, (uint8_t)(COS_KEY_F1 + (key - 0x3B)), cos_mods);
            } else if (!ev.extended && (key == 0x57 || key == 0x58)) {         /* F11, F12 */
                cos_app_window_push_key(w, 0, (uint8_t)(COS_KEY_F1 + 10 + (key - 0x57)), cos_mods);
            } else if (ascii >= 32 && ascii < 127) {
                cos_app_window_push_key(w, ascii, COS_KEY_NONE, cos_mods);
            }
        }
    }
}
/* Feeds raw mouse state to the topmost ring-3 v2 window under the cursor
 * (or the one that currently has a button held - so drags keep working
 * outside the window) so it can generate MOVE/DOWN/UP/WHEEL events. */
static void cos_app_feed_mouse_state(void) {
    extern void cos_app_window_mouse_state(void *win_ptr, int32_t cx, int32_t cy, uint8_t buttons, int32_t wheel, bool inside);
    extern bool cos_app_window_is_grabbing(const void *win_ptr);
    extern void cos_app_window_grab_feed(void *win_ptr, int64_t dx, int64_t dy, int32_t cx, int32_t cy, uint8_t buttons, int32_t wheel);
    extern void gui_set_cursor_hidden(bool hidden);
    static uint32_t s_capture_uid = 0;
    uint8_t buttons = (uint8_t)((mouse.left ? 1 : 0) | (mouse.right ? 2 : 0) | (mouse.middle ? 4 : 0));
    /* Pointer lock: a game window that holds it and has the keyboard focus gets the raw motion; the cursor is hidden and the
     * pointer parked in the middle of the window so it can never run into a screen edge or another window. */
    {
        int kt = gui_find_keyboard_target();
        window_t *gw = (kt >= 0 && windows[kt].kind == WIN_COS_APP && cos_app_window_is_grabbing(&windows[kt])) ? &windows[kt] : NULL;
        gui_set_cursor_hidden(gw != NULL);
        if (gw) {
            int64_t dx, dy;
            mouse_take_motion(&dx, &dy);
            int tb = gw->fullscreen ? 0 : TITLEBAR_H;
            int ccx = gw->w / 2, ccy = (gw->h - tb) / 2;
            mouse_warp(gw->x + ccx, gw->y + tb + ccy);
            cos_app_window_grab_feed(gw, dx, dy, ccx, ccy, buttons, (int)mouse.wheel);
            if (mouse.wheel != 0) { extern bool g_gui_wheel_app_only; g_gui_wheel_app_only = true; }
            s_capture_uid = 0;
            return;
        }
    }
    int target = -1;
    if (s_capture_uid) target = gui_window_index_by_uid(s_capture_uid);
    if (target < 0) {
        s_capture_uid = 0;
        for (int i = window_count - 1; i >= 0; --i) {
            window_t *w = &windows[i];
            if (mouse.x >= w->x && mouse.x < w->x + w->w && mouse.y >= w->y && mouse.y < w->y + w->h) {
                if (w->kind == WIN_COS_APP) target = i;
                break;   /* topmost window under the cursor, whatever its kind */
            }
        }
    }
    if (target < 0) return;
    window_t *w = &windows[target];
    int cx = mouse.x - w->x, cy = mouse.y - w->y - TITLEBAR_H;
    bool inside = cx >= 0 && cy >= 0 && cx < w->w && cy < w->h - TITLEBAR_H;
    if (w->fullscreen) { extern bool cos_app_window_fs_map(const void *, int, int, int *, int *); inside = cos_app_window_fs_map(w, mouse.x, mouse.y, &cx, &cy); }
    int wheel = 0;
    wheel = (int)mouse.wheel;   /* this frame's wheel delta, as the rest of the GUI reads it */
    cos_app_window_mouse_state(w, cx, cy, buttons, wheel, inside);
    if (wheel != 0 && inside && w->kind == WIN_COS_APP) { extern bool g_gui_wheel_app_only; g_gui_wheel_app_only = true; }
    s_capture_uid = (buttons && (inside || s_capture_uid)) ? w->uid : 0;
}

/* ---- ring-3 desktop actions (run on the GUI thread, from gui_shell_tick) ---- */
void gui_desktop_open_icon(int i)
{
    if (i < 0 || i >= desktop_icon_count) return;
    desktop_icon_t *ic = &desktop_icons[i];
    if (ic->win_kind == WIN_APP_FOLDER) {
        extern void gui_app_folder_open(int desktop_icon_index);
        gui_app_folder_open(i);
        return;
    }
    int existing = gui_find_window(ic->win_kind);
    if (existing >= 0) {
        if (windows[existing].minimized) windows[existing].minimized = FALSE;
        gui_bring_to_front(existing);
        return;
    }
    int open_w = 700, open_h = 500, open_x = 100 + i * 30, open_y = 80 + i * 20;
    if (ic->win_kind == WIN_VOXEL_GAME) { open_w = 1120; open_h = (int)SCREEN_H - 40; open_x = 80; open_y = 60; }
    gui_open_window(ic->win_kind, ic->label, open_x, open_y, open_w, open_h);
}

/* The icon has been dragged to its current x,y and released with the pointer at (mx,my): move a file into a folder, merge
 * app icons into a folder, or snap to the grid - the same three outcomes the kernel desktop had. */
void gui_desktop_drop_icon(int i, int mx, int my)
{
    if (i < 0 || i >= desktop_icon_count) return;
    desktop_icon_t *source = &desktop_icons[i];
    int icon_box = gui_get_desktop_icon_render_size();
    int drop_target = -1;
    if (source->is_dynamic && source->is_file && source->path[0]) {
        for (int di = 0; di < desktop_icon_count; ++di) {
            desktop_icon_t *t = &desktop_icons[di];
            if (di == i || t->is_file || !t->path[0]) continue;
            if (mx >= t->x && mx < t->x + icon_box && my >= t->y && my < t->y + icon_box + 22) {
                fs_entry_t *d = fs_find(t->path);
                if (d && d->is_dir) { drop_target = di; break; }
            }
        }
    }
    if (drop_target < 0 && !source->is_file && source->win_kind != WIN_APP_FOLDER) {
        extern bool gui_app_folder_try_merge(int source_idx, int target_idx);
        for (int di = 0; di < desktop_icon_count; ++di) {
            if (di == i) continue;
            desktop_icon_t *t = &desktop_icons[di];
            if (t->is_file) continue;
            if (mx >= t->x && mx < t->x + icon_box && my >= t->y && my < t->y + icon_box + 22) {
                if (gui_app_folder_try_merge(i, di)) { gui_snapshot_save_desktop(); gui_request_redraw(); return; }
                break;
            }
        }
    }
    if (drop_target >= 0) {
        const char *leaf = strrchr(source->path, '/');
        char dest[256];
        size_t base_len = strlen(desktop_icons[drop_target].path);
        if (leaf != NULL) leaf++; else leaf = source->path;
        if (leaf[0] && base_len + 1 + strlen(leaf) < sizeof(dest)) {
            memcpy(dest, desktop_icons[drop_target].path, base_len);
            dest[base_len] = '/';
            strcpy(dest + base_len + 1, leaf);
            if (fs_rename(source->path, dest) == 0) {
                gui_refresh_desktop_icons();
                gui_notify_simple(gui_text("File moved", "ファイルを移動しました"));
            } else {
                gui_notify_simple(gui_text("File move failed", "ファイルを移動できませんでした"));
            }
        }
    } else {
        int sx = source->x, sy = source->y;
        gui_snap_desktop_icon_position(&sx, &sy, i);
        source->x = sx; source->y = sy;
        gui_snapshot_save_desktop();
    }
    gui_request_redraw();
}


/* ---- a file dropped by a ring-3 program (SYS_FILE_DROP) -------------------------------------------------------------------
 * The program that owns the drag (Files) keeps getting the mouse while the button is down, even outside its window, and
 * reports the drop here. The pointer decides: an app window gets COS_EV_DROP with client coordinates (the program's own window
 * is skipped - it handles that itself); bare desktop gets the file in /desktop (copied with `copy`, else moved), placed where
 * it was let go; a folder icon on the desktop takes it inside. */
typedef struct { char path[256]; uint32_t pid; int copy, x, y; } fdrop_req_t;
static fdrop_req_t s_fdq[16];
static int s_fdq_head, s_fdq_tail;

int gui_file_drop(const char *path, uint32_t pid, int copy)
{
    if (!path || !path[0]) return -1;
    uint64_t f = sync_irq_save();
    int next = (s_fdq_head + 1) % 16;
    if (next == s_fdq_tail) { sync_irq_restore(f); return -1; }
    fdrop_req_t *q = &s_fdq[s_fdq_head];
    strncpy(q->path, path, sizeof q->path - 1); q->path[sizeof q->path - 1] = 0;
    q->pid = pid; q->copy = copy; q->x = mouse.x; q->y = mouse.y;
    s_fdq_head = next;
    sync_irq_restore(f);
    return 0;
}

static void file_drop_run(const fdrop_req_t *q)
{
    extern uint32_t cos_app_window_owner_of_uid(uint32_t);
    extern bool cos_app_window_deliver_drop_uid(uint32_t, const char *, int32_t, int32_t);
    int mx = q->x, my = q->y;
    for (int i = window_count - 1; i >= 0; --i) {
        window_t *w = &windows[i];
        if (!w->visible || w->minimized || mx < w->x || mx >= w->x + w->w || my < w->y || my >= w->y + w->h) continue;
        if (w->kind == WIN_COS_APP && cos_app_window_owner_of_uid(w->uid) != q->pid)
            cos_app_window_deliver_drop_uid(w->uid, q->path, mx - w->x, my - w->y - TITLEBAR_H);
        return;                                                           /* the topmost window decides */
    }
    if (my >= (int)SCREEN_H - TASKBAR_H) return;
    const char *dir = "/desktop";
    int box = gui_get_desktop_icon_render_size();
    for (int di = 0; di < desktop_icon_count; ++di) {
        desktop_icon_t *t = &desktop_icons[di];
        if (t->is_file || !t->path[0] || !t->is_dynamic) continue;
        if (mx >= t->x && mx < t->x + box && my >= t->y && my < t->y + box + 22) { fs_entry_t *d = fs_find(t->path); if (d && d->is_dir) { dir = t->path; break; } }
    }
    const char *leaf = strrchr(q->path, '/');
    leaf = leaf ? leaf + 1 : q->path;
    if (!leaf[0]) return;
    char dest[300];
    snprintf(dest, sizeof dest, "%s/%s", dir, leaf);
    if (strcmp(dest, q->path) == 0) return;                              /* it is already there */
    for (int n = 2; fs_find(dest) && n < 100; ++n) {                      /* never overwrite: "name (2).ext" */
        const char *dot = strrchr(leaf, '.');
        if (dot && dot != leaf) snprintf(dest, sizeof dest, "%s/%.*s (%d)%s", dir, (int)(dot - leaf), leaf, n, dot);
        else snprintf(dest, sizeof dest, "%s/%s (%d)", dir, leaf, n);
    }
    bool ok = q->copy ? fs_copy_path(q->path, dest) : fs_move_path(q->path, dest);
    if (!ok) { gui_notify_simple(gui_text("The file could not be moved here", "ファイルをここへ移動できませんでした")); return; }
    gui_refresh_desktop_icons();
    if (!strcmp(dir, "/desktop")) {
        for (int di = 0; di < desktop_icon_count; ++di) {
            if (strcmp(desktop_icons[di].path, dest) != 0) continue;
            int sx = mx - box / 2, sy = my - box / 2;
            gui_snap_desktop_icon_position(&sx, &sy, di);
            desktop_icons[di].x = sx; desktop_icons[di].y = sy;
            gui_snapshot_save_desktop();
            break;
        }
    }
    gui_notify_simple(q->copy ? gui_text("File copied", "ファイルをコピーしました") : gui_text("File moved", "ファイルを移動しました"));
    gui_request_redraw();
}

static void file_drop_tick(void)
{
    for (;;) {
        fdrop_req_t q;
        uint64_t f = sync_irq_save();
        if (s_fdq_tail == s_fdq_head) { sync_irq_restore(f); return; }
        q = s_fdq[s_fdq_tail];
        s_fdq_tail = (s_fdq_tail + 1) % 16;
        sync_irq_restore(f);
        file_drop_run(&q);
    }
}

static bool point_on_window(int mx, int my)
{
    for (int i = 0; i < window_count; i++) {
        window_t *w = &windows[i];
        if (w->visible && !w->minimized && mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) return true;
    }
    return false;
}

void gui_handle_input(void) {
    cos_app_feed_mouse_state();
    file_drop_tick();
    int mx = mouse.x, my = mouse.y, taskbar_y = (int)SCREEN_H - TASKBAR_H;

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
    if (gui_fullscreen_index() >= 0) goto gui_handle_input_finish;   /* a fullscreen game owns the pointer: no taskbar, menus, icons or window chrome underneath */

    /* A keyboard-driven click is translated into the exact same left/right
     * dispatch path as a physical pointer click. It intentionally has no
     * drag/release state: the secondary pointer performs discrete actions. */
    gui_shell_tick();

    /* Ring-3 shell layers (taskbar = 1, start menu = 2). A layer owns the mouse over its rectangle: a press starts a capture
     * that lasts until every button is up, and presses/drags/releases in that time go to the layer only. Plain hovering is
     * forwarded with no buttons so it can highlight, without stealing the end of a window drag that crosses it. The start
     * menu, while open, also takes the keyboard, and a press anywhere outside it (and outside the taskbar, which decides for
     * itself) closes it. */
    {
        extern bool cos_shell_layer_geometry(int layer, int *x, int *y, int *w, int *h);
        extern bool cos_shell_layer_visible(int layer);
        extern bool cos_shell_layer_set_visible(int layer, bool visible);
        extern void cos_shell_layer_mouse(int layer, int32_t cx, int32_t cy, uint8_t buttons, int32_t wheel, bool inside);
        extern void cos_shell_layer_key(int layer, char ascii, uint8_t special, uint8_t mods);
        static int s_capture_layer;
        static bool s_was_inside[4];
        bool press = (left_edge || right_edge || ((mouse_buttons & 0x04) && !(last_mouse_buttons & 0x04)));
        int menu_x, menu_y, menu_w, menu_h;
        bool menu_open = cos_shell_layer_visible(2) && cos_shell_layer_geometry(2, &menu_x, &menu_y, &menu_w, &menu_h);

        if (menu_open) {
            while (keyboard_has_event()) {
                keyboard_event_t kev;
                if (!gui_pop_keyboard_event(&kev)) break;
                if (!kev.pressed) continue;
                uint8_t k = kev.scancode, m = 0, sp = 0;
                if (kev.shift || (kev.modifiers & KEYBOARD_MOD_SHIFT)) m |= COS_MOD_SHIFT;
                if (kev.ctrl || (kev.modifiers & KEYBOARD_MOD_CTRL)) m |= COS_MOD_CTRL;
                if (kev.alt || (kev.modifiers & KEYBOARD_MOD_ALT)) m |= COS_MOD_ALT;
                if (k == KEY_ENTER) sp = COS_KEY_ENTER; else if (k == KEY_BACKSPACE) sp = COS_KEY_BACKSPACE;
                else if (k == KEY_ESC) sp = COS_KEY_ESC; else if (k == KEY_TAB) sp = COS_KEY_TAB;
                else if (kev.extended && k == KEY_UP) sp = COS_KEY_UP; else if (kev.extended && k == KEY_DOWN) sp = COS_KEY_DOWN;
                else if (kev.extended && k == KEY_LEFT) sp = COS_KEY_LEFT; else if (kev.extended && k == KEY_RIGHT) sp = COS_KEY_RIGHT;
                else if (kev.extended && k == 0x47) sp = COS_KEY_HOME; else if (kev.extended && k == 0x4F) sp = COS_KEY_END;
                else if (kev.extended && k == KEY_DELETE) sp = COS_KEY_DELETE;
                if (sp) cos_shell_layer_key(2, 0, sp, m);
                else if (kev.ascii) cos_shell_layer_key(2, kev.ascii, 0, m);
            }
        }
        static const int order[3] = { 2, 1, 3 };
        for (int oi = 0; oi < 3; oi++) {
            int layer = order[oi];
            int px, py, pw, ph;
            if (layer == 2) { if (!menu_open) continue; px = menu_x; py = menu_y; pw = menu_w; ph = menu_h; }
            else if (!cos_shell_layer_geometry(layer, &px, &py, &pw, &ph)) continue;
            bool inside = mx >= px && mx < px + pw && my >= py && my < py + ph;
            if (layer == 3) {
                extern bool gui_app_folder_is_active(void);
                /* the desktop only sees pointer activity over bare wallpaper, and never while a menu or folder is up */
                if (!s_capture_layer && (point_on_window(mx, my) || ctx_menu.visible || gui_app_folder_is_active() || dragging || resizing || drag_candidate || resize_candidate))
                    inside = false;
            }
            if (inside && press && !s_capture_layer) s_capture_layer = layer;
            if (s_capture_layer == layer) {
                if (inside && press && layer == 1) gui_shell_note_panel_press();
                if (inside && press && layer == 3) gui_shell_note_panel_press();    /* a click on the desktop closes the start menu */
                cos_shell_layer_mouse(layer, mx - px, my - py, mouse_buttons, (int)mouse.wheel, inside);
                if (!mouse_buttons) s_capture_layer = 0;
                last_mouse_buttons = mouse_buttons;
                minimal_mouse_clear_clicks();
                gui_request_redraw();
                return;
            }
            if (inside && !s_capture_layer) cos_shell_layer_mouse(layer, mx - px, my - py, 0, (int)mouse.wheel, true);
            else if (s_was_inside[layer]) cos_shell_layer_mouse(layer, -1, -1, 0, 0, true);      /* hover left the layer */
            s_was_inside[layer] = inside && !s_capture_layer;
        }
        if (menu_open && press) {
            int tx, ty, tw, th;
            bool on_taskbar = cos_shell_layer_geometry(1, &tx, &ty, &tw, &th) && mx >= tx && mx < tx + tw && my >= ty && my < ty + th;
            if (!on_taskbar) { cos_shell_layer_set_visible(2, false); gui_request_redraw(); }
        }
    }

    int multi_click_x = 0, multi_click_y = 0;
    bool multi_click_right = false;
    if (gui_multi_cursor_take_click(&multi_click_x, &multi_click_y, &multi_click_right)) {
        mx = multi_click_x;
        my = multi_click_y;
        if (multi_click_right) right_click = true;
        else left_click = true;
    }

    bool interaction_changed = (mouse_buttons != last_mouse_buttons) ||
                               left_click || right_click ||
                               left_release || right_release;

    /* An open (or opening/closing) app folder is modal: while it is
     * up, it gets first refusal on every click/drag/release so nothing
     * underneath (a desktop icon, a window, the taskbar) reacts to a
     * click that was really meant for the folder overlay or its dim
     * background. See gui_app_folder.c for what each input edge does
     * inside it (launch an item, drag one out, or close on an
     * outside click). */
    {
        extern bool gui_app_folder_is_active(void);
        extern bool gui_app_folder_handle_input(int mx, int my, bool left_down_edge, bool left_up_edge, bool left_held);
        if (gui_app_folder_is_active()) {
            (void)gui_app_folder_handle_input(mx, my, left_click, left_release, mouse.left);
            last_mouse_buttons = mouse_buttons;
            return;
        }
    }
    /* Split from a single unconditional call: pure position movement is
     * already detected and handled by gui_lifecycle.c's own tracking
     * (last_present_mouse_x/y, completely independent of this
     * function's own now-removed position tracking), which is what its
     * cursor-only fast
     * path (see gui.h) needs to be able to distinguish "just the mouse
     * moved" from "something else happened" - a distinction this call
     * used to erase by requesting a full redraw for a plain position
     * change too, redundantly with a check gui_lifecycle.c was already
     * doing.
     *
     * interaction_changed (buttons/clicks/releases) keeps the
     * unconditional, immediate call regardless of whether position also
     * changed in the same tick (e.g. dragging) - those are the discrete,
     * low-frequency events where "request now, don't wait for anything"
     * is correct and was never the source of the redundant-redraw cost
     * this addresses. */
    if (interaction_changed || keyboard_has_event()) {
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
                    /* A v2 ring-3 app gets COS_EV_CLOSE and closes its own
                     * window (it may want to save first); clicking again
                     * while that request is pending closes it regardless. */
                    extern bool cos_app_window_user_close(void *win_ptr);
                    if (w->kind == WIN_COS_APP && cos_app_window_user_close(w)) {
                        goto gui_handle_input_finish;
                    }
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
                if (w->kind == WIN_BROWSER) {
                    handle_browser_click(i, mx, my);
                }     else if (w->kind == WIN_COS_APP) {
                    extern void cos_app_window_push_mouse(void *win_ptr, int32_t x, int32_t y, uint8_t button);
                    /* Client-area coordinates, matching the space
                     * cos_app_window_fill_rect()/draw_text() already use
                     * (see cos_app_window.h) - the chrome offset is
                     * subtracted once here rather than leaving every
                     * ring3 program to know TITLEBAR_H itself. */
                    int client_x = mx - wx;
                    int client_y = my - wy - TITLEBAR_H;
                    uint8_t btn = right_click ? 0x02u /* COS_MOUSE_BTN_RIGHT */ : 0x01u /* COS_MOUSE_BTN_LEFT */;
                    cos_app_window_push_mouse(w, client_x, client_y, btn);
                    extern bool g_gui_click_app_only;
                    if (client_x >= 0 && client_y >= 0) g_gui_click_app_only = true;   /* inside the client area: the app repaints what it changes */
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
                    if (ic->win_kind == WIN_APP_FOLDER) {
                        extern void gui_app_folder_open(int desktop_icon_index);
                        gui_app_folder_open(i);
                        last_click_icon = -1;
                        last_click_time = 0;
                        icon_drag_candidate = -1;
                        icon_dragging = FALSE;
                        gui_desktop_icon_drag_end();
                        goto gui_handle_input_finish;
                    }
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
                    gui_desktop_icon_drag_end();
                    goto gui_handle_input_finish;
                }
                last_click_icon = i;
                last_click_time = current_time;
                drag_candidate = FALSE;
                drag_candidate_window = -1;
                icon_drag_candidate = i;
                icon_dragging = FALSE;
                gui_desktop_icon_drag_end();
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

                if (windows[i].kind == WIN_BROWSER) {
                    gui_bring_to_front(i);
                    handle_browser_right_click(window_count - 1, mx, my);
                    goto gui_handle_input_finish;
                }

                gui_bring_to_front(i);
                /* A ring-3 app owns right-clicks inside its client area (its own context menu); the window menu belongs to the title bar. */
                if (windows[window_count - 1].kind == WIN_COS_APP && my >= windows[window_count - 1].y + TITLEBAR_H) goto gui_handle_input_finish;
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

            /* An app icon dropped onto another app icon (or onto an
             * existing app folder) is the iOS "drag app onto app"
             * gesture: merge them into a folder instead of reordering.
             * Tried only when the file-onto-folder case above didn't
             * already claim this drop. */
            if (drop_target < 0 && !source->is_file && source->win_kind != WIN_APP_FOLDER) {
                extern bool gui_app_folder_try_merge(int source_idx, int target_idx);
                for (int di = 0; di < desktop_icon_count; ++di) {
                    if (di == icon_drag_candidate) continue;
                    desktop_icon_t* target = &desktop_icons[di];
                    if (target->is_file) continue;
                    if (mx >= target->x && mx < target->x + icon_box &&
                        my >= target->y && my < target->y + icon_box + 22) {
                        if (gui_app_folder_try_merge(icon_drag_candidate, di)) {
                            gui_snapshot_save_desktop();
                            icon_dragging = FALSE;
                            gui_desktop_icon_drag_end();
                            icon_drag_candidate = -1;
                            goto gui_folder_merge_consumed;
                        }
                        break; /* found the hover target; whether or not the
                                * merge itself succeeded, this was still the
                                * intended drop - don't also reorder onto it. */
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
        gui_folder_merge_consumed:
        dragging = FALSE;
        resizing = FALSE;
        drag_candidate = FALSE;
        drag_candidate_window = -1;
        icon_dragging = FALSE;
        gui_desktop_icon_drag_end();
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
            gui_desktop_icon_drag_begin(icon_drag_candidate);
            /* Clear stale double-click tracking once the icon is actually moved. */
            last_click_icon = -1;
            last_click_time = 0;
        }
        if (icon_dragging) {
            /* Follow the cursor 1:1 while actively dragging - grid
             * snapping happens once, at drop (see the mouse-release
             * handler below), not every frame here. Snapping on every
             * frame during the drag itself used to make the icon jump
             * from grid cell to grid cell instead of following the
             * cursor smoothly - exactly the "choppy" feel a real
             * desktop's drag never has. */
            int nx = mx - icon_drag_off_x;
            int ny = my - icon_drag_off_y;
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
            /* Only the area the window left and the area it now covers (frame + drop shadow) change. */
            gui_dirty_rect(moving->x, moving->y, moving->w + 12, moving->h + 12);
            moving->x = next_x;
            moving->y = next_y;
            gui_dirty_rect(moving->x, moving->y, moving->w + 12, moving->h + 12);
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

gui_handle_input_finish:
    // Clear click flags after processing
    screenshot_handle_mouse();
    minimal_mouse_clear_clicks();
    
    // Update mouse movement tracking after click handling so keyboard focus
    // changes caused by a click are visible before key events are routed.
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

            /* App Folder is modal (see the mouse-input intercept earlier
             * in this function) - Esc closing it takes priority over
             * every other global shortcut below, and consumes the key
             * either way so it never also reaches whatever window is
             * focused underneath. */
            {
                extern bool gui_app_folder_is_active(void);
                extern void gui_app_folder_request_close(void);
                if (gui_app_folder_is_active()) {
                    if (peek.scancode == KEY_ESC) {
                        gui_app_folder_request_close();
                    }
                    continue;
                }
            }

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

            if (peek.ctrl && peek.shift &&
                (peek.scancode == KEYBOARD_SCANCODE_S || peek.ascii == 's' || peek.ascii == 'S')) {
                screenshot_begin();
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
        if (peek.pressed && peek.ctrl && peek.alt && peek.scancode == KEYBOARD_SCANCODE_F12) {
            extern void gui_fullscreen_force_exit(void);
            gui_fullscreen_force_exit();
            continue;
        }
        if (peek.pressed && peek.scancode == KEY_TAB && peek.alt && gui_fullscreen_index() >= 0) continue;
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
