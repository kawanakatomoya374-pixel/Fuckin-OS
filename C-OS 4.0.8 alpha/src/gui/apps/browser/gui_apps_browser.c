/**
 * gui_apps_browser.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * Webブラウザ (NetSurf + QuickJS 完全結合)
 *
 * 元は単一の gui_apps.c (11,638行) に含まれていたコードを、
 * 保守性向上のため機能単位に分割したものの一部。
 *
 * C-OS 4.0.8 alpha: cos_netsurf_load_url_sync_nowait() および
 * cos_netsurf_eval_script() を実際に呼び出すように変更。
 * javascript: URLスキームの直接QuickJS評価も対応。
 */
#include "gui.h"
#include "mk_desktop.h"
#include "system/password_screen.h"
#include "vga.h"
#include "mk_mp3.h"
#include "../../../apps/jpeg_viewer.h"
#include "string.h"
#include "serial.h"
#include "cos_netsurf_browser.h"

#ifndef COS_BROWSER_FILE_SMOKE
#define COS_BROWSER_FILE_SMOKE 0
#endif
/* Validation-only start URL. CI or an explicit regression build can override
 * this string literal with a public HTTPS URL without embedding page-specific
 * browser behavior. Normal validation remains the local file smoke path. */
#ifndef COS_BROWSER_SMOKE_START_URL
#define COS_BROWSER_SMOKE_START_URL "file:///browser/index.html"
#endif

/* Implemented by the NetSurf engine; kept as a narrow declaration here so
 * the GUI does not depend on the retired DOM-to-lines renderer header. */
void cos_netsurf_eval_script(const char *script);
void cos_netsurf_load_url_sync_nowait(const char *url_string);
extern void gui_request_redraw(void);

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
 * BROWSER
 * ============================================================ */

#define BROWSER_MAX_URL_LEN        512
#define BROWSER_MAX_TITLE_LEN      128
#define BROWSER_MAX_CONTENT_LINES  512
#define BROWSER_MAX_LINE_LEN       160
#define BROWSER_MAX_LINKS          64
#define BROWSER_MAX_HISTORY        32
#define BROWSER_WRAP_CHARS         72

typedef enum {
    BROWSER_LINE_BLANK = 0,
    BROWSER_LINE_TEXT,
    BROWSER_LINE_HEADING,
    BROWSER_LINE_SUBHEADING,
    BROWSER_LINE_LINK,
    BROWSER_LINE_PRE,
    BROWSER_LINE_ERROR,
} browser_line_kind_t;

typedef enum {
    BROWSER_APP_BOOTING = 0,
    BROWSER_APP_INPUT,
    BROWSER_APP_LOADING,
    BROWSER_APP_DISPLAY,
    BROWSER_APP_ERROR,
} browser_app_state_t;

typedef struct {
    browser_line_kind_t kind;
    int link_index;
    int height;
    char text[BROWSER_MAX_LINE_LEN];
} browser_line_t;

typedef struct {
    char url[BROWSER_MAX_URL_LEN];
    char label[BROWSER_MAX_LINE_LEN];
    int x, y, w, h;
        int kind; /* 0=link, 1=form/button */
    char method[8];
    char data[BROWSER_MAX_URL_LEN];
    char enctype[32];
} browser_link_t;

typedef struct {
    int loaded;
    browser_app_state_t ui_state;
    char current_url[BROWSER_MAX_URL_LEN];
    char title[BROWSER_MAX_TITLE_LEN];
    char status[128];
    browser_line_t lines[BROWSER_MAX_CONTENT_LINES];
    int line_count;
    browser_link_t links[BROWSER_MAX_LINKS];
    int link_count;
    char history_urls[BROWSER_MAX_HISTORY][BROWSER_MAX_URL_LEN];
    char history_titles[BROWSER_MAX_HISTORY][BROWSER_MAX_TITLE_LEN];
    int history_count;
    int history_pos;
    int loading_progress;
} browser_state_t;

static browser_state_t g_browser;
/* True when the visible document is painted by NetSurf's standard
 * browser_window -> html_redraw -> plotter pipeline, not the legacy
 * DOM-line compatibility renderer. */
static bool g_browser_standard_renderer;

/* The desktop compositor redraws window chrome for mouse movement and other
 * applications.  Keep a dedicated copy of the already-plotted NetSurf body
 * so a static page can return through one BitBlt rather than re-running the
 * complete box-tree traversal and glyph plotting on every such composition.
 * The cache is keyed by the frontend's invalidation generation, never by an
 * assumed time interval, so page updates, caret changes and scrolling remain
 * immediately visible. */
static uint32_t* g_browser_netsurf_cache;
static int g_browser_netsurf_cache_w;
static int g_browser_netsurf_cache_h;
static uint32_t g_browser_netsurf_cache_generation;
static bool g_browser_netsurf_cache_valid;

static void browser_set_error(const char* msg, const char* url);

 /* Built-in pages the browser can render locally. */
typedef struct {
    const char* url;
    const char* title;
    const char* lines[24];
} builtin_page_t;

static const builtin_page_t builtin_pages[] = {
    {
        "c-os://home", "Home", {
        "[H] Home",
        NULL
    }
    },
    {"c-os://about", "About C-OS 4.0.8 alpha", {
        "[H] About C-OS 4.0.8 alpha",
        "",
        "[P] A lightweight operating system demo built in C.",
        "[H] C-OS 4.0.8 alpha",
        "[P] The browser renders readable text and lets you open local files.",
        NULL
    }},
    {"c-os://help", "Browser Help", {
        "[H] Browser Help",
        "",
        "[P] Type a path like /readme.txt or file:///readme.txt.",
        "[P] Type a full HTTP URL to use the network, including forms and buttons on the page.",
        "[P] Gemini and Gopher URLs are supported in the remote text browser mode.",
        "[P] Open a directory to see clickable entries.",
        "[P] Use Back, Forward, Reload, Home, and the address bar to navigate.",
        NULL
    }},
    {"c-os://files", "Local Files", {
        "[H] Local Files",
        "",
        "[P] File paths are browsed through the kernel filesystem.",
        "[P] Click a directory to open it, or click a file to view it.",
        "[P] Common sample files:",
        "[L] file:///welcome.txt welcome.txt",
        "[L] file:///readme.txt readme.txt",
        NULL
    }},
    {"c-os://storage", "Storage", {
        "[H] Storage",
        "",
        "[P] The browser can inspect the local RAM filesystem.",
        "[P] Data stored there is available until the OS reboots.",
        NULL
    }},
    {"c-os://remote", "Remote Text Browser", {
        "[H] Remote Text Browser",
        "",
        "[P] This mode keeps the OS side lightweight.",
        "[P] Use markdown pages, gopher menus, and Gemini text pages.",
        "[L] gopher://gopher.floodgap.com Try Gopher",
        "[L] c-os://help Browser help",
        NULL
    }},
    {NULL, NULL, {NULL}}
};

static const builtin_page_t* browser_find_builtin(const char* url) {
    for (int i = 0; builtin_pages[i].url; i++) {
        if (smatch(builtin_pages[i].url, url)) return &builtin_pages[i];
    }
    return NULL;
}

static void browser_reset_page(void) {
    g_browser.line_count = 0;
    g_browser.link_count = 0;
    g_browser.status[0] = '\0';
}

static int browser_line_height(browser_line_kind_t kind) {
    switch (kind) {
        case BROWSER_LINE_BLANK:      return 10;
        case BROWSER_LINE_HEADING:    return 28;
        case BROWSER_LINE_SUBHEADING: return 22;
        case BROWSER_LINE_LINK:       return 24;
        case BROWSER_LINE_PRE:        return 18;
        case BROWSER_LINE_ERROR:      return 18;
        case BROWSER_LINE_TEXT:
        default:                      return 18;
    }
}

static void browser_add_line(browser_line_kind_t kind, const char* text, int link_index) {
    if (g_browser.line_count >= BROWSER_MAX_CONTENT_LINES) return;
    browser_line_t* line = &g_browser.lines[g_browser.line_count++];
    line->kind = kind;
    line->link_index = link_index;
    line->height = browser_line_height(kind);
    if (text && text[0]) {
        scopy(line->text, text, BROWSER_MAX_LINE_LEN - 1);
    } else {
        line->text[0] = '\0';
    }
}

static int browser_add_link(const char* url, const char* label) {
    if (g_browser.link_count >= BROWSER_MAX_LINKS) return -1;
    browser_link_t* link = &g_browser.links[g_browser.link_count];
    link->url[0] = '\0';
    link->label[0] = '\0';
    link->x = link->y = link->w = link->h = 0;
    link->kind = 0;
    link->method[0] = '\0';
    link->data[0] = '\0';
    link->enctype[0] = '\0';
    if (url && url[0]) scopy(link->url, url, BROWSER_MAX_URL_LEN - 1);
    if (label && label[0]) scopy(link->label, label, BROWSER_MAX_LINE_LEN - 1);
    else if (url && url[0]) scopy(link->label, url, BROWSER_MAX_LINE_LEN - 1);
    return g_browser.link_count++;
}

static void browser_add_form_button(const char* action, const char* method, const char* label, const char* data, const char* enctype) {
    int idx = browser_add_link(action && action[0] ? action : g_browser.current_url,
                               label && label[0] ? label : "Submit");
    if (idx < 0) return;
    browser_link_t* link = &g_browser.links[idx];
    link->kind = 1;
    if (method && method[0]) scopy(link->method, method, sizeof(link->method) - 1);
    else scopy(link->method, "GET", sizeof(link->method) - 1);
    if (data && data[0]) scopy(link->data, data, sizeof(link->data) - 1);
    else link->data[0] = '\0';
    if (enctype && enctype[0]) scopy(link->enctype, enctype, sizeof(link->enctype) - 1);
    else scopy(link->enctype, "application/x-www-form-urlencoded", sizeof(link->enctype) - 1);
}


static int browser_ci_equal(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    return a == b;
}

static int browser_ci_starts_with(const char* text, const char* prefix) {
    if (!text || !prefix) return 0;
    while (*prefix) {
        if (*text == '\0' || !browser_ci_equal(*text, *prefix)) return 0;
        ++text;
        ++prefix;
    }
    return 1;
}

static int browser_tag_name_matches(const char* tag, const char* name) {
    if (!tag || !name) return 0;
    while (*tag && *name) {
        char a = *tag++;
        char b = *name++;
        if (!browser_ci_equal(a, b)) return 0;
    }
    return *name == '\0';
}

static void browser_copy_attr_value_ci(const char* tag, const char* attr, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!tag || !attr || !attr[0]) return;

    size_t attr_len = strlen(attr);
    const char* p = tag;
    while (*p) {
        const char* s = p;
        size_t i = 0;
        while (s[i] && i < attr_len && browser_ci_equal(s[i], attr[i])) i++;
        if (i == attr_len) {
            const char* q = s + attr_len;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            if (*q != '=') {
                p++;
                continue;
            }
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            char quote = 0;
            if (*q == '"' || *q == '\'') quote = *q++;
            size_t o = 0;
            while (*q && o + 1 < out_size) {
                if (quote) {
                    if (*q == quote) break;
                } else if (*q == '>' || *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') {
                    break;
                }
                out[o++] = *q++;
            }
            out[o] = '\0';
            return;
        }
        p++;
    }
}

static void browser_copy_attr_value(const char* tag, const char* attr, char* out, size_t out_size) {
    browser_copy_attr_value_ci(tag, attr, out, out_size);
}

static int browser_attr_is_truthy(const char* tag, const char* attr) {
    char value[32];
    browser_copy_attr_value_ci(tag, attr, value, sizeof(value));
    if (!value[0]) return 0;
    return !(smatch(value, "0") || smatch(value, "false") || smatch(value, "off") || smatch(value, "no"));
}

static void browser_url_encode_component(const char* src, char* dst, size_t dst_sz, int plus_for_space) {
    static const char hex[] = "0123456789ABCDEF";
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dst_sz; ++i) {
        unsigned char c = (unsigned char)src[i];
        int safe = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~');
        if (c == ' ' && plus_for_space) {
            dst[o++] = '+';
        } else if (safe) {
            dst[o++] = (char)c;
        } else {
            if (o + 3 >= dst_sz) break;
            dst[o++] = '%';
            dst[o++] = hex[(c >> 4) & 0xF];
            dst[o++] = hex[c & 0xF];
        }
    }
    dst[o] = '\0';
}

/* Google search URL generation is provided by the shared C++ browser core.
 * This C frontend supplies the input state and routes the resulting HTTPS
 * request through the vendored NetSurf 3.11 content pipeline. */
static int browser_is_search_query(const char* text) {
    if (!text || !text[0]) return 0;
    if (sstartswith(text, "http://") || sstartswith(text, "https://") ||
        sstartswith(text, "file://") || sstartswith(text, "data:") ||
        sstartswith(text, "javascript:") || sstartswith(text, "c-os://") ||
        sstartswith(text, "about:")) return 0;
    if (text[0] == '/' || text[0] == '.') return 0;
    for (size_t i = 0; text[i]; ++i) {
        if (text[i] == ' ' || text[i] == '\t') return 1;
    }
    return strchr(text, '.') == NULL && strchr(text, '/') == NULL;
}

static void browser_form_append_pair(char* out, size_t out_sz, const char* name, const char* value) {
    if (!out || out_sz == 0 || !name || !name[0]) return;
    char enc_name[192];
    char enc_value[384];
    browser_url_encode_component(name, enc_name, sizeof(enc_name), 1);
    browser_url_encode_component(value ? value : "", enc_value, sizeof(enc_value), 1);
    if (out[0]) scat(out, "&", out_sz - 1);
    scat(out, enc_name, out_sz - 1);
    scat(out, "=", out_sz - 1);
    scat(out, enc_value, out_sz - 1);
}

static void browser_form_join_url(const char* base, const char* query, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!base || !base[0]) return;
    scopy(out, base, out_sz - 1);
    if (!query || !query[0]) return;
    if (strchr(out, '?')) scat(out, "&", out_sz - 1);
    else scat(out, "?", out_sz - 1);
    scat(out, query, out_sz - 1);
}

static void browser_form_display_label(const char* type, const char* name, const char* value, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!type || !type[0]) type = "input";
    if (!name || !name[0]) name = "(unnamed)";
    scopy(out, "[", out_sz - 1);
    scat(out, type, out_sz - 1);
    scat(out, "] ", out_sz - 1);
    scat(out, name, out_sz - 1);
    if (value && value[0]) {
        scat(out, " = ", out_sz - 1);
        scat(out, value, out_sz - 1);
    }
}

static void browser_add_form_control_line(const char* kind, const char* name, const char* value) {
    char line[BROWSER_MAX_LINE_LEN];
    browser_form_display_label(kind, name, value, line, sizeof(line));
    if (line[0]) browser_add_line(BROWSER_LINE_TEXT, line, -1);
}

static void browser_add_image_placeholder(const char* src, const char* alt) {
    char line[BROWSER_MAX_LINE_LEN];
    line[0] = '\0';
    scopy(line, "[Image] ", sizeof(line) - 1);
    if (alt && alt[0]) {
        scat(line, alt, sizeof(line) - 1);
    } else if (src && src[0]) {
        scat(line, src, sizeof(line) - 1);
    } else {
        scat(line, "(no description)", sizeof(line) - 1);
    }
    browser_add_line(BROWSER_LINE_TEXT, line, -1);
}

