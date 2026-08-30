/*
 * http_downloader_gui.c - C-OS HTTP Downloader
 */

#include "gui.h"
#include "vga.h"
#include "../../kernel/drivers/http.h"
#include "../../fs/fs.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

extern window_t windows[MAX_WINDOWS];
extern int window_count;
extern int active_window;

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

#define DL_BG              rgb(26, 30, 38)
#define DL_PANEL           rgb(36, 41, 50)
#define DL_HEADER          rgb(52, 72, 102)
#define DL_FIELD_BG        rgb(18, 22, 28)
#define DL_FIELD_BORDER    rgb(90, 120, 170)
#define DL_BUTTON          rgb(44, 60, 82)
#define DL_BUTTON_HOT      rgb(72, 100, 138)
#define DL_LOG_BG          rgb(14, 18, 22)
#define DL_STATUS_BG       rgb(24, 28, 34)
#define DL_TEXT            rgb(235, 238, 242)
#define DL_MUTED           rgb(158, 166, 178)

#define DL_PAD             12
#define DL_FIELD_H         28
#define DL_BUTTON_H        28
#define DL_BUTTON_W        112
#define DL_LOG_LINE_H      (FONT_H + 4)

static bool dl_is_visible(window_t* w) {
    return w && w->kind == WIN_HTTP_DOWNLOADER && w->visible;
}

static window_t* dl_window(int idx) {
    if (idx < 0 || idx >= MAX_WINDOWS) return NULL;
    return dl_is_visible(&windows[idx]) ? &windows[idx] : NULL;
}

static void dl_set_status(window_t* w, const char* msg) {
    if (!w) return;
    strncpy(w->browser_title, msg ? msg : "Ready", sizeof(w->browser_title) - 1);
    w->browser_title[sizeof(w->browser_title) - 1] = '\0';
}

static void dl_log_clear(window_t* w) {
    if (!w) return;
    w->text_buf[0] = '\0';
    w->browser_scroll = 0;
}

static void dl_log_append(window_t* w, const char* msg) {
    if (!w || !msg || !msg[0]) return;
    size_t used = strlen(w->text_buf);
    if (used > 0 && used + 1 < sizeof(w->text_buf)) {
        w->text_buf[used++] = '\n';
        w->text_buf[used] = '\0';
    }
    size_t remain = sizeof(w->text_buf) - used - 1;
    if (remain > 0) {
        strncat(w->text_buf, msg, remain);
    }
    w->browser_scroll = 9999;
}

static void dl_appendf(window_t* w, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dl_log_append(w, buf);
}

static bool dl_is_url_like(const char* s) {
    return s && (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0);
}

