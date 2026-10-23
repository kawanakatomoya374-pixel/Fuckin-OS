/**
 * cos_hevc_shim.c - the libavcodec/libavutil functions FFmpeg's HEVC decoder
 * (n7.0 hevcdec.c and friends, in this directory) calls that the VP9 port's
 * cos_ffmpeg_shim.c does not already provide.
 *
 * Two kinds of thing live here:
 *
 *  - Real, small implementations of things the decoder genuinely needs:
 *    reference-counted buffers with a destructor, frame move/copy-props,
 *    pixel-format descriptors, av_reduce(), buffer growth helpers, the frame
 *    "threading" hooks (which, with one thread, only have to allocate the
 *    frame), and the profile table.
 *
 *  - Inert stand-ins for features this port deliberately leaves out: Dolby
 *    Vision RPUs, film grain synthesis, HDR/SEI side data export, MD5 picture
 *    hash verification and SMPTE timecodes. The decoder only reaches them for
 *    streams that carry that metadata AND when the matching export flags are
 *    set, which cos_hevc_entry.c never does; they exist so the decoder source
 *    can stay unmodified apart from include paths. Where a stand-in must
 *    return something, it returns "not supported" or "nothing".
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "threadframe.h"
#include "profiles.h"
#include "dovi_rpu.h"
#include "h274.h"
#include "aom_film_grain.h"
#include "dynamic_hdr_vivid.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "libavutil/md5.h"
#include "libavutil/timecode.h"
#include "libavutil/stereo3d.h"
#include "libavutil/display.h"
#include "libavutil/pixdesc.h"
#include "libavutil/film_grain_params.h"
#include "libavutil/hdr_dynamic_metadata.h"
#include "libavutil/ambient_viewing_environment.h"
#include "libavutil/mastering_display_metadata.h"

/* ---- reference-counted buffers ------------------------------------------ */

AVBufferRef* av_buffer_create(uint8_t* data, size_t size,
                              void (*free_fn)(void* opaque, uint8_t* data),
                              void* opaque, int flags) {
    (void)flags;
    AVBufferRef* buf = (AVBufferRef*)av_malloc(sizeof(AVBufferRef));
    if (!buf) return NULL;
    buf->refcount = (int*)av_malloc(sizeof(int));
    if (!buf->refcount) { av_free(buf); return NULL; }
    *buf->refcount = 1;
    buf->data = data;
    buf->size = size;
    buf->free_fn = free_fn;
    buf->opaque = opaque;
    return buf;
}

int av_buffer_is_writable(const AVBufferRef* buf) {
    return buf && *buf->refcount == 1;
}

int av_buffer_replace(AVBufferRef** pdst, const AVBufferRef* src) {
    AVBufferRef* dst = *pdst;
    if (!src) { av_buffer_unref(pdst); return 0; }
    if (dst && dst->refcount == src->refcount) {
        /* same underlying buffer already: just refresh the view */
        dst->data = src->data;
        dst->size = src->size;
        return 0;
    }
    AVBufferRef* nb = av_buffer_ref(src);
    if (!nb) return AVERROR(ENOMEM);
    av_buffer_unref(pdst);
    *pdst = nb;
    return 0;
}

/* ---- frames -------------------------------------------------------------- */

void av_frame_move_ref(AVFrame* dst, AVFrame* src) {
    av_frame_unref(dst);
    *dst = *src;
    memset(src, 0, sizeof(*src));
    src->format = AV_PIX_FMT_NONE;
}

int av_frame_copy_props(AVFrame* dst, const AVFrame* src) {
    dst->flags = (dst->flags & ~(AV_FRAME_FLAG_KEY | AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST | AV_FRAME_FLAG_CORRUPT)) |
                 (src->flags & (AV_FRAME_FLAG_KEY | AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST | AV_FRAME_FLAG_CORRUPT));
    dst->pict_type = src->pict_type;
    dst->pts = src->pts;
    dst->pkt_dts = src->pkt_dts;
    dst->duration = src->duration;
    dst->crop_top = src->crop_top;
    dst->crop_bottom = src->crop_bottom;
    dst->crop_left = src->crop_left;
    dst->crop_right = src->crop_right;
    dst->colorspace = src->colorspace;
    dst->color_range = src->color_range;
    dst->color_trc = src->color_trc;
    dst->color_primaries = src->color_primaries;
    dst->chroma_location = src->chroma_location;
    dst->sample_aspect_ratio = src->sample_aspect_ratio;
    dst->opaque = src->opaque;
    return 0;
}