static int browser_is_name_char(char c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_');
}


static void browser_extract_inner_text(const char* start, const char* end, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!start) return;
    if (!end) end = start + strlen(start);

    size_t i = 0;
    int in_tag = 0;
    int last_space = 1;
    for (const char* p = start; p < end && *p && i + 1 < out_size; ++p) {
        char c = *p;
        if (c == '<') { in_tag = 1; continue; }
        if (c == '>') { in_tag = 0; continue; }
        if (in_tag) continue;
        if (c == '&') {
            if (p + 3 < end && strncmp(p, "&lt;", 4) == 0) { c = '<'; p += 3; }
            else if (p + 3 < end && strncmp(p, "&gt;", 4) == 0) { c = '>'; p += 3; }
            else if (p + 4 < end && strncmp(p, "&amp;", 5) == 0) { c = '&'; p += 4; }
            else if (p + 5 < end && strncmp(p, "&quot;", 6) == 0) { c = '"'; p += 5; }
        }
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!last_space && i > 0) out[i++] = ' ';
            last_space = 1;
        } else {
            out[i++] = c;
            last_space = 0;
        }
    }
    out[i] = '\0';
}

static void browser_scan_form_buttons(const char* html, const char* base_url) {
    if (!html) return;
    const char* p = html;
    while ((p = strstr(p, "<form")) != NULL && g_browser.link_count < BROWSER_MAX_LINKS) {
        const char* open_end = strchr(p, '>');
        if (!open_end) break;

        char action[BROWSER_MAX_URL_LEN];
        char method[16];
        char enctype[64];
        char query[BROWSER_MAX_URL_LEN];
        char summary[BROWSER_MAX_LINE_LEN];
        action[0] = '\0';
        method[0] = '\0';
        enctype[0] = '\0';
        query[0] = '\0';
        summary[0] = '\0';

        browser_copy_attr_value_ci(p, "action", action, sizeof(action));
        browser_copy_attr_value_ci(p, "method", method, sizeof(method));
        browser_copy_attr_value_ci(p, "enctype", enctype, sizeof(enctype));
        if (!method[0]) scopy(method, "GET", sizeof(method) - 1);
        if (!enctype[0]) scopy(enctype, "application/x-www-form-urlencoded", sizeof(enctype) - 1);

        const char* body = open_end + 1;
        const char* close = strstr(body, "</form>");
        const char* end = close ? close : (body + strlen(body));

        if (!action[0] && base_url && base_url[0]) scopy(action, base_url, sizeof(action) - 1);
        if (!action[0]) scopy(action, g_browser.current_url, sizeof(action) - 1);

        char best_submit[64];
        best_submit[0] = '\0';
        int saw_file_input = 0;

        const char* cur = body;
        while (cur < end && g_browser.link_count < BROWSER_MAX_LINKS) {
            const char* tag = strchr(cur, '<');
            if (!tag || tag >= end) break;
            const char* tag_end = strchr(tag, '>');
            if (!tag_end || tag_end > end) break;

            if (browser_ci_starts_with(tag, "<input")) {
                char type[24], name[64], value[256], placeholder[128];
                type[0] = name[0] = value[0] = placeholder[0] = '\0';
                browser_copy_attr_value_ci(tag, "type", type, sizeof(type));
                browser_copy_attr_value_ci(tag, "name", name, sizeof(name));
                browser_copy_attr_value_ci(tag, "value", value, sizeof(value));
                browser_copy_attr_value_ci(tag, "placeholder", placeholder, sizeof(placeholder));
                int checked = browser_attr_is_truthy(tag, "checked");
                int disabled = browser_attr_is_truthy(tag, "disabled");
                if (!disabled && name[0]) {
                    char shown[256];
                    shown[0] = '\0';
                    if (!type[0] || smatch(type, "text") || smatch(type, "search") || smatch(type, "url") ||
                        smatch(type, "email") || smatch(type, "tel") || smatch(type, "number") || smatch(type, "hidden")) {
                        const char* val = value[0] ? value : (placeholder[0] ? placeholder : "");
                        browser_form_append_pair(query, sizeof(query), name, val);
                        if (smatch(type, "hidden")) {
                            browser_form_display_label("Hidden", name, val, shown, sizeof(shown));
                        } else if (smatch(type, "password")) {
                            char masked[16];
                            scopy(masked, "******", sizeof(masked) - 1);
                            browser_form_display_label("Password", name, masked, shown, sizeof(shown));
                        } else {
                            browser_form_display_label("Input", name, val, shown, sizeof(shown));
                        }
                    } else if (smatch(type, "password")) {
                        const char* val = value[0] ? value : "";
                        browser_form_append_pair(query, sizeof(query), name, val);
                        char masked[16];
                        scopy(masked, "******", sizeof(masked) - 1);
                        browser_form_display_label("Password", name, masked, shown, sizeof(shown));
                    } else if (smatch(type, "checkbox") || smatch(type, "radio")) {
                        if (checked) {
                            const char* val = value[0] ? value : "on";
                            browser_form_append_pair(query, sizeof(query), name, val);
                            browser_form_display_label(type, name, val, shown, sizeof(shown));
                        } else {
                            browser_form_display_label(type, name, "unchecked", shown, sizeof(shown));
                        }
                    } else if (smatch(type, "file")) {
                        saw_file_input = 1;
                        browser_form_display_label("File", name, "[upload]", shown, sizeof(shown));
                    } else if (smatch(type, "submit") || smatch(type, "button") || smatch(type, "image")) {
                        browser_copy_attr_value_ci(tag, "value", best_submit, sizeof(best_submit));
                        if (!best_submit[0]) scopy(best_submit, "Submit", sizeof(best_submit) - 1);
                    } else {
                        const char* val = value[0] ? value : "";
                        browser_form_append_pair(query, sizeof(query), name, val);
                        browser_form_display_label("Input", name, val, shown, sizeof(shown));
                    }
                    if (shown[0]) browser_add_line(BROWSER_LINE_TEXT, shown, -1);
                }
            } else if (browser_ci_starts_with(tag, "<textarea")) {
                char name[64];
                name[0] = '\0';
                browser_copy_attr_value_ci(tag, "name", name, sizeof(name));
                const char* text_start = tag_end + 1;
                const char* text_close = strstr(text_start, "</textarea>");
                const char* text_end = (text_close && text_close < end) ? text_close : end;
                char value[256];
                browser_extract_inner_text(text_start, text_end, value, sizeof(value));
                if (name[0]) browser_form_append_pair(query, sizeof(query), name, value);
                browser_form_display_label("Textarea", name[0] ? name : "(textarea)", value, summary, sizeof(summary));
                if (summary[0]) browser_add_line(BROWSER_LINE_TEXT, summary, -1);
            } else if (browser_ci_starts_with(tag, "<select")) {
                char name[64];
                name[0] = '\0';
                browser_copy_attr_value_ci(tag, "name", name, sizeof(name));
                const char* select_start = tag_end + 1;
                const char* select_close = strstr(select_start, "</select>");
                const char* select_end = (select_close && select_close < end) ? select_close : end;
                char selected[256];
                selected[0] = '\0';
                const char* opt = select_start;
                while ((opt = strstr(opt, "<option")) != NULL && opt < select_end) {
                    const char* opt_end = strchr(opt, '>');
                    if (!opt_end || opt_end > select_end) break;
                    char opt_value[128];
                    opt_value[0] = '\0';
                    browser_copy_attr_value_ci(opt, "value", opt_value, sizeof(opt_value));
                    int selected_flag = browser_attr_is_truthy(opt, "selected");
                    const char* opt_text_start = opt_end + 1;
                    const char* opt_text_close = strstr(opt_text_start, "</option>");
                    const char* opt_text_end = (opt_text_close && opt_text_close < select_end) ? opt_text_close : select_end;
                    char opt_text[128];
                    browser_extract_inner_text(opt_text_start, opt_text_end, opt_text, sizeof(opt_text));
                    if (!opt_value[0]) scopy(opt_value, opt_text, sizeof(opt_value) - 1);
                    if (!selected[0] || selected_flag) scopy(selected, opt_value[0] ? opt_value : opt_text, sizeof(selected) - 1);
                    if (selected_flag) break;
                    opt = opt_end + 8;
                }
                if (name[0]) browser_form_append_pair(query, sizeof(query), name, selected);
                browser_form_display_label("Select", name[0] ? name : "(select)", selected, summary, sizeof(summary));
                if (summary[0]) browser_add_line(BROWSER_LINE_TEXT, summary, -1);
            } else if (browser_ci_starts_with(tag, "<img")) {
                char alt[128], src[256];
                alt[0] = src[0] = '\0';
                browser_copy_attr_value_ci(tag, "alt", alt, sizeof(alt));
                browser_copy_attr_value_ci(tag, "src", src, sizeof(src));
                browser_add_image_placeholder(src, alt);
            }

            cur = tag_end + 1;
        }

        if (!best_submit[0]) scopy(best_submit, "Submit", sizeof(best_submit) - 1);
        if (query[0]) {
            char final_kind[64];
            if (smatch(method, "POST")) {
                scopy(final_kind, "POST", sizeof(final_kind) - 1);
            } else {
                scopy(final_kind, "GET", sizeof(final_kind) - 1);
            }
            char line[BROWSER_MAX_LINE_LEN];
            scopy(line, "[Form] ", sizeof(line) - 1);
            scat(line, final_kind, sizeof(line) - 1);
            scat(line, " ", sizeof(line) - 1);
            scat(line, action, sizeof(line) - 1);
            if (saw_file_input) scat(line, " (file upload)", sizeof(line) - 1);
            browser_add_line(BROWSER_LINE_SUBHEADING, line, -1);
        }

        if (!action[0] && base_url && base_url[0]) scopy(action, base_url, sizeof(action) - 1);
        if (saw_file_input && smatch(enctype, "multipart/form-data")) {
            scopy(best_submit, "Submit (upload)", sizeof(best_submit) - 1);
        }
        browser_add_form_button(action, method, best_submit, query, enctype);
        browser_add_line(BROWSER_LINE_LINK, best_submit, g_browser.link_count - 1);
        p = close ? close + 7 : open_end + 1;
    }
}




static void browser_scan_interactive_elements(const char* html) {
    if (!html) return;
    const char* p = html;
    while ((p = strstr(p, "<input")) != NULL && g_browser.line_count < BROWSER_MAX_CONTENT_LINES) {
        const char* tag_end = strchr(p, '>');
        if (!tag_end) break;
        char type[24], name[64], value[128];
        type[0] = name[0] = value[0] = '\0';
        browser_copy_attr_value_ci(p, "type", type, sizeof(type));
        browser_copy_attr_value_ci(p, "name", name, sizeof(name));
        browser_copy_attr_value_ci(p, "value", value, sizeof(value));
        if (name[0]) {
            char line[BROWSER_MAX_LINE_LEN];
            if (!type[0]) scopy(type, "text", sizeof(type) - 1);
            if (smatch(type, "password")) {
                scopy(line, "[Password] ", sizeof(line) - 1);
                scat(line, name, sizeof(line) - 1);
                if (value[0]) scat(line, " = ******", sizeof(line) - 1);
            } else if (smatch(type, "checkbox") || smatch(type, "radio")) {
                scopy(line, "[Choice] ", sizeof(line) - 1);
                scat(line, name, sizeof(line) - 1);
                if (browser_attr_is_truthy(p, "checked")) scat(line, " (checked)", sizeof(line) - 1);
                else scat(line, " (unchecked)", sizeof(line) - 1);
            } else if (smatch(type, "file")) {
                scopy(line, "[File] ", sizeof(line) - 1);
                scat(line, name, sizeof(line) - 1);
                scat(line, " (upload)", sizeof(line) - 1);
            } else if (smatch(type, "submit") || smatch(type, "button")) {
                scopy(line, "[Button] ", sizeof(line) - 1);
                scat(line, value[0] ? value : name, sizeof(line) - 1);
            } else {
                scopy(line, "[Input] ", sizeof(line) - 1);
                scat(line, name, sizeof(line) - 1);
                if (value[0]) {
                    scat(line, " = ", sizeof(line) - 1);
                    if (smatch(type, "password")) {
                        scat(line, "******", sizeof(line) - 1);
                    } else {
                        scat(line, value, sizeof(line) - 1);
                    }
                }
            }
            browser_add_line(BROWSER_LINE_TEXT, line, -1);
        }
        p = tag_end + 1;
    }

    p = html;
    while ((p = strstr(p, "<select")) != NULL && g_browser.line_count < BROWSER_MAX_CONTENT_LINES) {
        const char* tag_end = strchr(p, '>');
        if (!tag_end) break;
        char name[64];
        name[0] = '\0';
        browser_copy_attr_value_ci(p, "name", name, sizeof(name));
        const char* start = tag_end + 1;
        const char* close = strstr(start, "</select>");
        const char* end = close ? close : start + strlen(start);
        char selected[128];
        selected[0] = '\0';
        const char* opt = start;
        while ((opt = strstr(opt, "<option")) != NULL && opt < end) {
            const char* opt_end = strchr(opt, '>');
            if (!opt_end || opt_end > end) break;
            char value[128];
            value[0] = '\0';
            browser_copy_attr_value_ci(opt, "value", value, sizeof(value));
            int is_selected = browser_attr_is_truthy(opt, "selected");
            const char* txt_start = opt_end + 1;
            const char* txt_close = strstr(txt_start, "</option>");
            const char* txt_end = (txt_close && txt_close < end) ? txt_close : end;
            char text[128];
            browser_extract_inner_text(txt_start, txt_end, text, sizeof(text));
            if (!value[0]) scopy(value, text, sizeof(value) - 1);
            if (!selected[0] || is_selected) scopy(selected, value, sizeof(selected) - 1);
            if (is_selected) break;
            opt = opt_end + 8;
        }
        char line[BROWSER_MAX_LINE_LEN];
        scopy(line, "[Select] ", sizeof(line) - 1);
        scat(line, name[0] ? name : "(select)", sizeof(line) - 1);
        if (selected[0]) {
            scat(line, " = ", sizeof(line) - 1);
            scat(line, selected, sizeof(line) - 1);
        }
        browser_add_line(BROWSER_LINE_TEXT, line, -1);
        p = end;
    }

    p = html;
    while ((p = strstr(p, "<img")) != NULL && g_browser.line_count < BROWSER_MAX_CONTENT_LINES) {
        const char* tag_end = strchr(p, '>');
        if (!tag_end) break;
        char src[256], alt[128];
        src[0] = alt[0] = '\0';
        browser_copy_attr_value_ci(p, "src", src, sizeof(src));
        browser_copy_attr_value_ci(p, "alt", alt, sizeof(alt));
        if (src[0] || alt[0]) browser_add_image_placeholder(src, alt);
        p = tag_end + 1;
    }
}

static void browser_normalize_spaces(const char* src, char* dst, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    size_t o = 0;
    int saw_space = 1;
    for (size_t i = 0; src && src[i] && o + 1 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!saw_space && o > 0) {
                dst[o++] = ' ';
                saw_space = 1;
            }
        } else {
            dst[o++] = (char)c;
            saw_space = 0;
        }
    }
    while (o > 0 && dst[o - 1] == ' ') o--;
    dst[o] = '\0';
}


