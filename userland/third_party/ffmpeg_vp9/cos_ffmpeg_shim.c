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
#include "libavutil/refstruct.h"
#include "libavutil/video_enc_params.h"

#include "cos.h"

/* ---- logging ------------------------------------------------------- */

/* Only warnings and errors are worth a line on the console, and a compute job on
 * another core cannot make the system call at all. */
void av_log(void* avcl, int level, const char* fmt, ...) {
    (void)avcl;
    if (level > 24 /* AV_LOG_WARNING */ || cos_in_worker()) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cos_puts("[vp9] ");
    cos_puts(buf);
}

void av_log_once(void* avcl, int initial_level, int subsequent_level,
                  int* state, const char* fmt, ...) {
    (void)initial_level;
    (void)subsequent_level;
    if (cos_in_worker()) return;
    if (state && *state) return;
    if (state) *state = 1;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cos_puts("[vp9] ");
    cos_puts(buf);
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
    buf->free_fn = NULL;
    buf->opaque = NULL;
    *buf->refcount = 1;
    return buf;
}

AVBufferRef* av_buffer_ref(const AVBufferRef* src) {
    if (!src) return NULL;
    AVBufferRef* dup = (AVBufferRef*)av_malloc(sizeof(AVBufferRef));
    if (!dup) return NULL;
    *dup = *src;
    __atomic_fetch_add(dup->refcount, 1, __ATOMIC_RELAXED);
    return dup;
}
#define av_buffer_ref_dup(src) av_buffer_ref(src)

