/**
 * cos_ffmpeg_shim.c - implementations for every av_ and ff_ prefixed
 * function declared in this directory's compat headers (avcodec.h,
 * codec_internal.h, progressframe.h, libavutil/mem.h, libavutil/log.h, libavutil/refstruct.h,
 * libavutil/imgutils.h). See config.h for the port-wide ground rules.
 *
 * This is the ONLY file in this directory that is not a lightly-adapted
 * copy of real FFmpeg source - everything here is written fresh for this
 * port, specifically to satisfy what vp8.c/vp8dsp.c/h264pred.c/
 * videodsp.c/webp.c/vlc.c actually call.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include "avcodec.h"
#include "codec_internal.h"
#include "progressframe.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/imgutils.h"

extern void serial_puts(const char* s);

/* ---- logging ------------------------------------------------------- */

void av_log(void* avcl, int level, const char* fmt, ...) {
    (void)avcl;
    (void)level;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_puts("[webp] ");
    serial_puts(buf);
}

void av_log_once(void* avcl, int initial_level, int subsequent_level,
                  int* state, const char* fmt, ...) {
    (void)initial_level;
    (void)subsequent_level;
    if (state && *state) return;
    if (state) *state = 1;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_puts("[webp] ");
    serial_puts(buf);
}

/* ---- allocation ------------------------------------------------------
 * Thin wraps around this kernel's own malloc/free (declared in
 * <stdlib.h>, backed by the kernel heap - see src/kernel/memory.c). No
 * SIMD alignment is needed (config.h sets every ARCH_* to 0, so no
 * ported code takes an intrinsics path that would require it). */

void* av_malloc(size_t size) {
    if (size == 0) size = 1;
    return malloc(size);
}

void* av_mallocz(size_t size) {
    void* p = av_malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void* av_malloc_array(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) return NULL; /* overflow */
    return av_malloc(nmemb * size);
}

void* av_calloc(size_t nmemb, size_t size) {
    void* p = av_malloc_array(nmemb, size);
    if (p) memset(p, 0, nmemb * size);
    return p;
}

void* av_realloc(void* ptr, size_t size) {
    if (size == 0) size = 1;
    return realloc(ptr, size);
}

void* av_realloc_f(void* ptr, size_t nmemb, size_t size) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) {
        free(ptr);
        return NULL;
    }
    size_t total = nmemb * size;
    void* r = realloc(ptr, total ? total : 1);
    if (!r) free(ptr);
    return r;
}

void av_free(void* ptr) {
    free(ptr);
}

void av_freep(void* ptr) {
    void** p = (void**)ptr;
    if (p) {
        free(*p);
        *p = NULL;
    }
}

int av_image_check_size(unsigned w, unsigned h, int log_offset, void* log_ctx) {
    (void)log_offset;
    (void)log_ctx;
    /* Same ceiling real FFmpeg's default av_image_check_size2() uses in
     * spirit (reject anything that would overflow a plane-size
     * computation or is simply an unreasonable image to decode) -
     * INT_MAX/8 leaves ample headroom for a 4-byte-per-pixel plane
     * without overflowing a 32-bit multiply anywhere downstream. */
    if (w == 0 || h == 0) return AVERROR(EINVAL);
    if ((uint64_t)w * (uint64_t)h > (uint64_t)(INT32_MAX / 8)) return AVERROR(EINVAL);
    return 0;
}

/* ---- AVDictionary / misc metadata (deliberately inert) --------------
 * See avcodec.h's AVDictionary comment: this port displays decoded
 * pixels and has no metadata side-channel for a caller to read XMP/EXIF/
 * ICC data back out of, so these succeed without actually storing
 * anything. */

int av_dict_set(AVDictionary** pm, const char* key, const char* value, int flags) {
    (void)pm; (void)key; (void)value; (void)flags;
    return 0;
}

const char* av_fourcc2str(uint32_t fourcc) {
    static char buf[5];
    buf[0] = (char)(fourcc & 0xFF);
    buf[1] = (char)((fourcc >> 8) & 0xFF);
    buf[2] = (char)((fourcc >> 16) & 0xFF);
    buf[3] = (char)((fourcc >> 24) & 0xFF);
    buf[4] = '\0';
    for (int i = 0; i < 4; i++) {
        if (buf[i] < 0x20 || buf[i] > 0x7E) buf[i] = '.';
    }
    return buf;
}

/* ---- refcounted buffers (AVBufferRef) --------------------------------
 * Deliberately minimal versus real FFmpeg's AVBuffer (no custom free
 * callback, no pool) - every buffer this port ever creates is a plain
 * kmalloc'd block freed with plain kfree-via-free() once its refcount
 * hits zero, which is all av_frame_*() below ever needs. */

