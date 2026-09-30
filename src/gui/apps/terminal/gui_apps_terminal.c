/**
 * gui_apps_terminal.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * ターミナル
 *
 * 元は単一の gui_apps.c (11,638行) に含まれていたコードを、
 * 保守性向上のため機能単位に分割したものの一部。
 */
#include "gui.h"
#include "mk_desktop.h"
#include "system/password_screen.h"
#include "vga.h"
#include "mk_mp3.h"
#include "../../../apps/jpeg_viewer.h"
#include "string.h"
#include "serial.h"

#ifndef KEY_PAGEUP
#define KEY_PAGEUP   0x49
#endif
#ifndef KEY_PAGEDOWN
#define KEY_PAGEDOWN 0x51
#endif
#ifndef KEY_HOME
#define KEY_HOME     0x47
#endif
#ifndef KEY_END
#define KEY_END      0x4F
#endif
#include "memory.h"
#include "memory_physical.h"
#include "cos_version.h"
#include "rtc.h"
#include "scheduler.h"
#include "../../bios/bios.h"
#include "../../kernel/drivers/usb.h"
#include "../../kernel/drivers/pci.h"
#include "fs.h"
#include "keyboard.h"
#include "../../drivers/disk/storage.h"
#include "../../drivers/input/mouse_minimal.h"
#include <shell.h>
extern const char* fs_read_file_at(const char* path, const char* name);
extern const char* config_get_string(const char* key);
extern void gui_snapshot_save_desktop(void);
extern bool settings_set_desktop_icon_size(uint32_t size) __attribute__((weak));
extern void gui_normalize_desktop_icons(void);
#include "gui_apps_common.h"

/* ============================================================
 * TERMINAL - synced with the system shell
 * ============================================================ */
static window_t* gui_terminal_output_window = NULL;

static int term_visible_lines(window_t* w) {
    if (!w) return 1;
    int content_h = w->h - C_TITLEBAR_H - 64;
    int lines = content_h / (FONT_H + 3);
    if (lines < 1) lines = 1;
    return lines;
}

static int term_max_scroll(window_t* w) {
    if (!w) return 0;
    int visible = term_visible_lines(w);
    int max_scroll = w->term_line_count - visible;
    return max_scroll > 0 ? max_scroll : 0;
}

static void term_clear_output(window_t* w) {
    if (!w) return;
    memset(w->term_lines, 0, sizeof(w->term_lines));
    w->term_line_count = 0;
    w->term_scroll = 0;
}

static void term_ensure_first_line(window_t* w) {
    if (!w) return;
    if (w->term_line_count <= 0) {
        w->term_lines[0][0] = '\0';
        w->term_line_count = 1;
    }
}

static void term_scroll_if_needed(window_t* w) {
    if (!w) return;
    if (w->term_line_count < TERM_LINES) return;
    for (int i = 0; i < TERM_LINES - 1; ++i) {
        scopy(w->term_lines[i], w->term_lines[i + 1], TERM_LINE_LEN);
    }
    w->term_lines[TERM_LINES - 1][0] = '\0';
    w->term_line_count = TERM_LINES - 1;
}

static void term_append_line(window_t* w, const char* line) {
    if (!w || !line) return;
    if (w->term_line_count >= TERM_LINES) {
        term_scroll_if_needed(w);
    }
    term_ensure_first_line(w);
    if (w->term_line_count < TERM_LINES) {
        scopy(w->term_lines[w->term_line_count], line, TERM_LINE_LEN - 1);
        w->term_lines[w->term_line_count][TERM_LINE_LEN - 1] = '\0';
        w->term_line_count++;
    }
}

