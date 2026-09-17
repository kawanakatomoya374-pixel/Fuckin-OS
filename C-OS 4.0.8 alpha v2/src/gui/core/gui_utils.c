#include "gui_utils.h"

#include "gui.h"
#include "fs.h"
#include "vga.h"
#include <string.h>

#ifndef WINDOW_MIN_WIDTH
#define WINDOW_MIN_WIDTH 100
#endif
#ifndef WINDOW_MIN_HEIGHT
#define WINDOW_MIN_HEIGHT 80
#endif

void gui_split_path(const char* full, char* parent, size_t parent_size, char* leaf, size_t leaf_size) {
    if (!parent || !leaf || parent_size == 0 || leaf_size == 0) return;
    parent[0] = '\0';
    leaf[0] = '\0';
    if (!full || !full[0]) return;
    const char* last = NULL;
    for (const char* p = full; *p; ++p) if (*p == '/') last = p;
    if (!last) {
        strncpy(parent, "/", parent_size - 1); parent[parent_size - 1] = '\0';
        strncpy(leaf, full, leaf_size - 1); leaf[leaf_size - 1] = '\0';
        return;
    }
    size_t plen = (size_t)(last - full);
    if (plen == 0) {
        strncpy(parent, "/", parent_size - 1); parent[parent_size - 1] = '\0';
    } else {
        if (plen >= parent_size) plen = parent_size - 1;
        memcpy(parent, full, plen);
        parent[plen] = '\0';
    }
    strncpy(leaf, last + 1, leaf_size - 1);
    leaf[leaf_size - 1] = '\0';
}

void gui_copy_cstr(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    size_t i = 0;
    if (!src) src = "";
    while (i + 1 < dst_size && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void gui_append_cstr(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0 || !src) return;
    size_t len = strlen(dst);
    size_t i = 0;
    while (len + 1 < dst_size && src[i]) {
        dst[len++] = src[i++];
    }
    dst[len] = '\0';
}

void gui_make_unique_desktop_name(const char* base, const char* ext, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!base || !base[0]) base = "new_item";
    if (!ext) ext = "";

    char candidate[FS_MAX_NAME];
    char full[FS_MAX_PATH];
    char suffix[16];

    for (int n = 0; n < 1000; ++n) {
        candidate[0] = '\0';
        gui_copy_cstr(candidate, sizeof(candidate), base);
        if (n > 0) {
            gui_append_cstr(candidate, sizeof(candidate), " ");
            int value = n + 1;
            int pos = 0;
            do {
                if (pos + 1 < (int)sizeof(suffix)) {
                    suffix[pos++] = (char)('0' + (value % 10));
                }
                value /= 10;
            } while (value > 0 && pos + 1 < (int)sizeof(suffix));
            suffix[pos] = '\0';
            for (int i = 0; i < pos / 2; ++i) {
                char tmp = suffix[i];
                suffix[i] = suffix[pos - 1 - i];
                suffix[pos - 1 - i] = tmp;
            }
            gui_append_cstr(candidate, sizeof(candidate), suffix);
        }
        gui_append_cstr(candidate, sizeof(candidate), ext);

        full[0] = '\0';
        gui_copy_cstr(full, sizeof(full), "/desktop");
        if (candidate[0]) {
            size_t len = strlen(full);
            if (len + 1 < sizeof(full)) {
                full[len++] = '/';
                full[len] = '\0';
                gui_append_cstr(full, sizeof(full), candidate);
            }
        }

        if (fs_find(full) == NULL) {
            gui_copy_cstr(out, out_size, candidate);
            return;
        }
    }

    gui_copy_cstr(out, out_size, base);
    gui_append_cstr(out, out_size, ext);
}

int gui_parse_int_or_default(const char* s, int fallback) {
    if (!s || !*s) return fallback;
    int sign = 1;
    int value = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
    }
    return sign * value;
}

void gui_format_int(int value, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    char tmp[32];
    int i = 0;
    bool neg = value < 0;
    unsigned int v = neg ? (unsigned int)(-value) : (unsigned int)value;
    do {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && i < (int)sizeof(tmp) - 1);
    if (neg && i < (int)sizeof(tmp) - 1) tmp[i++] = '-';
    int j = 0;
    while (i > 0 && j < (int)out_size - 1) out[j++] = tmp[--i];
    out[j] = '\0';
}

int gui_find_text(const char* haystack, int hay_len, const char* needle) {
    if (!haystack || !needle || !needle[0] || hay_len < 0) return -1;
    int needle_len = (int)strlen(needle);
    if (needle_len <= 0 || needle_len > hay_len) return -1;
    for (int i = 0; i + needle_len <= hay_len; ++i) {
        int j = 0;
        for (; j < needle_len; ++j) {
            if (haystack[i + j] != needle[j]) break;
        }
        if (j == needle_len) return i;
    }
    return -1;
}

bool gui_replace_all_text(char* buf, int buf_cap, int* size_io, const char* needle, const char* replacement) {
    if (!buf || !size_io || !needle || !replacement) return false;
    int size = *size_io;
    int needle_len = (int)strlen(needle);
    int repl_len = (int)strlen(replacement);
    if (needle_len <= 0 || size < 0 || size >= buf_cap) return false;
    if (repl_len > needle_len && size + (repl_len - needle_len) >= buf_cap) return false;
    int pos = 0;
    int replaced = 0;
    while (pos <= size) {
        int hit = gui_find_text(buf + pos, size - pos, needle);
        if (hit < 0) break;
        hit += pos;
        if (repl_len != needle_len) {
            if (repl_len > needle_len) {
                int delta = repl_len - needle_len;
                for (int i = size; i >= hit + needle_len; --i) buf[i + delta] = buf[i];
            } else {
                int delta = needle_len - repl_len;
                for (int i = hit + needle_len; i <= size; ++i) buf[i - delta] = buf[i];
            }
        }
        for (int i = 0; i < repl_len; ++i) buf[hit + i] = replacement[i];
        size += repl_len - needle_len;
        pos = hit + repl_len;
        replaced++;
    }
    if (size >= buf_cap) size = buf_cap - 1;
    buf[size] = '\0';
    *size_io = size;
    return replaced > 0;
}

void gui_clamp_window_geometry(int* x, int* y, int* w, int* h) {
    if (!x || !y || !w || !h) return;

    int screen_w = (SCREEN_W > 0) ? (int)SCREEN_W : 1024;
    int screen_h = (SCREEN_H > 0) ? (int)SCREEN_H : 768;
    int usable_h = screen_h - TASKBAR_H;
    if (usable_h < 1) usable_h = screen_h;

    if (*w < WINDOW_MIN_WIDTH) *w = WINDOW_MIN_WIDTH;
    if (*h < WINDOW_MIN_HEIGHT) *h = WINDOW_MIN_HEIGHT;
    if (*w > screen_w - 8) *w = screen_w - 8;
    if (*h > usable_h - 8) *h = usable_h - 8;
    if (*w < WINDOW_MIN_WIDTH) *w = screen_w > 8 ? screen_w - 8 : WINDOW_MIN_WIDTH;
    if (*h < WINDOW_MIN_HEIGHT) *h = usable_h > 8 ? usable_h - 8 : WINDOW_MIN_HEIGHT;

    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x + *w > screen_w) *x = screen_w - *w;
    if (*y + *h > usable_h) *y = usable_h - *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}