static AVFrameSideData* cos_attach_side_data(AVFrame* frame, enum AVFrameSideDataType type,
                                              uint8_t* data, size_t size, AVBufferRef* buf) {
    AVFrameSideData* sd = (AVFrameSideData*)av_mallocz(sizeof(*sd));
    AVFrameSideData** arr = sd ? (AVFrameSideData**)av_realloc_array(frame->side_data,
                                                    (size_t)frame->nb_side_data + 1, sizeof(*arr)) : NULL;
    if (!sd || !arr) {
        av_free(sd);
        if (arr) frame->side_data = arr;
        return NULL;
    }
    sd->data = data; sd->size = size; sd->type = (int)type; sd->buf = buf;
    arr[frame->nb_side_data++] = sd;
    frame->side_data = arr;
    return sd;
}

AVFrameSideData* av_frame_new_side_data(AVFrame* frame, enum AVFrameSideDataType type, size_t size) {
    uint8_t* data = (uint8_t*)av_mallocz(size ? size : 1);
    if (!data) return NULL;
    AVFrameSideData* sd = cos_attach_side_data(frame, type, data, size, NULL);
    if (!sd) av_free(data);
    return sd;
}

/* On success the frame takes ownership of buf; on failure (NULL) the caller keeps it. */
AVFrameSideData* av_frame_new_side_data_from_buf(AVFrame* frame, enum AVFrameSideDataType type, AVBufferRef* buf) {
    if (!buf) return NULL;
    return cos_attach_side_data(frame, type, buf->data, buf->size, buf);
}

int ff_frame_new_side_data_from_buf(const AVCodecContext* avctx, AVFrame* frame,
                                    enum AVFrameSideDataType type, AVBufferRef** buf,
                                    AVFrameSideData** psd) {
    (void)avctx;
    AVFrameSideData* sd = av_frame_new_side_data_from_buf(frame, type, *buf);
    if (!sd) return AVERROR(ENOMEM);
    *buf = NULL;
    if (psd) *psd = sd;
    return 0;
}

void av_frame_remove_side_data(AVFrame* frame, enum AVFrameSideDataType type) {
    (void)frame; (void)type;
}

const AVPacketSideData* ff_get_coded_side_data(const AVCodecContext* avctx, int type) {
    (void)avctx; (void)type;
    return NULL;
}

int ff_set_sar(AVCodecContext* avctx, AVRational sar) {
    avctx->sample_aspect_ratio = sar;
    return 0;
}

int ff_decode_mastering_display_new(const AVCodecContext* avctx, AVFrame* frame, AVMasteringDisplayMetadata** mdm) {
    (void)avctx; (void)frame;
    if (mdm) *mdm = NULL;
    return 0;
}

int ff_decode_content_light_new(const AVCodecContext* avctx, AVFrame* frame, AVContentLightMetadata** clm) {
    (void)avctx; (void)frame;
    if (clm) *clm = NULL;
    return 0;
}

/* ---- frame threading hooks ------------------------------------------------
 * With one thread these only allocate the frame. With frame threading (several cores each decoding a
 * different frame) every frame also carries a progress record - the number of the last picture row that is
 * final - which a later frame polls before it reads reference pixels. The cores spin on it: they have no
 * system calls to sleep with, and the wait is short by construction (the reference was started earlier). */
volatile int* cos_hevc_mt_abort;   /* points at the group's abort flag; set by cos_hevc_entry.c */

int ff_thread_get_ext_buffer(AVCodecContext* avctx, ThreadFrame* f, int flags) {
    (void)flags;
    /* f->f was allocated by the caller (hevc_refs.c: av_frame_alloc()) and may
     * already carry the size/format; the decoder's own idea of both lives on
     * the context. */
    f->owner[0] = f->owner[1] = avctx;
    /* Like ff_get_buffer(): allocate for the *coded* size. HEVC sets avctx->width/height to
     * the cropped (conformance-window) size and coded_width/height to the full coded size,
     * then reports the crop rectangle on the frame (FF_CODEC_CAP_EXPORTS_CROPPING). */
    f->f->width  = avctx->coded_width  > avctx->width  ? avctx->coded_width  : avctx->width;
    f->f->height = avctx->coded_height > avctx->height ? avctx->coded_height : avctx->height;
    f->f->format = avctx->pix_fmt;
    if (avctx->active_thread_type & FF_THREAD_FRAME) {
        f->progress = av_buffer_alloc(2 * sizeof(int));
        if (!f->progress) return AVERROR(ENOMEM);
        ((int*)f->progress->data)[0] = ((int*)f->progress->data)[1] = -1;
    }
    return av_frame_get_buffer(f->f, 0);
}

