/**
 * modern_browser.c - C-OS modern browser
 *
 * A small browser implementation that can open HTTP pages and display
 * readable text from HTML pages.
 */

#include "modern_browser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vga.h>
#include <serial.h>

extern int http_fetch(const char* url, char* buffer, size_t max_len);
extern bool net_is_connected(void);
extern int cos_fs_read_file(const char* path, void* buffer, uint64_t size);

#ifndef NULL
#define NULL ((void*)0)
#endif

#define MB_MAX_URL       2048
#define MB_MAX_TITLE      256
#define MB_MAX_CONTENT   32768
#define MB_MAX_HISTORY     32
#define MB_MAX_LINES      384
#define MB_LINE_WIDTH     128
#define MB_SCROLL_STEP     3

#define MB_COLOR_BG       0x00101820u
#define MB_COLOR_BAR      0x002A3240u
#define MB_COLOR_PANEL    0x001A2230u
#define MB_COLOR_TEXT     0x00FFFFFFu
#define MB_COLOR_SUBTEXT  0x00D0D7E2u
#define MB_COLOR_HILITE   0x004D9FFFu
#define MB_COLOR_ERROR    0x00FF9A9Au

struct modern_browser_t {
    bool initialized;
    browser_state_t state;

    char url[MB_MAX_URL];
    char title[MB_MAX_TITLE];
    char status[128];

    char raw_html[MB_MAX_CONTENT];
    char content[MB_MAX_CONTENT];
    char lines[MB_MAX_LINES][MB_LINE_WIDTH];
    int line_count;

    char history[MB_MAX_HISTORY][MB_MAX_URL];
    int history_count;
    int history_pos;

    int scroll_y;
    int max_scroll;
    bool network_ok;

    char address_bar[MB_MAX_URL];
    int address_len;
    bool address_focused;
};

static modern_browser_t g_br;
static const char* HOME_URL = "c-os://home";
static const char* GOOGLE_SEARCH_URL = "https://www.google.com/search?hl=ja&gbv=1&q=";

static void mb_copy_text(char* dst, size_t dst_sz, const char* src) {
    if (!dst || dst_sz == 0) return;
    size_t i = 0;
    if (src) {
        while (i + 1 < dst_sz && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

static void mb_append_text(char* dst, size_t dst_sz, const char* src) {
    if (!dst || dst_sz == 0 || !src) return;
    size_t len = strlen(dst);
    if (len >= dst_sz - 1) return;
    size_t i = 0;
    while (len + i + 1 < dst_sz && src[i]) {
        dst[len + i] = src[i];
        ++i;
    }
    dst[len + i] = '\0';
}

static bool mb_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\0';
}

static void mb_trim(char* s) {
    if (!s) return;
    size_t start = 0;
    while (s[start] && mb_is_space(s[start])) ++start;
    size_t end = strlen(s);
    while (end > start && mb_is_space(s[end - 1])) --end;
    size_t out = 0;
    while (start < end) s[out++] = s[start++];
    s[out] = '\0';
}

static bool mb_starts_with_ci(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        char a = *s++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static bool mb_contains(const char* s, char needle) {
    if (!s) return false;
    while (*s) {
        if (*s == needle) return true;
        ++s;
    }
    return false;
}

static bool mb_is_absolute_url(const char* s) {
    return mb_starts_with_ci(s, "http://") ||
           mb_starts_with_ci(s, "https://") ||
           mb_starts_with_ci(s, "about:") ||
           mb_starts_with_ci(s, "file://") ||
           mb_starts_with_ci(s, "javascript:") ||
           mb_starts_with_ci(s, "data:");
}


static bool mb_is_local_file_url(const char* s) {
    return s && mb_starts_with_ci(s, "file://");
}

static void mb_strip_file_scheme(const char* url, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!url) return;
    const char* p = url;
    if (mb_starts_with_ci(p, "file://")) {
        p += 7;
        while (*p == '/') {
            /* accept file:///path and file://path */
            if (p[1] == '/') break;
            if (p[1] != '/') break;
            p++;
        }
    }
    mb_copy_text(out, out_sz, p);
}

static void mb_url_encode(const char* src, char* dst, size_t dst_sz) {
    static const char hex[] = "0123456789ABCDEF";
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dst_sz; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == ' ') {
            dst[out++] = '+';
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[out++] = (char)c;
        } else {
            if (out + 3 >= dst_sz) break;
            dst[out++] = '%';
            dst[out++] = hex[(c >> 4) & 0x0F];
            dst[out++] = hex[c & 0x0F];
        }
    }
    dst[out] = '\0';
}

static void mb_normalize_url(const char* raw, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';

    char input[MB_MAX_URL];
    mb_copy_text(input, sizeof(input), raw ? raw : "");
    mb_trim(input);

    if (input[0] == '\0') {
        mb_copy_text(out, out_sz, HOME_URL);
        return;
    }

    if (mb_is_absolute_url(input)) {
        mb_copy_text(out, out_sz, input);
        return;
    }

    if (input[0] == '/' || input[0] == '.') {
        mb_copy_text(out, out_sz, "file://");
        mb_append_text(out, out_sz, input);
        return;
    }

    if (mb_starts_with_ci(input, "www.") || mb_contains(input, '.') || mb_contains(input, ':')) {
        mb_copy_text(out, out_sz, "https://");
        mb_append_text(out, out_sz, input);
        return;
    }

    char encoded[MB_MAX_URL * 3];
    mb_url_encode(input, encoded, sizeof(encoded));
    mb_copy_text(out, out_sz, GOOGLE_SEARCH_URL);
    mb_append_text(out, out_sz, encoded);
}

