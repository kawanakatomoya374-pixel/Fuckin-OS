#ifndef COS_FFMPEG_THREADFRAME_H
#define COS_FFMPEG_THREADFRAME_H
#include "avcodec.h"
/* Frame threading is not used in this port (thread_count == 1), so
 * progress reporting is a no-op and a ThreadFrame is just a frame plus
 * the context that owns it. Implementations in cos_hevc_shim.c. */
typedef struct ThreadFrame {
    AVFrame* f;
    AVCodecContext* owner[2];
    AVBufferRef* progress;
} ThreadFrame;

#define FF_THREAD_COPY_FLAGS 0
void ff_thread_report_progress(ThreadFrame* f, int progress, int field);
void ff_thread_await_progress(const ThreadFrame* f, int progress, int field);
int  ff_thread_get_ext_buffer(AVCodecContext* avctx, ThreadFrame* f, int flags);
void ff_thread_release_ext_buffer(ThreadFrame* f);
int  ff_thread_ref_frame(ThreadFrame* dst, const ThreadFrame* src);
#endif
