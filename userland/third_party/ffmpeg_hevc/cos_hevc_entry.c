/**
 * cos_hevc_entry.c - a persistent-decoder C API over FFmpeg's HEVC decoder
 * (ff_hevc_decoder, the same FFCodec vtable FFmpeg's generic decode layer
 * would drive), shaped like cos_vp9_entry.c: open once per video, feed one MP4
 * sample per decode() call, drain at end of stream, close once.
 */
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "avcodec.h"
#include "codec_internal.h"
#include "libavutil/pixfmt.h"
#include "cos_hevc.h"

extern const FFCodec ff_hevc_decoder;

typedef struct cos_hevc_ctx {
    AVCodecContext avctx;
    AVCodecInternal internal;
    AVFrame* frame;
    uint8_t* extradata;
    uint8_t* pkt_buf;
    size_t   pkt_cap;
    struct cos_hevc_group* group;   /* frame-threaded decoding: the group this context belongs to */
    int      idx;
} cos_hevc_ctx;

static int cos_hevc_execute2(AVCodecContext* c,
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

/* AVCodecContext.execute(): run `count` jobs one after another on this thread. */
static int cos_hevc_execute(AVCodecContext* c, int (*func)(AVCodecContext* c2, void* arg),
                            void* arg2, int* ret, int count, int size) {
    int overall = 0;
    for (int i = 0; i < count; i++) {
        int r = func(c, (char*)arg2 + (size_t)i * (size_t)size);
        if (ret) ret[i] = r;
        if (r < 0) overall = r;
    }
    return overall;
}

static void* hevc_open_ex(const uint8_t* hvcc, uint32_t hvcc_len, int nthreads, int is_copy) {
    cos_hevc_ctx* c = (cos_hevc_ctx*)calloc(1, sizeof(cos_hevc_ctx));
    if (!c) return NULL;
    c->avctx.codec_id = AV_CODEC_ID_HEVC;
    c->avctx.execute = cos_hevc_execute;
    c->avctx.execute2 = cos_hevc_execute2;
    c->avctx.thread_count = nthreads;
    c->avctx.active_thread_type = nthreads > 1 ? FF_THREAD_FRAME : FF_THREAD_NONE;
    c->avctx.internal = &c->internal;
    c->internal.is_copy = is_copy;
    c->avctx.skip_frame = AVDISCARD_DEFAULT;
    c->avctx.skip_loop_filter = AVDISCARD_DEFAULT;
    c->avctx.pix_fmt = AV_PIX_FMT_NONE;
    c->avctx.priv_data = calloc(1, (size_t)ff_hevc_decoder.priv_data_size);
    if (!c->avctx.priv_data) { free(c); return NULL; }
    if (hvcc && hvcc_len) {
        c->extradata = (uint8_t*)calloc(1, (size_t)hvcc_len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!c->extradata) { free(c->avctx.priv_data); free(c); return NULL; }
        memcpy(c->extradata, hvcc, hvcc_len);
        c->avctx.extradata = c->extradata;
        c->avctx.extradata_size = (int)hvcc_len;
    }
    if (ff_hevc_decoder.init && ff_hevc_decoder.init(&c->avctx) < 0) {
        if (ff_hevc_decoder.close) ff_hevc_decoder.close(&c->avctx);
        free(c->avctx.priv_data); free(c->extradata); free(c);
        return NULL;
    }
    c->frame = av_frame_alloc();
    if (!c->frame) {
        if (ff_hevc_decoder.close) ff_hevc_decoder.close(&c->avctx);
        free(c->avctx.priv_data); free(c->extradata); free(c);
        return NULL;
    }
    return c;
}

void* cos_hevc_open(const uint8_t* hvcc, uint32_t hvcc_len) { return hevc_open_ex(hvcc, hvcc_len, 1, 0); }

void cos_hevc_close(void* h) {
    cos_hevc_ctx* c = (cos_hevc_ctx*)h;
    if (!c) return;
    if (ff_hevc_decoder.close) ff_hevc_decoder.close(&c->avctx);
    av_frame_free(&c->frame);
    free(c->avctx.priv_data);
    free(c->extradata);
    free(c->pkt_buf);
    free(c);
}

int cos_hevc_info(void* h, int* width, int* height, int* bit_depth) {
    cos_hevc_ctx* c = (cos_hevc_ctx*)h;
    if (!c) return -1;
    int w = c->avctx.coded_width > 0 ? c->avctx.coded_width : c->avctx.width;
    int ht = c->avctx.coded_height > 0 ? c->avctx.coded_height : c->avctx.height;
    if (w <= 0 || ht <= 0) return -1;
    int d;
    switch (c->avctx.pix_fmt) {
        case AV_PIX_FMT_YUV420P10: d = 10; break;
        case AV_PIX_FMT_YUV420P12: d = 12; break;
        case AV_PIX_FMT_NONE:      return -1;
        default:                   d = 8;  break;
    }
    if (width) *width = w;
    if (height) *height = ht;
    if (bit_depth) *bit_depth = d;
    return 0;
}

/* Describe the decoded frame c->frame to the caller, applying the cropping
 * rectangle the decoder reports (HEVC always codes whole CTBs and signals the
 * visible window separately). */
static int describe_avframe(const AVFrame* f, cos_hevc_frame_t* out) {
    int bit_depth;
    switch (f->format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:  bit_depth = 8;  break;
        case AV_PIX_FMT_YUV420P10: bit_depth = 10; break;
        case AV_PIX_FMT_YUV420P12: bit_depth = 12; break;
        default: return 2; /* 4:0:0 / 4:2:2 / 4:4:4 / 9-bit: not handed to the display path */
    }
    const int bps = bit_depth > 8 ? 2 : 1;
    const int cl = (int)f->crop_left, ct = (int)f->crop_top;
    const int w = f->width - cl - (int)f->crop_right;
    const int h = f->height - ct - (int)f->crop_bottom;
    if (w <= 0 || h <= 0) return -1;
    out->width = w;
    out->height = h;
    out->bit_depth = bit_depth;
    out->stride_y = f->linesize[0];
    out->stride_c = f->linesize[1];
    out->y = f->data[0] + (size_t)ct * (size_t)f->linesize[0] + (size_t)cl * (size_t)bps;
    out->u = f->data[1] + (size_t)(ct >> 1) * (size_t)f->linesize[1] + (size_t)(cl >> 1) * (size_t)bps;
    out->v = f->data[2] + (size_t)(ct >> 1) * (size_t)f->linesize[2] + (size_t)(cl >> 1) * (size_t)bps;
    return 1;
}

static int describe_frame(cos_hevc_ctx* c, cos_hevc_frame_t* out) { return describe_avframe(c->frame, out); }

static int run_decode(cos_hevc_ctx* c, const uint8_t* data, uint32_t size, int* got_frame) {
    AVPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    if (size) {
        size_t need = (size_t)size + AV_INPUT_BUFFER_PADDING_SIZE;
        if (need > c->pkt_cap) {
            uint8_t* nb = (uint8_t*)realloc(c->pkt_buf, need);
            if (!nb) return -1;
            c->pkt_buf = nb;
            c->pkt_cap = need;
        }
        memcpy(c->pkt_buf, data, size);
        memset(c->pkt_buf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        pkt.data = c->pkt_buf;
        pkt.size = (int)size;
    }
    av_frame_unref(c->frame);
    *got_frame = 0;
    int ret = ff_hevc_decoder.decode_cb(&c->avctx, c->frame, got_frame, &pkt);
    return ret < 0 ? -1 : 0;
}

int cos_hevc_decode(void* h, const uint8_t* data, uint32_t size, cos_hevc_frame_t* out) {
    cos_hevc_ctx* c = (cos_hevc_ctx*)h;
    if (!c || !out) return -1;
    memset(out, 0, sizeof(*out));
    if (!data || !size) return 0;
    int got = 0;
    if (run_decode(c, data, size, &got) < 0) return -1;
    return got ? describe_frame(c, out) : 0;
}

int cos_hevc_drain(void* h, cos_hevc_frame_t* out) {
    cos_hevc_ctx* c = (cos_hevc_ctx*)h;
    if (!c || !out) return -1;
    memset(out, 0, sizeof(*out));
    int got = 0;
    if (run_decode(c, NULL, 0, &got) < 0) return -1;
    return got ? describe_frame(c, out) : 0;
}


/* ---- frame-threaded decoding ------------------------------------------------
 * N workers (one per core) each own a full decoder context and decode a different packet at the same time,
 * the way FFmpeg's frame threading does: packet k goes to worker k % N; before it starts, that worker copies
 * the decoder state (parameter sets, reference pictures, POC state) from the context that handled packet k-1,
 * but only after k-1 has finished its setup (slice header parsed, picture allocated); from then on the two run
 * concurrently and a reference picture's rows are awaited through its progress record (see the shim). Results
 * are collected in packet order by the caller, so output order and content are exactly those of one decoder. */
struct cos_hevc_group {
    int n;
    volatile int abort;
    volatile int64_t setup_seq;      /* packets 0..setup_seq-1 have finished their setup */
    void* volatile ctx[8];
    volatile int64_t dbg[8];         /* per worker: seq * 10 + phase, for diagnosing stalls */
};

extern volatile int* cos_hevc_mt_abort;

cos_hevc_group_t* cos_hevc_group_create(int nthreads) {
    if (nthreads < 1 || nthreads > 8) return NULL;
    cos_hevc_group_t* g = (cos_hevc_group_t*)calloc(1, sizeof(*g));
    if (g) g->n = nthreads;
    return g;
}
int64_t cos_hevc_group_debug(cos_hevc_group_t* g, int idx) { return g ? g->dbg[idx] : -1; }
void cos_hevc_group_destroy(cos_hevc_group_t* g) { free(g); }
void cos_hevc_group_abort(cos_hevc_group_t* g) { if (g) __atomic_store_n(&g->abort, 1, __ATOMIC_RELEASE); }

void* cos_hevc_mt_open(cos_hevc_group_t* g, int idx, const uint8_t* hvcc, uint32_t hvcc_len) {
    if (!g || idx < 0 || idx >= g->n) return NULL;
    cos_hevc_mt_abort = &g->abort;
    /* Every thread after the first starts as a copy: it receives the parameter sets from its predecessor. */
    cos_hevc_ctx* c = (cos_hevc_ctx*)hevc_open_ex(idx == 0 ? hvcc : NULL, idx == 0 ? hvcc_len : 0, g->n, idx != 0);
    if (!c) return NULL;
    c->group = g; c->idx = idx;
    __atomic_store_n(&g->ctx[idx], c, __ATOMIC_RELEASE);
    return c;
}

typedef struct { cos_hevc_group_t* g; int64_t seq; } mt_setup_arg_t;
static void mt_setup_done(void* p) {
    mt_setup_arg_t* a = (mt_setup_arg_t*)p;
    __atomic_store_n(&a->g->setup_seq, a->seq + 1, __ATOMIC_RELEASE);
}

int cos_hevc_mt_decode(void* h, int64_t seq, const uint8_t* data, uint32_t size, void** out_frame) {
    cos_hevc_ctx* c = (cos_hevc_ctx*)h;
    cos_hevc_group_t* g = c ? c->group : NULL;
    if (!g || !out_frame) return -1;
    *out_frame = NULL;
    mt_setup_arg_t arg = { g, seq };
    int rc = 0;
    g->dbg[c->idx] = seq * 10 + 1;
    if (g->n > 1) {
        while (__atomic_load_n(&g->setup_seq, __ATOMIC_ACQUIRE) < seq) {
            if (__atomic_load_n(&g->abort, __ATOMIC_RELAXED)) return -1;
            __builtin_ia32_pause();
        }
        if (seq > 0) {
            cos_hevc_ctx* prev;
            g->dbg[c->idx] = seq * 10 + 2;
            while (!(prev = (cos_hevc_ctx*)__atomic_load_n(&g->ctx[(seq - 1) % g->n], __ATOMIC_ACQUIRE))) {
                if (__atomic_load_n(&g->abort, __ATOMIC_RELAXED)) return -1;
                __builtin_ia32_pause();
            }
            g->dbg[c->idx] = seq * 10 + 3;
            c->avctx.width = prev->avctx.width;   c->avctx.height = prev->avctx.height;
            c->avctx.coded_width = prev->avctx.coded_width; c->avctx.coded_height = prev->avctx.coded_height;
            c->avctx.pix_fmt = prev->avctx.pix_fmt; c->avctx.has_b_frames = prev->avctx.has_b_frames;
            c->avctx.profile = prev->avctx.profile; c->avctx.level = prev->avctx.level;
            if (ff_hevc_decoder.update_thread_context &&
                ff_hevc_decoder.update_thread_context(&c->avctx, &prev->avctx) < 0) rc = -1;
        }
        c->internal.setup_done_cb = mt_setup_done;
        c->internal.setup_done_arg = &arg;
    }
    if (rc == 0) {
        int got = 0;
        g->dbg[c->idx] = seq * 10 + 4;
        if (run_decode(c, data, size, &got) < 0) rc = -1;
        else if (got) {
            *out_frame = av_frame_clone(c->frame);
            rc = *out_frame ? 1 : -1;
        }
    }
    g->dbg[c->idx] = seq * 10 + 5;
    /* A packet that never started a picture (parameter sets, SEI, errors) finishes its setup here. */
    if (g->n > 1 && c->internal.setup_done_cb) { c->internal.setup_done_cb = NULL; mt_setup_done(&arg); }
    return rc;
}

int cos_hevc_describe(const void* frame, cos_hevc_frame_t* out) {
    if (!frame || !out) return -1;
    memset(out, 0, sizeof(*out));
    return describe_avframe((const AVFrame*)frame, out);
}
void cos_hevc_release(void* frame) { AVFrame* f = (AVFrame*)frame; av_frame_free(&f); }