static void mb_decode_entity(char* dst, size_t dst_sz, size_t* used, const char* src, size_t* consumed) {
    if (!dst || !used || !src || !consumed) return;
    *consumed = 1;
    if (*used + 1 >= dst_sz) return;

    if (strncmp(src, "&lt;", 4) == 0) { dst[(*used)++] = '<'; *consumed = 4; return; }
    if (strncmp(src, "&gt;", 4) == 0) { dst[(*used)++] = '>'; *consumed = 4; return; }
    if (strncmp(src, "&amp;", 5) == 0) { dst[(*used)++] = '&'; *consumed = 5; return; }
    if (strncmp(src, "&quot;", 6) == 0) { dst[(*used)++] = '"'; *consumed = 6; return; }
    if (strncmp(src, "&nbsp;", 6) == 0) { dst[(*used)++] = ' '; *consumed = 6; return; }
    if (strncmp(src, "&#39;", 5) == 0) { dst[(*used)++] = '\''; *consumed = 5; return; }
    dst[(*used)++] = *src;
}

static bool mb_is_block_tag(const char* tag) {
    if (!tag || !tag[0]) return false;
    return strcmp(tag, "p") == 0 || strcmp(tag, "div") == 0 || strcmp(tag, "li") == 0 ||
           strcmp(tag, "br") == 0 || strcmp(tag, "hr") == 0 || strcmp(tag, "tr") == 0 ||
           strcmp(tag, "td") == 0 || strcmp(tag, "th") == 0 || strcmp(tag, "section") == 0 ||
           strcmp(tag, "article") == 0 || strcmp(tag, "header") == 0 || strcmp(tag, "footer") == 0 ||
           (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6');
}


#define MB_DOM_MAX_NODES   1024
#define MB_DOM_TAG_MAX     32
#define MB_DOM_TEXT_MAX    256

typedef enum {
    MB_DOM_NODE_OPEN = 0,
    MB_DOM_NODE_CLOSE = 1,
    MB_DOM_NODE_TEXT = 2
} mb_dom_node_kind_t;

typedef struct {
    mb_dom_node_kind_t kind;
    int depth;
    bool block;
    bool self_closing;
    char tag[MB_DOM_TAG_MAX];
    char text[MB_DOM_TEXT_MAX];
    char href[MB_MAX_URL];
    char alt[MB_DOM_TEXT_MAX];
} mb_dom_node_t;

static mb_dom_node_t g_dom_nodes[MB_DOM_MAX_NODES];
static int g_dom_node_count = 0;
static char g_dom_links[64][MB_MAX_URL];
static int g_dom_link_count = 0;
static bool g_dom_title_overridden = false;
static bool g_dom_content_overridden = false;
static char g_dom_title_override[MB_DOM_TEXT_MAX * 2];
static char g_dom_content_override[MB_MAX_CONTENT];

/* Forward declarations for DOM-aware rendering helpers. */
static void mb_add_line(const char* text);
static void mb_content_from_lines(void);
static void mb_emit_wrapped_line(const char* src);

static bool mb_is_void_tag(const char* tag) {
    if (!tag || !tag[0]) return false;
    return strcmp(tag, "br") == 0 || strcmp(tag, "hr") == 0 ||
           strcmp(tag, "img") == 0 || strcmp(tag, "input") == 0 ||
           strcmp(tag, "meta") == 0 || strcmp(tag, "link") == 0 ||
           strcmp(tag, "area") == 0 || strcmp(tag, "base") == 0 ||
           strcmp(tag, "col") == 0 || strcmp(tag, "embed") == 0 ||
           strcmp(tag, "param") == 0 || strcmp(tag, "source") == 0 ||
           strcmp(tag, "track") == 0 || strcmp(tag, "wbr") == 0;
}

static void mb_dom_reset(void) {
    g_dom_node_count = 0;
    g_dom_link_count = 0;
    g_dom_title_overridden = false;
    g_dom_content_overridden = false;
    g_dom_title_override[0] = '\0';
    g_dom_content_override[0] = '\0';
    for (int i = 0; i < 64; ++i) g_dom_links[i][0] = '\0';
}

static void mb_dom_copy_text(char* dst, size_t dst_sz, const char* src) {
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t i = 0;
    while (src[i] && i + 1 < dst_sz) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void mb_dom_copy_lower_tag(const char* src, char* dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t i = 0;
    while (src[i] && i + 1 < dst_sz) {
        char c = src[i++];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[i - 1] = c;
    }
    dst[i] = '\0';
}

static void mb_dom_trim(char* s) {
    if (!s) return;
    size_t start = 0;
    while (s[start] && mb_is_space(s[start])) ++start;
    size_t end = strlen(s);
    while (end > start && mb_is_space(s[end - 1])) --end;
    size_t out = 0;
    while (start < end) s[out++] = s[start++];
    s[out] = '\0';
}

static void mb_dom_decode_entity(char* dst, size_t dst_sz, size_t* used, const char* src, size_t* consumed) {
    if (!dst || !used || !src || !consumed) return;
    *consumed = 1;
    if (*used + 1 >= dst_sz) return;

    if (strncmp(src, "&lt;", 4) == 0) { dst[(*used)++] = '<'; *consumed = 4; return; }
    if (strncmp(src, "&gt;", 4) == 0) { dst[(*used)++] = '>'; *consumed = 4; return; }
    if (strncmp(src, "&amp;", 5) == 0) { dst[(*used)++] = '&'; *consumed = 5; return; }
    if (strncmp(src, "&quot;", 6) == 0) { dst[(*used)++] = '"'; *consumed = 6; return; }
    if (strncmp(src, "&nbsp;", 6) == 0) { dst[(*used)++] = ' '; *consumed = 6; return; }
    if (strncmp(src, "&#39;", 5) == 0) { dst[(*used)++] = '\''; *consumed = 5; return; }
    dst[(*used)++] = *src;
}

static int mb_dom_add_node(mb_dom_node_kind_t kind, int depth) {
    if (g_dom_node_count >= MB_DOM_MAX_NODES) return -1;
    mb_dom_node_t* node = &g_dom_nodes[g_dom_node_count];
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->depth = depth;
    return g_dom_node_count++;
}

static bool mb_dom_attr_value(const char* start, const char* end, const char* attr, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    if (!start || !end || end <= start || !attr || !attr[0]) return false;

    size_t attr_len = strlen(attr);
    const char* p = start;
    while (p < end) {
        while (p < end && mb_is_space(*p)) ++p;
        if (p >= end || *p == '/' || *p == '>') break;

        const char* name_start = p;
        while (p < end && !mb_is_space(*p) && *p != '=' && *p != '/' && *p != '>') ++p;
        size_t name_len = (size_t)(p - name_start);

        while (p < end && mb_is_space(*p)) ++p;
        if (p >= end || *p != '=') continue;
        ++p;
        while (p < end && mb_is_space(*p)) ++p;

        const char* value_start = p;
        const char* value_end = p;
        if (p < end && (*p == '"' || *p == '\'')) {
            char quote = *p++;
            value_start = p;
            while (p < end && *p != quote) ++p;
            value_end = p;
            if (p < end && *p == quote) ++p;
        } else {
            while (p < end && !mb_is_space(*p) && *p != '/' && *p != '>') ++p;
            value_end = p;
        }

        if (name_len == attr_len) {
            bool match = true;
            for (size_t i = 0; i < attr_len; ++i) {
                char a = name_start[i];
                char b = attr[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) { match = false; break; }
            }
            if (match) {
                size_t len = (size_t)(value_end - value_start);
                if (len >= out_sz) len = out_sz - 1;
                memcpy(out, value_start, len);
                out[len] = '\0';
                return true;
            }
        }
    }
    return false;
}

static void mb_dom_store_link(const char* href) {
    if (!href || !href[0]) return;
    for (int i = 0; i < g_dom_link_count; ++i) {
        if (strcmp(g_dom_links[i], href) == 0) return;
    }
    if (g_dom_link_count >= 64) return;
    mb_dom_copy_text(g_dom_links[g_dom_link_count], sizeof(g_dom_links[g_dom_link_count]), href);
    ++g_dom_link_count;
}

static void mb_dom_strip_html_fragment(const char* html, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!html) return;

    int in_tag = 0;
    size_t out_len = 0;
    int last_space = 1;
    for (const char* p = html; *p && out_len + 1 < out_sz; ++p) {
        if (*p == '<') { in_tag = 1; continue; }
        if (*p == '>') { in_tag = 0; continue; }
        if (in_tag) continue;

        char c = *p;
        if (c == '&') {
            if (strncmp(p, "&lt;", 4) == 0) { c = '<'; p += 3; }
            else if (strncmp(p, "&gt;", 4) == 0) { c = '>'; p += 3; }
            else if (strncmp(p, "&amp;", 5) == 0) { c = '&'; p += 4; }
            else if (strncmp(p, "&quot;", 6) == 0) { c = '"'; p += 5; }
            else if (strncmp(p, "&nbsp;", 6) == 0) { c = ' '; p += 5; }
            else if (strncmp(p, "&#39;", 5) == 0) { c = '\''; p += 4; }
        }

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!last_space) {
                out[out_len++] = ' ';
                last_space = 1;
            }
        } else {
            out[out_len++] = c;
            last_space = 0;
        }
    }

    out[out_len] = '\0';
    mb_dom_trim(out);
}

