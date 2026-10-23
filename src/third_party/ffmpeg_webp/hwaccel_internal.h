/* Stand-in for libavcodec/hwaccel_internal.h. avctx->hwaccel is always
 * NULL in this port (nothing assigns it - see avcodec.h), so every
 * branch in vp8.c that reads through an FFHWAccel* is dead code that
 * still needs to compile. ffhwaccel() and ff_hwaccel_frame_priv_alloc()
 * are provided so it does, without ever needing to do real work. */
#ifndef COS_FFMPEG_WEBP_HWACCEL_INTERNAL_H
#define COS_FFMPEG_WEBP_HWACCEL_INTERNAL_H

#include "avcodec.h"

struct FFHWAccel {
    int (*start_frame)(AVCodecContext* avctx, AVBufferRef* buf,
                        const uint8_t* buf_data, uint32_t buf_size);
    int (*decode_slice)(AVCodecContext* avctx, const uint8_t* buf, uint32_t buf_size);
    int (*end_frame)(AVCodecContext* avctx);
    void (*flush)(AVCodecContext* avctx);
    int frame_priv_data_size;
};

static inline const FFHWAccel* ffhwaccel(const FFHWAccel* hwaccel) {
    return hwaccel; /* never actually called - avctx->hwaccel is always NULL */
}

/* FF_HW_HAS_CB(avctx, cb): real FFmpeg checks the AVHWAccel behind
 * avctx->hwaccel for a non-NULL callback pointer before calling through
 * FF_HW_SIMPLE_CALL. avctx->hwaccel is always NULL in this port (see
 * avcodec.h), so this can safely and permanently answer "no" rather
 * than replicate the real macro's field lookup - the branch it guards
 * accordingly never runs. */
#define FF_HW_HAS_CB(avctx, cb) 0
#define FF_HW_SIMPLE_CALL(avctx, cb) ((void)0)

int ff_hwaccel_frame_priv_alloc(AVCodecContext* avctx, void** hwaccel_picture_private);

#endif
