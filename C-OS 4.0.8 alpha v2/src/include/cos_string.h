/**
 * cos_string.h - Common String Utilities for C-OS
 * Provides basic string manipulation functions for freestanding kernel environment
 */

#ifndef COS_STRING_H
#define COS_STRING_H

#include <stddef.h>
#include <stdbool.h>

/**
 * Set memory to a specific value
 * @param ptr  Pointer to memory region
 * @param value Value to set (converted to unsigned char)
 * @param num  Number of bytes to set
 * @return Original pointer
 */
void* cos_memset(void* ptr, int value, size_t num);

/**
 * Copy memory from source to destination
 * @param dest Destination pointer
 * @param src Source pointer
 * @param num Number of bytes to copy
 * @return Original destination pointer
 */
void* cos_memcpy(void* dest, const void* src, size_t num);

/**
 * Copy string from source to destination
 * @param dest Destination buffer
 * @param src Source string
 * @return Original destination pointer
 */
char* cos_strcpy(char* dest, const char* src);

/**
 * Copy at most n characters from source to destination
 * @param dest Destination buffer
 * @param src Source string
 * @param n Maximum characters to copy
 * @return Original destination pointer
 */
char* cos_strncpy(char* dest, const char* src, size_t n);

/**
 * Append source string to destination
 * @param dest Destination buffer (must have space)
 * @param src Source string
 * @return Original destination pointer
 */
char* cos_strcat(char* dest, const char* src);

/**
 * Append at most n characters from source to destination
 * @param dest Destination buffer
 * @param src Source string
 * @param n Maximum characters to append
 * @return Original destination pointer
 */
char* cos_strncat(char* dest, const char* src, size_t n);

/**
 * Compare two strings
 * @param s1 First string
 * @param s2 Second string
 * @return 0 if equal, difference otherwise
 */
int cos_strcmp(const char* s1, const char* s2);

/**
 * Compare at most n characters of two strings
 * @param s1 First string
 * @param s2 Second string
 * @param n Maximum characters to compare
 * @return 0 if equal, difference otherwise
 */
int cos_strncmp(const char* s1, const char* s2, size_t n);

/**
 * Find substring in string
 * @param haystack String to search in
 * @param needle Substring to search for
 * @return Pointer to first occurrence or NULL
 */
char* cos_strstr(const char* haystack, const char* needle);

/**
 * Find first occurrence of character
 * @param str String to search
 * @param c Character to find
 * @return Pointer to first occurrence or NULL
 */
char* cos_strchr(const char* str, int c);

/**
 * Get string length
 * @param str String to measure
 * @return Number of characters before null terminator
 */
size_t cos_strlen(const char* str);

/**
 * Convert string to integer (base 10)
 * @param str String to convert
 * @return Converted integer value
 */
int cos_atoi(const char* str);

/**
 * Convert unsigned integer to string
 * @param value Value to convert
 * @param str Output buffer
 * @param base Numeric base (typically 10)
 */
char* cos_itoa(int value, char* str, int base);

/**
 * Convert unsigned integer to string
 * @param value Value to convert
 * @param str Output buffer
 * @param base Numeric base
 */
void cos_utoa(unsigned int value, char* str, int base);

/**
 * Find length of string with limit
 * @param str String to measure
 * @param maxlen Maximum length to check
 * @return Min(string length, maxlen)
 */
size_t cos_strnlen(const char* str, size_t maxlen);

#endif /* COS_STRING_H */