static void mb_dom_set_title_override(const char* title) {
    g_dom_title_overridden = true;
    mb_dom_copy_text(g_dom_title_override, sizeof(g_dom_title_override), title ? title : "");
    mb_dom_trim(g_dom_title_override);
    mb_copy_text(g_br.title, sizeof(g_br.title), g_dom_title_override);
}

static void mb_dom_set_content_override(const char* html_or_text, bool as_html) {
    g_dom_content_overridden = true;
    if (as_html) {
        mb_dom_strip_html_fragment(html_or_text, g_dom_content_override, sizeof(g_dom_content_override));
    } else {
        mb_dom_copy_text(g_dom_content_override, sizeof(g_dom_content_override), html_or_text ? html_or_text : "");
        mb_dom_trim(g_dom_content_override);
    }
}

static const char* mb_js_read_literal(const char* p, char* out, size_t out_sz) {
    if (!p || !out || out_sz == 0) return p;
    out[0] = '\0';
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p == '(') {
        ++p;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    }
    char quote = 0;
    if (*p == '"' || *p == '\'' || *p == '`') quote = *p++;
    size_t out_len = 0;
    while (*p) {
        if (quote) {
            if (*p == quote) { ++p; break; }
            if (*p == '\\' && p[1]) {
                ++p;
                char c = *p++;
                switch (c) {
                    case 'n': c = '\0'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    default: break;
                }
                if (out_len + 1 < out_sz) out[out_len++] = c;
                continue;
            }
            if (out_len + 1 < out_sz) out[out_len++] = *p++;
            else ++p;
        } else {
            if (*p == ';' || *p == '\n' || *p == '\r' || *p == ')') break;
            if (out_len + 1 < out_sz) out[out_len++] = *p;
            ++p;
        }
    }
    out[out_len] = '\0';
    mb_dom_trim(out);
    return p;
}

