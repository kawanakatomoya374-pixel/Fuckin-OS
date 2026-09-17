#include "string.h"
#include "memory.h"
#include "stdio.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

// String length function
size_t strlen(const char* s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

// Ultra-fast 64-bit optimized memset for VGA framebuffer
void* memset(void* dst, int val, size_t n) {
    uint64_t* p64 = (uint64_t*)dst;
    uint64_t v64 = (uint8_t)val;
    v64 |= (v64 << 8) | (v64 << 16) | (v64 << 24) | (v64 << 32) | (v64 << 40) | (v64 << 48) | (v64 << 56);
    
    while (n >= 8) {
        *p64++ = v64;
        n -= 8;
    }
    
    uint8_t* p8 = (uint8_t*)p64;
    while (n--) *p8++ = (uint8_t)val;
    
    return dst;
}

// Safe memcpy for freestanding use. Caller must ensure non-overlapping regions.
void* memcpy(void* dst, const void* src, size_t n) {
    if (!dst || !src || n == 0) return dst;
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

// memmove - handles overlapping regions
void* memmove(void* dst, const void* src, size_t n) {
    if (!dst || !src || n == 0 || dst == src) return dst;
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *(--d) = *(--s);
        }
    }

    return dst;
}

/* A malformed third-party parser value must not turn an ordinary string
 * comparison into a #GP fault: x86-64 rejects non-canonical addresses before
 * paging. Library callers still receive a deterministic non-equal result. */
static bool cos_ptr_is_canonical(const void *ptr) {
    uint64_t address = (uint64_t)(uintptr_t)ptr;
    uint64_t upper = address >> 48;
    return upper == 0 || upper == 0xffffu;
}

int memcmp(const void* a, const void* b, size_t n) {
    if (n == 0 || a == b) return 0;
    if (!cos_ptr_is_canonical(a) || !cos_ptr_is_canonical(b)) {
        return ((uintptr_t)a < (uintptr_t)b) ? -1 : 1;
    }
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;
    while (n--) {
        if (*p != *q) return *p - *q;
        p++; q++;
    }
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = (const uint8_t*)s;
    uint8_t target = (uint8_t)c;
    while (n--) {
        if (*p == target) return (void*)p;
        p++;
    }
    return NULL;
}


