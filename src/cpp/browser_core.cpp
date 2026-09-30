#include "browser_core.h"
extern "C" {
#include "string.h"
}

#include <cstddef>
#include <cstdint>

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\0';
}

static bool same_text(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool starts_with_ci(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    while (*prefix) {
        char a = *text++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static bool contains_char(const char* text, char needle) {
    if (!text) return false;
    while (*text) {
        if (*text == needle) return true;
        ++text;
    }
    return false;
}


static bool is_local_address(const char* input) {
    if (!input || !input[0]) return false;
    if (starts_with_ci(input, "localhost")) return true;
    if (starts_with_ci(input, "127.")) return true;
    if (starts_with_ci(input, "10.")) return true;
    if (starts_with_ci(input, "192.168.")) return true;
    if (starts_with_ci(input, "172.")) {
        const char* p = input + 4;
        int octet = 0;
        while (*p >= '0' && *p <= '9') {
            octet = octet * 10 + (*p - '0');
            ++p;
        }
        if (octet >= 16 && octet <= 31) return true;
    }
    return false;
}

static void copy_trimmed(const char* src, char* dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t start = 0;
    size_t end = strlen(src);
    while (start < end && is_space(src[start])) ++start;
    while (end > start && is_space(src[end - 1])) --end;

    size_t out = 0;
    while (start < end && out + 1 < dst_sz) {
        dst[out++] = src[start++];
    }
    dst[out] = '\0';
}

static void copy_text(const char* src, char* dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < dst_sz && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void append_text(const char* src, char* dst, size_t dst_sz) {
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


static bool looks_like_url_or_host(const char* input) {
    if (!input || !input[0]) return false;
    if (starts_with_ci(input, "about:") ||
        starts_with_ci(input, "c-os://") ||
        starts_with_ci(input, "file://") ||
        starts_with_ci(input, "http://") ||
        starts_with_ci(input, "https://") ||
        starts_with_ci(input, "gemini://") ||
        starts_with_ci(input, "gopher://")) {
        return true;
    }
    if (starts_with_ci(input, "www.") ||
        starts_with_ci(input, "localhost") ||
        contains_char(input, ':') ||
        contains_char(input, '.')) {
        return true;
    }
    return false;
}

static bool is_unreserved(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

static void url_encode_component(const char* src, char* dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;

    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dst_sz; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == ' ') {
            if (out + 1 >= dst_sz) break;
            dst[out++] = '+';
        } else if (is_unreserved((char)c)) {
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

extern "C" int cos_browser_build_google_search_url(const char* raw_query, char* url, size_t url_sz) {
    if (!url || url_sz == 0) return 0;
    char query[COS_BROWSER_URL_MAX];
    copy_trimmed(raw_query, query, sizeof(query));
    copy_text("https://www.google.com/search?hl=ja&gbv=1&q=", url, url_sz);
    if (query[0]) {
        char encoded[COS_BROWSER_URL_MAX];
        url_encode_component(query, encoded, sizeof(encoded));
        append_text(encoded, url, url_sz);
    }
    return 1;
}

extern "C" int cos_browser_normalize_address(const char* raw_url, char* url, size_t url_sz) {
    if (!url || url_sz == 0) return 0;

    char input[COS_BROWSER_URL_MAX];
    copy_trimmed(raw_url, input, sizeof(input));

    if (input[0] == '\0') {
        copy_text("http://example.com/", url, url_sz);
        return 1;
    }

    if (same_text(input, "home") || same_text(input, "welcome")) {
        copy_text("http://example.com/", url, url_sz);
        return 1;
    }

    if (same_text(input, "about") || same_text(input, "help") ||
        same_text(input, "files") || same_text(input, "storage") ||
        same_text(input, "remote")) {
        copy_text("c-os://", url, url_sz);
        strncat(url, input, url_sz - strlen(url) - 1);
        return 1;
    }

    if (input[0] == '/' || input[0] == '.') {
        copy_text("file://", url, url_sz);
        strncat(url, input, url_sz - strlen(url) - 1);
        return 1;
    }

    if (starts_with_ci(input, "about:") ||
        starts_with_ci(input, "c-os://") ||
        starts_with_ci(input, "file://") ||
        starts_with_ci(input, "http://") ||
        starts_with_ci(input, "https://") ||
        starts_with_ci(input, "gemini://") ||
        starts_with_ci(input, "gopher://")) {
        copy_text(input, url, url_sz);
        return 1;
    }

    if (looks_like_url_or_host(input)) {
        copy_text("http://", url, url_sz);
        strncat(url, input, url_sz - strlen(url) - 1);
        return 1;
    }

    return cos_browser_build_google_search_url(input, url, url_sz);
}

extern "C" int cos_browser_handle_url_key(char* buffer, size_t buffer_sz, char ascii, int scancode) {
    if (!buffer || buffer_sz == 0) return COS_BROWSER_URL_KEY_NONE;

    if (scancode == 0x1C || ascii == '\n' || ascii == '\r') {
        return COS_BROWSER_URL_KEY_COMMIT;
    }
    if (scancode == 0x01 || ascii == '\x1b') {
        buffer[0] = '\0';
        return COS_BROWSER_URL_KEY_CANCEL;
    }
    if (scancode == 0x0E || ascii == '\b') {
        size_t len = strlen(buffer);
        if (len > 0) buffer[len - 1] = '\0';
        return COS_BROWSER_URL_KEY_NONE;
    }
    if (ascii >= 32 && ascii <= 126) {
        size_t len = strlen(buffer);
        if (len + 1 < buffer_sz) {
            buffer[len] = ascii;
            buffer[len + 1] = '\0';
        }
    }
    return COS_BROWSER_URL_KEY_NONE;
}

extern "C" int cos_browser_history_push(char history[][COS_BROWSER_URL_MAX], int* count, int* pos, int max_count, const char* url) {
    if (!history || !count || !pos || max_count <= 0 || !url || url[0] == '\0') return 0;

    if (*pos >= 0 && *pos < *count) {
        if (same_text(history[*pos], url)) {
            return 0;
        }
        if (*pos < *count - 1) {
            *count = *pos + 1;
        }
    }

    if (*count < max_count) {
        copy_text(url, history[*count], COS_BROWSER_URL_MAX);
        *count += 1;
        *pos = *count - 1;
        return 1;
    }

    for (int i = 1; i < max_count; ++i) {
        copy_text(history[i], history[i - 1], COS_BROWSER_URL_MAX);
    }
    copy_text(url, history[max_count - 1], COS_BROWSER_URL_MAX);
    *count = max_count;
    *pos = max_count - 1;
    return 1;
}

extern "C" int cos_browser_history_move(int* pos, int count, int delta) {
    if (!pos || count <= 0 || delta == 0) return 0;
    int next = *pos + delta;
    if (next < 0 || next >= count) return 0;
    *pos = next;
    return 1;
}
