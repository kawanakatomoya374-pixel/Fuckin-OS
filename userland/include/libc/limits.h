/* C library half of <limits.h>. GCC's own limits.h (found first on the
 * system include path) defines CHAR_BIT, INT_MAX, LONG_MAX, ... and pulls
 * this file in via #include_next for the C library's additions. */
#ifndef _COS_LIMITS_H
#define _COS_LIMITS_H
#define _LIBC_LIMITS_H_ 1
#define PATH_MAX 256
#define NAME_MAX 255
#define CHAR_BIT 8
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647
#define UINT_MAX 4294967295U
#define ULONG_MAX 18446744073709551615UL
#endif
