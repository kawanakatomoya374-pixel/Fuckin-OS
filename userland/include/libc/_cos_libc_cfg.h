/* Shared definitions for the C-OS standard C library headers.
 * These headers are what cos-cc puts on the system include path, so
 * `#include <stdio.h>` in a .c-os program gets THIS library - never the
 * host's glibc headers (cos-cc passes -nostdinc; see tools/cos-cc). */
#ifndef _COS_LIBC_CFG_H
#define _COS_LIBC_CFG_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#ifdef __cplusplus
#define _COS_BEGIN extern "C" {
#define _COS_END }
#else
#define _COS_BEGIN
#define _COS_END
#endif
#endif