static void mb_js_apply_statement(const char* stmt, char* redirect_url, size_t redirect_sz) {
    if (!stmt || !stmt[0] || !redirect_url || redirect_sz == 0) return;
    char work[2048];
    mb_dom_copy_text(work, sizeof(work), stmt);
    mb_dom_trim(work);
    if (!work[0]) return;

    while (strncmp(work, "window.", 7) == 0 || strncmp(work, "globalThis.", 11) == 0) {
        size_t off = (strncmp(work, "window.", 7) == 0) ? 7 : 11;
        memmove(work, work + off, strlen(work + off) + 1);
        mb_dom_trim(work);
    }

    if (strncmp(work, "document.title", 14) == 0) {
        const char* eq = strchr(work, '=');
        if (eq) {
            char value[MB_DOM_TEXT_MAX * 2];
            mb_js_read_literal(eq + 1, value, sizeof(value));
            if (value[0]) mb_dom_set_title_override(value);
        }
        return;
    }

    if (strncmp(work, "location.replace", 16) == 0 || strncmp(work, "location.assign", 15) == 0) {
        const char* p = strchr(work, '(');
        if (p) {
            char value[MB_MAX_URL];
            mb_js_read_literal(p + 1, value, sizeof(value));
            if (value[0]) mb_copy_text(redirect_url, redirect_sz, value);
        }
        return;
    }

    if (strncmp(work, "location.href", 13) == 0 || strncmp(work, "location", 8) == 0 ||
        strncmp(work, "document.location.href", 22) == 0 || strncmp(work, "document.location", 17) == 0) {
        const char* eq = strchr(work, '=');
        if (eq) {
            char value[MB_MAX_URL];
            mb_js_read_literal(eq + 1, value, sizeof(value));
            if (value[0]) mb_copy_text(redirect_url, redirect_sz, value);
        }
        return;
    }

    if (strncmp(work, "document.write", 14) == 0) {
        const char* p = strchr(work, '(');
        if (p) {
            char value[MB_MAX_CONTENT];
            mb_js_read_literal(p + 1, value, sizeof(value));
            if (value[0]) mb_dom_set_content_override(value, true);
        }
        return;
    }

    if (strncmp(work, "document.body.innerHTML", 23) == 0) {
        const char* eq = strchr(work, '=');
        if (eq) {
            char value[MB_MAX_CONTENT];
            mb_js_read_literal(eq + 1, value, sizeof(value));
            if (value[0]) mb_dom_set_content_override(value, true);
        }
        return;
    }

    if (strncmp(work, "document.body.textContent", 25) == 0) {
        const char* eq = strchr(work, '=');
        if (eq) {
            char value[MB_MAX_CONTENT];
            mb_js_read_literal(eq + 1, value, sizeof(value));
            if (value[0]) mb_dom_set_content_override(value, false);
        }
        return;
    }
}

static void mb_js_execute_inline_scripts(const char* html, char* redirect_url, size_t redirect_sz) {
    if (!html || !redirect_url || redirect_sz == 0) return;
    redirect_url[0] = '\0';

    const char* p = html;
    while ((p = strstr(p, "<script")) != NULL) {
        const char* tag_end = strchr(p, '>');
        if (!tag_end) break;
        const char* close = strstr(tag_end + 1, "</script>");
        if (!close) break;

        char script[8192];
        size_t len = (size_t)(close - (tag_end + 1));
        if (len >= sizeof(script)) len = sizeof(script) - 1;
        memcpy(script, tag_end + 1, len);
        script[len] = '\0';

        char stmt[2048];
        size_t si = 0;
        for (size_t i = 0; script[i] && !redirect_url[0]; ++i) {
            char c = script[i];
            if (c == ';' || c == '\n' || c == '\r') {
                stmt[si] = '\0';
                mb_js_apply_statement(stmt, redirect_url, redirect_sz);
                si = 0;
                continue;
            }
            if (si + 1 < sizeof(stmt)) stmt[si++] = c;
        }
        if (!redirect_url[0] && si > 0) {
            stmt[si] = '\0';
            mb_js_apply_statement(stmt, redirect_url, redirect_sz);
        }

        if (redirect_url[0]) return;
        p = close + 9;
    }
}

