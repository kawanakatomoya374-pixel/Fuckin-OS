/* Stand-in for libavcodec/pthread_internal.h. Every call site in vp9.c
 * that uses these declarations is wrapped in "#if HAVE_THREADS" (see
 * config.h - HAVE_THREADS is 0, this port is single-threaded like the
 * existing ffmpeg_webp/VP8 port), so none of the functions below are
 * ever actually called or need a real implementation - only
 * DEFINE_OFFSET_ARRAY's expansion (a single static const array, unused
 * when HAVE_THREADS is 0) needs to compile. */
#ifndef COS_FFMPEG_VP9_PTHREAD_INTERNAL_H
#define COS_FFMPEG_VP9_PTHREAD_INTERNAL_H
#include "avcodec.h"
#include <stddef.h>

#define THREAD_SENTINEL 0
int  ff_pthread_init(void *obj, const unsigned offsets[]);
void ff_pthread_free(void *obj, const unsigned offsets[]);
#define OFFSET_ARRAY(...) __VA_ARGS__, THREAD_SENTINEL
#define DEFINE_OFFSET_ARRAY(type, name, cnt_variable, mutexes, conds)         \
static const unsigned name ## _offsets[] = { offsetof(type, cnt_variable),    \
                                             OFFSET_ARRAY mutexes,            \
                                             OFFSET_ARRAY conds }
#endif
