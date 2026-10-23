/* Stand-in for libavcodec/thread.h. Every use it guards in vp8.c/vp8.h
 * is behind "#if HAVE_THREADS" (0 - see config.h), so this only needs
 * to exist; it does not need to declare anything a single-threaded
 * build actually calls. */
#ifndef COS_FFMPEG_WEBP_LAVC_THREAD_H
#define COS_FFMPEG_WEBP_LAVC_THREAD_H

#include "avcodec.h"
/* Single-threaded stand-ins: nothing ever runs concurrently. */
static inline void ff_thread_report_progress2(AVCodecContext* avctx, int field, int thread, int n) { (void)avctx; (void)field; (void)thread; (void)n; }
static inline void ff_thread_await_progress2(AVCodecContext* avctx, int field, int thread, int shift) { (void)avctx; (void)field; (void)thread; (void)shift; }
static inline int ff_slice_thread_allocz_entries(AVCodecContext* avctx, int count) { (void)avctx; (void)count; return 0; }
static inline int ff_slice_thread_init_progress(AVCodecContext* avctx) { (void)avctx; return 0; }
/* Slice threading is never used (only frame threading); declared so the guarded call sites compile. */
static inline int ff_slice_thread_execute_with_mainfunc(AVCodecContext* avctx, int (*func2)(AVCodecContext* c, void* arg, int jobnr, int threadnr),
        int (*mainfunc)(AVCodecContext* c), void* arg, int* ret, int job_count) {
    (void)avctx; (void)func2; (void)mainfunc; (void)arg; (void)ret; (void)job_count; return -1;
}
#endif
