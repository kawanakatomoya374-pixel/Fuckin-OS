/**
 * cos_string_util.c - Common String Utilities Implementation
 * Provides basic string manipulation functions for freestanding kernel environment
 */

#include <cos_string.h>

void* cos_memset(void* ptr, int value, size_t num) {
    unsigned char* p = (unsigned char*)ptr;
    for (size_t i = 0; i < num; i++) {
        p[i] = (unsigned char)value;
    }
    return ptr;
}

void* cos_memcpy(void* dest, const void* src, size_t num) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < num; i++) {
        d[i] = s[i];
    }
    return dest;
}

char* cos_strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++) != 0);
    return dest;
}

char* cos_strncpy(char* dest, const char* src, size_t n) {
    if (!dest || n == 0) return dest;
    if (!src) {
        dest[0] = 0;
        return dest;
    }

    size_t i = 0;
    while (i + 1 < n && src[i] != 0) {
        dest[i] = src[i];
        ++i;
    }
    dest[i] = 0;
    while (++i < n) {
        dest[i] = 0;
    }
    return dest;
}

char* cos_strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != 0);
    return dest;
}

char* cos_strncat(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (*d) d++;
    while (n-- && (*d++ = *src++) != 0);
    *d = 0;
    return dest;
}

int cos_strcmp(const char* s1, const char* s2) {
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int cos_strncmp(const char* s1, const char* s2, size_t n) {
    while (n-- && *s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return n ? ((int)(unsigned char)*s1 - (int)(unsigned char)*s2) : 0;
}

char* cos_strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    while (*haystack) {
        const char* h = haystack;
        const char* n = needle;
        while (*n && *h && *h == *n) {
            h++;
            n++;
        }
        if (!*n) return (char*)haystack;
        haystack++;
    }
    return NULL;
}

char* cos_strchr(const char* str, int c) {
    while (*str) {
        if (*str == c) return (char*)str;
        str++;
    }
    return NULL;
}

size_t cos_strlen(const char* str) {
    size_t len = 0;
    while (*str++) len++;
    return len;
}

int cos_atoi(const char* str) {
    int result = 0;
    int sign = 1;
    
    /* Skip whitespace */
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;
    
    /* Handle sign */
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    /* Convert digits */
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return result * sign;
}

/* cos_itoa is defined in cos_itoa.c; declared via cos_string.h */

void cos_utoa(unsigned int value, char* str, int base) {
    cos_itoa((int)value, str, base);
}

size_t cos_strnlen(const char* str, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && str[len] != 0) {
        len++;
    }
    return len;
}