AVBufferRef* av_buffer_alloc(size_t size) {
    AVBufferRef* buf = (AVBufferRef*)av_malloc(sizeof(AVBufferRef));
    if (!buf) return NULL;
    buf->data = (uint8_t*)av_malloc(size);
    buf->refcount = (int*)av_malloc(sizeof(int));
    if (!buf->data || !buf->refcount) {
        av_free(buf->data);
        av_free(buf->refcount);
        av_free(buf);
        return NULL;
    }
    buf->size = size;
    *buf->refcount = 1;
    return buf;
}

static AVBufferRef* av_buffer_ref_dup(AVBufferRef* src) {
    if (!src) return NULL;
    AVBufferRef* dup = (AVBufferRef*)av_malloc(sizeof(AVBufferRef));
    if (!dup) return NULL;
    *dup = *src;
    (*dup->refcount)++;
    return dup;
}

void av_buffer_unref(AVBufferRef** bufp) {
    if (!bufp || !*bufp) return;
    AVBufferRef* buf = *bufp;
    if (--(*buf->refcount) <= 0) {
        av_free(buf->data);
        av_free(buf->refcount);
    }
    av_free(buf);
    *bufp = NULL;
}

/* ---- AVFrame ---------------------------------------------------------
 * Plane layout: tightly packed (no extra row padding beyond simple
 * width rounding needed by chroma subsampling), one AVBufferRef per
 * plane. Good enough for a decode-then-display pipeline that never
 * hands these frames to anything expecting real FFmpeg's stride
 * conventions. */

AVFrame* av_frame_alloc(void) {
    return (AVFrame*)av_mallocz(sizeof(AVFrame));
}

void av_frame_unref(AVFrame* frame) {
    if (!frame) return;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        av_buffer_unref(&frame->buf[i]);
        frame->data[i] = NULL;
        frame->linesize[i] = 0;
    }
    frame->width = frame->height = 0;
    frame->format = AV_PIX_FMT_NONE;
    frame->flags = 0;
    frame->key_frame = 0;
    frame->metadata = NULL;
}

void av_frame_free(AVFrame** frame) {
    if (!frame || !*frame) return;
    av_frame_unref(*frame);
    av_free(*frame);
    *frame = NULL;
}

/* Returns 0/1/2/3-plane count and per-plane subsampling for the pixel
 * formats this port's decoders ever produce (see AV_PIX_FMT_* in
 * avcodec.h) - not a general av_pix_fmt_desc_get() replacement, just
 * enough for av_frame_get_buffer()/ff_reget_buffer() to size planes
 * correctly for exactly these three formats. */
static int cos_plane_layout(enum AVPixelFormat fmt, int w, int h,
                             int plane_w[4], int plane_h[4], int bytes_per_px[4], int* nplanes) {
    switch (fmt) {
    case AV_PIX_FMT_YUV420P:
        *nplanes = 3;
        plane_w[0] = w;         plane_h[0] = h;         bytes_per_px[0] = 1;
        plane_w[1] = (w + 1)/2; plane_h[1] = (h + 1)/2; bytes_per_px[1] = 1;
        plane_w[2] = (w + 1)/2; plane_h[2] = (h + 1)/2; bytes_per_px[2] = 1;
        return 0;
    case AV_PIX_FMT_YUVA420P:
        *nplanes = 4;
        plane_w[0] = w;         plane_h[0] = h;         bytes_per_px[0] = 1;
        plane_w[1] = (w + 1)/2; plane_h[1] = (h + 1)/2; bytes_per_px[1] = 1;
        plane_w[2] = (w + 1)/2; plane_h[2] = (h + 1)/2; bytes_per_px[2] = 1;
        plane_w[3] = w;         plane_h[3] = h;         bytes_per_px[3] = 1;
        return 0;
    case AV_PIX_FMT_ARGB:
        *nplanes = 1;
        plane_w[0] = w; plane_h[0] = h; bytes_per_px[0] = 4;
        return 0;
    default:
        return AVERROR(EINVAL);
    }
    return AVERROR(EINVAL); /* unreachable given the cases above; keeps
                                the compiler happy without relying on it
                                proving the switch exhaustive. */
}