static void browser_resolve_address(const char* raw_url, char* url, size_t url_sz) {
    if (!url || url_sz == 0) return;

    /* NetSurf owns data: documents through its standard data fetcher.  A
     * start page can legitimately contain dots in its absolute link targets,
     * so it must bypass the hostname-without-scheme heuristic below. */
    if (raw_url && sstartswith(raw_url, "data:")) {
        scopy(url, raw_url, url_sz - 1);
        return;
    }

    /* External host names entered without a scheme must prefer HTTPS.  This
     * avoids obsolete plaintext HTTP endpoints and routes Wikipedia and other
     * modern sites through the already-validated BearSSL fetch path. */
    if (raw_url && raw_url[0] && strstr(raw_url, "://") == NULL &&
        strchr(raw_url, '.') != NULL) {
        scopy(url, "https://", url_sz - 1);
        scat(url, raw_url, url_sz - 1);
        return;
    }
    (void)cos_browser_normalize_address(raw_url, url, url_sz);
}

static int browser_url_length(const window_t* w) {
    return w ? (int)slen(w->browser_url) : 0;
}

static void browser_url_set_cursor(window_t* w, int pos) {
    if (!w) return;
    int len = browser_url_length(w);
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    w->browser_url_cursor = pos;
}

static void browser_url_begin_edit(window_t* w, bool select_all) {
    if (!w) return;
    w->browser_url_focus = 1;
    w->browser_url_selected = select_all;
    browser_url_set_cursor(w, browser_url_length(w));
}

static void browser_url_replace_all(window_t* w) {
    if (!w) return;
    w->browser_url[0] = '\0';
    w->browser_url_cursor = 0;
    w->browser_url_selected = false;
}

static void browser_url_delete_range(window_t* w, int start, int end) {
    if (!w) return;
    int len = browser_url_length(w);
    if (start < 0) start = 0;
    if (end < start) end = start;
    if (end > len) end = len;
    if (start >= end) return;
    for (int i = start; i <= len - (end - start); ++i) {
        w->browser_url[i] = w->browser_url[i + (end - start)];
    }
}

static void browser_url_insert_char(window_t* w, char ch) {
    if (!w) return;
    int len = browser_url_length(w);
    if (len + 1 >= (int)sizeof(w->browser_url)) return;
    int pos = w->browser_url_cursor;
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    for (int i = len; i >= pos; --i) {
        w->browser_url[i + 1] = w->browser_url[i];
    }
    w->browser_url[pos] = ch;
    w->browser_url_cursor = pos + 1;
}

static void browser_url_backspace(window_t* w) {
    if (!w) return;
    if (w->browser_url_cursor <= 0) return;
    browser_url_delete_range(w, w->browser_url_cursor - 1, w->browser_url_cursor);
    w->browser_url_cursor--;
}

static void browser_url_delete_forward(window_t* w) {
    if (!w) return;
    int len = browser_url_length(w);
    if (w->browser_url_cursor < 0 || w->browser_url_cursor >= len) return;
    browser_url_delete_range(w, w->browser_url_cursor, w->browser_url_cursor + 1);
}

static void browser_url_move_cursor(window_t* w, int delta) {
    if (!w) return;
    browser_url_set_cursor(w, w->browser_url_cursor + delta);
}

static void browser_url_move_cursor_to_click(window_t* w, int url_x, int url_w, int click_x) {
    if (!w) return;
    int len = browser_url_length(w);
    int max_chars = (url_w - 18) / FONT_W;
    int show_start = 0;
    if (max_chars < 1) max_chars = 1;
    if (len > max_chars && max_chars > 3) {
        if (w->browser_url_focus) {
            show_start = w->browser_url_cursor - (max_chars / 2);
            if (show_start < 0) show_start = 0;
            if (show_start > len - max_chars) show_start = len - max_chars;
        } else {
            show_start = len - max_chars;
        }
    }
    int pos = show_start + (click_x - (url_x + 8)) / FONT_W;
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    w->browser_url_cursor = pos;
}


static int browser_search_length(const window_t* w) {
    return w ? (int)slen(w->browser_search_text) : 0;
}
static int browser_utf8_is_continuation(unsigned char c) {
    return (c & 0xC0u) == 0x80u;
}
static int browser_utf8_prev_boundary(const char* s, int pos) {
    if (!s || pos <= 0) return 0;
    --pos;
    while (pos > 0 && browser_utf8_is_continuation((unsigned char)s[pos])) --pos;
    return pos;
}
static int browser_utf8_next_boundary(const char* s, int len, int pos) {
    if (!s || pos >= len) return len;
    ++pos;
    while (pos < len && browser_utf8_is_continuation((unsigned char)s[pos])) ++pos;
    return pos;
}
static void browser_search_set_cursor(window_t* w, int pos) {
    if (!w) return;
    int len = browser_search_length(w);
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    w->browser_search_cursor = pos;
}

static void browser_search_begin_edit(window_t* w, bool select_all) {
    if (!w) return;
    w->browser_search_focus = 1;
    w->browser_search_selected = select_all;
    browser_search_set_cursor(w, browser_search_length(w));
}

static void browser_search_replace_all(window_t* w) {
    if (!w) return;
    w->browser_search_text[0] = '\0';
    w->browser_search_cursor = 0;
    w->browser_search_selected = false;
}

static void browser_search_delete_range(window_t* w, int start, int end) {
    if (!w) return;
    int len = browser_search_length(w);
    if (start < 0) start = 0;
    if (end < start) end = start;
    if (end > len) end = len;
    if (start >= end) return;
    for (int i = start; i <= len - (end - start); ++i) {
        w->browser_search_text[i] = w->browser_search_text[i + (end - start)];
    }
}

static void browser_search_insert_char(window_t* w, char ch) {
    if (!w) return;
    int len = browser_search_length(w);
    if (len + 1 >= (int)sizeof(w->browser_search_text)) return;
    int pos = w->browser_search_cursor;
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    for (int i = len; i >= pos; --i) {
        w->browser_search_text[i + 1] = w->browser_search_text[i];
    }
    w->browser_search_text[pos] = ch;
    w->browser_search_cursor = pos + 1;
}

static void browser_search_backspace(window_t* w) {
    if (!w) return;
    if (w->browser_search_cursor <= 0) return;
    int start = browser_utf8_prev_boundary(w->browser_search_text,
                                           w->browser_search_cursor);
    browser_search_delete_range(w, start, w->browser_search_cursor);
    w->browser_search_cursor = start;
}

static void browser_search_delete_forward(window_t* w) {
    if (!w) return;
    int len = browser_search_length(w);
    if (w->browser_search_cursor < 0 || w->browser_search_cursor >= len) return;
    int end = browser_utf8_next_boundary(w->browser_search_text, len,
                                         w->browser_search_cursor);
    browser_search_delete_range(w, w->browser_search_cursor, end);
}

static void browser_search_move_cursor(window_t* w, int delta) {
    if (!w) return;
    int len = browser_search_length(w);
    int pos = w->browser_search_cursor;
    if (delta < 0) pos = browser_utf8_prev_boundary(w->browser_search_text, pos);
    else if (delta > 0) pos = browser_utf8_next_boundary(w->browser_search_text, len, pos);
    browser_search_set_cursor(w, pos);
}

static void browser_search_move_cursor_to_click(window_t* w, int box_x, int box_w, int click_x) {
    if (!w) return;
    int len = browser_search_length(w);
    int max_chars = (box_w - 24) / FONT_W;
    int show_start = 0;
    if (max_chars < 1) max_chars = 1;
    if (len > max_chars && max_chars > 3) {
        show_start = w->browser_search_cursor - (max_chars / 2);
        if (show_start < 0) show_start = 0;
        if (show_start > len - max_chars) show_start = len - max_chars;
    }
    int pos = show_start + (click_x - (box_x + 12)) / FONT_W;
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    w->browser_search_cursor = pos;
}

static int browser_is_google_search_url(const char* url) {
    return url && (sstartswith(url, "https://www.google.") || sstartswith(url, "http://www.google.") || sstartswith(url, "https://google.") || sstartswith(url, "http://google."));
}

