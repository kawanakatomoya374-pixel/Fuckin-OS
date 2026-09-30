/**
 * cos_webp_entry.c - the one function this whole port exists to provide:
 * decode a WebP file's bytes into a densely-packed BGRA buffer, in the
 * same shape as this codebase's other from-scratch decoders (see
 * png_decode() in src/apps/png_decoder.c) so jpeg_viewer.c's dispatcher
 * can call it exactly the same way.
 *
 * This is the only place that calls into the ported decoder through its
 * real FFmpeg entry points - ff_webp_decoder.init/.decode_cb/.close, the
 * same FFCodec vtable real FFmpeg's own generic decode dispatcher would
 * use (see webp.c's `const FFCodec ff_webp_decoder = { ... }`) - so
 * everything upstream of this file is genuinely FFmpeg's own decoder,
 * unaware it is not running inside FFmpeg.
 */
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "avcodec.h"
#include "codec_internal.h"
#include "libavutil/mem.h"
#include "libavutil/colorspace.h"

extern void serial_puts(const char* s);
extern const FFCodec ff_webp_decoder;
extern const uint8_t ff_crop_tab[]; /* see mathops.h / cos_ffmpeg_tables.c */
#define MAX_NEG_CROP 1024

/* Real FFmpeg's generic decode framework (avcodec_open2(), specifically
 * ff_thread_init() and its single-threaded fallback) always installs a
 * working avctx->execute/execute2 before a codec's own .init() runs,
 * even when thread_count is 1 - every codec that supports slice
 * threading (VP8 included: see vp8.c's avctx->execute2() call in
 * vp78_decode_frame(), used to dispatch macroblock-row decoding jobs
 * unconditionally, not just when real threading is active) calls
 * through it rather than looping over jobs itself. Nothing upstream of
 * this port ever installs that default, so it has to: a plain
 * synchronous "call every job in order, on this same thread" loop,
 * which is exactly what real FFmpeg's own non-threaded fallback does. */
static int cos_execute2(AVCodecContext* c,
                         int (*func)(AVCodecContext* c2, void* arg, int jobnr, int threadnr),
                         void* arg2, int* ret, int count) {
    int overall = 0;
    for (int i = 0; i < count; i++) {
        int r = func(c, arg2, i, 0);
        if (ret) ret[i] = r;
        if (r < 0) overall = r;
    }
    return overall;
}

/* YUV(A)420P -> packed BGRA, full pixel range (webp.c always sets
 * AVCOL_RANGE_JPEG for the still-image WebP case this port supports -
 * see its update_dimensions()/webp_decode_frame()) using colorspace.h's
 * own YUV_TO_RGB1/YUV_TO_RGB2 macros, which expect exactly these local
 * variable names (cb, cr, r_add, g_add, b_add, y, cm) - kept as the
 * macros define them rather than renamed, so the macro bodies do not
 * need touching. */
static void cos_yuv420_to_bgra(const AVFrame* f, uint8_t* out_bgra,
                                uint64_t out_w, uint64_t out_h, uint64_t out_stride_px) {
    const uint8_t* cm = ff_crop_tab + MAX_NEG_CROP;
    int cb, cr, r_add, g_add, b_add, y;
    bool has_alpha = (f->format == AV_PIX_FMT_YUVA420P);

    for (uint64_t j = 0; j < out_h; j++) {
        const uint8_t* yrow = f->data[0] + (size_t)j * f->linesize[0];
        const uint8_t* urow = f->data[1] + (size_t)(j / 2) * f->linesize[1];
        const uint8_t* vrow = f->data[2] + (size_t)(j / 2) * f->linesize[2];
        const uint8_t* arow = has_alpha ? f->data[3] + (size_t)j * f->linesize[3] : NULL;
        uint8_t* dst = out_bgra + (size_t)j * out_stride_px * 4;

        for (uint64_t i = 0; i < out_w; i++) {
            int r, g, b;
            YUV_TO_RGB1(urow[i / 2], vrow[i / 2]);
            YUV_TO_RGB2(r, g, b, yrow[i]);
            dst[i * 4 + 0] = (uint8_t)b;
            dst[i * 4 + 1] = (uint8_t)g;
            dst[i * 4 + 2] = (uint8_t)r;
            dst[i * 4 + 3] = arow ? arow[i] : 255;
        }
    }
}