int av_frame_get_buffer(AVFrame* frame, int align) {
    (void)align;
    if (!frame || frame->width <= 0 || frame->height <= 0) return AVERROR(EINVAL);

    int pw[4], ph[4], bpp[4], nplanes = 0;
    if (cos_plane_layout(frame->format, frame->width, frame->height, pw, ph, bpp, &nplanes) < 0)
        return AVERROR(EINVAL);

    /* A macroblock-based decoder (VP8's is 16x16 luma / 8x8 chroma)
     * reads a little past the visible edge of the last row/column of
     * blocks while filtering/predicting there - real FFmpeg's frame
     * allocator rounds every plane up to a whole macroblock and adds a
     * further fixed border for exactly this reason. Rounding up to 32
     * pixels (comfortably a multiple of both VP8's 16px luma and 8px
     * chroma block size) plus AV_INPUT_BUFFER_PADDING_SIZE bytes of
     * flat padding on every plane reproduces that safety margin without
     * needing this port's own border-extension pass. */
    for (int i = 0; i < nplanes; i++) {
        int rw = (pw[i] + 31) & ~31;
        int rh = (ph[i] + 31) & ~31;
        size_t plane_bytes = (size_t)rw * (size_t)rh * (size_t)bpp[i] + AV_INPUT_BUFFER_PADDING_SIZE;
        AVBufferRef* buf = av_buffer_alloc(plane_bytes);
        if (!buf) {
            av_frame_unref(frame);
            return AVERROR(ENOMEM);
        }
        memset(buf->data, 0, plane_bytes);
        frame->buf[i] = buf;
        frame->data[i] = buf->data;
        frame->linesize[i] = rw * bpp[i];
    }
    for (int i = nplanes; i < AV_NUM_DATA_POINTERS; i++) {
        frame->data[i] = NULL;
        frame->linesize[i] = 0;
    }
    return 0;
}

int av_frame_ref(AVFrame* dst, const AVFrame* src) {
    av_frame_unref(dst);
    *dst = *src;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        dst->buf[i] = NULL;
        if (src->buf[i]) dst->buf[i] = av_buffer_ref_dup(src->buf[i]);
    }
    return 0;
}

AVFrame* av_frame_clone(const AVFrame* src) {
    AVFrame* dst = av_frame_alloc();
    if (!dst) return NULL;
    if (av_frame_ref(dst, src) < 0) {
        av_frame_free(&dst);
        return NULL;
    }
    return dst;
}

int av_frame_make_writable(AVFrame* frame) {
    /* Every frame buffer this port ever creates starts at refcount 1
     * and is only ever shared via av_frame_ref() (which this decoder's
     * call pattern uses for read-only reference frames, never
     * simultaneously writing through two references) - so "make
     * writable" is always already true. A real multi-owner check would
     * inspect *frame->buf[i]->refcount, but no code path in this port
     * exercises the >1 case. */
    (void)frame;
    return 0;
}

/* ---- AVPacket ---------------------------------------------------------
 * This port never actually reference-counts packet data (the one
 * AVPacket in play at a time just points at the caller's file buffer
 * for the duration of one decode call), so avpkt->buf stays NULL and
 * av_packet_get_side_data() - only ever asked for a Matroska/WebM side
 * channel this port's file-based loader never attaches - always
 * reports "none". */

AVPacket* av_packet_alloc(void) {
    return (AVPacket*)av_mallocz(sizeof(AVPacket));
}

void av_packet_unref(AVPacket* pkt) {
    if (!pkt) return;
    av_buffer_unref(&pkt->buf);
    pkt->data = NULL;
    pkt->size = 0;
}

void av_packet_free(AVPacket** pkt) {
    if (!pkt || !*pkt) return;
    av_packet_unref(*pkt);
    av_free(*pkt);
    *pkt = NULL;
}

const uint8_t* av_packet_get_side_data(const AVPacket* pkt, int type, size_t* size) {
    (void)pkt; (void)type;
    if (size) *size = 0;
    return NULL;
}

/* ---- av_refstruct_* ----------------------------------------------------
 * A small fixed header immediately before the pointer handed back to
 * the caller, matching the classic "hidden header" refcounting idiom -
 * see refstruct.h's own comment for why this port only implements the
 * three entry points vp8.c actually calls (allocz/replace/unref) rather
 * than real FFmpeg's fuller API (custom free callbacks, opaque flags,
 * pooling). Not atomic - see config.h, this is a single-threaded
 * build, so plain increment/decrement cannot race. */

typedef struct {
    int refcount;
} cos_refstruct_header_t;

void* av_refstruct_allocz(size_t size) {
    cos_refstruct_header_t* hdr =
        (cos_refstruct_header_t*)av_mallocz(sizeof(cos_refstruct_header_t) + size);
    if (!hdr) return NULL;
    hdr->refcount = 1;
    return (void*)(hdr + 1);
}