static void mb_dom_parse_html(const char* html) {
    mb_dom_reset();
    if (!html) return;

    int depth = 0;
    char open_tags[64][MB_DOM_TAG_MAX];
    int open_count = 0;
    bool in_skip = false;
    char skip_tag[MB_DOM_TAG_MAX] = {0};

    char text_buf[MB_DOM_TEXT_MAX * 2];
    size_t text_len = 0;

    for (size_t i = 0; html[i];) {
        if (in_skip) {
            if (html[i] == '<' && html[i + 1] == '/') {
                const char* p = &html[i + 2];
                char close_tag[MB_DOM_TAG_MAX];
                size_t close_len = 0;
                while (*p && !mb_is_space(*p) && *p != '>' && close_len + 1 < sizeof(close_tag)) {
                    char c = *p++;
                    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                    close_tag[close_len++] = c;
                }
                close_tag[close_len] = '\0';
                if (strcmp(close_tag, skip_tag) == 0) in_skip = false;
            }
            ++i;
            continue;
        }

        if (html[i] == '<') {
            if (text_len > 0) {
                text_buf[text_len] = '\0';
                mb_dom_trim(text_buf);
                if (text_buf[0]) {
                    int idx = mb_dom_add_node(MB_DOM_NODE_TEXT, depth);
                    if (idx >= 0) mb_dom_copy_text(g_dom_nodes[idx].text, sizeof(g_dom_nodes[idx].text), text_buf);
                }
                text_len = 0;
            }

            const char* tag_start = &html[i + 1];
            const char* p = tag_start;
            bool closing = false;
            bool self_closing = false;

            while (*p && mb_is_space(*p)) ++p;
            if (*p == '/') { closing = true; ++p; }
            while (*p && mb_is_space(*p)) ++p;

            char tag[MB_DOM_TAG_MAX];
            size_t tag_len = 0;
            while (*p && !mb_is_space(*p) && *p != '>' && *p != '/' && tag_len + 1 < sizeof(tag)) {
                char c = *p++;
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                tag[tag_len++] = c;
            }
            tag[tag_len] = '\0';

            const char* tag_end = strchr(p, '>');
            if (!tag[0] || !tag_end) { ++i; continue; }

            if (tag_end > tag_start && tag_end[-1] == '/') self_closing = true;
            if (tag[0] == '!' || tag[0] == '?') { i = (size_t)(tag_end - html) + 1; continue; }

            if (closing) {
                int close_depth = depth > 0 ? depth - 1 : 0;
                for (int s = open_count - 1; s >= 0; --s) {
                    if (strcmp(open_tags[s], tag) == 0) {
                        close_depth = s;
                        open_count = s;
                        depth = s;
                        break;
                    }
                }
                int idx = mb_dom_add_node(MB_DOM_NODE_CLOSE, close_depth);
                if (idx >= 0) mb_dom_copy_text(g_dom_nodes[idx].tag, sizeof(g_dom_nodes[idx].tag), tag);
                if (strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0) in_skip = false;
            } else {
                int idx = mb_dom_add_node(MB_DOM_NODE_OPEN, depth);
                if (idx >= 0) {
                    mb_dom_copy_text(g_dom_nodes[idx].tag, sizeof(g_dom_nodes[idx].tag), tag);
                    g_dom_nodes[idx].block = mb_is_block_tag(tag);
                    g_dom_nodes[idx].self_closing = self_closing || mb_is_void_tag(tag);
                    mb_dom_attr_value(tag_start, tag_end, "href", g_dom_nodes[idx].href, sizeof(g_dom_nodes[idx].href));
                    mb_dom_attr_value(tag_start, tag_end, "alt", g_dom_nodes[idx].alt, sizeof(g_dom_nodes[idx].alt));
                    if (strcmp(tag, "a") == 0 && g_dom_nodes[idx].href[0]) {
                        mb_dom_store_link(g_dom_nodes[idx].href);
                    }
                }

                if (strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0) {
                    in_skip = true;
                    mb_dom_copy_text(skip_tag, sizeof(skip_tag), tag);
                }

                if (!self_closing && !mb_is_void_tag(tag) && open_count + 1 < (int)(sizeof(open_tags) / sizeof(open_tags[0]))) {
                    mb_dom_copy_text(open_tags[open_count], sizeof(open_tags[open_count]), tag);
                    ++open_count;
                    ++depth;
                } else {
                    int close_idx = mb_dom_add_node(MB_DOM_NODE_CLOSE, depth);
                    if (close_idx >= 0) mb_dom_copy_text(g_dom_nodes[close_idx].tag, sizeof(g_dom_nodes[close_idx].tag), tag);
                }
            }

            i = (size_t)(tag_end - html) + 1;
            continue;
        }

        if (html[i] == '&') {
            size_t consumed = 1;
            mb_dom_decode_entity(text_buf, sizeof(text_buf), &text_len, &html[i], &consumed);
            i += consumed;
            continue;
        }

        if (html[i] == '\r' || html[i] == '\t' || html[i] == '\n') {
            if (text_len + 1 < sizeof(text_buf) && (text_len == 0 || text_buf[text_len - 1] != ' ')) {
                text_buf[text_len++] = ' ';
            }
            ++i;
            continue;
        }

        if (text_len + 1 < sizeof(text_buf)) {
            text_buf[text_len++] = html[i];
        }
        ++i;
    }

    if (text_len > 0) {
        text_buf[text_len] = '\0';
        mb_dom_trim(text_buf);
        if (text_buf[0]) {
            int idx = mb_dom_add_node(MB_DOM_NODE_TEXT, depth);
            if (idx >= 0) mb_dom_copy_text(g_dom_nodes[idx].text, sizeof(g_dom_nodes[idx].text), text_buf);
        }
    }

    if (g_dom_node_count == 0) {
        int idx = mb_dom_add_node(MB_DOM_NODE_TEXT, 0);
        if (idx >= 0) mb_dom_copy_text(g_dom_nodes[idx].text, sizeof(g_dom_nodes[idx].text), "(no readable text found)");
    }
}

static void mb_dom_collect_title(void) {
    if (g_dom_title_overridden) {
        mb_copy_text(g_br.title, sizeof(g_br.title), g_dom_title_override);
        return;
    }
    g_br.title[0] = '\0';
    bool in_title = false;
    for (int i = 0; i < g_dom_node_count; ++i) {
        mb_dom_node_t* n = &g_dom_nodes[i];
        if (n->kind == MB_DOM_NODE_OPEN && strcmp(n->tag, "title") == 0) {
            in_title = true;
        } else if (n->kind == MB_DOM_NODE_CLOSE && strcmp(n->tag, "title") == 0) {
            in_title = false;
        } else if (in_title && n->kind == MB_DOM_NODE_TEXT && n->text[0]) {
            if (g_br.title[0]) mb_append_text(g_br.title, sizeof(g_br.title), " ");
            mb_append_text(g_br.title, sizeof(g_br.title), n->text);
        }
    }
    mb_dom_trim(g_br.title);
}

static void mb_append_normalized(char* line, size_t line_sz, size_t* len, const char* src) {
    if (!line || !len || !src) return;
    for (size_t i = 0; src[i]; ++i) {
        char c = src[i];
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c == ' ') {
            if (*len == 0 || line[*len - 1] == ' ') continue;
        }
        if (*len + 1 < line_sz) {
            line[(*len)++] = c;
        }
    }
}

static void mb_flush_line(char* line, size_t* len) {
    if (!line || !len || *len == 0) return;
    line[*len] = '\0';
    mb_dom_trim(line);
    if (line[0]) mb_emit_wrapped_line(line);
    *len = 0;
    line[0] = '\0';
}

