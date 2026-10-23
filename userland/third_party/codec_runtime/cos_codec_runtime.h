#ifndef COS_CODEC_RUNTIME_H
#define COS_CODEC_RUNTIME_H

/* Common freestanding portability surface for imported video decoders.
 * This header deliberately contains no POSIX or host libc dependency. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "../../include/cos.h"

#define COS_CODEC_MAX_DIMENSION 3840u
#define COS_CODEC_MAX_FRAME_BYTES (64u * 1024u * 1024u)
#define COS_CODEC_SINGLE_THREAD 1
#define COS_CODEC_NO_ASM 1

void *cos_codec_malloc(size_t size);
void *cos_codec_calloc(size_t count, size_t size);
void *cos_codec_realloc(void *ptr, size_t size);
void  cos_codec_free(void *ptr);
void *cos_codec_memcpy(void *dst, const void *src, size_t n);
void *cos_codec_memmove(void *dst, const void *src, size_t n);
void *cos_codec_memset(void *dst, int value, size_t n);
int   cos_codec_memcmp(const void *a, const void *b, size_t n);
void  cos_codec_abort(const char *reason);
void  cos_codec_log(const char *message);
void  cos_codec_qsort(void *base, size_t count, size_t size,
                     int (*compare)(const void *, const void *));

/* Import adapters use these names after including this header. */
#define malloc  cos_codec_malloc
#define calloc  cos_codec_calloc
#define realloc cos_codec_realloc
#define free    cos_codec_free
/* Use the compiler builtins rather than calls to cos_codec_mem*(): the
 * decoders (edge264 above all) copy fixed-size 8/16/32-byte vectors through
 * memcpy() and rely on the compiler expanding those inline. Because these
 * programs are built with -fno-builtin, plain memcpy() is an ordinary call, and
 * redirecting it to a wrapper made every such copy a function call (about a
 * third of all samples in an H.264 playback profile). Explicit __builtin_*
 * still expands constant sizes in place and falls back to the libc memcpy/
 * memset/memmove/memcmp symbols for variable sizes. */
#define memcpy  __builtin_memcpy
#define memmove __builtin_memmove
#define memset  __builtin_memset
#define memcmp  __builtin_memcmp
#define qsort   cos_codec_qsort

#endif
