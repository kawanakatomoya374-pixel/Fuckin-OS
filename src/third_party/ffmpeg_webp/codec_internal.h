/**
 * codec_internal.h - stand-in for libavcodec/codec_internal.h.
 *
 * The `const FFCodec ff_vp7_decoder = { ... }` / `ff_vp8_decoder`
 * structs at the bottom of vp8.c are FFmpeg's normal codec-registration
 * mechanism (so avcodec_find_decoder() etc. can locate them). This port
 * has no such registry - cos_ffmpeg_shim.c calls ff_vp8_decode_init() /
 * the frame-decode entry point / ff_vp8_decode_free() directly - so
 * those two struct literals only need to compile as inert, unused
 * global data. FFCodec and the macros below are shaped just permissively
 * enough for that: every field vp8.c's initializers touch exists with a
 * plausible type, nothing more.
 */
#ifndef COS_FFMPEG_WEBP_CODEC_INTERNAL_H
#define COS_FFMPEG_WEBP_CODEC_INTERNAL_H

#include "avcodec.h"
#include "codec_id.h"

#define AVMEDIA_TYPE_VIDEO 0

#define AV_CODEC_CAP_DR1            (1 << 0)
#define AV_CODEC_CAP_FRAME_THREADS  (1 << 1)
#define AV_CODEC_CAP_SLICE_THREADS  (1 << 2)
#define FF_CODEC_CAP_USES_PROGRESSFRAMES (1 << 0)

#define CODEC_LONG_NAME(x) .p.long_name = (x)
#define FF_CODEC_DECODE_CB(func) .decode_cb = (func)
/* Real FFmpeg's UPDATE_THREAD_CONTEXT expands to nothing when threads
 * are disabled (frame-threaded decode is the only thing that needs a
 * per-thread-context-copy callback) - vp8.c's own definition of the
 * function this wraps is itself compiled out under the same "#if
 * HAVE_THREADS" (see vp8.c, just above vp8_decode_update_thread_context).
 * This still has to expand to a *valid designated initializer*, not
 * nothing, since it appears as one comma-separated entry among several
 * in ff_vp8_decoder's initializer list - a bare NULL keeps the syntax
 * valid while assigning the same value the field would default to
 * anyway. */
#if HAVE_THREADS
#define UPDATE_THREAD_CONTEXT(func) .update_thread_context = (func)
#else
#define UPDATE_THREAD_CONTEXT(func) .update_thread_context = NULL
#endif

typedef struct AVCodecHWConfigInternal AVCodecHWConfigInternal;

typedef struct FFCodec {
    struct {
        const char* name;
        const char* long_name;
        int type;
        enum AVCodecID id;
        int capabilities;
    } p;
    int priv_data_size;
    int (*init)(AVCodecContext* avctx);
    int (*close)(AVCodecContext* avctx);
    int (*decode_cb)(AVCodecContext* avctx, AVFrame* frame, int* got_frame, AVPacket* avpkt);
    void (*flush)(AVCodecContext* avctx);
    int caps_internal;
    int (*update_thread_context)(AVCodecContext* dst, const AVCodecContext* src);
    const AVCodecHWConfigInternal* const* hw_configs;
} FFCodec;

/* Decode-path helpers actually called from the ported files (see the
 * long comment in avcodec.h about what "compatible, not ABI-identical"
 * means here). Implementations in cos_ffmpeg_shim.c. */
enum AVPixelFormat ff_get_format(AVCodecContext* avctx, const enum AVPixelFormat* fmts);
int ff_set_dimensions(AVCodecContext* avctx, int width, int height);
int ff_reget_buffer(AVCodecContext* avctx, AVFrame* frame, int flags);

/* Real FFmpeg's ff_thread_get_buffer() is ff_get_buffer() plus
 * frame-threading bookkeeping (registering the new frame with the
 * thread that owns it) - ff_get_buffer() itself just calls
 * av_frame_get_buffer() sized to frame->width/height/format, which the
 * caller (decode_entropy_coded_image() in webp.c, for lossless WebP's
 * Huffman-coded sub-images, which are their own dimensions - not
 * avctx's main-image width/height) has already set. This port is
 * single-threaded (see config.h), so the frame-threading half of that
 * is unnecessary and this is exactly av_frame_get_buffer(). */
static inline int ff_thread_get_buffer(AVCodecContext* avctx, AVFrame* frame, int flags) {
    (void)avctx;
    return av_frame_get_buffer(frame, 0);
}

/* Signals that an AVCodecContext's fields the next frame-threaded
 * decode thread would need (dimensions, pix_fmt, ...) are finalized for
 * this frame. Meaningful only for FF_THREAD_FRAME decoding (letting the
 * *next* thread start its own setup while this one keeps decoding);
 * this port never sets active_thread_type to FF_THREAD_FRAME (see
 * config.h - HAVE_THREADS is 0), so there is never a next thread
 * waiting on this, and it is a correct no-op rather than a
 * simplification. */
static inline void ff_thread_finish_setup(AVCodecContext* avctx) { (void)avctx; }

/* EXIF/ICC metadata attachment - deliberately inert (return success
 * without attaching anything). This port decodes pixels for on-screen
 * display; it does not surface a decoded image's metadata anywhere, so
 * there is nothing for these to usefully do. See avcodec.h's AVDictionary
 * comment for the same call made re: av_dict_set(). */
#define AV_EXIF_TIFF_HEADER 0
int ff_decode_exif_attach_buffer(AVCodecContext* avctx, AVFrame* frame,
                                  AVBufferRef** exif_buf, int flags);
#define AV_FRAME_DATA_ICC_PROFILE 0
/* Always reports "no side-data buffer" (*out_sd = NULL, return 0/success) -
 * every call site in webp.c already handles sd == NULL by skipping the
 * chunk's bytes instead of storing them (see the ICCP case in
 * webp_decode_frame()), so this is a correct "this port doesn't keep
 * embedded ICC profiles" outcome, not a partial/broken implementation
 * of one. */
int ff_frame_new_side_data(AVCodecContext* avctx, AVFrame* frame, int type,
                            size_t size, AVFrameSideData** out_sd);
#define FF_CODEC_CAP_ICC_PROFILES (1 << 1)

#endif