int strcmp(const char* a, const char* b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    if (n == 0 || a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (n > 0 && *a && *b && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

static int cos_tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}

/* Case-insensitive counterparts of strcmp/strncmp above - not part of
 * ISO C, but POSIX (<strings.h>, folded into our single string.h
 * rather than adding a second header) and relied on by ported code
 * such as libcss/libhubbub for things like HTML attribute name and
 * CSS keyword matching. */
int strcasecmp(const char* a, const char* b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && *b && cos_tolower((unsigned char)*a) == cos_tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return cos_tolower((unsigned char)*a) - cos_tolower((unsigned char)*b);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    if (n == 0 || a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (n > 0 && *a && *b && cos_tolower((unsigned char)*a) == cos_tolower((unsigned char)*b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0) return 0;
    return cos_tolower((unsigned char)*a) - cos_tolower((unsigned char)*b);
}

/* Allocates via kmalloc, same heap as everything else in the kernel -
 * callers are expected to free() (== kfree(), see stdlib.h's malloc
 * family) what they get back, same as any other strdup(). */
char* strdup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = (char*)kmalloc(len);
    if (!copy) return NULL;
    memcpy(copy, s, len);
    return copy;
}

/* Like strdup, but copies at most n bytes, then always NUL-terminates
 * (the copy is n+1 bytes, even if s is shorter than n - matching the
 * standard's semantics, not just a bounded memcpy). */
char* strndup(const char* s, size_t n) {
    if (!s) return NULL;
    size_t len = 0;
    while (len < n && s[len]) len++;
    char* copy = (char*)kmalloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char* strcpy(char* dst, const char* src) {
    if (!dst) return NULL;
    if (!src) { dst[0] = 0; return dst; }
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    if (!dst || n == 0) return dst;
    if (!src) {
        for (size_t i = 0; i < n; ++i) dst[i] = '\0';
        return dst;
    }

    /* Standard strncpy semantics copy up to exactly n bytes and pad only
     * if the source terminates sooner.  The previous n-1 implementation
     * inserted a NUL into fixed-length NetSurf IDNA labels, turning
     * "example.com" into "exampl" before DNS resolution. */
    size_t i = 0;
    while (i < n && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    while (i < n) {
        dst[i] = '\0';
        ++i;
    }
    return dst;
}

char* strcat(char* dst, const char* src) {
    if (!dst) return NULL;
    if (!src) return dst;
    char* d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

char* strncat(char* dst, const char* src, size_t n) {
    if (!dst || !src || n == 0) return dst;
    char* d = dst;
    while (*d) d++;
    while (n-- && *src) {
        *d++ = *src++;
    }
    *d = 0;
    return dst;
}

char* strchr(const char* s, int c) {
    if (!s) return NULL;
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return NULL;
}

char* strrchr(const char* s, int c) {
    if (!s) return NULL;
    char* last = NULL;
    while (*s) {
        if (*s == (char)c) last = (char*)s;
        s++;
    }
    return last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char*)haystack;
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; ++i) {
        if (strncmp(haystack + i, needle, nlen) == 0) return (char*)(haystack + i);
    }
    return NULL;
}

/* Case-insensitive strstr() - a BSD/GNU extension (not ISO C or
 * POSIX), needed by the vendored NetSurf html.c (content/handlers/
 * html/css.c's <style>/<link> tag scanning). Same shape as strstr()
 * above, just using strncasecmp(). */
char* strcasestr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char*)haystack;
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; ++i) {
        if (strncasecmp(haystack + i, needle, nlen) == 0) return (char*)(haystack + i);
    }
    return NULL;
}

/* Standard binary search (ISO C, <stdlib.h> - declared there already,
 * just never previously defined anywhere in this tree). Needed by
 * vendored NetSurf code (content/handlers/css/hints.c's named-colour
 * table lookup, libparserutils' charset alias table). Textbook
 * iterative implementation - no allocation, no recursion, no
 * overflow-prone (lo+hi)/2 midpoint. */
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*)) {
    if (!key || !base || !compar || nmemb == 0 || size == 0) return NULL;
    const char* data = (const char*)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void* elem = (const void*)(data + mid * size);
        int cmp = compar(key, elem);
        if (cmp == 0) return (void*)elem;
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

void itoa(int val, char* buf, int base) {
    char tmp[32];
    int i = 0, neg = 0;
    if (val < 0 && base == 10) { neg = 1; val = -val; }
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (val > 0) {
        int r = val % base;
        tmp[i++] = r < 10 ? '0' + r : 'a' + r - 10;
        val /= base;
    }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

void utoa(uint64_t val, char* buf, int base) {
    char tmp[32];
    int i = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (val > 0) {
        uint64_t r = val % base;
        tmp[i++] = r < 10 ? '0' + r : 'a' + r - 10;
        val /= base;
    }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

int atoi(const char* s) {
    int n = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}

static int cos_digit_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

static long long cos_parse_ll(const char* s, char** endptr, int base)
{
    if (!s) {
        if (endptr) *endptr = (char*)s;
        return 0;
    }

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') {
        s++;
    }

    int sign = 1;
    if (*s == '+') {
        s++;
    } else if (*s == '-') {
        sign = -1;
        s++;
    }

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) base = 16;
        else if (s[0] == '0') base = 8;
        else base = 10;
    }

    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    long long value = 0;
    const char* start = s;
    int digit;
    while ((digit = cos_digit_value(*s)) >= 0 && digit < base) {
        value = value * base + digit;
        s++;
    }

    if (endptr) *endptr = (char*)(s == start ? start : s);
    return sign * value;
}

long strtol(const char* s, char** endptr, int base)
{
    return (long)cos_parse_ll(s, endptr, base);
}

long long strtoll(const char* s, char** endptr, int base)
{
    return cos_parse_ll(s, endptr, base);
}

unsigned long strtoul(const char* s, char** endptr, int base)
{
    return (unsigned long)cos_parse_ll(s, endptr, base);
}

unsigned long long strtoull(const char* s, char** endptr, int base)
{
    return (unsigned long long)cos_parse_ll(s, endptr, base);
}

