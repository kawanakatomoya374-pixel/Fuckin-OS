#ifndef STUB_STRING_H
#define STUB_STRING_H
#include <stddef.h>
void* memset(void* d, int v, size_t n);
void* memcpy(void* d, const void* s, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int   strcmp(const char* a, const char* b);
int   strncmp(const char* a, const char* b, size_t n);
char* strncpy(char* d, const char* s, size_t n);
#endif