static void mb_dom_collect_content(void) {
    g_br.line_count = 0;
    g_br.content[0] = '\0';

    if (g_dom_content_overridden) {
        char tmp[MB_MAX_CONTENT];
        mb_dom_copy_text(tmp, sizeof(tmp), g_dom_content_override);
        char line[MB_LINE_WIDTH];
        size_t len = 0;
        line[0] = '\0';
        for (char* src = tmp; *src; ++src) {
            char c = *src;
            if (c == '\r') continue;
            if (c == '\n') {
                line[len] = '\0';
                mb_dom_trim(line);
                if (line[0]) mb_add_line(line);
                len = 0;
                line[0] = '\0';
                continue;
            }
            if (len + 1 < sizeof(line)) line[len++] = c;
        }
        if (len > 0) {
            line[len] = '\0';
            mb_dom_trim(line);
            if (line[0]) mb_add_line(line);
        }
        if (g_br.line_count == 0) mb_add_line("(no readable text found)");
        mb_content_from_lines();
        g_br.max_scroll = (g_br.line_count > 0) ? (g_br.line_count - 1) : 0;
        return;
    }

    char line[MB_LINE_WIDTH];
    line[0] = '\0';
    size_t line_len = 0;
    bool in_skip = false;
    bool prev_blank = true;

    for (int i = 0; i < g_dom_node_count; ++i) {
        mb_dom_node_t* n = &g_dom_nodes[i];

        if (n->kind == MB_DOM_NODE_OPEN) {
            if (strcmp(n->tag, "script") == 0 || strcmp(n->tag, "style") == 0) {
                in_skip = true;
                continue;
            }
            if (in_skip) continue;

            if (strcmp(n->tag, "br") == 0 || strcmp(n->tag, "hr") == 0) {
                mb_flush_line(line, &line_len);
                prev_blank = true;
                continue;
            }

            if (mb_is_block_tag(n->tag)) {
                mb_flush_line(line, &line_len);
                if (!prev_blank) {
                    mb_add_line("");
                    prev_blank = true;
                }
            }

            if (strcmp(n->tag, "li") == 0) {
                mb_flush_line(line, &line_len);
                mb_append_normalized(line, sizeof(line), &line_len, "• ");
                prev_blank = false;
            }

            if (strcmp(n->tag, "img") == 0 && n->alt[0]) {
                if (line_len > 0) mb_append_normalized(line, sizeof(line), &line_len, " ");
                mb_append_normalized(line, sizeof(line), &line_len, n->alt);
            }
            continue;
        }

        if (n->kind == MB_DOM_NODE_CLOSE) {
            if (strcmp(n->tag, "script") == 0 || strcmp(n->tag, "style") == 0) {
                in_skip = false;
                continue;
            }
            if (in_skip) continue;

            if (mb_is_block_tag(n->tag)) {
                mb_flush_line(line, &line_len);
                if (!prev_blank) {
                    mb_add_line("");
                    prev_blank = true;
                }
            }
            continue;
        }

        if (n->kind == MB_DOM_NODE_TEXT) {
            if (in_skip || !n->text[0]) continue;
            mb_append_normalized(line, sizeof(line), &line_len, n->text);
            prev_blank = false;
        }
    }

    mb_flush_line(line, &line_len);

    if (g_br.line_count == 0) {
        mb_add_line("(no readable text found)");
    }

    mb_content_from_lines();
    g_br.max_scroll = (g_br.line_count > 0) ? (g_br.line_count - 1) : 0;
}

static void mb_add_line(const char* text) {
    if (g_br.line_count >= MB_MAX_LINES) return;
    mb_copy_text(g_br.lines[g_br.line_count++], MB_LINE_WIDTH, text ? text : "");
}

static void mb_content_from_lines(void) {
    g_br.content[0] = '\0';
    for (int i = 0; i < g_br.line_count; ++i) {
        if (g_br.content[0]) mb_append_text(g_br.content, sizeof(g_br.content), "\n");
        mb_append_text(g_br.content, sizeof(g_br.content), g_br.lines[i]);
    }
}

static void mb_emit_wrapped_line(const char* src) {
    if (!src || !src[0]) return;
    char line[MB_LINE_WIDTH];
    size_t len = 0;
    for (size_t i = 0; src[i]; ++i) {
        char c = src[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (len > 0) {
                line[len] = '\0';
                mb_add_line(line);
                len = 0;
            }
            continue;
        }
        if (len + 1 < sizeof(line)) {
            line[len++] = c;
        }
        if (len >= sizeof(line) - 2) {
            line[len] = '\0';
            mb_add_line(line);
            len = 0;
        }
    }
    if (len > 0) {
        line[len] = '\0';
        mb_add_line(line);
    }
}

static void mb_extract_title(const char* html) {
    (void)html;
    mb_dom_collect_title();
}

static void mb_extract_text_from_html(const char* html) {
    (void)html;
    mb_dom_collect_content();
}


static void mb_update_status(const char* msg) {
    mb_copy_text(g_br.status, sizeof(g_br.status), msg ? msg : "");
}

static void mb_push_history(const char* url) {
    if (!url || !url[0]) return;

    if (g_br.history_pos >= 0 && g_br.history_pos < g_br.history_count) {
        if (strcmp(g_br.history[g_br.history_pos], url) == 0) return;
        if (g_br.history_pos < g_br.history_count - 1) {
            g_br.history_count = g_br.history_pos + 1;
        }
    }

    if (g_br.history_count < MB_MAX_HISTORY) {
        mb_copy_text(g_br.history[g_br.history_count], MB_MAX_URL, url);
        g_br.history_pos = g_br.history_count;
        g_br.history_count++;
    } else {
        for (int i = 1; i < MB_MAX_HISTORY; ++i) {
            mb_copy_text(g_br.history[i - 1], MB_MAX_URL, g_br.history[i]);
        }
        mb_copy_text(g_br.history[MB_MAX_HISTORY - 1], MB_MAX_URL, url);
        g_br.history_pos = MB_MAX_HISTORY - 1;
    }
}