static cos_refstruct_header_t* cos_refstruct_hdr(void* obj) {
    return obj ? ((cos_refstruct_header_t*)obj - 1) : NULL;
}

void av_refstruct_unref(void* objp) {
    void** p = (void**)objp;
    if (!p || !*p) return;
    cos_refstruct_header_t* hdr = cos_refstruct_hdr(*p);
    if (--hdr->refcount <= 0) av_free(hdr);
    *p = NULL;
}

void av_refstruct_replace(void* dstp, const void* src) {
    void** dst = (void**)dstp;
    if (*dst == src) return;
    if (*dst) av_refstruct_unref(dst);
    if (src) {
        cos_refstruct_header_t* hdr = cos_refstruct_hdr((void*)src);
        hdr->refcount++;
        *dst = (void*)src;
    }
}

/* ---- decode-context helpers (codec_internal.h) ------------------------ */

enum AVPixelFormat ff_get_format(AVCodecContext* avctx, const enum AVPixelFormat* fmts) {
    /* Real FFmpeg lets the caller (or a registered get_format callback)
     * pick among software/hwaccel candidates; this port never sets up
     * a hwaccel (avctx->hwaccel is always NULL - see avcodec.h), so the
     * first candidate - always a plain software format for this
     * decoder - is always the right, and only sensible, answer. */
    avctx->pix_fmt = fmts[0];
    return fmts[0];
}

int ff_set_dimensions(AVCodecContext* avctx, int width, int height) {
    if (av_image_check_size((unsigned)width, (unsigned)height, 0, avctx) < 0)
        return AVERROR(EINVAL);
    avctx->width = avctx->coded_width = width;
    avctx->height = avctx->coded_height = height;
    return 0;
}

int ff_reget_buffer(AVCodecContext* avctx, AVFrame* frame, int flags) {
    (void)flags;
    if (frame->data[0] && frame->width == avctx->width &&
        frame->height == avctx->height && frame->format == avctx->pix_fmt) {
        return 0; /* already the right shape - real FFmpeg's fast path too */
    }
    av_frame_unref(frame);
    frame->width = avctx->width;
    frame->height = avctx->height;
    frame->format = avctx->pix_fmt;
    return av_frame_get_buffer(frame, 0);
}

int ff_decode_exif_attach_buffer(AVCodecContext* avctx, AVFrame* frame,
                                  AVBufferRef** exif_buf, int flags) {
    (void)avctx; (void)frame; (void)flags;
    /* See codec_internal.h: this port surfaces no metadata side-channel,
     * so the EXIF buffer webp.c already extracted is simply released
     * rather than attached anywhere. */
    if (exif_buf) av_buffer_unref(exif_buf);
    return 0;
}

int ff_frame_new_side_data(AVCodecContext* avctx, AVFrame* frame, int type,
                            size_t size, AVFrameSideData** out_sd) {
    (void)avctx; (void)frame; (void)type; (void)size;
    if (out_sd) *out_sd = NULL;
    return 0;
}

int ff_hwaccel_frame_priv_alloc(AVCodecContext* avctx, void** hwaccel_picture_private) {
    (void)avctx;
    /* avctx->hwaccel is always NULL in this port (see avcodec.h) - this
     * pointer is never dereferenced by anything that runs, so leaving
     * it NULL is correct rather than a placeholder. */
    if (hwaccel_picture_private) *hwaccel_picture_private = NULL;
    return 0;
}

/* ---- ProgressFrame (see progressframe.h for why these are correct
 * no-ops in a single-threaded build, not simplifications of real work) */

int ff_progress_frame_get_buffer(AVCodecContext* avctx, ProgressFrame* pf, int flags) {
    pf->f = av_frame_alloc();
    if (!pf->f) return AVERROR(ENOMEM);
    pf->f->width = avctx->width;
    pf->f->height = avctx->height;
    pf->f->format = avctx->pix_fmt;
    int ret = av_frame_get_buffer(pf->f, 0);
    if (ret < 0) {
        av_frame_free(&pf->f);
        return ret;
    }
    (void)flags;
    return 0;
}

void ff_progress_frame_unref(ProgressFrame* pf) {
    if (pf->f) av_frame_free(&pf->f);
}

void ff_progress_frame_replace(ProgressFrame* dst, const ProgressFrame* src) {
    if (dst->f == src->f) return;
    if (dst->f) av_frame_free(&dst->f);
    if (src->f) dst->f = av_frame_clone(src->f);
}

void ff_progress_frame_report(ProgressFrame* pf, int n) {
    (void)pf; (void)n;
}

void ff_progress_frame_await(const ProgressFrame* pf, int n) {
    (void)pf; (void)n;
}
