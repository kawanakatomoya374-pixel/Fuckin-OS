/**
 * avcodec.h - hand-written stand-in for FFmpeg's real avcodec.h (which in
 * turn pulls in libavutil/frame.h and libavcodec/packet.h). See config.h
 * for the port-wide ground rules this follows.
 *
 * AVCodecContext/AVFrame/AVPacket below are NOT full copies of FFmpeg's
 * real structs - they define exactly the fields vp8.c/vp8dsp.c/webp.c/
 * h264pred.c/videodsp.c actually read or write (found by grepping every
 * `avctx->x` / frame-pointer `->x` / `pkt->x` in those files), in the
 * same field names and compatible types, so the *ported code is
 * unmodified* where it touches them. There is no ABI compatibility goal
 * with real FFmpeg here - nothing outside this decoder ever sees these
 * struct layouts.
 */
#ifndef COS_FFMPEG_WEBP_AVCODEC_H
#define COS_FFMPEG_WEBP_AVCODEC_H

#include <stdint.h>
#include <stddef.h>
#include "libavutil/log.h"
#include "defs.h"

enum AVPixelFormat {
    AV_PIX_FMT_NONE = -1,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUVA420P,
    AV_PIX_FMT_ARGB,
    /* Hardware-accelerated formats this port never produces (avctx->hwaccel
     * is always NULL - see codec_internal.h) - kept only so the vp8.c
     * source referencing them by name still compiles. */
    AV_PIX_FMT_CUDA,
    AV_PIX_FMT_CUARRAY,
    AV_PIX_FMT_VAAPI,
};

#define FF_THREAD_NONE  0
#define FF_THREAD_FRAME 1
#define FF_THREAD_SLICE 2

enum AVDiscard {
    AVDISCARD_NONE    = -16,
    AVDISCARD_DEFAULT = 0,
    AVDISCARD_NONREF  = 8,
    AVDISCARD_BIDIR   = 16,
    AVDISCARD_NONINTRA = 24,
    AVDISCARD_NONKEY  = 32,
    AVDISCARD_ALL     = 48,
};

enum AVColorSpace {
    AVCOL_SPC_BT470BG = 5,
};

enum AVColorRange {
    AVCOL_RANGE_MPEG = 1,
    AVCOL_RANGE_JPEG = 2,
};

/* AV_FRAME_FLAG_KEY/LOSSLESS are the only frame flags this port's
 * ported files set. */
#define AV_FRAME_FLAG_KEY (1 << 1)
#define AV_FRAME_FLAG_LOSSLESS (1 << 4)

/* Real FFmpeg pads every coded-data buffer it hands a bitstream reader
 * by this many bytes so bit/bytestream readers can overread a little
 * near the end without a bounds check on every single access - this
 * port's file-loading path (see jpeg_viewer.c's webp entry point once
 * wired up) allocates its read buffer with this same padding, exactly
 * as get_bits.h's init_get_bits() and this port's own bytestream use
 * of it both expect. */
#define AV_INPUT_BUFFER_PADDING_SIZE 64

#define AV_PICTURE_TYPE_NB 5 /* unused sizing constant some tables reference */

/* Opaque; nothing in this port dereferences an AVDictionary - webp.c's
 * one av_dict_set() call (attaching XMP metadata to the output frame)
 * is a deliberate no-op here, see cos_ffmpeg_shim.c: this port only
 * cares about decoded pixels, not metadata side-channels. */
typedef struct AVDictionary AVDictionary;
#define AV_DICT_DONT_STRDUP_VAL 4
int av_dict_set(AVDictionary** pm, const char* key, const char* value, int flags);

/* Opaque hardware-accel types: avctx->hwaccel is always NULL in this
 * port (nothing ever assigns it), so every "if (avctx->hwaccel)" branch
 * in the ported files is dead code that must compile but will never
 * execute - these exist only to give that dead code a type. */
