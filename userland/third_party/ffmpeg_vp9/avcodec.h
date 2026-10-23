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
#include "libavutil/pixfmt.h"
#include "libavutil/rational.h"
#include "libavutil/pixdesc.h"
#include "defs.h"

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

/* AV_FRAME_FLAG_KEY/LOSSLESS are the only frame flags this port's
 * ported files set. */
#define AV_FRAME_FLAG_CORRUPT (1 << 0)
#define AV_FRAME_FLAG_KEY (1 << 1)
#define AV_FRAME_FLAG_DISCARD (1 << 2)
#define AV_FRAME_FLAG_INTERLACED (1 << 3)
#define AV_FRAME_FLAG_TOP_FIELD_FIRST (1 << 4)
#undef AV_FRAME_FLAG_TOP_FIELD_FIRST
#define AV_FRAME_FLAG_TOP_FIELD_FIRST (1 << 5)
#define AV_FRAME_FLAG_LOSSLESS (1 << 4)

/* Real FFmpeg pads every coded-data buffer it hands a bitstream reader
 * by this many bytes so bit/bytestream readers can overread a little
 * near the end without a bounds check on every single access - this
 * port's file-loading path (see jpeg_viewer.c's webp entry point once
 * wired up) allocates its read buffer with this same padding, exactly
 * as get_bits.h's init_get_bits() and this port's own bytestream use
 * of it both expect. */
#define AV_CODEC_FLAG_BITEXACT (1 << 23)

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
    void   (*free_fn)(void* opaque, uint8_t* data); /* NULL: data came from av_malloc() */
    void*    opaque;
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
    int64_t  pkt_dts; /* set from AVPacket.dts on decode - this port never reads it back */
    int64_t  duration;
    AVDictionary* metadata;
    AVBufferRef* buf[AV_NUM_DATA_POINTERS]; /* ownership of data[] planes */
    int      key_frame; /* legacy alias some code paths still set */
    /* ---- fields added for the HEVC port (appended, so VP9 is unaffected) ---- */
    size_t   crop_top, crop_bottom, crop_left, crop_right;
    int      colorspace, color_range, color_trc, color_primaries, chroma_location;
    AVRational sample_aspect_ratio;
    void*    opaque;
    int      nb_side_data;
    struct AVFrameSideData** side_data;
    uint8_t* extended_data_unused;
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
    int64_t  dts; /* always equal to pts in this port - Media never supplies B-frame-reordered dts separately */
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
    int      type;      /* appended for the HEVC port */
    AVBufferRef* buf;   /* owner of data when attached from a buffer, else NULL (data is av_malloc'd) */
} AVFrameSideData;

/* Converts a four-character-code (as packed into a little-endian
 * uint32, FFmpeg's MKTAG() convention) into a printable string, purely
 * for av_log() diagnostics about a chunk type this port didn't
 * recognize. Implementation in cos_ffmpeg_shim.c. */
const char* av_fourcc2str(uint32_t fourcc);
/* Purely for an av_log() diagnostic naming a pixel format - implementation
 * in cos_ffmpeg_shim.c. */
const char* av_get_pix_fmt_name(enum AVPixelFormat fmt);

#define AV_PKT_DATA_ICC_PROFILE 0 /* the one side-data type webp.c queries */
#define FF_CODEC_PROPERTY_LOSSLESS (1 << 0) /* unused output - nothing reads avctx->properties back in this port */
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
    int export_side_data; /* always 0 in this port - see video_enc_params.h */
    int flags; /* always 0 in this port; AV_CODEC_FLAG_BITEXACT is the only bit vp9.c tests */
    int profile; /* set by vp9.c itself from the bitstream's own profile field */
    int properties; /* bitmask of FF_CODEC_PROPERTY_* this port never reads back */
    int skip_loop_filter;
    const FFHWAccel* hwaccel;
    const AVClass* av_class;

    int (*execute)(struct AVCodecContext* c, int (*func)(struct AVCodecContext* c2, void* arg),
                    void* arg2, int* ret, int count, int size);
    int (*execute2)(struct AVCodecContext* c,
                     int (*func)(struct AVCodecContext* c2, void* arg, int jobnr, int threadnr),
                     void* arg2, int* ret, int count);
    /* ---- fields added for the HEVC port (appended, so VP9 is unaffected) ---- */
    int codec_id;
    int err_recognition;
    int debug;
    int flags2;
    int level;
    int has_b_frames;
    int color_primaries, color_trc, chroma_sample_location;
    AVRational framerate;
    AVRational sample_aspect_ratio;
    AVRational pkt_timebase;
    const AVCodec* codec;
    struct AVCodecInternal* internal;
    struct AVPacketSideData* coded_side_data;
    int nb_coded_side_data;
} AVCodecContext;

/* Only what ported code reads: AVCodecContext::internal->is_copy. */
typedef struct AVCodecInternal {
    int is_copy; void* hwaccel_priv_data;
    /* frame threading (cos_hevc_entry.c): called when the frame's setup is complete */
    void (*setup_done_cb)(void* arg); void* setup_done_arg;
} AVCodecInternal;