static void dl_normalize_url(const char* raw, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!raw || !raw[0]) {
        strncpy(out, "http://example.com/", out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    if (dl_is_url_like(raw)) {
        strncpy(out, raw, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    snprintf(out, out_sz, "http://%s", raw);
}

static void dl_default_name_from_url(const char* url, char* name, size_t name_sz) {
    if (!name || name_sz == 0) return;
    strncpy(name, "download.bin", name_sz - 1);
    name[name_sz - 1] = '\0';
    if (!url || !url[0]) return;

    const char* slash = strrchr(url, '/');
    const char* start = slash ? slash + 1 : url;
    size_t n = 0;
    while (start[n] && start[n] != '?' && start[n] != '#') n++;
    if (n == 0) return;
    if (n >= name_sz) n = name_sz - 1;
    memcpy(name, start, n);
    name[n] = '\0';
    if (!strchr(name, '.')) {
        size_t l = strlen(name);
        if (l + 3 < name_sz) strcat(name, ".bin");
    }
}

static void dl_split_path(const char* full, char* parent, size_t parent_sz, char* name, size_t name_sz) {
    if (!parent || !name || parent_sz == 0 || name_sz == 0) return;
    strncpy(parent, "/", parent_sz - 1);
    parent[parent_sz - 1] = '\0';
    strncpy(name, "download.bin", name_sz - 1);
    name[name_sz - 1] = '\0';
    if (!full || !full[0]) return;

    const char* slash = strrchr(full, '/');
    if (!slash) {
        strncpy(name, full, name_sz - 1);
        name[name_sz - 1] = '\0';
        return;
    }

    size_t parent_len = (size_t)(slash - full);
    if (parent_len > 0) {
        if (parent_len >= parent_sz) parent_len = parent_sz - 1;
        memcpy(parent, full, parent_len);
        parent[parent_len] = '\0';
    }
    strncpy(name, slash + 1, name_sz - 1);
    name[name_sz - 1] = '\0';
    if (name[0] == '\0') strncpy(name, "download.bin", name_sz - 1);
}

static int dl_field_len(const char* s) {
    return s ? (int)strlen(s) : 0;
}
static const char* dl_guess_download_dir(char* out, size_t out_sz) {
    if (!out || out_sz == 0) return "/";
    out[0] = '\0';
    if (active_window >= 0 && active_window < window_count) {
        window_t* w = &windows[active_window];
        if (w->visible && w->kind == WIN_FILE_MGR && w->fm_path[0]) {
            strncpy(out, w->fm_path, out_sz - 1);
            out[out_sz - 1] = '\0';
            return out;
        }
    }
    for (int i = 0; i < window_count; ++i) {
        window_t* w = &windows[i];
        if (w->visible && w->kind == WIN_FILE_MGR && w->fm_path[0]) {
            strncpy(out, w->fm_path, out_sz - 1);
            out[out_sz - 1] = '\0';
            return out;
        }
    }
    strncpy(out, "/desktop", out_sz - 1);
    out[out_sz - 1] = '\0';
    return out;
}


static void dl_insert_char(window_t* w, int field, char c) {
    char* buf = (field == 1) ? w->browser_url : w->browser_search_text;
    int* cursor = (field == 1) ? &w->browser_url_cursor : &w->browser_search_cursor;
    size_t cap = (field == 1) ? sizeof(w->browser_url) : sizeof(w->browser_search_text);
    int len = (int)strlen(buf);
    if (*cursor < 0) *cursor = 0;
    if (*cursor > len) *cursor = len;
    if (len + 1 >= (int)cap) return;
    memmove(&buf[*cursor + 1], &buf[*cursor], (size_t)(len - *cursor + 1));
    buf[*cursor] = c;
    (*cursor)++;
}

static void dl_backspace(window_t* w, int field) {
    char* buf = (field == 1) ? w->browser_url : w->browser_search_text;
    int* cursor = (field == 1) ? &w->browser_url_cursor : &w->browser_search_cursor;
    int len = (int)strlen(buf);
    if (*cursor > len) *cursor = len;
    if (*cursor > 0) {
        memmove(&buf[*cursor - 1], &buf[*cursor], (size_t)(len - *cursor + 1));
        (*cursor)--;
    }
}

static void dl_paste(window_t* w) {
    const char* clip = gui_clipboard_get_text();
    if (!clip || !clip[0]) return;
    int field = (w->browser_url_focus == 2) ? 2 : 1;
    for (const char* p = clip; *p; ++p) {
        if (*p == '\r' || *p == '\n') continue;
        dl_insert_char(w, field, *p);
    }
}

static int dl_line_count(const char* text) {
    if (!text || !text[0]) return 0;
    int count = 1;
    for (const char* p = text; *p; ++p) if (*p == '\n') count++;
    return count;
}

static const char* dl_nth_line(const char* text, int n, int* out_len) {
    if (!text) return NULL;
    const char* p = text;
    for (int i = 0; i < n && *p; ++i) {
        const char* nl = strchr(p, '\n');
        if (!nl) return NULL;
        p = nl + 1;
    }
    if (!*p) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    const char* nl = strchr(p, '\n');
    int len = nl ? (int)(nl - p) : (int)strlen(p);
    if (out_len) *out_len = len;
    return p;
}

static void dl_draw_field(int x, int y, int w, int h, const char* label, const char* text, bool active, int cursor_pos) {
    vga_fill_rect(x, y, w, h, DL_FIELD_BG);
    vga_draw_rect(x, y, w, h, active ? DL_FIELD_BORDER : rgb(70, 78, 88));
    vga_draw_string(x + 10, y + 6, label, DL_MUTED, DL_FIELD_BG);
    if (text && text[0]) {
        char disp[512];
        strncpy(disp, text, sizeof(disp) - 1);
        disp[sizeof(disp) - 1] = '\0';
        int max_chars = (w - 120) / FONT_W;
        if (max_chars < 4) max_chars = 4;
        int len = (int)strlen(disp);
        int offset = 0;
        if (len > max_chars) {
            offset = len - max_chars + 1;
            memmove(disp, &disp[offset], (size_t)(max_chars - 1));
            disp[0] = '~';
            disp[max_chars - 1] = '\0';
        }
        vga_draw_string(x + 110, y + 6, disp, DL_TEXT, DL_FIELD_BG);
        if (active) {
            int rel = cursor_pos - offset;
            if (rel < 0) rel = 0;
            if (rel > max_chars) rel = max_chars;
            int caret_x = x + 110 + rel * FONT_W;
            vga_draw_line(caret_x, y + 5, caret_x, y + h - 6, DL_TEXT);
        }
    } else if (active) {
        vga_draw_line(x + 110, y + 5, x + 110, y + h - 6, DL_TEXT);
    }
}

static bool dl_button_hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void dl_do_download(window_t* w) {
    if (!w) return;
    const char* raw_url = w->browser_url;
    char url[HTTP_MAX_URL];
    dl_normalize_url(raw_url, url, sizeof(url));

    http_client_t* http = http_create();
    if (!http) {
        dl_set_status(w, "HTTP init failed");
        dl_log_append(w, "Unable to initialize HTTP client.");
        return;
    }

    dl_set_status(w, "Downloading...");
    dl_appendf(w, "GET %s", url);

    int status = 0;
    int ret = http_get(http, url);
    status = http_status_code(http);

    if (ret != 0 || status != HTTP_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Download failed (HTTP %d)", status);
        dl_set_status(w, msg);
        dl_log_append(w, msg);
        http_destroy(http);
        return;
    }

    const char* body = http_response_body(http);
    uint64_t len = http_response_length(http);
    const char* content_type = http_get_header(http, "Content-Type");
    bool is_html = content_type &&
        (strncmp(content_type, "text/html", 9) == 0 ||
         strncmp(content_type, "application/xhtml+xml", 21) == 0);
    if (!body || len == 0) {
        dl_set_status(w, "Empty response");
        dl_log_append(w, "Response was empty.");
        http_destroy(http);
        return;
    }

    char save_full[FS_MAX_PATH];
    char save_parent[FS_MAX_PATH];
    char save_name[FS_MAX_NAME];
    char target_dir[FS_MAX_PATH];
    if (w->browser_search_text[0]) {
        if (w->browser_search_text[0] == '/' || strstr(w->browser_search_text, "://")) {
            strncpy(save_full, w->browser_search_text, sizeof(save_full) - 1);
            save_full[sizeof(save_full) - 1] = '\0';
        } else {
            dl_guess_download_dir(target_dir, sizeof(target_dir));
            snprintf(save_full, sizeof(save_full), "%s/%s", target_dir, w->browser_search_text);
        }
    } else {
        dl_guess_download_dir(target_dir, sizeof(target_dir));
        char base[FS_MAX_NAME];
        dl_default_name_from_url(url, base, sizeof(base));
        /* A URL ending at '/' has no filename. Persist a detected HTML
         * document with a browser-recognized extension so NetSurf's local
         * file handler executes its markup and JavaScript offline. */
        if (is_html && strcmp(base, "download.bin") == 0) {
            strncpy(base, "offline.html", sizeof(base) - 1);
            base[sizeof(base) - 1] = '\0';
        }
        snprintf(save_full, sizeof(save_full), "%s/%s", target_dir, base);
    }
    dl_split_path(save_full, save_parent, sizeof(save_parent), save_name, sizeof(save_name));

    bool ok = fs_write_file_at(save_parent, save_name, body, len);
    if (!ok) {
        if (fs_create_file_at(save_parent, save_name)) {
            ok = fs_write_file_at(save_parent, save_name, body, len);
        }
    }

    if (!ok) {
        dl_set_status(w, "Save failed");
        dl_appendf(w, "Failed to save to %s/%s", save_parent, save_name);
        http_destroy(http);
        return;
    }

    char msg[192];
    snprintf(msg, sizeof(msg), "Saved %llu bytes to %s/%s", (unsigned long long)len, save_parent, save_name);
    dl_set_status(w, "Download complete");
    dl_log_append(w, msg);
    gui_refresh_file_managers_for_path(save_full);
    if (is_html) {
        dl_log_append(w, "Opening saved HTML in NetSurf offline mode.");
        /* gui_open_file_in_app routes .html through browser_commit_navigation,
         * which normalizes the local path to file:// and loads it via NetSurf
         * rather than showing it as plain text. */
        gui_open_file_in_app(save_full, 0);
    }
    gui_request_redraw();
    http_destroy(http);
}

void http_downloader_init(window_t* w) {
    if (!w) return;
    if (w->browser_url[0] == '\0') {
        strncpy(w->browser_url, "http://example.com/", sizeof(w->browser_url) - 1);
        w->browser_url[sizeof(w->browser_url) - 1] = '\0';
    }
    if (w->browser_search_text[0] == '\0') {
        strncpy(w->browser_search_text, "download.bin", sizeof(w->browser_search_text) - 1);
        w->browser_search_text[sizeof(w->browser_search_text) - 1] = '\0';
    }
    w->browser_url_focus = 1;
    w->browser_search_focus = 0;
    w->browser_url_cursor = (int)strlen(w->browser_url);
    w->browser_search_cursor = (int)strlen(w->browser_search_text);
    w->browser_url_selected = false;
    w->browser_search_selected = false;
    w->browser_scroll = 0;
    dl_log_clear(w);
    dl_set_status(w, "Ready");
    dl_log_append(w, "HTTP Downloader ready.");
}

static void dl_draw_log(window_t* w, int x, int y, int wdt, int h) {
    vga_fill_rect(x, y, wdt, h, DL_LOG_BG);
    vga_draw_rect(x, y, wdt, h, rgb(70, 78, 88));
    vga_draw_string(x + 10, y + 6, "Log", DL_MUTED, DL_LOG_BG);

    int line_count = dl_line_count(w->text_buf);
    int visible = (h - (FONT_H + 10)) / DL_LOG_LINE_H;
    if (visible < 1) visible = 1;
    int max_scroll = line_count > visible ? line_count - visible : 0;
    if (w->browser_scroll < 0) w->browser_scroll = 0;
    if (w->browser_scroll > max_scroll) w->browser_scroll = max_scroll;

    int start = w->browser_scroll;
    int end = MIN(line_count, start + visible);
    int dy = y + FONT_H + 10;
    for (int i = start; i < end; ++i) {
        int len = 0;
        const char* line = dl_nth_line(w->text_buf, i, &len);
        if (!line) continue;
        char buf[512];
        if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
        memcpy(buf, line, (size_t)len);
        buf[len] = '\0';
        vga_draw_string(x + 10, dy, buf, DL_TEXT, DL_LOG_BG);
        dy += DL_LOG_LINE_H;
    }
}

void http_downloader_draw(int idx) {
    window_t* w = dl_window(idx);
    if (!w) return;
    int x = w->x + 2;
    int y = w->y + TITLEBAR_H + 2;
    int ww = w->w - 4;
    int hh = w->h - TITLEBAR_H - 4;
    if (ww < 200 || hh < 150) return;

    vga_fill_rect(x, y, ww, hh, DL_BG);

    int header_h = 28;
    vga_fill_rect(x, y, ww, header_h, DL_HEADER);
    vga_draw_string(x + DL_PAD, y + 6, "HTTP Downloader", DL_TEXT, DL_HEADER);
    vga_draw_string(x + ww - 250, y + 6, w->browser_title, DL_MUTED, DL_HEADER);

    int field_w = ww - DL_PAD * 2;
    int fx = x + DL_PAD;
    int fy = y + header_h + 12;
    dl_draw_field(fx, fy, field_w, DL_FIELD_H, "URL", w->browser_url, w->browser_url_focus == 1, w->browser_url_cursor);
    fy += DL_FIELD_H + 10;
    dl_draw_field(fx, fy, field_w, DL_FIELD_H, "Save as", w->browser_search_text, w->browser_url_focus == 2, w->browser_search_cursor);
    fy += DL_FIELD_H + 12;

    int bx = fx;
    int by = fy;
    const char* labels[] = { "Download", "Paste", "Clear" };
    for (int i = 0; i < 3; ++i) {
        int bw = (i == 0) ? 120 : 100;
        int hot = 0;
        vga_fill_rect(bx, by, bw, DL_BUTTON_H, hot ? DL_BUTTON_HOT : DL_BUTTON);
        vga_draw_rect(bx, by, bw, DL_BUTTON_H, rgb(96, 114, 138));
        int tw = (int)strlen(labels[i]) * FONT_W;
        vga_draw_string(bx + (bw - tw) / 2, by + 6, labels[i], DL_TEXT, hot ? DL_BUTTON_HOT : DL_BUTTON);
        bx += bw + 10;
    }

    int log_y = by + DL_BUTTON_H + 12;
    int log_h = y + hh - log_y - DL_PAD - 24;
    if (log_h < 80) log_h = 80;
    dl_draw_log(w, fx, log_y, field_w, log_h);

    int status_y = y + hh - 22;
    vga_fill_rect(x, status_y, ww, 22, DL_STATUS_BG);
    vga_draw_rect(x, status_y, ww, 22, rgb(70, 78, 88));
    vga_draw_string(x + 10, status_y + 3, w->browser_title[0] ? w->browser_title : "Ready", DL_TEXT, DL_STATUS_BG);
}

void http_downloader_handle_click(int idx, int mx, int my) {
    window_t* w = dl_window(idx);
    if (!w) return;
    int x = w->x + 2;
    int y = w->y + TITLEBAR_H + 2;
    int ww = w->w - 4;
    int header_h = 28;
    int field_w = ww - DL_PAD * 2;
    int fx = x + DL_PAD;
    int fy = y + header_h + 12;

    if (dl_button_hit(mx, my, fx, fy, field_w, DL_FIELD_H)) {
        w->browser_url_focus = 1;
        w->browser_url_cursor = dl_field_len(w->browser_url);
        return;
    }
    fy += DL_FIELD_H + 10;
    if (dl_button_hit(mx, my, fx, fy, field_w, DL_FIELD_H)) {
        w->browser_url_focus = 2;
        w->browser_search_cursor = dl_field_len(w->browser_search_text);
        return;
    }
    fy += DL_FIELD_H + 12;

    int bx = fx;
    int by = fy;
    if (dl_button_hit(mx, my, bx, by, 120, DL_BUTTON_H)) {
        dl_do_download(w);
        return;
    }
    bx += 120 + 10;
    if (dl_button_hit(mx, my, bx, by, 100, DL_BUTTON_H)) {
        dl_paste(w);
        return;
    }
    bx += 100 + 10;
    if (dl_button_hit(mx, my, bx, by, 100, DL_BUTTON_H)) {
        dl_log_clear(w);
        dl_set_status(w, "Log cleared");
        dl_log_append(w, "Log cleared.");
        return;
    }
}

void http_downloader_handle_key(int idx, char ascii, int scancode, bool ctrl) {
    window_t* w = dl_window(idx);
    if (!w) return;

    if (ctrl) {
        switch (scancode) {
            case KEYBOARD_SCANCODE_V:
                dl_paste(w);
                return;
            case KEYBOARD_SCANCODE_D:
                dl_do_download(w);
                return;
            case KEYBOARD_SCANCODE_L:
                dl_log_clear(w);
                dl_set_status(w, "Log cleared");
                dl_log_append(w, "Log cleared.");
                return;
            default:
                break;
        }
    }

    if (scancode == KEY_TAB) {
        w->browser_url_focus = (w->browser_url_focus == 1) ? 2 : 1;
        return;
    }
    if (scancode == KEY_ENTER) {
        dl_do_download(w);
        return;
    }
    if (scancode == KEY_BACKSPACE) {
        dl_backspace(w, w->browser_url_focus == 2 ? 2 : 1);
        return;
    }
    if (ascii >= 32 && ascii < 127) {
        dl_insert_char(w, w->browser_url_focus == 2 ? 2 : 1, ascii);
        return;
    }
}