void ff_thread_release_ext_buffer(ThreadFrame* f) {
    f->owner[0] = f->owner[1] = NULL;
    av_buffer_unref(&f->progress);
    if (f->f) av_frame_unref(f->f);
}

int ff_thread_ref_frame(ThreadFrame* dst, const ThreadFrame* src) {
    dst->owner[0] = src->owner[0];
    dst->owner[1] = src->owner[1];
    dst->progress = src->progress ? av_buffer_ref(src->progress) : NULL;
    return av_frame_ref(dst->f, src->f);
}

void ff_thread_report_progress(ThreadFrame* f, int progress, int field) {
    if (!f->progress) return;
    int* p = (int*)f->progress->data + field;
    if (__atomic_load_n(p, __ATOMIC_RELAXED) >= progress) return;
    __atomic_store_n(p, progress, __ATOMIC_RELEASE);
}

void ff_thread_await_progress(const ThreadFrame* f, int progress, int field) {
    if (!f->progress) return;
    const int* p = (const int*)f->progress->data + field;
    while (__atomic_load_n(p, __ATOMIC_ACQUIRE) < progress) {
        if (cos_hevc_mt_abort && __atomic_load_n(cos_hevc_mt_abort, __ATOMIC_RELAXED)) return;
        if(!cos_idle_work()) __builtin_ia32_pause();
    }
}

void ff_thread_finish_setup(AVCodecContext* avctx) {
    AVCodecInternal* in = avctx->internal;
    if (in && in->setup_done_cb) { void (*cb)(void*) = in->setup_done_cb; in->setup_done_cb = NULL; cb(in->setup_done_arg); }
}

/* ---- pixel formats -------------------------------------------------------- */

#define PIXDESC(n, name_, nc, cw, ch, dep) \
    case n: { static const AVPixFmtDescriptor d = { name_, nc, cw, ch, AV_PIX_FMT_FLAG_PLANAR, \
        { {0,1,0,0,dep}, {1,1,0,0,dep}, {2,1,0,0,dep}, {0,0,0,0,0} } }; return &d; }

const AVPixFmtDescriptor* av_pix_fmt_desc_get(enum AVPixelFormat f) {
    switch (f) {
    PIXDESC(AV_PIX_FMT_GRAY8,     "gray",        1, 0, 0, 8)
    PIXDESC(AV_PIX_FMT_GRAY9,     "gray9le",     1, 0, 0, 9)
    PIXDESC(AV_PIX_FMT_GRAY10,    "gray10le",    1, 0, 0, 10)
    PIXDESC(AV_PIX_FMT_GRAY12,    "gray12le",    1, 0, 0, 12)
    PIXDESC(AV_PIX_FMT_YUV420P,   "yuv420p",     3, 1, 1, 8)
    PIXDESC(AV_PIX_FMT_YUV422P,   "yuv422p",     3, 1, 0, 8)
    PIXDESC(AV_PIX_FMT_YUV444P,   "yuv444p",     3, 0, 0, 8)
    PIXDESC(AV_PIX_FMT_YUVJ420P,  "yuvj420p",    3, 1, 1, 8)
    PIXDESC(AV_PIX_FMT_YUVJ422P,  "yuvj422p",    3, 1, 0, 8)
    PIXDESC(AV_PIX_FMT_YUVJ444P,  "yuvj444p",    3, 0, 0, 8)
    PIXDESC(AV_PIX_FMT_YUV420P9,  "yuv420p9le",  3, 1, 1, 9)
    PIXDESC(AV_PIX_FMT_YUV422P9,  "yuv422p9le",  3, 1, 0, 9)
    PIXDESC(AV_PIX_FMT_YUV444P9,  "yuv444p9le",  3, 0, 0, 9)
    PIXDESC(AV_PIX_FMT_YUV420P10, "yuv420p10le", 3, 1, 1, 10)
    PIXDESC(AV_PIX_FMT_YUV422P10, "yuv422p10le", 3, 1, 0, 10)
    PIXDESC(AV_PIX_FMT_YUV444P10, "yuv444p10le", 3, 0, 0, 10)
    PIXDESC(AV_PIX_FMT_YUV420P12, "yuv420p12le", 3, 1, 1, 12)
    PIXDESC(AV_PIX_FMT_YUV422P12, "yuv422p12le", 3, 1, 0, 12)
    PIXDESC(AV_PIX_FMT_YUV444P12, "yuv444p12le", 3, 0, 0, 12)
    PIXDESC(AV_PIX_FMT_GBRP,      "gbrp",        3, 0, 0, 8)
    PIXDESC(AV_PIX_FMT_GBRP10,    "gbrp10le",    3, 0, 0, 10)
    PIXDESC(AV_PIX_FMT_GBRP12,    "gbrp12le",    3, 0, 0, 12)
    default: return NULL;
    }
}

