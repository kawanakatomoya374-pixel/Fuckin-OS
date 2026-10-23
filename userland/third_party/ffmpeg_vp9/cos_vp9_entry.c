/**
 * cos_vp9_entry.c - a clean, persistent-decoder C API over FFmpeg's own
 * VP9 decoder (ff_vp9_decoder - the same FFCodec vtable real FFmpeg's
 * generic decode dispatcher would use, see vp9.c's bottom), in the same
 * shape as this codebase's other video decoders (edge264_decode_NAL/
 * edge264_get_frame for H.264, dav1d for AV1) so media/main.c can call
 * it the same way it calls those: open once, decode()/get-frame many
 * times across a video's lifetime, close once.
 *
 * Unlike cos_webp_entry.c's single-shot still-image decode, this keeps
 * one AVCodecContext + VP9Context alive across every sample of a video
 * (vp9.c's own inter-frame prediction state - reference frames, motion
 * vectors, probability model adaptation - depends on that persistence
 * exactly like every other VP9 decoder does), matching how
 * vpx_codec_decode()/vpx_codec_get_frame() were used for the same
 * purpose before this port existed.
 */
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "avcodec.h"
#include "codec_internal.h"
#include "libavutil/pixfmt.h"
#include "cos_vp9.h"

extern const FFCodec ff_vp9_decoder;

typedef struct cos_vp9_ctx {
    AVCodecContext avctx;
    AVCodecInternal internal;
    AVFrame* frame;
    struct cos_vp9_group* group;   /* frame-threaded decoding: the group this context belongs to */
    int idx;
} cos_vp9_ctx;

