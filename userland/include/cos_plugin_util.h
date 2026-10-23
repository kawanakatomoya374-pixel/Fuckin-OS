/* cos_plugin_util.h - small string helpers for plugins (a plugin is freestanding: no libc inside the library). Header only, all static. */
#ifndef COS_PLUGIN_UTIL_H
#define COS_PLUGIN_UTIL_H
#include <stdint.h>
#include <stddef.h>
static inline size_t pu_len(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static inline int pu_cmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static inline int pu_ncmp(const char *a, const char *b, size_t n) { for (; n; n--, a++, b++) { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; if (!*a) return 0; } return 0; }
static inline void *pu_cpy(void *d, const void *s, size_t n) { char *p = (char *)d; const char *q = (const char *)s; while (n--) *p++ = *q++; return d; }
static inline void *pu_set(void *d, int c, size_t n) { char *p = (char *)d; while (n--) *p++ = (char)c; return d; }
static inline char pu_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static inline char pu_upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static inline const char *pu_find(const char *h, const char *n, int icase) {
    size_t nl = pu_len(n); if (!nl) return h;
    for (; *h; h++) { size_t i = 0; while (i < nl && h[i] && (icase ? pu_lower(h[i]) == pu_lower(n[i]) : h[i] == n[i])) i++; if (i == nl) return h; }
    return 0;
}
static inline long pu_atol(const char *s) { long v = 0; int neg = 0; while (*s == ' ') s++; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return neg ? -v : v; }
static inline void pu_copy_str(char *d, size_t cap, const char *s) { size_t i = 0; for (; s[i] && i + 1 < cap; i++) d[i] = s[i]; d[i] = 0; }
/* append to a bounded buffer (always terminated) */
static inline size_t pu_cat(char *d, size_t cap, const char *s) { size_t o = pu_len(d); while (*s && o + 1 < cap) d[o++] = *s++; d[o] = 0; return o; }
static inline uint32_t pu_crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { crc ^= p[i]; for (int k = 0; k < 8; k++) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1; }
    return crc;
}
#endif