static void mb_sync_address(void) {
    mb_copy_text(g_br.address_bar, sizeof(g_br.address_bar), g_br.url);
    g_br.address_len = (int)strlen(g_br.address_bar);
}

static int mb_load_url(const char* input_url, bool add_history) {
    char url[MB_MAX_URL];
    mb_normalize_url(input_url, url, sizeof(url));
    if (!url[0]) return -1;

    mb_copy_text(g_br.url, sizeof(g_br.url), url);
    mb_sync_address();
    g_br.scroll_y = 0;
    g_br.state = BROWSER_STATE_LOADING;
    mb_update_status("Loading...");

    if (mb_starts_with_ci(url, "c-os://home") || mb_starts_with_ci(url, "about:blank") || mb_starts_with_ci(url, "about:welcome")) {
        const char* html =
            "<html><head><title>NetSurf HTML Viewer</title></head>"
            "<body><h1>NetSurf HTML Viewer</h1>"
            "<p>This is the C-OS HTML viewer entry point.</p>"
            "<p>Open a local HTML file from the File Manager, or browse to a file:// path.</p>"
            "<p>Supported quick links: <a href=\"c-os://help\">Help</a> | "
            "<a href=\"c-os://files\">Local Files</a></p>"
            "</body></html>";
        mb_copy_text(g_br.raw_html, sizeof(g_br.raw_html), html);
        mb_copy_text(g_br.title, sizeof(g_br.title), "NetSurf HTML Viewer");
        mb_dom_parse_html(html);
        mb_extract_title(html);
        mb_extract_text_from_html(html);
        if (g_br.title[0] == '\0') {
            mb_copy_text(g_br.title, sizeof(g_br.title), "NetSurf HTML Viewer");
        }
        if (add_history) {
            mb_push_history(url);
        }
        g_br.state = BROWSER_STATE_READY;
        mb_update_status("Ready");
        g_br.initialized = true;
        return 0;
    }

    if (mb_starts_with_ci(url, "file://") || (url[0] == '/' && url[1] != '\0')) {
        char local_path[MB_MAX_URL];
        mb_strip_file_scheme(url, local_path, sizeof(local_path));
        char response[MB_MAX_CONTENT];
        response[0] = '\0';
        int rc = cos_fs_read_file(local_path, response, sizeof(response) - 1);
        if (rc < 0) {
            mb_copy_text(g_br.title, sizeof(g_br.title), "Load failed");
            mb_copy_text(g_br.content, sizeof(g_br.content), "Failed to read local file.");
            g_br.state = BROWSER_STATE_ERROR;
            mb_update_status("Load failed");
            return -1;
        }
        mb_copy_text(g_br.raw_html, sizeof(g_br.raw_html), response);
        mb_copy_text(g_br.title, sizeof(g_br.title), "Local file");
        mb_dom_parse_html(response);
        mb_extract_title(response);
        mb_extract_text_from_html(response);
        if (g_br.title[0] == '\0') {
            mb_copy_text(g_br.title, sizeof(g_br.title), local_path);
        }
        if (add_history) {
            mb_push_history(url);
        }
        g_br.state = BROWSER_STATE_READY;
        mb_update_status("Ready");
        g_br.initialized = true;
        return 0;
    }

    g_br.network_ok = net_is_connected();
    if (!g_br.network_ok) {
        mb_copy_text(g_br.title, sizeof(g_br.title), "Network unavailable");
        mb_copy_text(g_br.content, sizeof(g_br.content), "Please check the network connection and try again.");
        g_br.state = BROWSER_STATE_ERROR;
        mb_update_status("Offline");
        return -1;
    }

    char response[MB_MAX_CONTENT];
    response[0] = '\0';
    if (http_fetch(url, response, sizeof(response)) != 0) {
        mb_copy_text(g_br.title, sizeof(g_br.title), "Load failed");
        mb_copy_text(g_br.content, sizeof(g_br.content), "Failed to fetch the page.");
        g_br.state = BROWSER_STATE_ERROR;
        mb_update_status("Load failed");
        return -1;
    }

    mb_copy_text(g_br.raw_html, sizeof(g_br.raw_html), response);
    mb_copy_text(g_br.title, sizeof(g_br.title), "Web page");
    mb_dom_parse_html(response);

    char redirect[MB_MAX_URL];
    mb_js_execute_inline_scripts(response, redirect, sizeof(redirect));
    if (redirect[0] && strcmp(redirect, url) != 0) {
        if (add_history) mb_push_history(url);
        return mb_load_url(redirect, false);
    }

    mb_extract_title(response);
    mb_extract_text_from_html(response);

    if (g_br.title[0] == '\0') {
        mb_copy_text(g_br.title, sizeof(g_br.title), url);
    }

    if (add_history) {
        mb_push_history(url);
    }

    g_br.state = BROWSER_STATE_READY;
    mb_update_status("Ready");
    g_br.initialized = true;
    return 0;
}

modern_browser_t* modern_browser_get_instance(void) {
    return &g_br;
}

int modern_browser_init(void) {
    memset(&g_br, 0, sizeof(g_br));
    g_br.state = BROWSER_STATE_IDLE;
    g_br.history_pos = -1;
    g_br.scroll_y = 0;
    g_br.max_scroll = 0;
    g_br.address_focused = true;
    g_br.initialized = true;
    mb_copy_text(g_br.title, sizeof(g_br.title), "NetSurf HTML Viewer");
    mb_copy_text(g_br.status, sizeof(g_br.status), "Ready");
    mb_dom_reset();
    return mb_load_url(HOME_URL, true);
}