/* Deliberately narrow: handles literal characters (matched exactly),
 * ' ' in the format (matches zero or more whitespace in the input,
 * standard scanf behaviour), and the conversions actually used
 * anywhere in this tree: %d (int*), %u (unsigned int*), %zu (size_t*).
 * Stops and returns the conversion count so far as soon as it hits a
 * specifier it doesn't recognise, a literal mismatch, or a conversion
 * that fails to find any digits - the same "how far did we get"
 * contract as the real sscanf, just over a smaller specifier set.
 * Grep for `%` in the format string of any new caller before assuming
 * this covers it. */
int sscanf(const char* str, const char* format, ...) {
    if (!str || !format) return 0;

    va_list args;
    va_start(args, format);

    const char* s = str;
    const char* f = format;
    int matched = 0;

    while (*f) {
        if (*f == '%') {
            f++;
            if (f[0] == 'z' && f[1] == 'u') {
                f += 2;
                while (*s == ' ' || *s == '\t') s++;
                char* endp;
                long long v = cos_parse_ll(s, &endp, 10);
                if (endp == s) break;
                *va_arg(args, size_t*) = (size_t)v;
                s = endp;
                matched++;
            } else if (*f == 'u') {
                f++;
                while (*s == ' ' || *s == '\t') s++;
                char* endp;
                long long v = cos_parse_ll(s, &endp, 10);
                if (endp == s) break;
                *va_arg(args, unsigned int*) = (unsigned int)v;
                s = endp;
                matched++;
            } else if (*f == 'd') {
                f++;
                while (*s == ' ' || *s == '\t') s++;
                char* endp;
                long long v = cos_parse_ll(s, &endp, 10);
                if (endp == s) break;
                *va_arg(args, int*) = (int)v;
                s = endp;
                matched++;
            } else {
                /* Unsupported specifier - stop rather than
                 * misinterpret the argument list. */
                break;
            }
        } else if (*f == ' ') {
            while (*s == ' ' || *s == '\t') s++;
            f++;
        } else {
            if (*s != *f) break;
            s++;
            f++;
        }
    }

    va_end(args);
    return matched;
}