const char* av_color_transfer_name(enum AVColorTransferCharacteristic trc) { (void)trc; return "unknown"; }
const char* av_color_primaries_name(enum AVColorPrimaries p) { (void)p; return "unknown"; }
const char* av_color_space_name(enum AVColorSpace s) { (void)s; return "unknown"; }

/* ---- misc libavutil -------------------------------------------------------- */

const char* av_default_item_name(void* ctx) { (void)ctx; return "hevc"; }

int av_reduce(int* dst_num, int* dst_den, int64_t num, int64_t den, int64_t max) {
    int64_t a = num < 0 ? -num : num, b = den < 0 ? -den : den;
    int64_t x = a, y = b;
    while (y) { int64_t t = x % y; x = y; y = t; }
    if (x > 1) { a /= x; b /= x; }
    int exact = 1;
    if (a > max || b > max) {
        /* Not exactly representable: scale both down together. */
        while (a > max || b > max) { a >>= 1; b >>= 1; exact = 0; }
        if (b == 0) b = 1;
    }
    *dst_num = (int)(num < 0 ? -a : a);
    *dst_den = (int)(den < 0 ? -b : b);
    if (den < 0 && num >= 0) { *dst_num = -*dst_num; *dst_den = -*dst_den; }
    return exact;
}

void* av_realloc_array(void* ptr, size_t nmemb, size_t size) {
    if (nmemb && size > (size_t)-1 / nmemb) return NULL;
    return av_realloc(ptr, nmemb * size);
}

int av_reallocp_array(void* ptr, size_t nmemb, size_t size) {
    void** p = (void**)ptr;
    void* r = av_realloc_array(*p, nmemb, size);
    if (!r && nmemb && size) { av_freep(p); return AVERROR(ENOMEM); }
    *p = r;
    return 0;
}

void* av_fast_realloc(void* ptr, unsigned int* size, size_t min_size) {
    if (min_size <= *size) return ptr;
    size_t grown = min_size + min_size / 16 + 32;
    if (grown < min_size) return NULL; /* overflow */
    void* r = av_realloc(ptr, grown);
    if (!r) return NULL;
    *size = (unsigned int)grown;
    return r;
}

void av_memcpy_backptr(uint8_t* dst, int back, int cnt) {
    const uint8_t* src = dst - back;
    for (int i = 0; i < cnt; i++) dst[i] = src[i]; /* may overlap on purpose: repeats the pattern */
}

size_t av_strlcatf(char* dst, size_t size, const char* fmt, ...) {
    size_t len = strlen(dst);
    if (len >= size) return len;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst + len, size - len, fmt, ap);
    va_end(ap);
    return len + (n > 0 ? (size_t)n : 0);
}

/* ---- profiles ----------------------------------------------------------------- */

const AVProfile ff_hevc_profiles[] = {
    { AV_PROFILE_HEVC_MAIN,               "Main"               },
    { AV_PROFILE_HEVC_MAIN_10,            "Main 10"            },
    { AV_PROFILE_HEVC_MAIN_STILL_PICTURE, "Main Still Picture" },
    { AV_PROFILE_HEVC_REXT,               "Rext"               },
    { AV_PROFILE_HEVC_SCC,                "Scc"                },
    { AV_PROFILE_UNKNOWN },
};

/* ---- MD5 picture-hash verification (off: AV_EF_CRCCHECK is never set) ----------- */