int modern_browser_open(const char* url) {
    if (!g_br.initialized) {
        modern_browser_init();
    }
    return mb_load_url(url ? url : HOME_URL, true);
}

void modern_browser_close(void) {
    g_br.state = BROWSER_STATE_IDLE;
    g_br.url[0] = '\0';
    g_br.title[0] = '\0';
    g_br.status[0] = '\0';
    g_br.raw_html[0] = '\0';
    g_br.content[0] = '\0';
    g_br.line_count = 0;
    g_br.scroll_y = 0;
    g_br.max_scroll = 0;
    g_br.address_focused = false;
}

void modern_browser_refresh(void) {
    if (g_br.url[0]) mb_load_url(g_br.url, false);
}

browser_state_t modern_browser_get_state(void) {
    return g_br.state;
}

const char* modern_browser_get_url(void) {
    return g_br.url;
}

const char* modern_browser_get_title(void) {
    return g_br.title;
}

int browser_navigate(const char* url) {
    return modern_browser_open(url);
}

int browser_back(void) {
    if (g_br.history_pos > 0) {
        --g_br.history_pos;
        return mb_load_url(g_br.history[g_br.history_pos], false);
    }
    return -1;
}

int browser_forward(void) {
    if (g_br.history_pos >= 0 && g_br.history_pos + 1 < g_br.history_count) {
        ++g_br.history_pos;
        return mb_load_url(g_br.history[g_br.history_pos], false);
    }
    return -1;
}

int browser_reload(void) {
    modern_browser_refresh();
    return 0;
}

int browser_home(void) {
    return modern_browser_open(HOME_URL);
}

void browser_scroll(int delta) {
    g_br.scroll_y += delta;
    if (g_br.scroll_y < 0) g_br.scroll_y = 0;
    if (g_br.scroll_y > g_br.max_scroll) g_br.scroll_y = g_br.max_scroll;
}

void browser_handle_keypress(char key, int scancode) {
    (void)scancode;

    if (key == '\r' || key == '\n') {
        if (g_br.address_focused) {
            mb_load_url(g_br.address_bar, true);
            g_br.address_focused = false;
        }
        return;
    }

    if (key == '\b') {
        size_t len = strlen(g_br.address_bar);
        if (len > 0) g_br.address_bar[len - 1] = '\0';
        g_br.address_len = (int)strlen(g_br.address_bar);
        return;
    }

    if (key == 27) {
        g_br.address_focused = false;
        mb_sync_address();
        return;
    }

    if (key >= 32 && key <= 126) {
        size_t len = strlen(g_br.address_bar);
        if (len + 1 < sizeof(g_br.address_bar)) {
            g_br.address_bar[len] = key;
            g_br.address_bar[len + 1] = '\0';
            g_br.address_len = (int)(len + 1);
            g_br.address_focused = true;
        }
    }
}

void browser_handle_click(int mx, int my, int button) {
    (void)mx;
    (void)my;
    (void)button;
    g_br.address_focused = true;
}

static void mb_draw_lines(int x, int y, int w, int h) {
    (void)w;
    const int line_h = FONT_H + 2;
    int first = g_br.scroll_y;
    int visible = h / line_h;
    if (visible < 1) visible = 1;

    for (int i = 0; i < visible; ++i) {
        int idx = first + i;
        if (idx >= g_br.line_count) break;
        vga_draw_string_len(x, y + i * line_h, g_br.lines[idx], (int)strlen(g_br.lines[idx]), MB_COLOR_TEXT, MB_COLOR_BG);
    }
}

void browser_draw(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;

    vga_fill_rect(x, y, w, h, MB_COLOR_BG);

    const int bar_h = 28;
    const int status_h = 20;
    const int content_y = y + bar_h + 4;
    const int content_h = h - bar_h - status_h - 8;

    vga_fill_rect(x, y, w, bar_h, MB_COLOR_BAR);
    vga_fill_rect(x, y + bar_h + content_h + 4, w, status_h, MB_COLOR_PANEL);

    char header[MB_MAX_TITLE + MB_MAX_URL + 8];
    snprintf(header, sizeof(header), "%s", g_br.title[0] ? g_br.title : g_br.url);
    vga_draw_string_len(x + 8, y + 6, header, (int)strlen(header), MB_COLOR_TEXT, MB_COLOR_BAR);

    char addr[MB_MAX_URL + 8];
    snprintf(addr, sizeof(addr), "%s", g_br.address_bar[0] ? g_br.address_bar : g_br.url);
    vga_draw_string_len(x + 8, y + bar_h + 8, addr, (int)strlen(addr),
                        g_br.address_focused ? MB_COLOR_HILITE : MB_COLOR_SUBTEXT, MB_COLOR_BG);

    if (g_br.state == BROWSER_STATE_LOADING) {
        vga_draw_string_len(x + 8, content_y, "Loading...", 10, MB_COLOR_SUBTEXT, MB_COLOR_BG);
    } else if (g_br.state == BROWSER_STATE_ERROR) {
        vga_draw_string_len(x + 8, content_y, g_br.content, (int)strlen(g_br.content), MB_COLOR_ERROR, MB_COLOR_BG);
    } else {
        mb_draw_lines(x + 8, content_y, w - 16, content_h);
    }

    char footer[192];
    snprintf(footer, sizeof(footer), "%s",
             g_br.status[0] ? g_br.status : "Ready");
    vga_draw_string_len(x + 8, y + h - status_h + 2, footer, (int)strlen(footer),
                        g_br.state == BROWSER_STATE_ERROR ? MB_COLOR_ERROR : MB_COLOR_SUBTEXT,
                        MB_COLOR_PANEL);
}