static void term_append_output(window_t* w, const char* text) {
    if (!w || !text) return;

    for (const char* p = text; *p; ++p) {
        char c = *p;
        if (c == '\r') continue;

        if (c == '\n') {
            term_ensure_first_line(w);
            term_append_line(w, "");
            continue;
        }

        term_ensure_first_line(w);
        if (w->term_line_count == 0) {
            w->term_lines[0][0] = '\0';
            w->term_line_count = 1;
        }

        int idx = w->term_line_count - 1;
        size_t len = strlen(w->term_lines[idx]);
        if (len >= TERM_LINE_LEN - 1) {
            term_append_line(w, "");
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

static void term_shell_output_callback(const char* text) {
    if (!gui_terminal_output_window || !text) return;
    term_append_output(gui_terminal_output_window, text);
    if (gui_get_terminal_autoscroll()) {
        gui_terminal_output_window->term_scroll = 0;
    }
}

static void term_sync_cwd(window_t* w) {
    if (!w) return;
    const char* cwd = shell_get_cwd();
    if (cwd && cwd[0]) {
        scopy(w->term_cwd, cwd, sizeof(w->term_cwd) - 1);
        w->term_cwd[sizeof(w->term_cwd) - 1] = '\0';
    }
}

static void term_exec(window_t* w, const char* cmd) {
    if (!w || !cmd) return;

    if (gui_get_terminal_autoscroll()) {
        w->term_scroll = 0;
    }

    if (!gui_terminal_command_allowed(cmd)) {
        if (w->term_line_count >= TERM_LINES) term_scroll_if_needed(w);
        if (w->term_line_count < TERM_LINES) {
            char blocked[TERM_LINE_LEN];
            scopy(blocked, "[Safe mode] Command blocked in GUI terminal: ", TERM_LINE_LEN);
            scat(blocked, cmd, TERM_LINE_LEN);
            scopy(w->term_lines[w->term_line_count], blocked, TERM_LINE_LEN);
            w->term_line_count++;
        }
        gui_terminal_output_window = NULL;
        return;
    }

    if (smatch(cmd, "clear") || smatch(cmd, "cls")) {
        term_clear_output(w);
    }

    char prompt_line[TERM_LINE_LEN];
    prompt_line[0] = '\0';
    scopy(prompt_line, "user@", TERM_LINE_LEN - 1);
    scat(prompt_line, gui_get_prompt_hostname(), TERM_LINE_LEN - 1);
    scat(prompt_line, ":", TERM_LINE_LEN - 1);
    scat(prompt_line, w->term_cwd, TERM_LINE_LEN - 1);
    scat(prompt_line, "$ ", TERM_LINE_LEN - 1);
    scat(prompt_line, cmd, TERM_LINE_LEN - 1);
    term_append_line(w, prompt_line);

    gui_terminal_output_window = w;
    shell_set_output_callback(term_shell_output_callback);

    char cmd_copy[TERM_LINE_LEN];
    scopy(cmd_copy, cmd, TERM_LINE_LEN - 1);
    shell_execute(cmd_copy, false);
    if (gui_get_terminal_autoscroll()) {
        w->term_scroll = 0;
    }

    shell_set_output_callback(NULL);
    gui_terminal_output_window = NULL;

    term_sync_cwd(w);
}

void draw_terminal(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w, ch = w->h - C_TITLEBAR_H;
    bool act = (idx == active_window);

    if (cw < 180 || ch < 120) {
        vga_fill_rect(cx, cy, cw, ch, C_TERM_BG);
        vga_draw_string(cx + 8, cy + 8, gui_text("Terminal", "ターミナル"), C_TERM_PROMPT, 0xFFFFFFFF);
        return;
    }

     /* Terminal background */
    vga_fill_rect(cx, cy, cw, ch, C_TERM_BG);

     /* Tab bar */
    vga_fill_rect(cx, cy, cw, 32, rgb(30,30,30));
    vga_fill_rect(cx, cy+31, cw, 1, rgb(50,50,50));
    vga_fill_rounded_rect(cx+8, cy+4, 120, 24, 4, rgb(50,50,50));
    vga_draw_string(cx+16, cy+8, gui_text("Terminal 1", "ターミナル 1"), rgb(200,200,200), 0xFFFFFFFF);
    vga_draw_string(cx+116, cy+8, "x", rgb(150,150,150), 0xFFFFFFFF);
    vga_fill_rounded_rect(cx+136, cy+8, 20, 16, 3, rgb(50,50,50));
    vga_draw_string(cx+140, cy+10, "+", rgb(150,150,150), 0xFFFFFFFF);

     /* Content */
    int lh = FONT_H + 3;
    int content_y = cy + 32;
    int content_h = ch - 32 - 32; /* minus tab bar and input bar */
    if (content_h < lh) content_h = lh;
    int max_lines = content_h / lh;
    if (max_lines < 1) max_lines = 1;

    int max_scroll = w->term_line_count - max_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (w->term_scroll < 0) w->term_scroll = 0;
    if (w->term_scroll > max_scroll) w->term_scroll = max_scroll;

     /* Render output lines */
    int start = w->term_line_count - max_lines - w->term_scroll;
    if (start < 0) start = 0;
    if (start > w->term_line_count) start = w->term_line_count;

    for (int i = start; i < w->term_line_count && i - start < max_lines; i++) {
        int ly = content_y + (i - start) * lh;
        const char* line = w->term_lines[i];

        uint64_t lc = C_TERM_TEXT;
        if (smatch(line, "bash:") || smatch(line, "Error:")) lc = C_TERM_ERR;
        else if (smatch(line, "  PID") || smatch(line, "Filesystem")) lc = C_TERM_INFO;
        else if (smatch(line, "Available") || smatch(line, "  ls") ||
                 smatch(line, "  cd") || smatch(line, "  echo")) lc = C_TERM_INFO;
        else if (smatch(line, "drwx") || smatch(line, "-rw")) lc = rgb(150,220,150);
        else if (smatch(line, "total")) lc = C_TEXT_DIM;

        vga_draw_string(cx+8, ly, line, lc, 0xFFFFFFFF);
    }

    if (max_scroll > 0) {
        int sb_x = cx + cw - 10;
        int sb_y = content_y + 6;
        int sb_h = content_h - 12;
        int thumb_h = (max_lines * sb_h) / (w->term_line_count > 0 ? w->term_line_count : 1);
        if (thumb_h < 18) thumb_h = 18;
        if (thumb_h > sb_h) thumb_h = sb_h;
        int thumb_y = sb_y;
        if (max_scroll > 0 && sb_h > thumb_h) {
            thumb_y = sb_y + ((sb_h - thumb_h) * (max_scroll - w->term_scroll)) / max_scroll;
        }
        vga_fill_rect(sb_x, sb_y, 4, sb_h, rgb(55,55,55));
        vga_fill_rect(sb_x, thumb_y, 4, thumb_h, C_ACCENT);
    }

     /* Input bar */
    int input_y = cy + ch - 32;
    vga_fill_rect(cx, input_y, cw, 32, rgb(20,20,20));
    vga_fill_rect(cx, input_y, cw, 1, rgb(50,50,50));

     /* Prompt */
    char prompt[64];
    prompt[0] = '\0';
    scopy(prompt, "user@", 63);
    scat(prompt, gui_get_prompt_hostname(), 63);
    scat(prompt, ":", 63);
    scat(prompt, w->term_cwd, 63);
    scat(prompt, "$ ", 63);
    int pw = slen(prompt)*FONT_W;
    vga_draw_string(cx+8, input_y+8, prompt, C_TERM_PROMPT, 0xFFFFFFFF);
    vga_draw_string(cx+8+pw, input_y+8, w->term_input, C_TERM_CMD, 0xFFFFFFFF);

     /* Cursor */
    if (act && (int)(frame_counter_get() / 30) % 2 == 0) {
        int cur_x = cx+8+pw + slen(w->term_input)*FONT_W;
        vga_fill_rect(cur_x, input_y+8, 8, FONT_H, rgb(200,200,200));
    }
}

void handle_terminal_key(int idx, char ascii, int scancode) {
    window_t* w = &windows[idx];
    int ilen = slen(w->term_input);
    int max_scroll = term_max_scroll(w);
    int visible_lines = term_visible_lines(w);

    if (ascii >= 32 && ascii <= 126) {
        if (ilen < TERM_LINE_LEN-2) {
            w->term_input[ilen] = ascii;
            w->term_input[ilen+1] = 0;
        }
    } else if (ascii == '\n' || ascii == '\r') {
        char cmd_copy[TERM_LINE_LEN];
        scopy(cmd_copy, w->term_input, TERM_LINE_LEN - 1);
        term_exec(w, cmd_copy);
        w->term_input[0] = 0;
        if (w->term_hist_count < TERM_HISTORY) {
            scopy(w->term_history[w->term_hist_count++], cmd_copy, TERM_LINE_LEN - 1);
        }
        w->term_hist_pos = -1;
    } else if (ascii == '\b' || scancode == 0x0E) {
        if (ilen > 0) w->term_input[ilen-1] = 0;
    /* } else if (scancode == KEY_PAGEUP || scancode == 0x49) {  PageUp */
        w->term_scroll += visible_lines;
        if (w->term_scroll > max_scroll) w->term_scroll = max_scroll;
    /* } else if (scancode == KEY_PAGEDOWN || scancode == 0x51) {  PageDown */
        if (w->term_scroll > visible_lines) w->term_scroll -= visible_lines;
        else w->term_scroll = 0;
    /* } else if (scancode == KEY_HOME || scancode == 0x47) {  Home */
        w->term_scroll = max_scroll;
    /* } else if (scancode == KEY_END || scancode == 0x4F) {  End */
        w->term_scroll = 0;
    /* } else if (scancode == 0x48) {  Up - history */
        if (w->term_hist_count > 0) {
            if (w->term_hist_pos < w->term_hist_count-1) w->term_hist_pos++;
            int hi = w->term_hist_count - 1 - w->term_hist_pos;
            scopy(w->term_input, w->term_history[hi], TERM_LINE_LEN-1);
        }
    /* } else if (scancode == 0x50) {  Down - history */
        if (w->term_hist_pos > 0) {
            w->term_hist_pos--;
            int hi = w->term_hist_count - 1 - w->term_hist_pos;
            scopy(w->term_input, w->term_history[hi], TERM_LINE_LEN-1);
        } else {
            w->term_hist_pos = -1;
            w->term_input[0] = 0;
        }
    } else if (scancode == KEY_TAB || ascii == '	') {
        char prefix[TERM_LINE_LEN];
        int pi = 0;
        while (w->term_input[pi] && w->term_input[pi] != ' ' && pi < TERM_LINE_LEN - 1) {
            prefix[pi] = w->term_input[pi];
            pi++;
        }
        prefix[pi] = '\0';
        char completed[TERM_LINE_LEN];
        if (shell_complete_command(prefix, completed, sizeof(completed)) > 0 && strcmp(completed, prefix) != 0) {
            char suffix[TERM_LINE_LEN];
            scopy(suffix, w->term_input + pi, TERM_LINE_LEN - 1);
            scopy(w->term_input, completed, TERM_LINE_LEN - 1);
            scat(w->term_input, suffix, TERM_LINE_LEN - 1);
        }
    /* } else if (scancode == 0x4B) {  Ctrl+C equivalent - clear input */
        if (ilen == 0) {
            term_append_line(w, "^C");
        }
        w->term_input[0] = 0;
    }
}


