#ifndef STRING_H
#define STRING_H

#include "types.h"
#include <stdarg.h>

// Custom string functions (non-standard)
void  utoa(uint64_t val, char* buf, int base);
int   atoi(const char* s);
long  strtol(const char* s, char** endptr, int base);
long long strtoll(const char* s, char** endptr, int base);
unsigned long strtoul(const char* s, char** endptr, int base);
unsigned long long strtoull(const char* s, char** endptr, int base);
void  sprintf_simple(char* buf, const char* fmt, ...);
int   snprintf(char* str, size_t size, const char* format, ...);
int   vsnprintf(char* str, size_t size, const char* format, va_list args);
int   sprintf(char* str, const char* format, ...);

// Custom itoa to avoid conflict with standard library
char* cos_itoa(int val, char* buf, int base);

// Standard string functions
void* memset(void* dst, int val, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
void* memchr(const void* s, int c, size_t n);
size_t strlen(const char* s);
int   strcmp(const char* a, const char* b);
int   strncmp(const char* a, const char* b, size_t n);
int   strcasecmp(const char* a, const char* b);
int   strncasecmp(const char* a, const char* b, size_t n);
char* strdup(const char* s);
char* strndup(const char* s, size_t n);
/* Deliberately not a general ISO C sscanf: supports exactly what
 * ported code in this tree actually uses (%d, %u, %zu, literal
 * whitespace/characters), documented in full at the implementation.
 * Extend it if something new needs a specifier it doesn't have yet -
 * don't assume it's a complete implementation. */
int sscanf(const char* str, const char* format, ...);
char* strcpy(char* dst, const char* src);
char* strncpy(char* dst, const char* src, size_t n);
char* strcat(char* dst, const char* src);
char* strncat(char* dst, const char* src, size_t n);
size_t cos_strlcpy(char* dst, const char* src, size_t size);
size_t cos_strlcat(char* dst, const char* src, size_t size);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
char* strcasestr(const char* haystack, const char* needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char* strtok(char* s, const char* delimiters);

#endif