typedef struct FFHWAccel FFHWAccel;
typedef struct AVHWAccel AVHWAccel;

/* Minimal refcounted buffer, backing AVFrame's own reference counting
 * (av_frame_ref/av_frame_unref in this port's frame.c-equivalent, see
 * cos_ffmpeg_shim.c) - not general-purpose like FFmpeg's real AVBuffer
 * (no custom free callback, no pooling), since nothing here needs more
 * than "decode a frame, use it, free it". */
typedef struct AVBufferRef {
    uint8_t* data;
    size_t   size;
    int*     refcount; /* shared between every AVBufferRef over the same data */
} AVBufferRef;

AVBufferRef* av_buffer_alloc(size_t size);
void         av_buffer_unref(AVBufferRef** buf);

#define AV_NUM_DATA_POINTERS 4

typedef struct AVFrame {
    uint8_t* data[AV_NUM_DATA_POINTERS];
    int      linesize[AV_NUM_DATA_POINTERS];
    int      width, height;
    enum AVPixelFormat format;
    int      flags;
    enum AVPictureType pict_type;
    int64_t  pts;
    int64_t  duration;
    AVDictionary* metadata;
    AVBufferRef* buf[AV_NUM_DATA_POINTERS]; /* ownership of data[] planes */
    int      key_frame; /* legacy alias some code paths still set */
} AVFrame;

AVFrame* av_frame_alloc(void);
void     av_frame_free(AVFrame** frame);
void     av_frame_unref(AVFrame* frame);
int      av_frame_ref(AVFrame* dst, const AVFrame* src);
AVFrame* av_frame_clone(const AVFrame* src);
int      av_frame_get_buffer(AVFrame* frame, int align);
int      av_frame_make_writable(AVFrame* frame);

typedef struct AVPacket {
    AVBufferRef* buf;
    const uint8_t* data;
    int      size;
    int64_t  pts;
    int      flags;
} AVPacket;

AVPacket* av_packet_alloc(void);
void      av_packet_free(AVPacket** pkt);
void      av_packet_unref(AVPacket* pkt);
const uint8_t* av_packet_get_side_data(const AVPacket* pkt, int type, size_t* size);

/* Opaque - see codec_internal.h's ff_frame_new_side_data() comment for
 * why this port always reports "no side data buffer" (NULL) rather
 * than providing a real one, and why that is a correct outcome rather
 * than a missing feature for what this port does with a decoded frame. */
typedef struct AVFrameSideData {
    uint8_t* data;
    size_t   size;
} AVFrameSideData;

/* Converts a four-character-code (as packed into a little-endian
 * uint32, FFmpeg's MKTAG() convention) into a printable string, purely
 * for av_log() diagnostics about a chunk type this port didn't
 * recognize. Implementation in cos_ffmpeg_shim.c. */
const char* av_fourcc2str(uint32_t fourcc);

#define AV_PKT_DATA_ICC_PROFILE 0 /* the one side-data type webp.c queries */
#define AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL 1 /* queried by vp8.c's WebM-alpha warning path */

typedef struct AVCodec AVCodec;

typedef struct AVCodecContext {
    void* priv_data;
    int width, height;
    int coded_width, coded_height;
    enum AVPixelFormat pix_fmt;
    int color_range;
    int colorspace;
    uint8_t* extradata;
    int      extradata_size;
    int thread_count;
    int active_thread_type;
    int skip_frame;
    int skip_loop_filter;
    const FFHWAccel* hwaccel;
    const AVClass* av_class;

    int (*execute)(struct AVCodecContext* c, int (*func)(struct AVCodecContext* c2, void* arg),
                    void* arg2, int* ret, int count, int size);
    int (*execute2)(struct AVCodecContext* c,
                     int (*func)(struct AVCodecContext* c2, void* arg, int jobnr, int threadnr),
                     void* arg2, int* ret, int count);
} AVCodecContext;

#endif