struct AVMD5 { int unused; };
struct AVMD5* av_md5_alloc(void) { return (struct AVMD5*)av_mallocz(sizeof(struct AVMD5)); }
void av_md5_init(struct AVMD5* ctx) { (void)ctx; }
void av_md5_update(struct AVMD5* ctx, const uint8_t* src, size_t len) { (void)ctx; (void)src; (void)len; }
void av_md5_final(struct AVMD5* ctx, uint8_t* dst) { (void)ctx; memset(dst, 0, 16); }

/* ---- SMPTE timecodes (SEI side data is not exported) ------------------------------ */

uint32_t av_timecode_get_smpte(AVRational rate, int drop, int hh, int mm, int ss, int ff) {
    (void)rate; (void)drop; (void)hh; (void)mm; (void)ss; (void)ff;
    return 0;
}

char* av_timecode_make_smpte_tc_string2(char* buf, AVRational rate, uint32_t tcsmpte, int prevent_df, int skip_field) {
    (void)rate; (void)tcsmpte; (void)prevent_df; (void)skip_field;
    buf[0] = 0;
    return buf;
}

/* ---- HDR / stereo / display-orientation side data (not exported) -------------------- */

AVStereo3D* av_stereo3d_create_side_data(AVFrame* frame) { (void)frame; return NULL; }
AVAmbientViewingEnvironment* av_ambient_viewing_environment_create_side_data(AVFrame* frame) { (void)frame; return NULL; }
void av_display_rotation_set(int32_t matrix[9], double angle) { (void)angle; memset(matrix, 0, 9 * sizeof(int32_t)); }
void av_display_matrix_flip(int32_t matrix[9], int hflip, int vflip) { (void)matrix; (void)hflip; (void)vflip; }
AVDynamicHDRPlus* av_dynamic_hdr_plus_alloc(size_t* size) { if (size) *size = 0; return NULL; }
int av_dynamic_hdr_plus_from_t35(AVDynamicHDRPlus* s, const uint8_t* data, size_t size) { (void)s; (void)data; (void)size; return AVERROR(ENOSYS); }
AVDynamicHDRVivid* av_dynamic_hdr_vivid_alloc(size_t* size) { if (size) *size = 0; return NULL; }
int ff_parse_itu_t_t35_to_dynamic_hdr_vivid(AVDynamicHDRVivid* s, const uint8_t* data, int size) { (void)s; (void)data; (void)size; return AVERROR(ENOSYS); }
int ff_parse_a53_cc(AVBufferRef** pbuf, const uint8_t* data, int size) { (void)pbuf; (void)data; (void)size; return 0; }

/* ---- film grain (not applied) ----------------------------------------------------------- */

AVFilmGrainParams* av_film_grain_params_create_side_data(AVFrame* frame) { (void)frame; return NULL; }
const AVFilmGrainParams* av_film_grain_params_select(const AVFrame* frame) { (void)frame; return NULL; }
int ff_aom_parse_film_grain_sets(AVFilmGrainAFGS1Params* s, const uint8_t* payload, int payload_size) {
    (void)s; (void)payload; (void)payload_size; return AVERROR(ENOSYS);
}
int ff_aom_attach_film_grain_sets(const AVFilmGrainAFGS1Params* s, AVFrame* frame) { (void)s; (void)frame; return 0; }
int ff_aom_apply_film_grain(AVFrame* out, const AVFrame* in, const AVFilmGrainParams* params) {
    (void)out; (void)in; (void)params; return AVERROR(ENOSYS);
}
int ff_h274_apply_film_grain(AVFrame* out, const AVFrame* in, H274FilmGrainDatabase* db, const AVFilmGrainParams* params) {
    (void)out; (void)in; (void)db; (void)params; return AVERROR(ENOSYS);
}

/* ---- Dolby Vision (not handled) ------------------------------------------------------------- */

void ff_dovi_ctx_unref(DOVIContext* s) { (void)s; }
void ff_dovi_ctx_flush(DOVIContext* s) { (void)s; }
void ff_dovi_ctx_replace(DOVIContext* s, const DOVIContext* s0) { (void)s; (void)s0; }   /* Dolby Vision is not supported */
void ff_dovi_update_cfg(DOVIContext* s, const AVDOVIDecoderConfigurationRecord* cfg) { (void)s; (void)cfg; }
int  ff_dovi_rpu_parse(DOVIContext* s, const uint8_t* rpu, size_t rpu_size) {
    (void)s; (void)rpu; (void)rpu_size; return 0;
}
int  ff_dovi_attach_side_data(DOVIContext* s, AVFrame* frame) { (void)s; (void)frame; return 0; }