typedef struct AVPacketSideData { uint8_t* data; size_t size; int type; } AVPacketSideData;

#define AV_EF_CRCCHECK  (1<<0)
#define AV_EF_BITSTREAM (1<<1)
#define AV_EF_BUFFER    (1<<2)
#define AV_EF_EXPLODE   (1<<3)
#define AV_EF_IGNORE_ERR (1<<15)
#define AV_EF_CAREFUL    (1<<16)
#define AV_EF_COMPLIANT  (1<<17)
#define AV_EF_AGGRESSIVE (1<<18)

#define AV_CODEC_FLAG2_CHUNKS (1 << 15)
#define AV_CODEC_FLAG_OUTPUT_CORRUPT (1 << 3)
#define AV_CODEC_EXPORT_DATA_FILM_GRAIN (1 << 2)
#define AV_CODEC_EXPORT_DATA_MVS (1 << 0)
#define FF_CODEC_PROPERTY_CLOSED_CAPTIONS (1 << 1)
#define FF_CODEC_PROPERTY_FILM_GRAIN (1 << 4)
#define AV_INPUT_BUFFER_PADDING_SIZE 64
#define AV_PKT_DATA_NEW_EXTRADATA 1
#define AV_PKT_DATA_DOVI_CONF 2

enum AVFrameSideDataType {
    AV_FRAME_DATA_PANSCAN, AV_FRAME_DATA_A53_CC, AV_FRAME_DATA_STEREO3D, AV_FRAME_DATA_MATRIXENCODING,
    AV_FRAME_DATA_DOWNMIX_INFO, AV_FRAME_DATA_REPLAYGAIN, AV_FRAME_DATA_DISPLAYMATRIX,
    AV_FRAME_DATA_AFD, AV_FRAME_DATA_MOTION_VECTORS, AV_FRAME_DATA_SKIP_SAMPLES,
    AV_FRAME_DATA_AUDIO_SERVICE_TYPE, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
    AV_FRAME_DATA_GOP_TIMECODE, AV_FRAME_DATA_SPHERICAL, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
    AV_FRAME_DATA_ICC_PROFILE, AV_FRAME_DATA_S12M_TIMECODE, AV_FRAME_DATA_DYNAMIC_HDR_PLUS,
    AV_FRAME_DATA_REGIONS_OF_INTEREST, AV_FRAME_DATA_VIDEO_ENC_PARAMS, AV_FRAME_DATA_SEI_UNREGISTERED,
    AV_FRAME_DATA_FILM_GRAIN_PARAMS, AV_FRAME_DATA_DETECTION_BBOXES, AV_FRAME_DATA_DOVI_RPU_BUFFER,
    AV_FRAME_DATA_DOVI_METADATA, AV_FRAME_DATA_DYNAMIC_HDR_VIVID, AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT,
};

AVBufferRef* av_buffer_ref(const AVBufferRef* buf);
AVBufferRef* av_buffer_create(uint8_t* data, size_t size, void (*free)(void* opaque, uint8_t* data), void* opaque, int flags);
int          av_buffer_is_writable(const AVBufferRef* buf);
int          av_buffer_replace(AVBufferRef** dst, const AVBufferRef* src);
void         av_frame_move_ref(AVFrame* dst, AVFrame* src);
AVFrameSideData* av_frame_new_side_data(AVFrame* frame, enum AVFrameSideDataType type, size_t size);



/* ---- more additions for the HEVC port ---- */
#ifndef SIZE_SPECIFIER
#define SIZE_SPECIFIER "zu"
#define PTRDIFF_SPECIFIER "td"
#endif
#define AV_CODEC_FLAG2_IGNORE_CROP (1 << 16)
#define FF_DEBUG_PICT_INFO 1
#define FF_DEBUG_BITSTREAM 4
#define AV_GET_BUFFER_FLAG_REF (1 << 0)
enum AVPictureStructure {
    AV_PICTURE_STRUCTURE_UNKNOWN, AV_PICTURE_STRUCTURE_TOP_FIELD,
    AV_PICTURE_STRUCTURE_BOTTOM_FIELD, AV_PICTURE_STRUCTURE_FRAME,
};
int  av_frame_copy_props(AVFrame* dst, const AVFrame* src);
void av_frame_remove_side_data(AVFrame* frame, enum AVFrameSideDataType type);
const char* av_color_primaries_name(enum AVColorPrimaries primaries);
const char* av_color_space_name(enum AVColorSpace space);
int ff_parse_a53_cc(AVBufferRef** pbuf, const uint8_t* data, int size);
#ifndef CONFIG_H264_SEI
#define CONFIG_H264_SEI 0
#endif
#ifndef CONFIG_HEVC_SEI
#define CONFIG_HEVC_SEI 1
#endif
#endif