void av_buffer_unref(AVBufferRef** bufp) {
    if (!bufp || !*bufp) return;
    AVBufferRef* buf = *bufp;
    /* Atomic: with frame threading a buffer is referenced from several cores. */
    if (__atomic_fetch_sub(buf->refcount, 1, __ATOMIC_ACQ_REL) <= 1) {
        if (buf->free_fn) buf->free_fn(buf->opaque, buf->data);
        else av_free(buf->data);
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

/* Side data attached to a frame (HEVC SEI/HDR metadata, which this port stores but never
 * uses): freed together with the frame's pixel buffers. */
static void cos_frame_free_side_data(AVFrame* frame) {
    for (int i = 0; i < frame->nb_side_data; i++) {
        AVFrameSideData* sd = frame->side_data[i];
        if (!sd) continue;
        if (sd->buf) av_buffer_unref(&sd->buf);
        else av_free(sd->data);
        av_free(sd);
    }
    av_free(frame->side_data);
    frame->side_data = NULL;
    frame->nb_side_data = 0;
}

void av_frame_unref(AVFrame* frame) {
    if (!frame) return;
    cos_frame_free_side_data(frame);
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

/* Returns 0/1/2/3-plane count and per-plane subsampling/sample-width
 * for every pixel format this port's decoders ever produce (see
 * AV_PIX_FMT_* in avcodec.h / libavutil/pixfmt.h) - not a general
 * av_pix_fmt_desc_get() replacement, just enough for
 * av_frame_get_buffer()/ff_reget_buffer() to size planes correctly.
 * The YUV420P/YUVA420P/ARGB cases are the original VP8/WebP set; the
 * rest (4:2:2/4:4:0/4:4:4 at 8/10/12-bit, plus GBRP) were added for
 * VP9, which - unlike VP8 - can signal any of them via its uncompressed
 * header's profile/color-config bits. A 10/12-bit plane's samples are
 * 2-byte (see cos_vp9_entry.c's bit_depth handling downstream), hence
 * bytes_per_px 2 for every *P10/*P12/GBRP10/GBRP12 case below. */
static int cos_plane_layout(enum AVPixelFormat fmt, int w, int h,
                             int plane_w[4], int plane_h[4], int bytes_per_px[4], int* nplanes) {
    int ss_h = 0, ss_v = 0, bpp = 1;
    switch (fmt) {
    case AV_PIX_FMT_YUVA420P:
        *nplanes = 4;
        plane_w[3] = w; plane_h[3] = h; bytes_per_px[3] = 1;
        ss_h = ss_v = 1; bpp = 1;
        goto yuv;
    case AV_PIX_FMT_ARGB:
        *nplanes = 1;
        plane_w[0] = w; plane_h[0] = h; bytes_per_px[0] = 4;
        return 0;
    case AV_PIX_FMT_YUV420P:   *nplanes = 3; ss_h = 1; ss_v = 1; bpp = 1; goto yuv;
    case AV_PIX_FMT_YUV422P:   *nplanes = 3; ss_h = 1; ss_v = 0; bpp = 1; goto yuv;
    case AV_PIX_FMT_YUV440P:   *nplanes = 3; ss_h = 0; ss_v = 1; bpp = 1; goto yuv;
    case AV_PIX_FMT_YUV444P:   *nplanes = 3; ss_h = 0; ss_v = 0; bpp = 1; goto yuv;
    case AV_PIX_FMT_YUVJ420P:  *nplanes = 3; ss_h = 1; ss_v = 1; bpp = 1; goto yuv;
    case AV_PIX_FMT_YUVJ422P:  *nplanes = 3; ss_h = 1; ss_v = 0; bpp = 1; goto yuv;
    case AV_PIX_FMT_YUVJ444P:  *nplanes = 3; ss_h = 0; ss_v = 0; bpp = 1; goto yuv;
    case AV_PIX_FMT_YUV420P9:  *nplanes = 3; ss_h = 1; ss_v = 1; bpp = 2; goto yuv;
    case AV_PIX_FMT_YUV422P9:  *nplanes = 3; ss_h = 1; ss_v = 0; bpp = 2; goto yuv;
    case AV_PIX_FMT_YUV444P9:  *nplanes = 3; ss_h = 0; ss_v = 0; bpp = 2; goto yuv;
    case AV_PIX_FMT_GRAY8:     *nplanes = 1; plane_w[0] = w; plane_h[0] = h; bytes_per_px[0] = 1; return 0;
    case AV_PIX_FMT_GRAY9:
    case AV_PIX_FMT_GRAY10:
    case AV_PIX_FMT_GRAY12:    *nplanes = 1; plane_w[0] = w; plane_h[0] = h; bytes_per_px[0] = 2; return 0;
    case AV_PIX_FMT_YUV420P10: *nplanes = 3; ss_h = 1; ss_v = 1; bpp = 2; goto yuv;
    case AV_PIX_FMT_YUV422P10: *nplanes = 3; ss_h = 1; ss_v = 0; bpp = 2; goto yuv;
    case AV_PIX_FMT_YUV440P10: *nplanes = 3; ss_h = 0; ss_v = 1; bpp = 2; goto yuv;
    case AV_PIX_FMT_YUV444P10: *nplanes = 3; ss_h = 0; ss_v = 0; bpp = 2; goto yuv;
    case AV_PIX_FMT_YUV420P12: *nplanes = 3; ss_h = 1; ss_v = 1; bpp = 2; goto yuv;
    case AV_PIX_FMT_YUV422P12: *nplanes = 3; ss_h = 1; ss_v = 0; bpp = 2; goto yuv;
    case AV_PIX_FMT_YUV440P12: *nplanes = 3; ss_h = 0; ss_v = 1; bpp = 2; goto yuv;
    case AV_PIX_FMT_YUV444P12: *nplanes = 3; ss_h = 0; ss_v = 0; bpp = 2; goto yuv;
    case AV_PIX_FMT_GBRP:      *nplanes = 3; ss_h = 0; ss_v = 0; bpp = 1; goto yuv;
    case AV_PIX_FMT_GBRP10:    *nplanes = 3; ss_h = 0; ss_v = 0; bpp = 2; goto yuv;
    case AV_PIX_FMT_GBRP12:    *nplanes = 3; ss_h = 0; ss_v = 0; bpp = 2; goto yuv;
    default:
        return AVERROR(EINVAL);
    }
yuv:
    plane_w[0] = w; plane_h[0] = h; bytes_per_px[0] = bpp;
    plane_w[1] = (w + ss_h) >> ss_h; plane_h[1] = (h + ss_v) >> ss_v; bytes_per_px[1] = bpp;
    plane_w[2] = (w + ss_h) >> ss_h; plane_h[2] = (h + ss_v) >> ss_v; bytes_per_px[2] = bpp;
    return 0;
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
        int rw = (pw[i] + 63) & ~63;
        int rh = (ph[i] + 63) & ~63;
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
    dst->side_data = NULL;
    dst->nb_side_data = 0;
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

typedef struct cos_refstruct_header_s {
    int refcount;
    /* NULL for a plain av_refstruct_allocz() object. Set for an object
     * handed out by ff_refstruct_pool_get(): instead of av_free()ing the
     * block when the refcount hits zero, it goes back on the pool's
     * free list - see ff_refstruct_unref() below. */
    struct FFRefStructPool* pool;
    struct cos_refstruct_header_s* pool_next; /* free-list link, pool objects only */
    /* Optional destructor (ff_refstruct_alloc_ext), run on the last unref. */
    void (*free_cb)(FFRefStructOpaque opaque, void* obj);
    FFRefStructOpaque free_opaque;
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

struct FFRefStructPool {
    size_t size;
    unsigned flags;
    cos_refstruct_header_t* free_list;
    volatile int lock;   /* objects are handed back from whichever core drops the last reference */
};
static inline void cos_pool_lock(FFRefStructPool* p) { while (__atomic_exchange_n(&p->lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
static inline void cos_pool_unlock(FFRefStructPool* p) { __atomic_store_n(&p->lock, 0, __ATOMIC_RELEASE); }

FFRefStructPool* ff_refstruct_pool_alloc(size_t size, unsigned flags) {
    FFRefStructPool* pool = (FFRefStructPool*)av_mallocz(sizeof(*pool));
    if (!pool) return NULL;
    pool->size = size;
    pool->flags = flags;
    pool->free_list = NULL;
    return pool;
}

void ff_refstruct_pool_uninit(FFRefStructPool** poolp) {
    if (!poolp || !*poolp) return;
    FFRefStructPool* pool = *poolp;
    cos_refstruct_header_t* h = pool->free_list;
    while (h) {
        cos_refstruct_header_t* next = (cos_refstruct_header_t*)h->pool_next;
        av_free(h);
        h = next;
    }
    av_free(pool);
    *poolp = NULL;
}

void* ff_refstruct_pool_get(FFRefStructPool* pool) {
    if (!pool) return NULL;
    cos_refstruct_header_t* hdr;
    cos_pool_lock(pool);
    hdr = pool->free_list;
    if (hdr) pool->free_list = (cos_refstruct_header_t*)hdr->pool_next;
    cos_pool_unlock(pool);
    if (hdr) {
        if (pool->flags & 1 /* FF_REFSTRUCT_POOL_FLAG_ZERO_EVERY_TIME */)
            memset((void*)(hdr + 1), 0, pool->size);
    } else {
        hdr = (cos_refstruct_header_t*)av_mallocz(sizeof(cos_refstruct_header_t) + pool->size);
        if (!hdr) return NULL;
    }
    __atomic_store_n(&hdr->refcount, 1, __ATOMIC_RELEASE);
    hdr->pool = pool;
    hdr->pool_next = NULL;
    return (void*)(hdr + 1);
}

/* ff_refstruct_unref/replace are the exact same logic as
 * av_refstruct_unref/replace above, just reached under vp9.c's own
 * (newer) names - except that an object whose header carries a pool
 * back-pointer (handed out by ff_refstruct_pool_get()) returns to that
 * pool's free list on its last unref instead of being freed, so the
 * next ff_refstruct_pool_get() call can reuse the allocation. */
void ff_refstruct_unref(void* objp) {
    void** p = (void**)objp;
    if (!p || !*p) return;
    cos_refstruct_header_t* hdr = cos_refstruct_hdr(*p);
    if (__atomic_fetch_sub(&hdr->refcount, 1, __ATOMIC_ACQ_REL) <= 1) {
        if (hdr->free_cb) hdr->free_cb(hdr->free_opaque, *p);
        if (hdr->pool) {
            cos_pool_lock(hdr->pool);
            hdr->pool_next = hdr->pool->free_list;
            hdr->pool->free_list = hdr;
            cos_pool_unlock(hdr->pool);
        } else {
            av_free(hdr);
        }
    }
    *p = NULL;
}

void av_refstruct_unref(void* objp) { ff_refstruct_unref(objp); }

void ff_refstruct_replace(void* dstp, const void* src) {
    void** dst = (void**)dstp;
    if (*dst == src) return;
    if (*dst) ff_refstruct_unref(dst);
    if (src) {
        cos_refstruct_header_t* hdr = cos_refstruct_hdr((void*)src);
        __atomic_fetch_add(&hdr->refcount, 1, __ATOMIC_RELAXED);
        *dst = (void*)src;
    }
}

void av_refstruct_replace(void* dstp, const void* src) { ff_refstruct_replace(dstp, src); }

void* ff_refstruct_alloc_ext_c(size_t size, unsigned flags, FFRefStructOpaque opaque,
                               void (*free_cb)(FFRefStructOpaque opaque, void* obj)) {
    cos_refstruct_header_t* hdr = (cos_refstruct_header_t*)av_malloc(sizeof(cos_refstruct_header_t) + size);
    if (!hdr) return NULL;
    memset(hdr, 0, sizeof(*hdr));
    if (!(flags & FF_REFSTRUCT_FLAG_NO_ZEROING)) memset((void*)(hdr + 1), 0, size);
    hdr->refcount = 1;
    hdr->free_cb = free_cb;
    hdr->free_opaque = opaque;
    return (void*)(hdr + 1);
}

void* ff_refstruct_ref(void* obj) {
    if (obj) __atomic_fetch_add(&cos_refstruct_hdr(obj)->refcount, 1, __ATOMIC_RELAXED);
    return obj;
}

const void* ff_refstruct_ref_c(const void* obj) { return ff_refstruct_ref((void*)obj); }

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

/* ProgressFrame: a picture plus a shared progress record (the last row that is final), so that with frame
 * threading a later frame can wait for just the rows of a reference that it needs. Without threading nobody
 * waits, and the record costs one small allocation per picture. The cores spin: they have no system calls
 * to sleep with. */
extern volatile int* cos_hevc_mt_abort;   /* set by the frame-thread group; shared with the HEVC port */

int ff_progress_frame_get_buffer(AVCodecContext* avctx, ProgressFrame* pf, int flags) {
    pf->f = av_frame_alloc();
    pf->progress = NULL;
    if (!pf->f) return AVERROR(ENOMEM);
    pf->progress = av_buffer_alloc(sizeof(int));
    if (!pf->progress) { av_frame_free(&pf->f); return AVERROR(ENOMEM); }
    *(int*)pf->progress->data = -1;
    pf->f->width = avctx->width;
    pf->f->height = avctx->height;
    pf->f->format = avctx->pix_fmt;
    int ret = av_frame_get_buffer(pf->f, 0);
    if (ret < 0) {
        av_buffer_unref(&pf->progress);
        av_frame_free(&pf->f);
        return ret;
    }
    (void)flags;
    return 0;
}

void ff_progress_frame_unref(ProgressFrame* pf) {
    if (pf->f) av_frame_free(&pf->f);
    av_buffer_unref(&pf->progress);
}

void ff_progress_frame_replace(ProgressFrame* dst, const ProgressFrame* src) {
    if (dst->f && dst->f == src->f) return;
    if (dst->f) av_frame_free(&dst->f);
    av_buffer_unref(&dst->progress);
    if (src->f) dst->f = av_frame_clone(src->f);
    if (src->progress) dst->progress = av_buffer_ref(src->progress);
}

void ff_progress_frame_report(ProgressFrame* pf, int n) {
    if (!pf->progress) return;
    int* p = (int*)pf->progress->data;
    if (__atomic_load_n(p, __ATOMIC_RELAXED) >= n) return;
    __atomic_store_n(p, n, __ATOMIC_RELEASE);
}

void ff_progress_frame_await(const ProgressFrame* pf, int n) {
    if (!pf->progress) return;
    const int* p = (const int*)pf->progress->data;
    while (__atomic_load_n(p, __ATOMIC_ACQUIRE) < n) {
        if (cos_hevc_mt_abort && __atomic_load_n(cos_hevc_mt_abort, __ATOMIC_RELAXED)) return;
        if(!cos_idle_work()) __builtin_ia32_pause();
    }
}

int ff_pthread_init(void* obj, const unsigned offsets[]) { (void)obj; (void)offsets; return 0; }
void ff_pthread_free(void* obj, const unsigned offsets[]) { (void)obj; (void)offsets; }

/* video_enc_params.h stubs - see that header's comment: export_side_data
 * is never set in this port, so these are never actually reached; NULL
 * drives vp9.c's own ENOMEM-and-skip path on the dead call site. */
AVVideoEncParams *av_video_enc_params_create_side_data(AVFrame *frame, int type, unsigned int nb_blocks) {
    (void)frame; (void)type; (void)nb_blocks;
    return NULL;
}
AVVideoBlockParams *av_video_enc_params_block(AVVideoEncParams *par, unsigned int idx) {
    (void)par; (void)idx;
    return NULL;
}

/* Purely a diagnostics string for av_log() - names only the formats this
 * port's vp9dec.h / vp9.c actually produce or test for. */
const char* av_get_pix_fmt_name(enum AVPixelFormat fmt) {
    switch (fmt) {
        case AV_PIX_FMT_YUV420P:   return "yuv420p";
        case AV_PIX_FMT_YUV422P:   return "yuv422p";
        case AV_PIX_FMT_YUV440P:   return "yuv440p";
        case AV_PIX_FMT_YUV444P:   return "yuv444p";
        case AV_PIX_FMT_YUV420P10: return "yuv420p10le";
        case AV_PIX_FMT_YUV422P10: return "yuv422p10le";
        case AV_PIX_FMT_YUV440P10: return "yuv440p10le";
        case AV_PIX_FMT_YUV444P10: return "yuv444p10le";
        case AV_PIX_FMT_YUV420P12: return "yuv420p12le";
        case AV_PIX_FMT_YUV422P12: return "yuv422p12le";
        case AV_PIX_FMT_YUV440P12: return "yuv440p12le";
        case AV_PIX_FMT_YUV444P12: return "yuv444p12le";
        case AV_PIX_FMT_GBRP:      return "gbrp";
        case AV_PIX_FMT_GBRP10:    return "gbrp10le";
        case AV_PIX_FMT_GBRP12:    return "gbrp12le";
        default: return "unknown";
    }
}

/* ---- av_log2 / av_log2_16bit -------------------------------------------
 * Real FFmpeg's ff_log2()/ff_log2_16bit() (see libavutil/common.h) are
 * builtin-clz-based floor(log2(v)) with a -1-for-0 convention; this is
 * the same compiler-builtin implementation, just given the av_log2/
 * av_log2_16bit names vp9block.c/mathops.h call directly instead of
 * going through FFmpeg's ff_log2-macro-aliasing indirection (which this
 * port's common.h deliberately does not reproduce - see its own
 * av_log2 declaration comment). */
int av_log2(unsigned v) {
    return v ? 31 - __builtin_clz(v) : -1;
}
int av_log2_16bit(unsigned v) {
    return av_log2(v);
}