static void browser_copy_decoded_component(const char* src, char* dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dst_sz; ++i) {
        char c = src[i];
        if (c == '+') {
            dst[o++] = ' ';
        } else if (c == '%' && src[i + 1] && src[i + 2]) {
            char h1 = src[i + 1];
            char h2 = src[i + 2];
            int v = 0;
            if (h1 >= '0' && h1 <= '9') v = (h1 - '0') << 4;
            else if (h1 >= 'A' && h1 <= 'F') v = (h1 - 'A' + 10) << 4;
            else if (h1 >= 'a' && h1 <= 'f') v = (h1 - 'a' + 10) << 4;
            else v = -1;
            if (v >= 0) {
                if (h2 >= '0' && h2 <= '9') v |= (h2 - '0');
                else if (h2 >= 'A' && h2 <= 'F') v |= (h2 - 'A' + 10);
                else if (h2 >= 'a' && h2 <= 'f') v |= (h2 - 'a' + 10);
                else v = -1;
            }
            if (v >= 0) {
                dst[o++] = (char)v;
                i += 2;
            } else {
                dst[o++] = c;
            }
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
}

static void browser_sync_search_from_url(window_t* w, const char* url) {
    if (!w) return;
    w->browser_search_text[0] = '\0';
    w->browser_search_cursor = 0;
    w->browser_search_selected = false;
    if (!url || !url[0] || !browser_is_google_search_url(url)) return;

    const char* q = strstr(url, "q=");
    if (!q) return;
    q += 2;
    char query[sizeof(w->browser_search_text)];
    size_t qi = 0;
    while (*q && *q != '&' && qi + 1 < sizeof(query)) {
        query[qi++] = *q++;
    }
    query[qi] = '\0';
    browser_copy_decoded_component(query, w->browser_search_text, sizeof(w->browser_search_text));
    browser_search_set_cursor(w, browser_search_length(w));
}

static void browser_set_status_and_mode(window_t* w, browser_app_state_t mode, const char* msg) {
    (void)w;
    g_browser.ui_state = mode;
    if (msg && msg[0]) scopy(g_browser.status, msg, sizeof(g_browser.status) - 1);
}

static void browser_commit_search(window_t* w) {
    if (!w) return;
    char url[BROWSER_MAX_URL_LEN];
    if (!cos_browser_build_google_search_url(w->browser_search_text, url, sizeof(url))) {
        browser_set_status_and_mode(w, BROWSER_APP_ERROR, "Search failed");
        return;
    }
    browser_commit_navigation(w, url, true);
}

static void browser_clear_link_rects(void) {
    for (int i = 0; i < g_browser.link_count; i++) {
        g_browser.links[i].x = -1;
        g_browser.links[i].y = -1;
        g_browser.links[i].w = 0;
        g_browser.links[i].h = 0;
    }
}

static void browser_append_wrapped_text(browser_line_kind_t kind, const char* text, int link_index) {
    char clean[1024];
    browser_normalize_spaces(text ? text : "", clean, sizeof(clean));
    if (!clean[0]) {
        browser_add_line(BROWSER_LINE_BLANK, "", -1);
        return;
    }

    char word[128];
    int wi = 0;
    char out[BROWSER_MAX_LINE_LEN];
    out[0] = '\0';
    int out_len = 0;
    const char* p = clean;

    while (1) {
        char c = *p;
        int end_word = (c == '\0' || c == ' ');
        if (!end_word && wi < (int)sizeof(word) - 1) {
            word[wi++] = c;
        }
        if (end_word) {
            word[wi] = '\0';
            if (wi > 0) {
                int add_len = wi + (out_len > 0 ? 1 : 0);
                if (out_len + add_len >= BROWSER_MAX_LINE_LEN - 1) {
                    browser_add_line(kind, out, link_index);
                    out[0] = '\0';
                    out_len = 0;
                }
                if (out_len > 0) {
                    out[out_len++] = ' ';
                    out[out_len] = '\0';
                }
                for (int i = 0; i < wi && out_len + 1 < BROWSER_MAX_LINE_LEN; i++) {
                    out[out_len++] = word[i];
                }
                out[out_len] = '\0';
                wi = 0;
            }
        }
        if (c == '\0') break;
        p++;
    }

    if (out_len > 0 || kind == BROWSER_LINE_LINK) {
        browser_add_line(kind, out, link_index);
    }
}

static void browser_emit_text_block(browser_line_kind_t kind, const char* text) {
    if (!text) return;
    char linebuf[1024];
    const char* p = text;
    while (*p) {
        int n = 0;
        while (p[n] && p[n] != '\n' && p[n] != '\r' && n < (int)sizeof(linebuf) - 1) {
            linebuf[n] = p[n];
            n++;
        }
        linebuf[n] = '\0';
        browser_append_wrapped_text(kind, linebuf, -1);
        while (p[n] == '\r' || p[n] == '\n') n++;
        p += n;
        if (!*p) break;
    }
}


static int browser_path_has_ext(const char* path, const char* ext) {
    if (!path || !ext || !ext[0]) return 0;
    size_t lp = strlen(path);
    size_t le = strlen(ext);
    if (lp < le) return 0;
    const char* tail = path + (lp - le);
    for (size_t i = 0; i < le; ++i) {
        char a = tail[i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static int browser_detect_markdown(const char* path, const char* data) {
    if (browser_path_has_ext(path, ".md") || browser_path_has_ext(path, ".markdown") || browser_path_has_ext(path, ".mkd")) return 1;
    if (!data) return 0;
    const char* p = data;
    while (*p) {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (p[0] == '#' || p[0] == '>' || p[0] == '-' || p[0] == '*' || (p[0] == '=' && p[1] == '>') || (p[0] == '[' && strchr(p, ']') && strchr(p, '(') && strchr(p, ')'))) return 1;
        while (*p && *p != '\n' && *p != '\r') p++;
    }
    return 0;
}

static void browser_emit_markdown_text(const char* text) {
    if (!text) return;
    const char* p = text;
    bool in_pre = false;
    while (*p) {
        char line[1024];
        int li = 0;
        while (p[li] && p[li] != '\n' && p[li] != '\r' && li < (int)sizeof(line) - 1) {
            line[li] = p[li];
            li++;
        }
        line[li] = '\0';
        while (*p == '\r' || *p == '\n') p++;
        p += li;

        char* t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (t[0] == '`' && t[1] == '`' && t[2] == '`') {
            in_pre = !in_pre;
            browser_add_line(BROWSER_LINE_BLANK, "", -1);
            continue;
        }
        if (in_pre) {
            browser_add_line(BROWSER_LINE_PRE, t, -1);
            continue;
        }
        if (!t[0]) {
            browser_add_line(BROWSER_LINE_BLANK, "", -1);
            continue;
        }
        if (t[0] == '#' && t[1] == '#') {
            t += 2; while (*t == ' ') t++;
            browser_add_line(BROWSER_LINE_SUBHEADING, t, -1);
            continue;
        }
        if (t[0] == '#') {
            t += 1; while (*t == ' ') t++;
            browser_add_line(BROWSER_LINE_HEADING, t, -1);
            continue;
        }
        if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
            char buf[BROWSER_MAX_LINE_LEN];
            scopy(buf, "• ", sizeof(buf) - 1);
            scat(buf, t + 2, sizeof(buf) - 1);
            browser_append_wrapped_text(BROWSER_LINE_TEXT, buf, -1);
            continue;
        }
        if (t[0] == '>' && t[1] == ' ') {
            browser_append_wrapped_text(BROWSER_LINE_TEXT, t + 2, -1);
            continue;
        }
        if (t[0] == '=' && t[1] == '>') {
            const char* s = t + 2;
            while (*s == ' ') s++;
            char url[BROWSER_MAX_URL_LEN];
            char label[BROWSER_MAX_LINE_LEN];
            int ui = 0;
            while (*s && *s != ' ' && *s != '\t' && ui + 1 < BROWSER_MAX_URL_LEN) url[ui++] = *s++;
            url[ui] = '\0';
            while (*s == ' ' || *s == '\t') s++;
            browser_normalize_spaces(s, label, sizeof(label));
            if (!label[0]) scopy(label, url, sizeof(label) - 1);
            int link_idx = browser_add_link(url, label);
            browser_add_line(BROWSER_LINE_LINK, label, link_idx);
            continue;
        }
        if (t[0] == '[') {
            const char* rb = strchr(t, ']');
            const char* lp = rb ? strchr(rb, '(') : NULL;
            const char* rp = lp ? strchr(lp, ')') : NULL;
            if (rb && lp && rp && rp > lp) {
                char label[BROWSER_MAX_LINE_LEN];
                char url[BROWSER_MAX_URL_LEN];
                int ll = (int)(rb - t - 1);
                if (ll < 0) ll = 0;
                if (ll >= (int)sizeof(label)) ll = (int)sizeof(label) - 1;
                memcpy(label, t + 1, (size_t)ll);
                label[ll] = '\0';
                int ul = (int)(rp - lp - 1);
                if (ul < 0) ul = 0;
                if (ul >= (int)sizeof(url)) ul = (int)sizeof(url) - 1;
                memcpy(url, lp + 1, (size_t)ul);
                url[ul] = '\0';
                browser_normalize_spaces(label, label, sizeof(label));
                browser_normalize_spaces(url, url, sizeof(url));
                if (!label[0]) scopy(label, url, sizeof(label) - 1);
                int link_idx = browser_add_link(url, label);
                browser_add_line(BROWSER_LINE_LINK, label, link_idx);
                continue;
            }
        }
        browser_append_wrapped_text(BROWSER_LINE_TEXT, t, -1);
    }
}

static int browser_recv_all_socket(socket_t* sock, char* out, size_t out_sz) {
    if (!sock || !out || out_sz == 0) return -1;
    size_t total = 0;
    out[0] = '\0';
    while (total + 1 < out_sz) {
        char buf[1024];
        int got = socket_recv(sock, buf, sizeof(buf) - 1, 0);
        if (got <= 0) break;
        if (total + (size_t)got >= out_sz) got = (int)(out_sz - total - 1);
        memcpy(out + total, buf, (size_t)got);
        total += (size_t)got;
        out[total] = '\0';
        if ((size_t)got < sizeof(buf) - 1) break;
    }
    return (int)total;
}

static int browser_parse_host_port(const char* url, const char* scheme, char* host, size_t host_sz, int* port, const char** path_out, int default_port) {
    if (!url || !scheme || !host || host_sz == 0) return -1;
    size_t sch_len = strlen(scheme);
    if (!sstartswith(url, scheme)) return -1;
    const char* p = url + sch_len;
    const char* slash = strchr(p, '/');
    const char* colon = strchr(p, ':');
    size_t host_len = slash ? (size_t)(slash - p) : strlen(p);
    if (colon && (!slash || colon < slash)) {
        host_len = (size_t)(colon - p);
        if (port) *port = atoi(colon + 1);
    } else if (port) {
        *port = default_port;
    }
    if (host_len >= host_sz) host_len = host_sz - 1;
    memcpy(host, p, host_len);
    host[host_len] = '\0';
    if (path_out) *path_out = slash ? slash : "";
    return 0;
}

static int browser_load_gopher_url(const char* url) {
    char host[256];
    const char* path = NULL;
    int port = 70;
    if (browser_parse_host_port(url, "gopher://", host, sizeof(host), &port, &path, 70) < 0) return -1;

    ip_addr_t ip;
    if (dns_resolve(host, &ip) < 0) {
        browser_set_error("Gopher DNS lookup failed", url);
        return -1;
    }

    socket_t* sock = socket_create(AF_INET, SOCK_STREAM, 0);
    if (!sock) { browser_set_error("Gopher socket create failed", url); return -1; }
    sock_addr_t addr;
    addr.family = AF_INET;
    addr.port = (uint16_t)port;
    addr.addr = ip;
    if (socket_connect(sock, &addr) < 0) {
        socket_close(sock);
        browser_set_error("Gopher connect failed", url);
        return -1;
    }

    const char* selector = path;
    if (selector && selector[0] == '/') {
        selector++;
        if (selector[0] == '0' || selector[0] == '1' || selector[0] == '7' || selector[0] == '8' || selector[0] == '9' || selector[0] == 'i' || selector[0] == 'h' || selector[0] == 'g' || selector[0] == 'I') {
            selector++;
        }
    }
    char request[512];
    scopy(request, selector ? selector : "", sizeof(request) - 1);
    scat(request, "\r\n", sizeof(request) - 1);
    socket_send(sock, request, strlen(request), 0);

    char resp[8192];
    int resp_len = browser_recv_all_socket(sock, resp, sizeof(resp));
    socket_close(sock);
    if (resp_len < 0) {
        browser_set_error("Gopher read failed", url);
        return -1;
    }

    browser_reset_page();
    scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
    g_browser.link_count = 0;
    char title[BROWSER_MAX_TITLE_LEN];
    scopy(title, "Gopher: ", sizeof(title) - 1);
    scat(title, host, sizeof(title) - 1);
    g_browser.title[0] = '\0';
    scopy(g_browser.title, title, BROWSER_MAX_TITLE_LEN - 1);
    browser_add_line(BROWSER_LINE_HEADING, title, -1);
    browser_add_line(BROWSER_LINE_BLANK, "", -1);

    char* cursor = resp;
    while (*cursor) {
        char* line_end = cursor;
        while (*line_end && *line_end != '\n' && *line_end != '\r') line_end++;
        char saved = *line_end;
        *line_end = '\0';
        char* line = cursor;
        if (line[0] != '\0' && line[0] != '.') {
            if (line[0] == 'i') {
                const char* text = line + 1;
                while (*text == '\t') text++;
                browser_add_line(BROWSER_LINE_TEXT, text, -1);
            } else if (strchr(line, '\t')) {
                char type = line[0];
                char* p1 = strchr(line, '\t');
                *p1++ = '\0';
                char* p2 = strchr(p1, '\t');
                if (p2) *p2++ = '\0';
                char* p3 = p2 ? strchr(p2, '\t') : NULL;
                if (p3) *p3++ = '\0';
                const char* disp = p1 && p1[0] ? p1 : line + 1;
                const char* sel = p2 && p2[0] ? p2 : "";
                const char* h = p3 && p3[0] ? p3 : host;
                int p = port;
                if (p3 && p3[0]) p = atoi(p3);
                char link[BROWSER_MAX_URL_LEN];
                scopy(link, "gopher://", sizeof(link) - 1);
                scat(link, h, sizeof(link) - 1);
                if (p > 0) {
                    char pb[16];
                    cos_itoa(p, pb, 10);
                    scat(link, ":", sizeof(link) - 1);
                    scat(link, pb, sizeof(link) - 1);
                }
                scat(link, "/", sizeof(link) - 1);
                char type_sel[2] = { type ? type : '0', '\0' };
                scat(link, type_sel, sizeof(link) - 1);
                scat(link, sel, sizeof(link) - 1);
                int li = browser_add_link(link, disp);
                browser_add_line(BROWSER_LINE_LINK, disp, li);
            } else {
                browser_add_line(BROWSER_LINE_TEXT, line, -1);
            }
        }
        *line_end = saved;
        if (!saved) break;
        cursor = line_end;
        while (*cursor == '\r' || *cursor == '\n') cursor++;
    }
    scopy(g_browser.status, "Gopher loaded", sizeof(g_browser.status) - 1);
    return 0;
}

static int browser_load_gemini_url(const char* url) {
    char host[256];
    const char* path = NULL;
    int port = 1965;
    if (browser_parse_host_port(url, "gemini://", host, sizeof(host), &port, &path, 1965) < 0) return -1;

    if (!tls_backend_available()) {
        browser_set_error("Gemini requires a TLS backend", url);
        browser_add_line(BROWSER_LINE_TEXT, "TLS backend is not linked in this build.", -1);
        browser_add_line(BROWSER_LINE_TEXT, "Gopher still works without TLS.", -1);
        browser_add_line(BROWSER_LINE_LINK, "Try Gopher", browser_add_link("gopher://gopher.floodgap.com", "Try Gopher"));
        return -1;
    }

    tls_session_t* tls = tls_connect(host, (uint64_t)port);
    if (!tls) {
        browser_set_error("Gemini connect failed", url);
        return -1;
    }

    char request[768];
    scopy(request, url, sizeof(request) - 1);
    scat(request, "\r\n", sizeof(request) - 1);
    if (tls_send(tls, request, strlen(request)) < 0) {
        tls_close(tls);
        browser_set_error("Gemini send failed", url);
        return -1;
    }

    char resp[8192];
    int total = 0;
    while (total + 1 < (int)sizeof(resp)) {
        int got = tls_recv(tls, resp + total, sizeof(resp) - total - 1);
        if (got <= 0) break;
        total += got;
        resp[total] = '\0';
        if (got < 1024) break;
    }
    tls_close(tls);
    if (total <= 0) {
        browser_set_error("Gemini empty response", url);
        return -1;
    }

    browser_reset_page();
    scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
    char title[BROWSER_MAX_TITLE_LEN];
    scopy(title, "Gemini: ", sizeof(title) - 1);
    scat(title, host, sizeof(title) - 1);
    scopy(g_browser.title, title, BROWSER_MAX_TITLE_LEN - 1);
    browser_add_line(BROWSER_LINE_HEADING, title, -1);
    browser_add_line(BROWSER_LINE_BLANK, "", -1);

    char* body = strstr(resp, "\r\n");
    if (!body) body = strstr(resp, "\n");
    if (body) {
        while (*body == '\r' || *body == '\n') body++;
    } else {
        body = resp;
    }
    browser_emit_markdown_text(body);
    scopy(g_browser.status, "Gemini loaded", sizeof(g_browser.status) - 1);
    return 0;
}

static void browser_emit_builtin_page(const builtin_page_t* page) {
    browser_reset_page();
    g_browser.ui_state = BROWSER_APP_DISPLAY;
    if (!page) return;
    scopy(g_browser.title, page->title ? page->title : "NetSurf", BROWSER_MAX_TITLE_LEN - 1);
    for (int i = 0; page->lines[i]; i++) {
        const char* ln = page->lines[i];
        if (ln[0] == '[' && ln[2] == ']' && ln[3] == ' ') {
            char kind = ln[1];
            const char* body = ln + 4;
            if (kind == 'H') {
                browser_add_line(BROWSER_LINE_HEADING, body, -1);
            } else if (kind == 'T') {
                browser_add_line(BROWSER_LINE_SUBHEADING, body, -1);
            } else if (kind == 'P') {
                browser_emit_text_block(BROWSER_LINE_TEXT, body);
            } else if (kind == 'L') {
                char url[BROWSER_MAX_URL_LEN];
                char label[BROWSER_MAX_LINE_LEN];
                url[0] = '\0';
                        const char* s = body;
                while (*s == ' ') s++;
                int ui = 0;
                while (*s && *s != ' ' && ui + 1 < BROWSER_MAX_URL_LEN) url[ui++] = *s++;
                url[ui] = '\0';
                while (*s == ' ') s++;
                browser_normalize_spaces(s, label, sizeof(label));
                if (!label[0]) scopy(label, url, sizeof(label) - 1);
                int link_idx = browser_add_link(url, label);
                browser_add_line(BROWSER_LINE_LINK, label, link_idx);
            } else {
                browser_add_line(BROWSER_LINE_TEXT, body, -1);
            }
        } else if (ln[0] == '\0') {
            browser_add_line(BROWSER_LINE_BLANK, "", -1);
        } else {
            browser_emit_text_block(BROWSER_LINE_TEXT, ln);
        }
    }
    scopy(g_browser.status, "Done", sizeof(g_browser.status) - 1);
}

static int browser_load_text_file(const char* path, const char* data, const char* fallback_title) {
    (void)path;
    browser_reset_page();
    g_browser.ui_state = BROWSER_APP_DISPLAY;
    if (!data) {
        scopy(g_browser.status, "File is empty", sizeof(g_browser.status) - 1);
        return -1;
    }

    if (fallback_title && fallback_title[0]) {
        scopy(g_browser.title, fallback_title, BROWSER_MAX_TITLE_LEN - 1);
    }

    if (strstr(data, "<html") || strstr(data, "<HTML") || strstr(data, "<body") || strstr(data, "<BODY") || strstr(data, "<!doctype") || strstr(data, "<!DOCTYPE")) {
        const char* title_start = strstr(data, "<title>");
        const char* title_end = strstr(data, "</title>");
        if (title_start && title_end && title_end > title_start) {
            title_start += 7;
            int len = (int)(title_end - title_start);
            if (len >= BROWSER_MAX_TITLE_LEN) len = BROWSER_MAX_TITLE_LEN - 1;
            memcpy(g_browser.title, title_start, (size_t)len);
            g_browser.title[len] = '\0';
        }

        char body[FS_MAX_DATA + 1];
        int bi = 0;
        int in_tag = 0;
        int last_space = 1;
        for (const char* p = data; *p && bi < (int)sizeof(body) - 1; p++) {
            char c = *p;
            if (c == '<') {
                in_tag = 1;
                continue;
            }
            if (c == '>') {
                in_tag = 0;
                continue;
            }
            if (in_tag) continue;
            if (c == '&') {
                if (strncmp(p, "&lt;", 4) == 0) { c = '<'; p += 3; }
                else if (strncmp(p, "&gt;", 4) == 0) { c = '>'; p += 3; }
                else if (strncmp(p, "&amp;", 5) == 0) { c = '&'; p += 4; }
                else if (strncmp(p, "&quot;", 6) == 0) { c = '"'; p += 5; }
            }
            if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
                if (!last_space && bi > 0) body[bi++] = ' ';
                last_space = 1;
            } else {
                body[bi++] = c;
                last_space = 0;
            }
        }
        body[bi] = '\0';

        if (browser_detect_markdown(path, body)) browser_emit_markdown_text(body);
        else browser_emit_text_block(BROWSER_LINE_TEXT, body);

         /* Extract links as a clickable section at the end. */
        const char* p = data;
        while ((p = strstr(p, "href=")) != NULL && g_browser.link_count < BROWSER_MAX_LINKS) {
            p += 5;
            while (*p == ' ') p++;
            char quote = 0;
            if (*p == '\'' || *p == '"') quote = *p++;
            char url[BROWSER_MAX_URL_LEN];
            int ui = 0;
            while (*p && ((quote && *p != quote) || (!quote && *p != ' ' && *p != '>')) && ui + 1 < BROWSER_MAX_URL_LEN) {
                url[ui++] = *p++;
            }
            url[ui] = '\0';
            if (quote && *p == quote) p++;
            if (!url[0]) continue;
            char label[BROWSER_MAX_LINE_LEN];
                const char* anchor_text = strchr(p, '>');
            if (anchor_text) {
                anchor_text++;
                const char* end = strstr(anchor_text, "</a>");
                if (end && end > anchor_text) {
                    int li = 0;
                    int in_anchor_tag = 0;
                    for (const char* q = anchor_text; q < end && li + 1 < (int)sizeof(label); q++) {
                        char c = *q;
                        if (c == '<') { in_anchor_tag = 1; continue; }
                        if (c == '>') { in_anchor_tag = 0; continue; }
                        if (in_anchor_tag) continue;
                        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
                            if (li > 0 && label[li - 1] != ' ') label[li++] = ' ';
                        } else {
                            label[li++] = c;
                        }
                    }
                    label[li] = '\0';
                    if (!label[0]) scopy(label, url, sizeof(label) - 1);
                } else {
                    scopy(label, url, sizeof(label) - 1);
                }
            } else {
                scopy(label, url, sizeof(label) - 1);
            }
            int link_idx = browser_add_link(url, label);
            browser_add_line(BROWSER_LINE_LINK, label, link_idx);
            p = url;
        }
        browser_scan_form_buttons(data, path);
        browser_scan_interactive_elements(data);
        scopy(g_browser.status, "Loaded HTML file", sizeof(g_browser.status) - 1);
        return 0;
    }

    browser_emit_text_block(BROWSER_LINE_TEXT, data);
    scopy(g_browser.status, "Loaded text file", sizeof(g_browser.status) - 1);
    return 0;
}

static void browser_emit_directory_listing(const char* path, fs_entry_t* entries) {
    browser_reset_page();
    char title[BROWSER_MAX_TITLE_LEN];
    scopy(title, "Directory: ", sizeof(title) - 1);
    if (path && path[0]) {
        scat(title, path, sizeof(title) - 1);
    } else {
        scat(title, "/", sizeof(title) - 1);
    }
    scopy(g_browser.title, title, BROWSER_MAX_TITLE_LEN - 1);
    browser_add_line(BROWSER_LINE_HEADING, title, -1);
    browser_add_line(BROWSER_LINE_BLANK, "", -1);
    browser_add_line(BROWSER_LINE_SUBHEADING, "Click an entry to open it", -1);

    if (path && path[0] && !(path[0] == '/' && path[1] == '\0')) {
        char parent[BROWSER_MAX_URL_LEN];
        scopy(parent, path, sizeof(parent) - 1);
        int pl = slen(parent);
        while (pl > 1 && parent[pl - 1] == '/') parent[--pl] = '\0';
        while (pl > 1 && parent[pl - 1] != '/') parent[--pl] = '\0';
        if (pl <= 1) { parent[0] = '/'; parent[1] = '\0'; }
        browser_add_link(parent, "..");
        browser_add_line(BROWSER_LINE_LINK, "..", g_browser.link_count - 1);
    }

    for (int i = 0; entries && i < FS_MAX_ENTRIES && entries[i].name[0]; i++) {
        char full[BROWSER_MAX_URL_LEN];
        char label[BROWSER_MAX_LINE_LEN];
        char prefix[8];
        const fs_entry_t* e = &entries[i];
        if (e->is_hidden) continue;
        scopy(prefix, e->is_dir ? "[D] " : "[F] ", sizeof(prefix) - 1);
        scopy(label, prefix, sizeof(label) - 1);
        scat(label, e->name, sizeof(label) - 1);
        if (e->is_dir) scat(label, "/", sizeof(label) - 1);
        if (path && path[0] && !(path[0] == '/' && path[1] == '\0')) {
            scopy(full, path, sizeof(full) - 1);
            int fl = slen(full);
            if (fl > 0 && full[fl - 1] != '/') scat(full, "/", sizeof(full) - 1);
            scat(full, e->name, sizeof(full) - 1);
        } else {
            full[0] = '/'; full[1] = '\0';
            scat(full, e->name, sizeof(full) - 1);
        }
        char target[BROWSER_MAX_URL_LEN];
        scopy(target, "file://", sizeof(target) - 1);
        scat(target, full, sizeof(target) - 1);
        int link_idx = browser_add_link(target, label);
        browser_add_line(BROWSER_LINE_LINK, label, link_idx);
    }
    scopy(g_browser.status, "Directory listing", sizeof(g_browser.status) - 1);
}

static const char* browser_basename(const char* path) {
    if (!path || !path[0]) return "Untitled";
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last && last[0] ? last : path;
}

static void browser_set_error(const char* msg, const char* url) {
    browser_reset_page();
    g_browser.ui_state = BROWSER_APP_ERROR;
    scopy(g_browser.title, "NetSurf", BROWSER_MAX_TITLE_LEN - 1);
    browser_add_line(BROWSER_LINE_HEADING, "NetSurf", -1);
    browser_add_line(BROWSER_LINE_BLANK, "", -1);
    browser_add_line(BROWSER_LINE_TEXT, "Unable to load the page right now.", -1);
    browser_add_line(BROWSER_LINE_BLANK, "", -1);
    browser_add_line(BROWSER_LINE_ERROR, msg ? msg : "Network error", -1);
    if (url && url[0]) browser_add_line(BROWSER_LINE_TEXT, url, -1);
    scopy(g_browser.status, "Retry", sizeof(g_browser.status) - 1);
}



/* C-OS 4.0.8 second alpha: routes the page through the REAL NetSurf
 * engine (cos_netsurf_render_page(), src/netsurf/cos_netsurf_render.c)
 * - real fetch, real HTML5 parse via libhubbub into a real DOM via
 * libdom, real CSS association via libcss, real <script> execution
 * via the QuickJS backend - and turns its output into this window's
 * existing line/link display structures.
 *
 * This is the piece that makes the GUI's "NetSurf" window show
 * content that actually came from real NetSurf, rather than from
 * browser_load_text_file()'s hand-rolled HTML-tag stripper below
 * (which - regardless of the "NetSurf" label on the window - is all
 * that ever actually drove this window's visible content before
 * this). See PORTING_NOTES.md for the fuller history.
 *
 * There is still no CSS box-model layout (Tier B in
 * PORTING_NOTES.md) - what you get is a genuine top-to-bottom read of
 * the real parsed DOM (title/headings/paragraphs/links), reusing this
 * window's existing chrome, scrolling and link-click handling, not a
 * pixel-accurate laid-out page.
 *
 * Returns 0 on success (g_browser now holds the real result) or -1
 * on failure, in which case browser_set_error() has already been
 * called and the caller should do nothing further - matching
 * browser_load_http_url()'s existing contract for its old direct
 * http_get()-based path. */
static int browser_load_via_real_netsurf(const char* url,
                                         const char* post_urlenc) {
    char err[160];
    err[0] = '\0';

    /* Every network document uses upstream browser_window, its content
     * factory, form processing, layout and plotter.  No C-OS HTML stripping
     * or direct HTTP rendering path remains for GET or URL-encoded POST.
     * Preserve the existing browsing context for a top-level navigation:
     * destroying it while a page-local QuickJS runtime still owns DOM wrapper
     * objects trips QuickJS's gc_obj_list assertion. `browser_window_navigate`
     * is the upstream-safe path for replacing the current document. */
    bool opened = (post_urlenc == NULL)
        ? cos_netsurf_browser_open(url, 760, 500, err, sizeof(err))
        : cos_netsurf_browser_open_post(url, post_urlenc, 760, 500,
                                        err, sizeof(err));
    if (!opened) {
        /* Keep the document surface assigned to NetSurf even on a navigation
         * error; the GUI only presents status chrome rather than falling back
         * to the obsolete DOM-to-lines renderer. */
        g_browser_standard_renderer = true;
        browser_set_error(err[0] ? err : "NetSurf browser navigation failed", url);
        return -1;
    }

    browser_reset_page();
    g_browser_standard_renderer = true;
    g_browser.ui_state = BROWSER_APP_LOADING;
    scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
    scopy(g_browser.title, browser_basename(url), BROWSER_MAX_TITLE_LEN - 1);
    scopy(g_browser.status, "NetSurf 3.11 loading", sizeof(g_browser.status) - 1);
    /* The upstream window changes content asynchronously.  Request a frame
     * immediately so the old C-OS start page cannot remain visible until an
     * unrelated desktop event invalidates the surface. */
    gui_request_redraw();
    return 0;
}

static int browser_load_http_url(const char* url, const char* method, const char* data) {
    if (!url || !url[0]) return -1;

    /* javascript: URLs are evaluated directly through QuickJS rather
     * than going through the fetch/render pipeline below. */
    if (url[0] == 'j' && url[1] == 'a' && url[2] == 'v' &&
        url[3] == 'a' && url[4] == 's' && url[5] == 'c' &&
        url[6] == 'r' && url[7] == 'i' && url[8] == 'p' &&
        url[9] == 't' && url[10] == ':') {
        cos_netsurf_eval_script(url + 11);
        return 0;
    }

    if (!method || smatch(method, "GET")) {
        return browser_load_via_real_netsurf(url, NULL);
    }
    if (smatch(method, "POST")) {
        return browser_load_via_real_netsurf(url, data ? data : "");
    }

    browser_set_error("Unsupported NetSurf form method", url);
    scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
    return -1;
}

static int browser_submit_form_link(window_t* w, const browser_link_t* link) {
    if (!w || !link || link->kind != 1) return -1;

    char action[BROWSER_MAX_URL_LEN];
    browser_resolve_address(link->url, action, sizeof(action));

    if (link->enctype[0] && smatch(link->enctype, "multipart/form-data")) {
        browser_set_error("Multipart form upload is not supported yet", action);
        return -1;
    }

    char submitted_url[BROWSER_MAX_URL_LEN];
    if (link->method[0] && smatch(link->method, "POST")) {
        return browser_load_http_url(action, "POST", link->data[0] ? link->data : "");
    }

    browser_form_join_url(action, link->data, submitted_url, sizeof(submitted_url));
    return browser_load_http_url(submitted_url, "GET", NULL);
}

static void browser_load_from_url(const char* url) {
    browser_reset_page();
    g_browser.ui_state = BROWSER_APP_LOADING;

    if (!url || !url[0] || smatch(url, "c-os://home") || smatch(url, "about:welcome")) {
        url = "http://example.com/";
    }

    const builtin_page_t* builtin = browser_find_builtin(url);
    if (builtin) {
        browser_emit_builtin_page(builtin);
        scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
        return;
    }

    if (sstartswith(url, "about:")) {
        if (smatch(url, "about:blank")) {
            browser_add_line(BROWSER_LINE_TEXT, "Blank page", -1);
            scopy(g_browser.title, "Blank Page", BROWSER_MAX_TITLE_LEN - 1);
            scopy(g_browser.status, "Ready", sizeof(g_browser.status) - 1);
            g_browser.ui_state = BROWSER_APP_DISPLAY;
        } else {
            browser_set_error("Unsupported about: page", url);
        }
        scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
        return;
    }

    if (sstartswith(url, "c-os://")) {
        browser_set_error("Unknown built-in page", url);
        scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
        return;
    }

    if (sstartswith(url, "file://")) {
        /* A file: URL is a document navigation, not a directory probe.  The
         * old path called fs_list_dir() before determining the node type, but
         * that API always returns its static listing buffer even on failure;
         * consequently every local HTML document was overwritten visually by
         * a spurious Directory listing after NetSurf had already executed it.
         * Keep the complete file: lifecycle in the storage-backed NetSurf
         * fetcher.  It emits an ordinary fetch error for a missing file while
         * valid HTML/CSS/JS follows exactly the remote content path. */
        scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
        (void)browser_load_via_real_netsurf(url, NULL);
        return;
    }

    if (sstartswith(url, "/") || fs_find(url)) {
        fs_entry_t* entry = fs_find(url);
        if (entry && entry->is_dir) {
            browser_emit_directory_listing(url, fs_list_dir(url));
            scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
            return;
        }
        if (entry) {
            char local_url[BROWSER_MAX_URL_LEN];
            scopy(local_url, "file://", sizeof(local_url) - 1);
            scat(local_url, url[0] == '/' ? url : "/", sizeof(local_url) - 1);
            if (url[0] != '/') scat(local_url, url, sizeof(local_url) - 1);
            scopy(g_browser.current_url, local_url, BROWSER_MAX_URL_LEN - 1);
            (void)browser_load_via_real_netsurf(local_url, NULL);
            return;
        }
    }

    if (sstartswith(url, "gemini://")) {
        if (browser_load_gemini_url(url) == 0) {
            scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
            g_browser.ui_state = BROWSER_APP_DISPLAY;
            return;
        }
        scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
        return;
    }

    if (sstartswith(url, "gopher://")) {
        if (browser_load_gopher_url(url) == 0) {
            scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
            g_browser.ui_state = BROWSER_APP_DISPLAY;
            return;
        }
        scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
        return;
    }

    if (sstartswith(url, "javascript:")) {
        /* Local privileged script entry point.  This never enters the HTTP
         * fetcher or a remote page context, so `OS.*` capabilities remain
         * unavailable to external documents. */
        scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
        cos_netsurf_eval_script(url + 11);
        scopy(g_browser.status, "Local JavaScript executed", sizeof(g_browser.status) - 1);
        gui_request_redraw();
        return;
    }

    if (sstartswith(url, "data:")) {
        /* Keep local start documents on the ordinary NetSurf content path so
         * their <a> controls use the same browser_window click handling as
         * real network pages. */
        scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
        (void)browser_load_via_real_netsurf(url, NULL);
        return;
    }

    if (sstartswith(url, "http://") || sstartswith(url, "https://")) {
        /* Keep the attempted address even when networking is unavailable.
         * browser_load_http_url() renders a useful error page on failure, but
         * previously left current_url empty; draw_browser_app() then retried
         * the same request on every frame and flooded the serial log. */
        scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
        (void)browser_load_http_url(url, "GET", NULL);
        return;
    }

    browser_set_error("Unknown address", url);
    scopy(g_browser.current_url, url, BROWSER_MAX_URL_LEN - 1);
}

/* 他アプリ(ファイルマネージャ等)からも呼ばれるため非static。宣言は gui.h 側。 */
void browser_commit_navigation(window_t* w, const char* raw_url, bool add_history) {
    if (!w) return;
    char url[BROWSER_MAX_URL_LEN];
    /* Preserve local JavaScript verbatim.  Generic address resolution treats
     * unknown schemes as host names and used to rewrite this as
     * https://javascript/, sending a local capability string to DNS/TLS. */
    if (raw_url != NULL && sstartswith(raw_url, "javascript:")) {
        scopy(url, raw_url, sizeof(url) - 1);
    } else if (browser_is_search_query(raw_url)) {
        if (!cos_browser_build_google_search_url(raw_url, url, sizeof(url))) {
            browser_set_error("Search query is empty or too long", raw_url ? raw_url : "");
            return;
        }
    } else {
        browser_resolve_address(raw_url, url, sizeof(url));
    }
    if (smatch(url, "c-os://home") || smatch(url, "about:welcome")) {
        scopy(url, "http://example.com/", sizeof(url) - 1);
    }

    if (add_history) {
        (void)cos_browser_history_push(g_browser.history_urls, &g_browser.history_count, &g_browser.history_pos, BROWSER_MAX_HISTORY, url);
        if (g_browser.history_pos >= 0 && g_browser.history_pos < BROWSER_MAX_HISTORY) {
            g_browser.history_titles[g_browser.history_pos][0] = '\0';
        }
    }

    browser_load_from_url(url);
    scopy(w->browser_url, url, sizeof(w->browser_url) - 1);
    browser_url_set_cursor(w, browser_url_length(w));
    if (g_browser.title[0]) scopy(w->browser_title, g_browser.title, sizeof(w->browser_title) - 1);
    else scopy(w->browser_title, browser_basename(url), sizeof(w->browser_title) - 1);

    w->browser_search_text[0] = '\0';
    w->browser_search_cursor = 0;
    w->browser_search_selected = false;
    w->browser_search_focus = 0;

    w->browser_scroll = 0;
    w->browser_url_focus = 0;
    w->browser_url_selected = 0;

    /* Navigation is asynchronous in the C-OS NetSurf fetch pump.  Force the
     * next lifecycle pass to discard the previous page immediately; later
     * CONTENT_MSG_REDRAW notifications request subsequent reformat/redraws
     * when HTML, CSS and image content becomes ready. */
    gui_request_redraw();
}

static void browser_reload_current(window_t* w) {
    if (!w || !w->browser_url[0]) return;
    browser_commit_navigation(w, w->browser_url, false);
}

static void browser_go_history(window_t* w, int delta) {
    if (!w) return;
    if (!cos_browser_history_move(&g_browser.history_pos, g_browser.history_count, delta)) return;
    browser_commit_navigation(w, g_browser.history_urls[g_browser.history_pos], false);
}

#define BROWSER_CONTEXT_MENU_W 176
#define BROWSER_CONTEXT_ITEM_H 24
#define BROWSER_CONTEXT_SEPARATOR_H 8
#define BROWSER_CONTEXT_ITEM_COUNT 7

enum {
    BROWSER_CONTEXT_BACK = 0,
    BROWSER_CONTEXT_FORWARD,
    BROWSER_CONTEXT_RELOAD,
    BROWSER_CONTEXT_SEPARATOR,
    BROWSER_CONTEXT_TOP,
    BROWSER_CONTEXT_BOTTOM,
    BROWSER_CONTEXT_EDIT_ADDRESS,
};

static int browser_content_height(void);

static int browser_content_viewport_y(const window_t* w) {
    return w ? w->y + C_TITLEBAR_H + 38 + 26 : 0;
}

static int browser_content_viewport_height(const window_t* w) {
    if (!w) return 40;
    int h = w->h - C_TITLEBAR_H - 38 - 26 - C_STATUSBAR_H;
    return h < 40 ? 40 : h;
}

static void browser_invalidate_netsurf_cache(void) {
    /* Scrolling changes visible pixels without necessarily changing the
     * document's paint generation.  Never reuse the old viewport BitBlt cache
     * after a scroll request. */
    g_browser_netsurf_cache_valid = false;
}

static bool browser_scroll_by(window_t* w, int delta_y) {
    if (!w || delta_y == 0) return false;
    if (g_browser_standard_renderer) {
        /* The NetSurf wrapper accepts viewport-local coordinates and applies
         * its current document scroll offset internally. */
        int page_x = w->w / 2;
        int page_y = browser_content_viewport_height(w) / 2;
        if (!cos_netsurf_browser_scroll_at(page_x, page_y, 0, delta_y)) {
            return false;
        }
        browser_invalidate_netsurf_cache();
        scopy(g_browser.status, "NetSurf 3.11 scrolled", sizeof(g_browser.status) - 1);
        return true;
    }

    int content_h = browser_content_height() + 20;
    int viewport_h = browser_content_viewport_height(w);
    int max_scroll = content_h > viewport_h ? content_h - viewport_h : 0;
    int next = w->browser_scroll + delta_y;
    if (next < 0) next = 0;
    if (next > max_scroll) next = max_scroll;
    if (next == w->browser_scroll) return false;
    w->browser_scroll = next;
    return true;
}

static int browser_context_menu_height(void) {
    return BROWSER_CONTEXT_ITEM_H * (BROWSER_CONTEXT_ITEM_COUNT - 1) +
           BROWSER_CONTEXT_SEPARATOR_H + 8;
}

static int browser_context_item_at(const window_t* w, int mx, int my) {
    if (!w || !w->browser_context_visible ||
        mx < w->browser_context_x || mx >= w->browser_context_x + BROWSER_CONTEXT_MENU_W) {
        return -1;
    }
    int row_y = w->browser_context_y + 4;
    for (int item = 0; item < BROWSER_CONTEXT_ITEM_COUNT; ++item) {
        int height = item == BROWSER_CONTEXT_SEPARATOR ? BROWSER_CONTEXT_SEPARATOR_H : BROWSER_CONTEXT_ITEM_H;
        if (my >= row_y && my < row_y + height) return item;
        row_y += height;
    }
    return -1;
}

static void browser_context_menu_close(window_t* w) {
    if (w) w->browser_context_visible = false;
}

static void browser_draw_context_menu(const window_t* w) {
    if (!w || !w->browser_context_visible) return;
    static const char* const labels[BROWSER_CONTEXT_ITEM_COUNT] = {
        "Back", "Forward", "Reload", "", "Top", "Bottom", "Edit address"
    };
    const int menu_x = w->browser_context_x;
    const int menu_y = w->browser_context_y;
    const int menu_h = browser_context_menu_height();
    vga_fill_rect(menu_x + 2, menu_y + 2, BROWSER_CONTEXT_MENU_W, menu_h, rgb(170, 175, 185));
    vga_fill_rounded_rect(menu_x, menu_y, BROWSER_CONTEXT_MENU_W, menu_h, 5, rgb(255, 255, 255));
    vga_draw_rounded_rect(menu_x, menu_y, BROWSER_CONTEXT_MENU_W, menu_h, 5, rgb(170, 180, 195));
    int row_y = menu_y + 4;
    for (int item = 0; item < BROWSER_CONTEXT_ITEM_COUNT; ++item) {
        if (item == BROWSER_CONTEXT_SEPARATOR) {
            vga_fill_rect(menu_x + 8, row_y + 3, BROWSER_CONTEXT_MENU_W - 16, 1, rgb(225, 229, 235));
            row_y += BROWSER_CONTEXT_SEPARATOR_H;
            continue;
        }
        vga_draw_string(menu_x + 12, row_y + 5, labels[item], C_TEXT, 0xFFFFFFFF);
        row_y += BROWSER_CONTEXT_ITEM_H;
    }
}

static void browser_context_menu_invoke(window_t* w, int item) {
    if (!w) return;
    switch (item) {
        case BROWSER_CONTEXT_BACK:
            serial_puts("[GUI/Browser] context action: back\n");
            browser_go_history(w, -1);
            break;
        case BROWSER_CONTEXT_FORWARD:
            serial_puts("[GUI/Browser] context action: forward\n");
            browser_go_history(w, +1);
            break;
        case BROWSER_CONTEXT_RELOAD:
            serial_puts("[GUI/Browser] context action: reload\n");
            browser_reload_current(w);
            break;
        case BROWSER_CONTEXT_TOP:
            serial_puts("[GUI/Browser] context action: top\n");
            (void)browser_scroll_by(w, -1000000);
            break;
        case BROWSER_CONTEXT_BOTTOM:
            serial_puts("[GUI/Browser] context action: bottom\n");
            (void)browser_scroll_by(w, 1000000);
            break;
        case BROWSER_CONTEXT_EDIT_ADDRESS:
            serial_puts("[GUI/Browser] context action: edit address\n");
            browser_url_begin_edit(w, true);
            break;
        default:
            break;
    }
    browser_context_menu_close(w);
    gui_request_redraw();
}

static int browser_content_height(void) {
    int total = 0;
    for (int i = 0; i < g_browser.line_count; i++) total += g_browser.lines[i].height;
    return total;
}

static void browser_render_line(int x, int y, int w, const browser_line_t* line, int line_top, int* link_rect_index) {
    if (!line) return;
    if (line->kind == BROWSER_LINE_BLANK) return;

    int text_x = x + 12;
    int text_w = w - 24;
    uint64_t fg = rgb(30,30,30);
    uint64_t bg = 0xFFFFFFFF;
    int ty = y;

    switch (line->kind) {
        case BROWSER_LINE_HEADING:
            vga_fill_rect(x, ty - 2, w, 28, rgb(0,100,200));
            vga_draw_string(text_x, ty + 5, line->text, 0xFFFFFFFF, rgb(0,100,200));
            break;
        case BROWSER_LINE_SUBHEADING:
            vga_fill_rect(x, ty, w, 22, rgb(230,240,255));
            vga_draw_string(text_x, ty + 4, line->text, rgb(0,80,160), rgb(230,240,255));
            break;
        case BROWSER_LINE_LINK: {
            int idx = -1;
            if (g_browser.lines[line_top].link_index >= 0 && g_browser.lines[line_top].link_index < g_browser.link_count) {
                idx = g_browser.lines[line_top].link_index;
            }
            if (idx >= 0 && g_browser.links[idx].kind == 1) {
                int btn_w = slen(line->text) * FONT_W + 26;
                if (btn_w > text_w) btn_w = text_w;
                if (btn_w < 88) btn_w = 88;
                vga_fill_rounded_rect(text_x, ty + 1, btn_w, 20, 6, rgb(52, 120, 220));
                vga_draw_rounded_rect(text_x, ty + 1, btn_w, 20, 6, rgb(30, 80, 160));
                vga_draw_string(text_x + 13, ty + 6, line->text, 0xFFFFFFFF, rgb(52, 120, 220));
                g_browser.links[idx].x = text_x;
                g_browser.links[idx].y = ty;
                g_browser.links[idx].w = btn_w;
                g_browser.links[idx].h = 20;
            } else {
                uint64_t link_bg = 0xFFFFFFFF;
                vga_draw_string(text_x, ty + 2, line->text, rgb(0,102,204), link_bg);
                int text_len = slen(line->text);
                int link_w = text_len * FONT_W;
                if (link_w > text_w) link_w = text_w;
                if (idx >= 0) {
                    g_browser.links[idx].x = text_x;
                    g_browser.links[idx].y = ty;
                    g_browser.links[idx].w = link_w;
                    g_browser.links[idx].h = FONT_H;
                }
                vga_draw_line(text_x, ty + FONT_H + 1, text_x + link_w, ty + FONT_H + 1, rgb(0,102,204));
            }
            if (link_rect_index && idx >= 0) *link_rect_index = idx;
            break;
        }
        case BROWSER_LINE_PRE:
            fg = rgb(40,40,40);
            vga_draw_string(text_x, ty + 2, line->text, fg, bg);
            break;
        case BROWSER_LINE_ERROR:
            vga_draw_string(text_x, ty + 2, line->text, rgb(200,0,0), rgb(255,240,240));
            break;
        case BROWSER_LINE_TEXT:
        default:
            vga_draw_string(text_x, ty + 2, line->text, fg, bg);
            break;
    }
}

static bool browser_netsurf_cache_reserve(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (g_browser_netsurf_cache != NULL &&
        g_browser_netsurf_cache_w == w && g_browser_netsurf_cache_h == h) {
        return true;
    }
    if (g_browser_netsurf_cache != NULL) {
        kfree(g_browser_netsurf_cache);
        g_browser_netsurf_cache = NULL;
    }
    size_t pixels = (size_t)w * (size_t)h;
    if (pixels > (SIZE_MAX / sizeof(uint32_t))) return false;
    g_browser_netsurf_cache = (uint32_t*)kmalloc(pixels * sizeof(uint32_t));
    if (g_browser_netsurf_cache == NULL) {
        g_browser_netsurf_cache_w = 0;
        g_browser_netsurf_cache_h = 0;
        g_browser_netsurf_cache_valid = false;
        return false;
    }
    g_browser_netsurf_cache_w = w;
    g_browser_netsurf_cache_h = h;
    g_browser_netsurf_cache_valid = false;
    return true;
}

static void browser_netsurf_cache_capture(int x, int y, int w, int h,
                                          uint32_t generation) {
    if (!g_browser_netsurf_cache || !backbuffer ||
        w != g_browser_netsurf_cache_w || h != g_browser_netsurf_cache_h ||
        x < 0 || y < 0 || x + w > (int)SCREEN_W || y + h > (int)SCREEN_H) {
        g_browser_netsurf_cache_valid = false;
        return;
    }
    for (int row = 0; row < h; ++row) {
        memcpy(g_browser_netsurf_cache + (size_t)row * (size_t)w,
               backbuffer + (size_t)(y + row) * (size_t)SCREEN_W + (size_t)x,
               (size_t)w * sizeof(uint32_t));
    }
    g_browser_netsurf_cache_generation = generation;
    g_browser_netsurf_cache_valid = true;
}

static void browser_render_content(int x, int y, int w, int h, window_t* win) {
    if (g_browser_standard_renderer) {
        /* A link click is committed inside upstream browser_window, bypassing
         * browser_commit_navigation(). Mirror that authoritative URL once the
         * user is not actively editing the address field. */
        char committed_url[BROWSER_MAX_URL_LEN];
        if (!win->browser_url_focus &&
            cos_netsurf_browser_get_url(committed_url, sizeof(committed_url)) &&
            !smatch(committed_url, win->browser_url)) {
            /* The compact internal data: document is longer than the address
             * field and should not force the caret to its clipped tail every
             * frame. Keep the launch field readable; real navigations still
             * mirror the authoritative NetSurf URL below. */
            if (!sstartswith(committed_url, "data:text/html,")) {
                scopy(g_browser.current_url, committed_url, sizeof(g_browser.current_url) - 1);
                scopy(win->browser_url, committed_url, sizeof(win->browser_url) - 1);
                browser_url_set_cursor(win, browser_url_length(win));
            }
        }
        /* A deferred QuickJS/libdom rebox completes inside NetSurf's schedule
         * callback. Consume its completion edge before considering the cached
         * BitBlt source, so the next frame must redraw the new box tree. */
        if (cos_netsurf_browser_take_dom_rebuild_complete()) {
            g_browser_netsurf_cache_valid = false;
        }
        uint32_t generation = cos_netsurf_window_paint_generation();
        if (g_browser_netsurf_cache_valid &&
            generation != 0 && generation == g_browser_netsurf_cache_generation &&
            g_browser_netsurf_cache_w == w && g_browser_netsurf_cache_h == h) {
            /* Cached source is tightly packed to the browser viewport; this
             * is a single clipped BitBlt into the main GUI backbuffer. */
            vga_copy_rect(x, y, 0, 0, w, h, g_browser_netsurf_cache);
            return;
        }

        vga_fill_rect(x, y, w, h, 0xFFFFFFFF);
        if (cos_netsurf_browser_redraw(x, y, w, h)) {
            g_browser.ui_state = BROWSER_APP_DISPLAY;
            scopy(g_browser.status, "NetSurf 3.11", sizeof(g_browser.status) - 1);
            uint32_t painted_generation = cos_netsurf_window_paint_generation();
            if (painted_generation != 0 && browser_netsurf_cache_reserve(w, h)) {
                browser_netsurf_cache_capture(x, y, w, h, painted_generation);
            }
        } else {
            g_browser_netsurf_cache_valid = false;
        }
        return;
    }

    vga_fill_rect(x, y, w, h, 0xFFFFFFFF);
    browser_clear_link_rects();

    int content_h = browser_content_height() + 20;
    int max_scroll = content_h > h ? content_h - h : 0;
    if (win->browser_scroll > max_scroll) win->browser_scroll = max_scroll;
    if (win->browser_scroll < 0) win->browser_scroll = 0;

    int top = y + 10 - win->browser_scroll;
    int cur_y = top;
    int visible_top = y;
    int visible_bottom = y + h;

    for (int i = 0; i < g_browser.line_count; i++) {
        const browser_line_t* line = &g_browser.lines[i];
        int line_h = line->height;
        if (cur_y + line_h >= visible_top && cur_y <= visible_bottom) {
            int dummy = -1;
            browser_render_line(x, cur_y, w, line, i, &dummy);
        }
        cur_y += line_h;
        if (cur_y > visible_bottom + 40) break;
    }

    if (max_scroll > 0) {
        int sb_x = x + w - 12;
        int sb_y = y + 8;
        int sb_h = h - 16;
        int thumb_h = sb_h * h / content_h;
        if (thumb_h < 24) thumb_h = 24;
        if (thumb_h > sb_h) thumb_h = sb_h;
        int thumb_y = sb_y + (sb_h - thumb_h) * win->browser_scroll / max_scroll;
        vga_fill_rect(sb_x, sb_y, 8, sb_h, rgb(238,238,238));
        vga_fill_rect(sb_x, thumb_y, 8, thumb_h, rgb(170,170,170));
    }
}

static void browser_search_ui_layout(int x, int y, int w, int h, int* panel_x, int* panel_y, int* panel_w, int* panel_h, int* box_x, int* box_y, int* box_w, int* btn_x, int* btn_y, int* btn_w) {
    int pw = w - 80;
    int ph = 220;
    if (pw > 560) pw = 560;
    if (pw < 320) pw = w - 20;
    if (pw < 240) pw = 240;
    if (ph > h - 40) ph = h - 40;
    if (ph < 180) ph = 180;
    if (pw > w - 20) pw = w - 20;
    if (pw < 200) pw = 200;
    int px = x + (w - pw) / 2;
    int py = y + (h - ph) / 2;
    int bw = pw - 44;
    if (bw > 460) bw = 460;
    if (bw < 220) bw = pw - 44;
    int bx = px + (pw - bw) / 2;
    int by = py + ph / 2 - 18;
    int button_w = 120;
    int button_x = px + (pw - button_w) / 2;
    int button_y = by + 44;
    if (panel_x) *panel_x = px;
    if (panel_y) *panel_y = py;
    if (panel_w) *panel_w = pw;
    if (panel_h) *panel_h = ph;
    if (box_x) *box_x = bx;
    if (box_y) *box_y = by;
    if (box_w) *box_w = bw;
    if (btn_x) *btn_x = button_x;
    if (btn_y) *btn_y = button_y;
    if (btn_w) *btn_w = button_w;
}

static void browser_draw_search_field(window_t* win, int box_x, int box_y, int box_w, int active) {
    uint64_t bg = active ? rgb(232, 240, 254) : 0xFFFFFFFF;
    uint64_t border = active ? C_INPUT_FOCUS : C_INPUT_BORDER;
    vga_fill_rounded_rect(box_x, box_y, box_w, 30, 6, bg);
    vga_draw_rounded_rect(box_x, box_y, box_w, 30, 6, border);

    int len = browser_search_length(win);
    int max_chars = (box_w - 24) / FONT_W;
    int show_start = 0;
    if (max_chars < 1) max_chars = 1;
    if (active && len > max_chars && max_chars > 3) {
        show_start = win->browser_search_cursor - (max_chars / 2);
        if (show_start < 0) show_start = 0;
        if (show_start > len - max_chars) show_start = len - max_chars;
    } else if (len > max_chars && max_chars > 3) {
        show_start = len - max_chars;
    }

    if (win->browser_search_text[0]) {
        char visible[256];
        int vi = 0;
        for (int i = show_start; win->browser_search_text[i] && vi + 1 < (int)sizeof(visible); ++i) visible[vi++] = win->browser_search_text[i];
        visible[vi] = '\0';
        vga_draw_string(box_x + 10, box_y + 8, visible, C_TEXT, bg);
        if (active) {
            int cur_rel = win->browser_search_cursor - show_start;
            if (cur_rel < 0) cur_rel = 0;
            if (cur_rel > vi) cur_rel = vi;
            int caret_x = box_x + 10 + cur_rel * FONT_W;
            if (caret_x < box_x + box_w - 4) vga_fill_rect(caret_x, box_y + 7, 2, 16, C_ACCENT);
        }
    } else {
        vga_draw_string(box_x + 10, box_y + 8, "Search Google", C_TEXT_GRAY, bg);
        if (active) {
            vga_fill_rect(box_x + 10, box_y + 7, 2, 16, C_ACCENT);
        }
    }
}

static void browser_render_search_screen(int x, int y, int w, int h, window_t* win) {
    (void)win;
    int px = x + 16;
    int py = y + 16;
    int pw = w - 32;
    int ph = h - 32;
    if (pw < 120) pw = w;
    if (ph < 120) ph = h;

    vga_fill_rounded_rect(px, py, pw, ph, 18, rgb(250, 250, 250));
    vga_draw_rounded_rect(px, py, pw, ph, 18, rgb(220, 220, 220));

    const char* label = "Home";
    int label_w = slen(label) * FONT_W;
    int label_x = px + (pw - label_w) / 2;
    int label_y = py + (ph / 2) - (FONT_H / 2);
    vga_draw_string(label_x, label_y, label, rgb(66, 133, 244), 0xFFFFFFFF);
}



static void browser_render_chrome(int x, int y, int w, int h, window_t* win) {
    (void)h;
    int bar_h = 40;
    vga_fill_rect(x, y, w, bar_h, C_TOOLBAR);
    vga_draw_rect(x, y, w, bar_h, C_BORDER);

    /* Ctrl+K replaces the location field with a visible search editor.
     * This makes the independent Google query draft observable and avoids
     * confusing it with the currently loaded URL. */
    if (win->browser_search_focus) {
        browser_draw_search_field(win, x + 12, y + 5, w - 24, 1);
        return;
    }

    /* Compact browser controls leave the URL field dominant while making
     * history/reload discoverable without relying on unavailable Alt-key
     * routing in the current GUI input layer. */
    const int control_y = y + 8;
    const int control_w = 22;
    const int control_gap = 4;
    const int back_x = x + 8;
    const int forward_x = back_x + control_w + control_gap;
    const int reload_x = forward_x + control_w + control_gap;
    const uint64_t control_bg = rgb(248, 249, 250);
    const uint64_t control_border = rgb(205, 210, 218);
    vga_fill_rounded_rect(back_x, control_y, control_w, 24, 5, control_bg);
    vga_draw_rounded_rect(back_x, control_y, control_w, 24, 5, control_border);
    vga_fill_rounded_rect(forward_x, control_y, control_w, 24, 5, control_bg);
    vga_draw_rounded_rect(forward_x, control_y, control_w, 24, 5, control_border);
    vga_fill_rounded_rect(reload_x, control_y, control_w, 24, 5, control_bg);
    vga_draw_rounded_rect(reload_x, control_y, control_w, 24, 5, control_border);
    vga_draw_string(back_x + 7, control_y + 5, "<", C_TEXT, control_bg);
    vga_draw_string(forward_x + 7, control_y + 5, ">", C_TEXT, control_bg);
    vga_draw_string(reload_x + 7, control_y + 5, "R", C_TEXT, control_bg);

    int url_x = reload_x + control_w + 8;
    int url_w = x + w - 12 - url_x;
    if (url_w < 120) url_w = 120;
    uint64_t url_bg = (win->browser_url_focus && win->browser_url_selected)
                    ? rgb(232, 240, 254) : 0xFFFFFFFF;
    vga_fill_rounded_rect(url_x, y + 7, url_w, 26, 6, url_bg);
    vga_draw_rounded_rect(url_x, y + 7, url_w, 26, 6, win->browser_url_focus ? C_INPUT_FOCUS : C_INPUT_BORDER);

    const char* url_disp = win->browser_url[0] ? win->browser_url : "http://example.com/";
    int max_chars = (url_w - 18) / FONT_W;
    int url_len = slen(url_disp);
    int show_start = 0;
    if (win->browser_url_focus && url_len > max_chars && max_chars > 3) {
        show_start = win->browser_url_cursor - (max_chars / 2);
        if (show_start < 0) show_start = 0;
        if (show_start > url_len - max_chars) show_start = url_len - max_chars;
    } else if (url_len > max_chars && max_chars > 3) {
        /* Normal network addresses are most useful at their host/path tail,
         * but the built-in data: start document must retain its recognizable
         * scheme and title prefix in the non-editing browser chrome. */
        show_start = sstartswith(url_disp, "data:text/html,") ? 0 : url_len - max_chars;
    }
    char visible[512];
    int vi = 0;
    for (int i = show_start; url_disp[i] && vi + 1 < (int)sizeof(visible); ++i) visible[vi++] = url_disp[i];
    visible[vi] = '\0';
    vga_draw_string(url_x + 8, y + 13, visible, C_TEXT, 0xFFFFFFFF);
    if (win->browser_url_focus) {
        int cur_rel = win->browser_url_cursor - show_start;
        if (cur_rel < 0) cur_rel = 0;
        if (cur_rel > vi) cur_rel = vi;
        int cur_x = url_x + 8 + cur_rel * FONT_W;
        if (cur_x < url_x + url_w - 4) vga_fill_rect(cur_x, y + 10, 2, 16, C_ACCENT);
    }

    if (g_browser.ui_state == BROWSER_APP_LOADING) {
        int prog_w = w;
        if (g_browser.loading_progress < 0) g_browser.loading_progress = 0;
        if (g_browser.loading_progress > 100) g_browser.loading_progress = 100;
        int fill_w = (prog_w * g_browser.loading_progress) / 100;
        vga_fill_rect(x, y + bar_h - 3, fill_w, 3, C_ACCENT);
    }
}


void draw_browser_app(int idx) {
    window_t* w = &windows[idx];
    /* The standard renderer owns the authoritative HTML title.  Synchronise
     * it through the normal GUI title API so DOM `document.title` mutations
     * become visibly observable without attachment-specific UI code. */
    if (g_browser_standard_renderer) {
        char page_title[BROWSER_MAX_TITLE_LEN];
        if (cos_netsurf_browser_get_title(page_title, sizeof(page_title))) {
            gui_set_window_title(w, page_title);
        }
    }
    static char debug_last_url[BROWSER_MAX_URL_LEN];
    if (!smatch(debug_last_url, g_browser.current_url)) {
        scopy(debug_last_url, g_browser.current_url, sizeof(debug_last_url) - 1);
        serial_puts("[GUI/Browser] draw URL: ");
        serial_puts(g_browser.current_url);
        serial_puts(g_browser_standard_renderer ? " renderer=NetSurf\\n" : " renderer=legacy\\n");
    }
    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w, ch = w->h - C_TITLEBAR_H;

    if (w->browser_initial_load_pending) {
        w->browser_initial_load_pending = FALSE;
        /* The previous launch path set the NetSurf renderer flag but only
         * populated legacy text lines; no browser_window existed, so the GUI
         * correctly called redraw yet had nothing to paint. Open the compact
         * data: start document through the same upstream browser_window used
         * for every network page. */
#if COS_BROWSER_FILE_SMOKE
        static const char start_document[] = COS_BROWSER_SMOKE_START_URL;
#else
        static const char start_document[] =
            "data:text/html,%3Cmeta%20charset=utf-8%3E"
            "%3Cstyle%3E%23js-status%7Bpadding%3A8px%3Bcolor%3A%23006020%3Bfont-weight%3Abold%3B%7D%3C/style%3E"
            "%3Ch1%3EC-OS%20NetSurf%203.11%3C/h1%3E"
            "%3Cp%3E%3Ca%20href=https://www.google.com/%3EGoogle%3C/a%3E%20%7C%20"
            "%3Ca%20href=https://www.wikipedia.org/%3EWikipedia%3C/a%3E%20%7C%20"
            "%3Ca%20href=https://github.com/%3EGitHub%3C/a%3E%20%7CGitHub%3C/p%3E"
            "%3Cp%20id=js-status%3EJavaScript%20DOM%20self-test%20pending%3C/p%3E"
            "%3Cscript%3E%28function%28%29%7Btry%7Bvar%20e%3Ddocument.querySelector%28%27%23js-status%27%29%3Bvar%20k%3D%27cos-dom-self-test%27%3BlocalStorage.setItem%28k%2C%27ok%27%29%3Bvar%20ok%3DlocalStorage.getItem%28k%29%3D%3D%3D%27ok%27%3Bvar%20fired%3Dfalse%3Be.addEventListener%28%27cos-test%27%2Cfunction%28%29%7Bfired%3Dtrue%7D%29%3Be.classList.add%28%27verified%27%29%3Be.style.backgroundColor%3D%27%23d0ffd0%27%3Be.dispatchEvent%28new%20Event%28%27cos-test%27%29%29%3Be.textContent%3D%28ok%26%26fired%26%26e.classList.contains%28%27verified%27%29%29%3F%27QuickJS%20DOM%20%2B%20Event%20%2B%20Storage%20PASS%27%3A%27QuickJS%20self-test%20FAIL%27%3B%7Dcatch%28x%29%7Be.textContent%3D%27QuickJS%20self-test%20ERROR%3A%20%27%2Bx%3B%7D%7D%29%28%29%3C/script%3E";
#endif
        scopy(w->browser_url, start_document, sizeof(w->browser_url) - 1);
        /* Keep the address bar at its readable prefix instead of horizontally
         * scrolling to the end of the internal data: start document. */
        browser_url_set_cursor(w, 0);
        cos_netsurf_load_url_sync_nowait(start_document);
        g_browser.loaded = 1;
        g_browser.loading_progress = 0;
    } else if (!w->browser_url[0]) {
        browser_commit_navigation(w, "http://example.com/", true);
    } else if (!w->browser_url_focus && !w->browser_search_focus &&
               (!g_browser.loaded || !smatch(g_browser.current_url, w->browser_url))) {
        /* Do not synchronise navigation while the user is editing an address
         * or search query: the draft necessarily differs from current_url. */
        browser_commit_navigation(w, w->browser_url, false);
    }

    g_browser.loaded = 1;

    vga_fill_rect(cx, cy, cw, ch, C_WIN_BG);
    browser_render_chrome(cx, cy, cw, ch, w);

    int nav_h = 38;
    int tab_h = 26;
    int content_y = cy + nav_h + tab_h;
    int content_h = ch - nav_h - tab_h - C_STATUSBAR_H;
    if (content_h < 40) content_h = 40;
    browser_render_content(cx, content_y, cw, content_h, w);

    draw_statusbar(cx, cy + ch - C_STATUSBAR_H, cw,
                   g_browser.status[0] ? g_browser.status : "Ready",
                   g_browser.current_url[0] ? g_browser.current_url : "http://example.com/");
    /* The browser menu is owned by this window and is drawn last so it stays
     * above the NetSurf canvas, chrome and status bar. */
    browser_draw_context_menu(w);
}


void handle_browser_key(int idx, char ascii, int scancode, bool ctrl) {
    window_t* w = &windows[idx];
    const int is_printable = (ascii >= 32 && ascii <= 126);
    const int is_backspace = (scancode == KEY_BACKSPACE || ascii == '\b');
    const int is_enter = (ascii == '\r' || ascii == '\n' || scancode == KEY_ENTER);
    const int is_escape = (ascii == '\x1b' || scancode == KEY_ESC);

    if (ctrl) {
        switch (ascii) {
            /* Some keyboard paths deliver Ctrl+letter as an ASCII control
             * character rather than the printable letter.  Accept both so
             * Ctrl+L/Ctrl+K reliably focus the address bar in QEMU and on
             * physical keyboards. */
            case '\x0c': /* Ctrl+L */
            case 'l':
            case 'L':
                browser_url_begin_edit(w, true);
                w->browser_url_focus = 1;
                w->browser_search_focus = 0;
                return;
            case '\x0b': /* Ctrl+K */
            case 'k':
            case 'K':
                browser_search_begin_edit(w, true);
                w->browser_url_focus = 0;
                return;
            case '\x07': /* Ctrl+G */
            case 'g':
            case 'G':
                /* Accessible submit accelerator for the visible Ctrl+K
                 * search field; Enter remains the ordinary submit key. */
                if (w->browser_search_focus) browser_commit_search(w);
                return;
            case 'c':
            case 'C':
                if (w->browser_url_focus && w->browser_url[0]) {
                    gui_clipboard_set_text(w->browser_url);
                    scopy(g_browser.status, "URL copied", sizeof(g_browser.status) - 1);
                } else if (w->browser_search_focus && w->browser_search_text[0]) {
                    gui_clipboard_set_text(w->browser_search_text);
                    scopy(g_browser.status, "Search copied", sizeof(g_browser.status) - 1);
                }
                return;
            case 'v':
            case 'V': {
                const char* clip = gui_clipboard_get_text();
                if (clip && clip[0]) {
                    if (w->browser_url_focus) {
                        browser_url_begin_edit(w, true);
                        browser_url_replace_all(w);
                        for (int i = 0; clip[i] && i < (int)sizeof(w->browser_url) - 1; ++i) {
                            unsigned char ch = (unsigned char)clip[i];
                            if (ch >= 32 && ch <= 126) {
                                browser_url_insert_char(w, (char)ch);
                            }
                        }
                        w->browser_url_selected = 0;
                    } else {
                        browser_search_begin_edit(w, true);
                        browser_search_replace_all(w);
                        for (int i = 0; clip[i] && i < (int)sizeof(w->browser_search_text) - 1; ++i) {
                            unsigned char ch = (unsigned char)clip[i];
                            /* Search text is UTF-8. Preserve continuation and
                             * multibyte bytes so the renderer and URL encoder
                             * receive the original Japanese query. */
                            if (ch >= 0x20u && ch != 0x7Fu) {
                                browser_search_insert_char(w, (char)ch);
                            }
                        }
                        w->browser_search_selected = 0;
                    }
                }
                return;
            }
            case 'r':
            case 'R':
                browser_reload_current(w);
                return;
            case 'h':
            case 'H':
                browser_commit_navigation(w, "http://example.com/", true);
                return;
            default:
                break;
        }
    }

    /* Keep normal caret navigation inside the URL/search controls.  When the
     * page owns focus, expose the conventional document scrolling keys. */
    if (!w->browser_search_focus && !w->browser_url_focus) {
        int viewport_h = browser_content_viewport_height(w);
        if (scancode == KEY_PAGEUP) {
            (void)browser_scroll_by(w, -(viewport_h * 3) / 4);
            return;
        }
        if (scancode == KEY_PAGEDOWN) {
            (void)browser_scroll_by(w, (viewport_h * 3) / 4);
            return;
        }
        if (scancode == KEY_HOME) {
            (void)browser_scroll_by(w, -1000000);
            return;
        }
        if (scancode == KEY_END) {
            (void)browser_scroll_by(w, 1000000);
            return;
        }
    }

        if (w->browser_search_focus) {
        if (is_enter) {
            browser_commit_search(w);
            return;
        }
        if (is_escape) {
            w->browser_search_focus = 0;
            w->browser_search_selected = 0;
            return;
        }
        if (scancode == KEY_LEFT)  { browser_search_move_cursor(w, -1); w->browser_search_selected = 0; return; }
        if (scancode == KEY_RIGHT) { browser_search_move_cursor(w, +1); w->browser_search_selected = 0; return; }
        if (scancode == KEY_HOME)  { browser_search_set_cursor(w, 0); w->browser_search_selected = 0; return; }
        if (scancode == KEY_END)   { browser_search_set_cursor(w, browser_search_length(w)); w->browser_search_selected = 0; return; }
        if (scancode == KEY_DELETE) { browser_search_delete_forward(w); w->browser_search_selected = 0; return; }
        if (w->browser_search_selected && (is_printable || is_backspace)) browser_search_replace_all(w);
        if (is_backspace) { browser_search_backspace(w); w->browser_search_selected = 0; return; }
        if (is_printable) { browser_search_insert_char(w, ascii); w->browser_search_selected = 0; return; }
        return;
    }
    if (w->browser_url_focus) {
        if (is_enter) {
            browser_commit_navigation(w, w->browser_url, true);
            return;
        }
        if (is_escape) {
            w->browser_url_focus = 0;
            w->browser_url_selected = 0;
            return;
        }

        if (scancode == KEY_LEFT)  { browser_url_move_cursor(w, -1); w->browser_url_selected = 0; return; }
        if (scancode == KEY_RIGHT) { browser_url_move_cursor(w, +1); w->browser_url_selected = 0; return; }
        if (scancode == KEY_HOME)  { browser_url_set_cursor(w, 0); w->browser_url_selected = 0; return; }
        if (scancode == KEY_END)   { browser_url_set_cursor(w, browser_url_length(w)); w->browser_url_selected = 0; return; }
        if (scancode == KEY_DELETE) { browser_url_delete_forward(w); w->browser_url_selected = 0; return; }

        if (w->browser_url_selected && (is_printable || is_backspace)) {
            browser_url_replace_all(w);
        }

        if (is_backspace) {
            browser_url_backspace(w);
            w->browser_url_selected = 0;
            return;
        }
        if (is_printable) {
            browser_url_insert_char(w, ascii);
            w->browser_url_selected = 0;
            return;
        }

        return;
    }

    if (g_browser_standard_renderer && (is_printable || is_backspace || is_enter)) {
        uint32_t ns_key = is_backspace ? 8u : (is_enter ? 13u : (uint32_t)(unsigned char)ascii);
        if (cos_netsurf_browser_keypress(ns_key)) {
            scopy(g_browser.status, "NetSurf 3.11 input", sizeof(g_browser.status) - 1);
        }
        return;
    }
    if (is_printable || is_backspace || is_enter) {
        if (is_enter) {
            browser_commit_navigation(w, w->browser_url, true);
            return;
        }
        browser_url_begin_edit(w, true);
        browser_url_replace_all(w);
        if (is_printable) {
            browser_url_insert_char(w, ascii);
        } else if (is_backspace) {
            browser_url_backspace(w);
        }
        w->browser_url_selected = 0;
        w->browser_search_focus = 0;
        w->browser_search_selected = 0;
        return;
    }

    switch (ascii) {
        case 'r':
        case 'R':
            browser_reload_current(w);
            break;
        case 'l':
        case 'L':
            browser_url_begin_edit(w, true);
            break;
        default:
            break;
    }
}


void handle_browser_click(int idx, int mx, int my) {

    window_t* w = &windows[idx];
    if (w->browser_context_visible) {
        int item = browser_context_item_at(w, mx, my);
        if (item >= 0 && item != BROWSER_CONTEXT_SEPARATOR) {
            browser_context_menu_invoke(w, item);
        } else {
            browser_context_menu_close(w);
            gui_request_redraw();
        }
        return;
    }
    int cx = w->x, cy = w->y + C_TITLEBAR_H;
    int cw = w->w;
    int ch = w->h - C_TITLEBAR_H;
    int nav_h = 38;
    int tab_h = 26;

    const int control_y = cy + 8;
    const int control_w = 22;
    const int control_gap = 4;
    const int back_x = cx + 8;
    const int forward_x = back_x + control_w + control_gap;
    const int reload_x = forward_x + control_w + control_gap;
    if (!w->browser_search_focus && my >= control_y && my < control_y + 24) {
        if (mx >= back_x && mx < back_x + control_w) {
            serial_puts("[GUI/Browser] navigation control: back\n");
            browser_go_history(w, -1);
            return;
        }
        if (mx >= forward_x && mx < forward_x + control_w) {
            serial_puts("[GUI/Browser] navigation control: forward\n");
            browser_go_history(w, +1);
            return;
        }
        if (mx >= reload_x && mx < reload_x + control_w) {
            serial_puts("[GUI/Browser] navigation control: reload\n");
            browser_reload_current(w);
            return;
        }
    }

    int bx = reload_x + control_w + 8;
    int url_w = cx + cw - 12 - bx;
    if (url_w < 120) url_w = 120;
    if (mx >= bx && mx < bx + url_w && my >= cy + 7 && my < cy + 33) {
        browser_url_begin_edit(w, false);
        w->browser_search_focus = 0;
        browser_url_move_cursor_to_click(w, bx, url_w, mx);
        return;
    }
    if (my >= cy + nav_h + tab_h && my < cy + ch - C_STATUSBAR_H) {
        int content_y = cy + nav_h + tab_h;
        if (g_browser_standard_renderer) {
            /* The local data: home document has a deliberately fixed,
             * lightweight quick-link row.  Map its rendered anchor cells to
             * normal browser_commit_navigation() requests as a reliable C-OS
             * input fallback while still letting NetSurf own every destination
             * fetch, DOM conversion, layout, form and image operation. */
            if (sstartswith(g_browser.current_url, "data:text/html")) {
                int page_x = mx - cx;
                int page_y = my - content_y;
                /* NetSurf's data: document may receive a slightly different
                 * font ascent depending on the active VGA font.  Keep the
                 * fallback aligned with the visible quick-link row, but use a
                 * generous vertical band so a real click on the painted text
                 * cannot degrade into a no-op. */
                if (page_y >= 52 && page_y < 104) {
                    const char* target = NULL;
                    /* The start document renders its anchors in a single
                     * horizontal row: Google | Wikipedia | GitHub | Pixiv.
                     * Keep the fallback hit regions aligned to those painted
                     * cells so a visible Wikipedia click cannot open Google. */
                    if (page_x >= 8 && page_x < 76) {
                        target = "https://www.google.com/";
                    } else if (page_x >= 76 && page_x < 176) {
                        target = "https://www.wikipedia.org/";
                    } else if (page_x >= 176 && page_x < 252) {
                        target = "https://github.com/";
                    } else if (page_x >= 252 && page_x < 324) {
                        target = "https://www.pixiv.net/";
                    }
                    if (target != NULL) {
                        serial_puts("[NetSurf/Home] quick link selected -> ");
                        serial_puts(target);
                        serial_puts("\n");
                        browser_commit_navigation(w, target, true);
                        return;
                    }
                }
            }
            /* browser_window_redraw() translates document boxes into the GUI
             * canvas at (cx, content_y).  Input must use the inverse transform:
             * viewport-local coordinates.  Passing raw desktop coordinates
             * selected BODY below controls whose document y is near zero. */
            int ns_x = mx - cx;
            int ns_y = my - content_y;
            serial_puts("[NetSurf] click x=");
            serial_putdec((uint64_t)(ns_x < 0 ? 0 : ns_x));
            serial_puts(" y=");
            serial_putdec((uint64_t)(ns_y < 0 ? 0 : ns_y));
            serial_puts("\n");
            cos_netsurf_browser_click(ns_x, ns_y);
            scopy(g_browser.status, "NetSurf 3.11 navigating", sizeof(g_browser.status) - 1);
            return;
        }
        for (int i = 0; i < g_browser.link_count; i++) {
            browser_link_t* link = &g_browser.links[i];
            if (link->url[0] == '\0' || link->w <= 0 || link->h <= 0) continue;
            if (mx >= link->x && mx < link->x + link->w &&
                my >= link->y && my < link->y + link->h) {
                if (link->kind == 1) {
                    (void)browser_submit_form_link(w, link);
                } else {
                    browser_commit_navigation(w, link->url, true);
                }
                return;
            }
        }

        int local_y = my - (content_y + 10) + w->browser_scroll;
        int cur_y = 0;
        for (int i = 0; i < g_browser.line_count; i++) {
            const browser_line_t* line = &g_browser.lines[i];
            int line_h = line->height;
            if (local_y >= cur_y && local_y < cur_y + line_h && line->kind == BROWSER_LINE_LINK && line->link_index >= 0 && line->link_index < g_browser.link_count) {
                browser_link_t* link = &g_browser.links[line->link_index];
                if (link->kind == 1) (void)browser_submit_form_link(w, link);
                else browser_commit_navigation(w, link->url, true);
                return;
            }
            cur_y += line_h;
        }
    }

    w->browser_url_focus = 0;
    w->browser_url_selected = 0;
}

void handle_browser_wheel(int idx, int wheel_delta) {
    if (idx < 0 || idx >= window_count || wheel_delta == 0) return;
    window_t* w = &windows[idx];
    if (w->kind != WIN_BROWSER || !w->visible || w->minimized) return;
    /* Match the GUI-wide natural wheel convention: a positive PS/2 detent
     * moves toward the document top, and a negative detent moves downward. */
    int delta_y = -wheel_delta * 72;
    if (browser_scroll_by(w, delta_y)) {
        serial_puts("[GUI/Browser] wheel scroll dy=");
        serial_putdec((uint64_t)(delta_y < 0 ? -delta_y : delta_y));
        serial_puts("\n");
        gui_request_redraw();
    }
}

void handle_browser_right_click(int idx, int mx, int my) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    if (w->kind != WIN_BROWSER || !w->visible || w->minimized) return;

    const int menu_h = browser_context_menu_height();
    int min_x = w->x + 2;
    int max_x = w->x + w->w - BROWSER_CONTEXT_MENU_W - 2;
    int min_y = w->y + C_TITLEBAR_H + 2;
    int max_y = w->y + w->h - C_STATUSBAR_H - menu_h - 2;
    if (max_x < min_x) max_x = min_x;
    if (max_y < min_y) max_y = min_y;
    int menu_x = mx;
    int menu_y = my;
    if (menu_x < min_x) menu_x = min_x;
    if (menu_x > max_x) menu_x = max_x;
    if (menu_y < min_y) menu_y = min_y;
    if (menu_y > max_y) menu_y = max_y;

    /* The shared desktop/window menu cannot remain above a browser-owned menu. */
    ctx_menu.visible = FALSE;
    w->browser_context_x = menu_x;
    w->browser_context_y = menu_y;
    w->browser_context_page_x = mx;
    w->browser_context_page_y = my;
    w->browser_context_visible = true;
    w->browser_url_focus = 0;
    w->browser_url_selected = 0;
    w->browser_search_focus = 0;
    w->browser_search_selected = 0;
    serial_puts("[GUI/Browser] context menu opened\n");
    gui_request_redraw();
}

/* window_kind_name() は gui_apps_common.c に移動済み（taskmanagerからも使用） */