static void cos_argb_to_bgra(const AVFrame* f, uint8_t* out_bgra,
                              uint64_t out_w, uint64_t out_h, uint64_t out_stride_px) {
    for (uint64_t j = 0; j < out_h; j++) {
        const uint8_t* src = f->data[0] + (size_t)j * f->linesize[0];
        uint8_t* dst = out_bgra + (size_t)j * out_stride_px * 4;
        for (uint64_t i = 0; i < out_w; i++) {
            /* AV_PIX_FMT_ARGB byte order is A,R,G,B; this port's display
             * buffers are always B,G,R,A (see png_decode()/decode_bmp()
             * in this codebase for the same convention). */
            uint8_t a = src[i * 4 + 0];
            uint8_t r = src[i * 4 + 1];
            uint8_t g = src[i * 4 + 2];
            uint8_t b = src[i * 4 + 3];
            dst[i * 4 + 0] = b;
            dst[i * 4 + 1] = g;
            dst[i * 4 + 2] = r;
            dst[i * 4 + 3] = a;
        }
    }
}

bool cos_webp_decode(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                      uint64_t max_out_w, uint64_t max_out_h,
                      uint64_t* out_w, uint64_t* out_h) {
    if (!data || size == 0 || !out_bgra || !out_w || !out_h) return false;
    *out_w = 0;
    *out_h = 0;

    /* get_bits.h's bitstream reader (and this decoder's own byte-stream
     * reader) are allowed to read up to AV_INPUT_BUFFER_PADDING_SIZE
     * bytes past the declared end of a packet's data - real FFmpeg
     * guarantees every AVPacket's buffer has this padding; a plain file
     * buffer read from disk does not, so this copies into one that
     * does rather than pass `data` directly. */
    uint8_t* padded = (uint8_t*)av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!padded) return false;
    memcpy(padded, data, size);

    AVCodecContext avctx;
    memset(&avctx, 0, sizeof(avctx));
    avctx.execute2 = cos_execute2;
    avctx.thread_count = 1;
    avctx.active_thread_type = FF_THREAD_NONE;
    avctx.priv_data = av_mallocz((size_t)ff_webp_decoder.priv_data_size);
    if (!avctx.priv_data) {
        av_free(padded);
        return false;
    }

    bool ok = false;
    AVFrame* frame = av_frame_alloc();
    if (!frame) goto cleanup;

    if (ff_webp_decoder.init && ff_webp_decoder.init(&avctx) < 0) {
        serial_puts("[webp] decoder init failed\n");
        goto cleanup;
    }

    {
        AVPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.data = padded;
        pkt.size = (int)size;

        int got_frame = 0;
        int ret = ff_webp_decoder.decode_cb(&avctx, frame, &got_frame, &pkt);
        if (ret < 0 || !got_frame) {
            serial_puts("[webp] decode failed or produced no frame\n");
            goto cleanup;
        }
    }

    if (frame->width <= 0 || frame->height <= 0) goto cleanup;

    {
        uint64_t w = (uint64_t)frame->width < max_out_w ? (uint64_t)frame->width : max_out_w;
        uint64_t h = (uint64_t)frame->height < max_out_h ? (uint64_t)frame->height : max_out_h;

        /* Decode always happens at the image's real dimensions (the
         * ported decoder itself picks them - see ff_set_dimensions()
         * calls in vp8.c/webp.c); if the caller's buffer is smaller
         * than that in either axis, this crops rather than scales
         * (matching png_decode()'s own out_w/out_h clamping
         * convention in this codebase) - the GUI's own display path
         * (see jpeg_viewer_draw_scaled() in jpeg_viewer.c) is what
         * actually scales an image to fit a window, so decode-time
         * clamping only needs to guard the output buffer's real size.
         */
        switch (frame->format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVA420P:
            cos_yuv420_to_bgra(frame, out_bgra, w, h, w);
            break;
        case AV_PIX_FMT_ARGB:
            cos_argb_to_bgra(frame, out_bgra, w, h, w);
            break;
        default:
            serial_puts("[webp] decoded to an unexpected pixel format\n");
            goto cleanup;
        }

        *out_w = w;
        *out_h = h;
        ok = true;
    }

cleanup:
    if (ff_webp_decoder.close) ff_webp_decoder.close(&avctx);
    av_frame_free(&frame);
    av_free(avctx.priv_data);
    av_free(padded);
    return ok;
}