static int cos_vp9_execute2(AVCodecContext* c,
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

static void* vp9_open_ex(int nthreads, int is_copy) {
    cos_vp9_ctx* c = (cos_vp9_ctx*)calloc(1, sizeof(cos_vp9_ctx));
    if (!c) return NULL;
    c->avctx.execute2 = cos_vp9_execute2;
    c->avctx.thread_count = nthreads;
    c->avctx.active_thread_type = nthreads > 1 ? FF_THREAD_FRAME : FF_THREAD_NONE;
    c->avctx.internal = &c->internal;
    c->internal.is_copy = is_copy;
    c->avctx.priv_data = calloc(1, (size_t)ff_vp9_decoder.priv_data_size);
    if (!c->avctx.priv_data) { free(c); return NULL; }
    if (ff_vp9_decoder.init && ff_vp9_decoder.init(&c->avctx) < 0) {
        free(c->avctx.priv_data);
        free(c);
        return NULL;
    }
    c->frame = av_frame_alloc();
    if (!c->frame) {
        if (ff_vp9_decoder.close) ff_vp9_decoder.close(&c->avctx);
        free(c->avctx.priv_data);
        free(c);
        return NULL;
    }
    return c;
}

void* cos_vp9_open(void) { return vp9_open_ex(1, 0); }

void cos_vp9_close(void* h) {
    cos_vp9_ctx* c = (cos_vp9_ctx*)h;
    if (!c) return;
    if (ff_vp9_decoder.close) ff_vp9_decoder.close(&c->avctx);
    av_frame_free(&c->frame);
    free(c->avctx.priv_data);
    free(c);
}

/* Returns -1 on a real decode error, 0 if the packet produced no
 * displayable frame yet (normal for the first few packets - VP9, like
 * every modern codec here, can have reordering/lookahead), 1 if *out
 * was filled with a decoded frame's description. The returned pointers
 * are only valid until the next cos_vp9_decode()/cos_vp9_close() call,
 * exactly like edge264's/dav1d's own frame-ownership convention this
 * port's other codec entry points already follow.
 *
 * VP9 "superframes" pack several whole encoded frames (typically one
 * invisible altref frame plus the visible frame that references it)
 * into a single demuxer-level chunk - common whenever altref is used,
 * which libvpx enables automatically on many encodes even without the
 * caller asking for it. Real FFmpeg splits these at the demuxer/bsf
 * layer (see bsf/vp9_superframe_split.c) before any packet ever reaches
 * the decoder - ff_vp9_decoder itself assumes it is only ever handed
 * one frame per call and has no superframe-awareness of its own. This
 * port has no bitstream-filter layer, so decode_one_vp9_frame() below
 * reimplements that same split (same marker-byte/index-size layout)
 * directly over an MP4 sample before feeding each piece to the decoder. */
static int decode_one_vp9_frame(cos_vp9_ctx* c, const uint8_t* data, uint32_t size, int* got_frame) {
    uint8_t* padded = (uint8_t*)malloc((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!padded) return -1;
    memcpy(padded, data, size);
    memset(padded + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    AVPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.data = padded;
    pkt.size = (int)size;

    int ret = ff_vp9_decoder.decode_cb(&c->avctx, c->frame, got_frame, &pkt);
    free(padded);
    return ret < 0 ? -1 : 0;
}

int cos_vp9_decode(void* h, const uint8_t* data, uint32_t size, cos_vp9_frame_t* out) {
    cos_vp9_ctx* c = (cos_vp9_ctx*)h;
    if (!c || !out) return -1;
    memset(out, 0, sizeof(*out));
    if (!data || !size) return 0;

    int got_frame = 0;
    uint8_t marker = data[size - 1];
    if ((marker & 0xe0) == 0xc0) {
        int length_size = 1 + ((marker >> 3) & 0x3);
        int nb_frames = 1 + (marker & 0x7);
        uint32_t idx_size = (uint32_t)(2 + nb_frames * length_size);
        if (size >= idx_size && data[size - idx_size] == marker) {
            const uint8_t* idx = data + size - idx_size + 1;
            uint32_t off = 0;
            for (int i = 0; i < nb_frames; i++) {
                uint32_t fsz = 0;
                for (int j = 0; j < length_size; j++) fsz |= (uint32_t)idx[i * length_size + j] << (j * 8);
                if (!fsz || off + fsz > size - idx_size) return -1;
                int g = 0;
                if (decode_one_vp9_frame(c, data + off, fsz, &g) < 0) return -1;
                if (g) got_frame = 1; /* only the final visible sub-frame (if any) sets this */
                off += fsz;
            }
            goto have_result;
        }
    }
    if (decode_one_vp9_frame(c, data, size, &got_frame) < 0) return -1;

have_result:
    if (!got_frame) return 0;

    AVFrame* f = c->frame;
    int ss_h = 1, ss_v = 1, bit_depth = 8;
    switch (f->format) {
        case AV_PIX_FMT_YUV420P:   ss_h = 1; ss_v = 1; bit_depth = 8;  break;
        case AV_PIX_FMT_YUV420P10: ss_h = 1; ss_v = 1; bit_depth = 10; break;
        case AV_PIX_FMT_YUV420P12: ss_h = 1; ss_v = 1; bit_depth = 12; break;
        default:
            /* Not 4:2:0 (4:2:2/4:4:0/4:4:4 or RGB profile 1/3 content) -
             * media/main.c only wants 4:2:0, same policy as its AV1/VP9
             * 8-bit gates elsewhere; report "no usable frame" rather
             * than a wrong-layout one. */
            return 0;
    }
    out->width = f->width;
    out->height = f->height;
    out->bit_depth = bit_depth;
    out->y = f->data[0]; out->u = f->data[1]; out->v = f->data[2];
    out->stride_y = f->linesize[0]; out->stride_c = f->linesize[1];
    (void)ss_h; (void)ss_v;
    return 1;
}


/* ---- frame-threaded decoding ----------------------------------------------------------------------------
 * Same scheme as the HEVC port (see cos_hevc_entry.c): packet k goes to worker k % N, which first copies the
 * decoder state from the context that handled packet k-1 once k-1 has finished its setup, then decodes while
 * later packets already run on the other workers; references are awaited row by row through ProgressFrame.
 * A packet here is one VP9 frame - cos_vp9_split() cuts a superframe into its frames, as FFmpeg's
 * vp9_superframe_split bitstream filter does ahead of its decoder. */
struct cos_vp9_group {
    int n;
    volatile int abort;
    volatile int64_t setup_seq;
    void* volatile ctx[8];
};

extern volatile int* cos_hevc_mt_abort;

cos_vp9_group_t* cos_vp9_group_create(int nthreads) {
    if (nthreads < 1 || nthreads > 8) return NULL;
    cos_vp9_group_t* g = (cos_vp9_group_t*)calloc(1, sizeof(*g));
    if (g) g->n = nthreads;
    return g;
}
void cos_vp9_group_destroy(cos_vp9_group_t* g) { free(g); }
void cos_vp9_group_abort(cos_vp9_group_t* g) { if (g) __atomic_store_n(&g->abort, 1, __ATOMIC_RELEASE); }

void* cos_vp9_mt_open(cos_vp9_group_t* g, int idx) {
    if (!g || idx < 0 || idx >= g->n) return NULL;
    cos_hevc_mt_abort = &g->abort;
    cos_vp9_ctx* c = (cos_vp9_ctx*)vp9_open_ex(g->n, idx != 0);
    if (!c) return NULL;
    c->group = g; c->idx = idx;
    __atomic_store_n(&g->ctx[idx], c, __ATOMIC_RELEASE);
    return c;
}

int cos_vp9_split(const uint8_t* data, uint32_t size, uint32_t offs[8], uint32_t lens[8]) {
    if (!data || !size) return 0;
    uint8_t marker = data[size - 1];
    if ((marker & 0xe0) == 0xc0) {
        int length_size = 1 + ((marker >> 3) & 0x3);
        int nb_frames = 1 + (marker & 0x7);
        uint32_t idx_size = (uint32_t)(2 + nb_frames * length_size);
        if (size >= idx_size && data[size - idx_size] == marker) {
            const uint8_t* idx = data + size - idx_size + 1;
            uint32_t off = 0;
            for (int i = 0; i < nb_frames; i++) {
                uint32_t fsz = 0;
                for (int j = 0; j < length_size; j++) fsz |= (uint32_t)idx[i * length_size + j] << (j * 8);
                if (!fsz || off + fsz > size - idx_size) return -1;
                offs[i] = off; lens[i] = fsz;
                off += fsz;
            }
            return nb_frames;
        }
    }
    offs[0] = 0; lens[0] = size;
    return 1;
}

typedef struct { cos_vp9_group_t* g; int64_t seq; } vp9_setup_arg_t;
static void vp9_setup_done(void* p) {
    vp9_setup_arg_t* a = (vp9_setup_arg_t*)p;
    __atomic_store_n(&a->g->setup_seq, a->seq + 1, __ATOMIC_RELEASE);
}

int cos_vp9_mt_decode(void* h, int64_t seq, const uint8_t* data, uint32_t size, void** out_frame) {
    cos_vp9_ctx* c = (cos_vp9_ctx*)h;
    cos_vp9_group_t* g = c ? c->group : NULL;
    if (!g || !out_frame || !data || !size) return -1;
    *out_frame = NULL;
    vp9_setup_arg_t arg = { g, seq };
    int rc = 0;
    if (g->n > 1) {
        while (__atomic_load_n(&g->setup_seq, __ATOMIC_ACQUIRE) < seq) {
            if (__atomic_load_n(&g->abort, __ATOMIC_RELAXED)) return -1;
            if(!cos_idle_work()) __builtin_ia32_pause();
        }
        if (seq > 0) {
            cos_vp9_ctx* prev;
            while (!(prev = (cos_vp9_ctx*)__atomic_load_n(&g->ctx[(seq - 1) % g->n], __ATOMIC_ACQUIRE))) {
                if (__atomic_load_n(&g->abort, __ATOMIC_RELAXED)) return -1;
                if(!cos_idle_work()) __builtin_ia32_pause();
            }
            c->avctx.width = prev->avctx.width;   c->avctx.height = prev->avctx.height;
            c->avctx.coded_width = prev->avctx.coded_width; c->avctx.coded_height = prev->avctx.coded_height;
            c->avctx.pix_fmt = prev->avctx.pix_fmt; c->avctx.profile = prev->avctx.profile;
            c->avctx.color_range = prev->avctx.color_range; c->avctx.colorspace = prev->avctx.colorspace;
            if (ff_vp9_decoder.update_thread_context &&
                ff_vp9_decoder.update_thread_context(&c->avctx, &prev->avctx) < 0) rc = -1;
        }
        c->internal.setup_done_cb = vp9_setup_done;
        c->internal.setup_done_arg = &arg;
    }
    if (rc == 0) {
        av_frame_unref(c->frame);
        int got = 0;
        if (decode_one_vp9_frame(c, data, size, &got) < 0) rc = -1;
        else if (got) {
            *out_frame = av_frame_clone(c->frame);
            rc = *out_frame ? 1 : -1;
        }
    }
    if (g->n > 1 && c->internal.setup_done_cb) { c->internal.setup_done_cb = NULL; vp9_setup_done(&arg); }
    return rc;
}

int cos_vp9_describe(const void* frame, cos_vp9_frame_t* out) {
    if (!frame || !out) return -1;
    memset(out, 0, sizeof(*out));
    const AVFrame* f = (const AVFrame*)frame;
    int bit_depth;
    switch (f->format) {
        case AV_PIX_FMT_YUV420P:   bit_depth = 8;  break;
        case AV_PIX_FMT_YUV420P10: bit_depth = 10; break;
        case AV_PIX_FMT_YUV420P12: bit_depth = 12; break;
        default: return 0;   /* not 4:2:0: nothing the display path can use */
    }
    out->width = f->width; out->height = f->height; out->bit_depth = bit_depth;
    out->y = f->data[0]; out->u = f->data[1]; out->v = f->data[2];
    out->stride_y = f->linesize[0]; out->stride_c = f->linesize[1];
    return 1;
}
void cos_vp9_release(void* frame) { AVFrame* f = (AVFrame*)frame; av_frame_free(&f); }
