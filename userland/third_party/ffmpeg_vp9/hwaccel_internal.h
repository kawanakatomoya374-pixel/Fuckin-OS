/* Stand-in for libavcodec/hwaccel_internal.h. avctx->hwaccel is always
 * NULL in this port (nothing assigns it - see avcodec.h), so every
 * branch in vp8.c that reads through an FFHWAccel* is dead code that
 * still needs to compile. ffhwaccel() and ff_hwaccel_frame_priv_alloc()
 * are provided so it does, without ever needing to do real work. */
#ifndef COS_FFMPEG_WEBP_HWACCEL_INTERNAL_H
#define COS_FFMPEG_WEBP_HWACCEL_INTERNAL_H

#include "avcodec.h"

struct FFHWAccel {
    /* vp9.c (unlike vp8.c) calls this with 3 args, matching the older
     * FFmpeg hwaccel ABI it was written against - never actually
     * invoked either way (avctx->hwaccel is always NULL). */
    int (*start_frame)(AVCodecContext* avctx, const uint8_t* buf, uint32_t buf_size);
    int (*decode_slice)(AVCodecContext* avctx, const uint8_t* buf, uint32_t buf_size);
    int (*end_frame)(AVCodecContext* avctx);
    void (*flush)(AVCodecContext* avctx);
    int frame_priv_data_size;
    int (*decode_params)(AVCodecContext* avctx, int type, const uint8_t* buf, uint32_t buf_size);
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
#define FF_HW_SIMPLE_CALL(avctx, cb) (0)

int ff_hwaccel_frame_priv_alloc(AVCodecContext* avctx, void** hwaccel_picture_private);

/* Dead code in this port (avctx->hwaccel is always NULL): the HEVC decoder
 * guards every use with "if (avctx->hwaccel)". */
#ifndef FF_HW_CALL
#define FF_HW_CALL(avctx, function, ...) \
    (ffhwaccel((avctx)->hwaccel)->function((avctx), __VA_ARGS__))
#endif
#endif