size_t cos_strlcpy(char* dst, const char* src, size_t size)
{
    if (!dst || size == 0) return 0;
    if (!src) {
        dst[0] = '\0';
        return 0;
    }

    size_t i = 0;
    while (i + 1 < size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';

    while (src[i] != '\0') {
        i++;
    }
    return i;
}

size_t cos_strlcat(char* dst, const char* src, size_t size)
{
    if (!dst || size == 0) return 0;

    size_t dlen = 0;
    while (dlen < size && dst[dlen] != '\0') {
        dlen++;
    }

    if (dlen == size) {
        return dlen + (src ? strlen(src) : 0);
    }

    size_t copied = cos_strlcpy(dst + dlen, src, size - dlen);
    return dlen + copied;
}

// Minimal freestanding printf-style formatter used by GUI and app modules.
static void append_char(char** out, size_t* remaining, char c) {
    if (*remaining > 1) {
        **out = c;
        (*out)++;
        (*remaining)--;
    }
}

static void append_str(char** out, size_t* remaining, const char* s) {
    if (!s) s = "(null)";
    while (*s) {
        append_char(out, remaining, *s++);
    }
}

static void append_strn(char** out, size_t* remaining, const char* s, int limit) {
    if (!s) s = "(null)";
    while (*s && (limit < 0 || limit-- > 0)) {
        append_char(out, remaining, *s++);
    }
}

static void append_uint64(char** out, size_t* remaining, uint64_t v, int base, bool upper) {
    char tmp[65];
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (v == 0) {
        append_char(out, remaining, '0');
        return;
    }
    while (v && i < (int)sizeof(tmp)) {
        tmp[i++] = digits[v % (uint64_t)base];
        v /= (uint64_t)base;
    }
    while (i--) append_char(out, remaining, tmp[i]);
}

static int cos_vsnprintf_impl(char* str, size_t size, const char* format, va_list args_in) {
    if (!str || size == 0 || !format) {
        return 0;
    }

    va_list args;
    va_copy(args, args_in);

    char* out = str;
    size_t remaining = size;
    while (*format) {
        if (*format != '%') {
            append_char(&out, &remaining, *format++);
            continue;
        }

        format++;
        bool long_long = false;
        bool long_mod = false;
        bool size_mod = false;
        int precision = -1;

        /* Parse the common printf flags and field width.  Padding is not
         * rendered by this freestanding formatter, but consuming these
         * tokens keeps the varargs stream aligned for ported libraries. */
        while (*format == '-' || *format == '+' || *format == ' ' ||
               *format == '#' || *format == '0') {
            format++;
        }
        if (*format == '*') {
            (void)va_arg(args, int);
            format++;
        } else {
            while (*format >= '0' && *format <= '9') format++;
        }
        if (*format == '.') {
            format++;
            precision = 0;
            if (*format == '*') {
                precision = va_arg(args, int);
                format++;
            } else {
                while (*format >= '0' && *format <= '9') {
                    precision = precision * 10 + (*format - '0');
                    format++;
                }
            }
        }

        if (*format == 'l') {
            long_mod = true;
            format++;
            if (*format == 'l') {
                long_long = true;
                format++;
            }
        } else if (*format == 'z') {
            size_mod = true;
            format++;
        }

        switch (*format) {
            case 's': {
                const char* s = va_arg(args, const char*);
                append_strn(&out, &remaining, s, precision);
                break;
            }
            case 'c': {
                append_char(&out, &remaining, (char)va_arg(args, int));
                break;
            }
            case 'd':
            case 'i': {
                long long v = 0;
                if (long_long) v = va_arg(args, long long);
                else if (long_mod) v = va_arg(args, long);
                else v = va_arg(args, int);
                if (v < 0) { append_char(&out, &remaining, '-'); v = -v; }
                append_uint64(&out, &remaining, (uint64_t)v, 10, false);
                break;
            }
            case 'u': {
                unsigned long long v = 0;
                if (long_long) v = va_arg(args, unsigned long long);
                else if (long_mod || size_mod) v = va_arg(args, unsigned long);
                else v = va_arg(args, unsigned int);
                append_uint64(&out, &remaining, (uint64_t)v, 10, false);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long long v = 0;
                if (long_long) v = va_arg(args, unsigned long long);
                else if (long_mod || size_mod) v = va_arg(args, unsigned long);
                else v = va_arg(args, unsigned int);
                append_uint64(&out, &remaining, (uint64_t)v, 16, *format == 'X');
                break;
            }
            case 'p': {
                append_str(&out, &remaining, "0x");
                append_uint64(&out, &remaining, (uint64_t)(uintptr_t)va_arg(args, void*), 16, false);
                break;
            }
            case '%':
                append_char(&out, &remaining, '%');
                break;
            case '\0':
                format--;
                break;
            default:
                append_char(&out, &remaining, '%');
                append_char(&out, &remaining, *format);
                break;
        }
        format++;
    }

    va_end(args);
    if (remaining > 0) *out = '\0';
    else str[size - 1] = '\0';
    /* Callers use this value as the byte length of generated protocol
     * headers.  Returning one count per conversion (the old behaviour)
     * truncated "Content-Type: text/html" to "Content-Type: t" in
     * NetSurf's llcache. */
    return (int)(out - str);
}

int vsnprintf(char* str, size_t size, const char* format, va_list args) {
    return cos_vsnprintf_impl(str, size, format, args);
}

int snprintf(char* str, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = cos_vsnprintf_impl(str, size, format, args);
    va_end(args);
    return ret;
}

/* True unbounded sprintf() can't be made safe in general (no way to
 * know the caller's buffer size), but every real caller in ported
 * code sizes its buffer for the specific format string it passes, so
 * a large-but-finite bound behaves identically for all of them while
 * still catching a runaway/malformed format before it can walk off
 * the end of the kernel heap. */
int sprintf(char* str, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = cos_vsnprintf_impl(str, (size_t)65536, format, args);
    va_end(args);
    return ret;
}

int fflush(FILE* stream) {
    (void)stream;
    return 0;
}

int atexit(void (*func)(void)) {
    (void)func;
    return 0;
}


// Freestanding ctype locale tables for ACPICA/glibc-style ctype macros.
// These satisfy code paths compiled against __ctype_* accessors without libc.
static unsigned short g_ctype_b[384];
static int g_ctype_tolower[384];
static int g_ctype_toupper[384];
static bool g_ctype_tables_ready = false;

static inline int ctype_ascii_islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int ctype_ascii_isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int ctype_ascii_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int ctype_ascii_isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
static inline int ctype_ascii_isxdigit(int c) { return ctype_ascii_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static void ctype_init_tables(void) {
    if (g_ctype_tables_ready) return;
    for (int i = 0; i < 384; ++i) {
        int c = i - 128;
        unsigned short bits = 0;
        if (ctype_ascii_isupper(c)) bits |= 0x0100; /* _ISupper */
        if (ctype_ascii_islower(c)) bits |= 0x0200; /* _ISlower */
        if (ctype_ascii_isupper(c) || ctype_ascii_islower(c)) bits |= 0x0400; /* _ISalpha */
        if (ctype_ascii_isdigit(c)) bits |= 0x0800; /* _ISdigit */
        if (ctype_ascii_isxdigit(c)) bits |= 0x1000; /* _ISxdigit */
        if (ctype_ascii_isspace(c)) bits |= 0x2000; /* _ISspace */
        if ((c >= 0x20 && c <= 0x7E) || c == '\t') bits |= 0x4000; /* _ISprint */
        if ((c >= 0x21 && c <= 0x7E)) bits |= 0x8000; /* _ISgraph */
        if (c == ' ' || c == '\t') bits |= 0x0001; /* _ISblank */
        if (c < 0x20 || c == 0x7F) bits |= 0x0002; /* _IScntrl */
        if (!ctype_ascii_isdigit(c) && !ctype_ascii_isupper(c) && !ctype_ascii_islower(c) && (c >= 0x21 && c <= 0x7E)) bits |= 0x0004; /* _ISpunct */
        if (ctype_ascii_isdigit(c) || ctype_ascii_isupper(c) || ctype_ascii_islower(c)) bits |= 0x0008; /* _ISalnum */
        g_ctype_b[i] = bits;
        g_ctype_toupper[i] = ctype_ascii_islower(c) ? (c - 32) : c;
        g_ctype_tolower[i] = ctype_ascii_isupper(c) ? (c + 32) : c;
    }
    g_ctype_tables_ready = true;
}

const unsigned short int **__ctype_b_loc(void) {
    ctype_init_tables();
    static const unsigned short int* table = &g_ctype_b[128];
    return &table;
}

const int **__ctype_tolower_loc(void) {
    ctype_init_tables();
    static const int* table = &g_ctype_tolower[128];
    return &table;
}

const int **__ctype_toupper_loc(void) {
    ctype_init_tables();
    static const int* table = &g_ctype_toupper[128];
    return &table;
}

size_t strspn(const char *s, const char *accept) {
    size_t count = 0;

    while (*s) {
        const char *a = accept;
        int found = 0;

        while (*a) {
            if (*s == *a) {
                found = 1;
                break;
            }
            a++;
        }

        if (!found)
            return count;

        count++;
        s++;
    }

    return count;
}



/* ISO C strcspn: count initial bytes in s that do not occur in reject.
 * box_construct.c uses it when normalising HTML line endings. */
size_t strcspn(const char *s, const char *reject) {
    if (s == NULL || reject == NULL) return 0;
    size_t count = 0;
    while (s[count] != '\0') {
        const char *r = reject;
        while (*r != '\0' && *r != s[count]) {
            ++r;
        }
        if (*r != '\0') break;
        ++count;
    }
    return count;
}

/* ISO C strtok with translation-unit local continuation state. NetSurf uses
 * it only synchronously while parsing imagemap coordinate lists. */
char* strtok(char* s, const char* delimiters) {
    static char* next;
    if (delimiters == NULL) return NULL;
    if (s != NULL) next = s;
    if (next == NULL) return NULL;
    while (*next) {
        const char* d = delimiters;
        while (*d && *d != *next) ++d;
        if (!*d) break;
        ++next;
    }
    if (*next == '\0') { next = NULL; return NULL; }
    char* token = next;
    while (*next) {
        const char* d = delimiters;
        while (*d && *d != *next) ++d;
        if (*d) { *next++ = '\0'; return token; }
        ++next;
    }
    next = NULL;
    return token;
}
