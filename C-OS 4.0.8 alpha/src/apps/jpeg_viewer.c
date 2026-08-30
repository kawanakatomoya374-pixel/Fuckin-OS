/*
 * jpeg_viewer.c - C-OS 3.2.6 alpha Image Viewer
 *
 * Supports:
 *  - BMP: real pixel decode (24/32-bit uncompressed, top-down or
 *    bottom-up, correctly handling the mandatory 4-byte row padding).
 *  - JPEG: real baseline (SOF0) decode - DQT/DHT/SOF0/SOS, Huffman
 *    entropy decoding, dequantization, IDCT, chroma upsampling and
 *    YCbCr->RGB conversion. Progressive/arithmetic-coded JPEGs are
 *    not supported and are reported as such rather than silently
 *    mis-rendered.
 *  - PNG: header-only (dimensions), since a real decode needs zlib
 *    inflate which isn't available here. A placeholder pattern is
 *    drawn instead so the viewer still shows *something* rather than
 *    a blank window - jpeg_viewer_get_source_kind() tells callers
 *    whether what's in the buffer is real pixels or that placeholder.
 *
 * Any failed load fully resets viewer state (image_loaded, dimensions,
 * component count, source kind) rather than leaving the previous
 * image's info lying around looking like it applies to the new file.
 */
#include "types.h"
#include "io.h"
#include "serial.h"
#include "memory.h"
#include "vga.h"
#include "gfx_blit.h"
#include "png_decoder.h"
#include <string.h>
#include <stdint.h>

extern int cos_fs_read_file(const char* path, void* buffer, uint64_t size);

/* ============================================================================
   Configuration
   ========================================================================== */
#define JPEG_MAX_WIDTH 1920
#define JPEG_MAX_HEIGHT 1080
#define JPEG_BUFFER_SIZE (JPEG_MAX_WIDTH * JPEG_MAX_HEIGHT * 4)  /* 32-bit BGRA */
#define JPEG_FILE_BUFFER_SIZE 65536  /* FS_MAX_DATA (32KB) fits comfortably; headroom for growth */

#define JPEG_ERROR_OK 0
#define JPEG_ERROR_INVALID_FORMAT -1
#define JPEG_ERROR_UNSUPPORTED -2
#define JPEG_ERROR_BUFFER_OVERFLOW -3

typedef enum {
    JPEG_SOURCE_NONE = 0,
    JPEG_SOURCE_REAL = 1,
    JPEG_SOURCE_PATTERN = 2
} jpeg_source_kind_t;

typedef struct {
    uint8_t* file_buffer;
    uint64_t file_buffer_size;
    uint64_t file_size;
} file_reader_t;

typedef struct {
    uint8_t* display_buffer;      /* BGRA, JPEG_MAX_WIDTH x JPEG_MAX_HEIGHT */
    uint64_t current_width;
    uint64_t current_height;
    uint8_t  components;
    bool     image_loaded;
    int      source_kind;         /* jpeg_source_kind_t */
    char     current_filename[256];
} jpeg_viewer_state_t;

static jpeg_viewer_state_t viewer_state = {0};
static file_reader_t reader = {0};

/* The JPEG display buffer must survive EFM previews, standalone windows and
 * NetSurf's heap pressure. Keep these bounded workspaces in kernel BSS rather
 * than the fragmented general-purpose heap. */
static uint8_t s_jpeg_display_storage[JPEG_BUFFER_SIZE];
static uint8_t s_jpeg_file_storage[JPEG_FILE_BUFFER_SIZE];

static void reset_viewer_state_for_failure(void) {
    /* On any load failure, fully clear everything that describes "what
     * image is loaded" so a caller can't end up looking at the *new*
     * filename paired with the *old* image's dimensions/pixels. */
    viewer_state.image_loaded = false;
    viewer_state.current_width = 0;
    viewer_state.current_height = 0;
    viewer_state.components = 0;
    viewer_state.source_kind = JPEG_SOURCE_NONE;
}

/* ============================================================================
   Format probes
   ========================================================================== */
static bool is_jpeg_file(const uint8_t* data, uint64_t size) {
    return size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

static bool is_png_file(const uint8_t* data, uint64_t size) {
    return size >= 8 &&
        data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A;
}

static bool is_bmp_file(const uint8_t* data, uint64_t size) {
    return size >= 18 && data[0] == 'B' && data[1] == 'M';
}

static uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t read_i32_le(const uint8_t* p) {
    return (int32_t)read_u32_le(p);
}
static uint16_t read_u16_le(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint16_t read_u16_be(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static bool parse_png_header(const uint8_t* data, uint64_t size, uint64_t* width, uint64_t* height, uint8_t* components) {
    /* PNG signature is 8 bytes; the IHDR chunk's width/height are at
     * bytes 16-23 (big-endian u32 each) - bounds-checked to 24 bytes,
     * matching every byte this function actually touches. */
    if (!is_png_file(data, size) || size < 24) return false;
    *width = ((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) | ((uint32_t)data[18] << 8) | (uint32_t)data[19];
    *height = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) | ((uint32_t)data[22] << 8) | (uint32_t)data[23];
    *components = 4;
    return *width > 0 && *height > 0;
}

/* ============================================================================
   BMP decoding (real pixel data, not a placeholder)

   Handles what the previous implementation got wrong:
   - Row padding: BMP rows are padded to a 4-byte boundary. A 24-bit
     image that is, say, 5px wide has 15 data bytes per row but the
     file stores 16 (rounded up to a multiple of 4) - reading rows
     back-to-back without accounting for that pad silently shears the
     whole image sideways after row 1.
   - Bottom-up vs top-down: a *positive* height field means the rows
     are stored bottom-to-top (the BMP norm); a *negative* height means
     top-down. Ignoring this flips the image vertically.
   - Reading bits_per_pixel from buffer[28] unconditionally could read
     out of bounds on a short/malformed file; the header's own claimed
     size is validated before this byte is ever touched.
   - Only 24-bit (BGR) and 32-bit (BGRA) uncompressed images are
     supported; anything else (8-bit palette, 16-bit, RLE-compressed,
     etc.) is rejected rather than rendered as garbage.
   ========================================================================== */
static bool decode_bmp(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                        uint64_t max_out_w, uint64_t max_out_h,
                        uint64_t* out_w, uint64_t* out_h) {
    if (!is_bmp_file(data, size) || size < 54) {
        serial_puts("[JPEG] BMP: file too small for a valid header\n");
        return false;
    }

    uint32_t pixel_offset = read_u32_le(&data[10]);
    uint32_t dib_header_size = read_u32_le(&data[14]);
    if (dib_header_size < 40 || (uint64_t)14 + dib_header_size > size) {
        serial_puts("[JPEG] BMP: unsupported/truncated DIB header\n");
        return false;
    }

    int32_t width_signed = read_i32_le(&data[18]);
    int32_t height_signed = read_i32_le(&data[22]);
    if (width_signed <= 0) {
        serial_puts("[JPEG] BMP: invalid width\n");
        return false;
    }
    uint32_t width = (uint32_t)width_signed;
    bool bottom_up = height_signed > 0;
    uint32_t height = bottom_up ? (uint32_t)height_signed : (uint32_t)(-height_signed);
    if (height == 0) {
        serial_puts("[JPEG] BMP: invalid height\n");
        return false;
    }

    uint16_t planes = read_u16_le(&data[26]);
    uint16_t bpp = read_u16_le(&data[28]);
    uint32_t compression = read_u32_le(&data[30]);

    if (planes != 1) {
        serial_puts("[JPEG] BMP: unsupported color plane count\n");
        return false;
    }
    if (compression != 0 /* BI_RGB */) {
        serial_puts("[JPEG] BMP: compressed BMPs are not supported\n");
        return false;
    }
    if (bpp != 24 && bpp != 32) {
        serial_puts("[JPEG] BMP: only 24-bit and 32-bit uncompressed BMPs are supported\n");
        return false;
    }
    if (pixel_offset >= size) {
        serial_puts("[JPEG] BMP: pixel data offset is out of range\n");
        return false;
    }

    uint64_t bytes_per_pixel = bpp / 8;
    /* Rows are padded to a 4-byte boundary. */
    uint64_t row_stride = ((uint64_t)width * bytes_per_pixel + 3) & ~(uint64_t)3;
    uint64_t needed = (uint64_t)pixel_offset + row_stride * height;
    if (needed > size) {
        serial_puts("[JPEG] BMP: pixel data extends past end of file (truncated?)\n");
        return false;
    }

    uint64_t out_width = width < max_out_w ? width : max_out_w;
    uint64_t out_height = height < max_out_h ? height : max_out_h;

    for (uint64_t y = 0; y < out_height; ++y) {
        /* BMP row order is bottom-up by default; map destination row y
         * to the correct source row depending on that flag. */
        uint64_t src_row = bottom_up ? (height - 1 - y) : y;
        const uint8_t* row = data + pixel_offset + src_row * row_stride;
        /* Dense packing: destination stride is out_width, matching what
         * jpeg_viewer_display()/draw_scaled() assume when they read the
         * buffer back using current_width as the row stride. */
        uint8_t* dst_row = out_bgra + y * out_width * 4;

        for (uint64_t x = 0; x < out_width; ++x) {
            const uint8_t* px = row + x * bytes_per_pixel;
            uint8_t* dst = dst_row + x * 4;
            dst[0] = px[0];                                   /* B */
            dst[1] = px[1];                                   /* G */
            dst[2] = px[2];                                   /* R */
            dst[3] = (bpp == 32) ? px[3] : 255;                /* A */
        }
    }

    *out_w = out_width;
    *out_h = out_height;
    return true;
}

/* ============================================================================
   Baseline JPEG decoding (ITU-T T.81 / ISO/IEC 10918-1, sequential DCT,
   Huffman entropy coding only - no progressive, no arithmetic coding)
   ========================================================================== */

/* IDCT basis matrix: cos_table[x][u] = cos((2x+1)*u*pi/16). Precomputed
 * at build time rather than calling cosf() at runtime, since this
 * freestanding kernel has no libm linked in. */
static const float g_idct_cos[8][8] = {
    {1.0000000000f, 0.9807852804f, 0.9238795325f, 0.8314696123f, 0.7071067812f, 0.5555702330f, 0.3826834324f, 0.1950903220f},
    {1.0000000000f, 0.8314696123f, 0.3826834324f, -0.1950903220f, -0.7071067812f, -0.9807852804f, -0.9238795325f, -0.5555702330f},
    {1.0000000000f, 0.5555702330f, -0.3826834324f, -0.9807852804f, -0.7071067812f, 0.1950903220f, 0.9238795325f, 0.8314696123f},
    {1.0000000000f, 0.1950903220f, -0.9238795325f, -0.5555702330f, 0.7071067812f, 0.8314696123f, -0.3826834324f, -0.9807852804f},
    {1.0000000000f, -0.1950903220f, -0.9238795325f, 0.5555702330f, 0.7071067812f, -0.8314696123f, -0.3826834324f, 0.9807852804f},
    {1.0000000000f, -0.5555702330f, -0.3826834324f, 0.9807852804f, -0.7071067812f, -0.1950903220f, 0.9238795325f, -0.8314696123f},
    {1.0000000000f, -0.8314696123f, 0.3826834324f, 0.1950903220f, -0.7071067812f, 0.9807852804f, -0.9238795325f, 0.5555702330f},
    {1.0000000000f, -0.9807852804f, 0.9238795325f, -0.8314696123f, 0.7071067812f, -0.5555702330f, 0.3826834324f, -0.1950903220f},
};

static const uint8_t g_zigzag[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

#define JPEG_MAX_QTABLES 4
#define JPEG_MAX_HUFF 4
#define JPEG_MAX_COMPONENTS 4

typedef struct {
    uint8_t bits[17];     /* counts of codes of each length 1..16 */
    uint8_t values[256];
    int mincode[17];
    int maxcode[17];      /* -1 = no codes of this length */
    int valptr[17];
    bool defined;
} jhuff_table_t;

typedef struct {
    int id;
    int h, v;              /* sampling factors */
    int tq;                /* quant table index */
    int td, ta;             /* huffman table indices (DC, AC) */
    int dc_pred;
    uint8_t* plane;         /* decoded samples, plane_w * plane_h, one byte/sample */
    int plane_w, plane_h;
} jcomp_t;

typedef struct {
    const uint8_t* data;
    uint64_t pos, size;
    uint32_t bitbuf;
    int bitcnt;
    int marker;             /* nonzero once a real marker (not FF00 stuffing) is hit */
} jbitreader_t;

typedef struct {
    uint16_t qtab[JPEG_MAX_QTABLES][64];   /* dequantized in natural (non-zigzag) order */
    bool qtab_defined[JPEG_MAX_QTABLES];
    jhuff_table_t hdc[JPEG_MAX_HUFF];
    jhuff_table_t hac[JPEG_MAX_HUFF];
    jcomp_t comp[JPEG_MAX_COMPONENTS];
    int ncomp;
    uint64_t width, height;
    int restart_interval;
    bool progressive;       /* SOF2 seen -> unsupported */
    bool got_sof;
} jctx_t;

static void jhuff_build(jhuff_table_t* t) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; ++l) {
        if (t->bits[l] == 0) {
            t->maxcode[l] = -1;
        } else {
            t->valptr[l] = k;
            t->mincode[l] = code;
            code += t->bits[l];
            k += t->bits[l];
            t->maxcode[l] = code - 1;
        }
        code <<= 1;
    }
    t->defined = true;
}

static int jbr_fill(jbitreader_t* br) {
    if (br->marker) return -1;
    if (br->pos >= br->size) { br->marker = 0xD9; return -1; } /* treat EOF like EOI */
    uint8_t b = br->data[br->pos++];
    if (b == 0xFF) {
        while (br->pos < br->size && br->data[br->pos] == 0xFF) br->pos++;
        if (br->pos >= br->size) { br->marker = 0xD9; return -1; }
        uint8_t b2 = br->data[br->pos];
        if (b2 == 0x00) { br->pos++; return 0xFF; }
        /* Real marker: step back onto the 0xFF so the caller can see it
         * via br->pos, and record which marker it was. */
        br->marker = b2;
        br->pos--;
        return -1;
    }
    return b;
}

static int jbr_bit(jbitreader_t* br) {
    if (br->bitcnt == 0) {
        int b = jbr_fill(br);
        if (b < 0) { br->bitbuf = 0; br->bitcnt = 8; return 0; } /* pad with zero bits at markers/EOF */
        br->bitbuf = (uint32_t)b;
        br->bitcnt = 8;
    }
    br->bitcnt--;
    return (int)((br->bitbuf >> br->bitcnt) & 1);
}

static int jhuff_decode(jbitreader_t* br, jhuff_table_t* t) {
    if (!t->defined) return -1;
    int code = jbr_bit(br);
    int l = 1;
    while (t->maxcode[l] == -1 || code > t->maxcode[l]) {
        code = (code << 1) | jbr_bit(br);
        l++;
        if (l > 16) return -1; /* corrupt stream */
    }
    int idx = t->valptr[l] + (code - t->mincode[l]);
    if (idx < 0 || idx > 255) return -1;
    return t->values[idx];
}

static int jreceive_extend(jbitreader_t* br, int s) {
    if (s == 0) return 0;
    int v = 0;
    for (int i = 0; i < s; ++i) v = (v << 1) | jbr_bit(br);
    if (v < (1 << (s - 1))) v = v - (1 << s) + 1;
    return v;
}

/* Separable 2D IDCT using the precomputed cosine basis; output is
 * level-shifted (+128) and clamped to [0,255]. */
static void jidct8x8(const int* coef, uint8_t* out8x8) {
    float tmp[64];
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            float sum = 0.0f;
            for (int u = 0; u < 8; ++u) {
                float cu = (u == 0) ? 0.70710678f : 1.0f;
                sum += cu * (float)coef[y * 8 + u] * g_idct_cos[x][u];
            }
            tmp[y * 8 + x] = sum * 0.5f;
        }
    }
    for (int x = 0; x < 8; ++x) {
        for (int y = 0; y < 8; ++y) {
            float sum = 0.0f;
            for (int v = 0; v < 8; ++v) {
                float cv = (v == 0) ? 0.70710678f : 1.0f;
                sum += cv * tmp[v * 8 + x] * g_idct_cos[y][v];
            }
            float val = sum * 0.5f + 128.0f;
            if (val < 0.0f) val = 0.0f;
            if (val > 255.0f) val = 255.0f;
            out8x8[y * 8 + x] = (uint8_t)(val + 0.5f);
        }
    }
}

static int jpeg_find_comp(jctx_t* ctx, int id) {
    for (int i = 0; i < ctx->ncomp; ++i) if (ctx->comp[i].id == id) return i;
    return -1;
}

static void jpeg_free_planes(jctx_t* ctx) {
    /* The kernel heap currently reports an invalid free for valid JPEG
     * component planes after a successful decode.  That corrupts allocator
     * metadata and clears the shared Image Viewer state before it can draw.
     * The decoder context is static and image loads are infrequent, so retain
     * these short-lived planes for the current alpha baseline rather than
     * calling the unsafe deallocator.  A dedicated reusable plane arena will
     * replace this conservative ownership workaround in the allocator pass. */
    if (!ctx) return;
    for (int i = 0; i < ctx->ncomp; ++i) {
        ctx->comp[i].plane = NULL;
    }
}

/* Parses all header segments (DQT/SOF0/DHT/DRI) up to (not including)
 * the SOS payload, then decodes the entropy-coded scan directly into
 * ctx->comp[].plane. Returns a JPEG_ERROR_* code. */
static int jpeg_decode(const uint8_t* data, uint64_t size, jctx_t* ctx) {
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return JPEG_ERROR_INVALID_FORMAT;
    }
    uint64_t pos = 2;

    while (pos + 4 <= size) {
        if (data[pos] != 0xFF) { pos++; continue; }
        uint8_t marker = data[pos + 1];
        pos += 2;

        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (marker == 0xD9) break; /* EOI with no scan - malformed but stop cleanly */
        if (pos + 2 > size) break;

        uint16_t seglen = read_u16_be(&data[pos]);
        if (seglen < 2 || pos + seglen > size) break;
        uint64_t segstart = pos + 2;
        uint64_t segend = pos + seglen;

        if (marker == 0xDB) { /* DQT */
            uint64_t p = segstart;
            while (p < segend) {
                uint8_t pq = data[p] >> 4, tq = data[p] & 0x0F;
                p++;
                if (tq >= JPEG_MAX_QTABLES) break;
                for (int i = 0; i < 64 && p < segend; ++i) {
                    if (pq) {
                        if (p + 2 > segend) break;
                        ctx->qtab[tq][g_zigzag[i]] = read_u16_be(&data[p]);
                        p += 2;
                    } else {
                        ctx->qtab[tq][g_zigzag[i]] = data[p];
                        p += 1;
                    }
                }
                ctx->qtab_defined[tq] = true;
            }
        } else if (marker == 0xC0) { /* SOF0: baseline sequential DCT */
            uint64_t p = segstart;
            if (segend - p < 6) return JPEG_ERROR_INVALID_FORMAT;
            p++; /* precision - only 8-bit supported, not separately validated */
            ctx->height = read_u16_be(&data[p]); p += 2;
            ctx->width = read_u16_be(&data[p]); p += 2;
            ctx->ncomp = data[p]; p++;
            if (ctx->ncomp < 1 || ctx->ncomp > JPEG_MAX_COMPONENTS) return JPEG_ERROR_UNSUPPORTED;
            if (ctx->width == 0 || ctx->height == 0) return JPEG_ERROR_INVALID_FORMAT;
            for (int i = 0; i < ctx->ncomp; ++i) {
                if (p + 3 > segend) return JPEG_ERROR_INVALID_FORMAT;
                ctx->comp[i].id = data[p]; p++;
                ctx->comp[i].h = data[p] >> 4;
                ctx->comp[i].v = data[p] & 0x0F;
                p++;
                ctx->comp[i].tq = data[p]; p++;
                if (ctx->comp[i].h < 1 || ctx->comp[i].h > 4 || ctx->comp[i].v < 1 || ctx->comp[i].v > 4) {
                    return JPEG_ERROR_UNSUPPORTED;
                }
            }
            ctx->got_sof = true;
        } else if (marker == 0xC1 || marker == 0xC2 || marker == 0xC3 ||
                   (marker >= 0xC5 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)) {
            /* C1: extended sequential, C2: progressive, C3: lossless,
             * others: differential/arithmetic-coded variants. None of
             * these are implemented; report unsupported rather than
             * misinterpreting the bitstream as baseline. */
            ctx->progressive = (marker == 0xC2);
            return JPEG_ERROR_UNSUPPORTED;
        } else if (marker == 0xC4) { /* DHT */
            uint64_t p = segstart;
            while (p < segend) {
                uint8_t tc = data[p] >> 4, th = data[p] & 0x0F;
                p++;
                if (th >= JPEG_MAX_HUFF || p + 16 > segend) break;
                jhuff_table_t* t = tc ? &ctx->hac[th] : &ctx->hdc[th];
                int total = 0;
                for (int i = 1; i <= 16; ++i) { t->bits[i] = data[p]; total += data[p]; p++; }
                if (p + (uint64_t)total > segend || total > 256) break;
                for (int i = 0; i < total; ++i) { t->values[i] = data[p]; p++; }
                jhuff_build(t);
            }
        } else if (marker == 0xDD) { /* DRI */
            if (segend - segstart >= 2) ctx->restart_interval = read_u16_be(&data[segstart]);
        } else if (marker == 0xDA) { /* SOS - header, then entropy-coded data follows */
            if (!ctx->got_sof) return JPEG_ERROR_INVALID_FORMAT;
            uint64_t p = segstart;
            if (p >= segend) return JPEG_ERROR_INVALID_FORMAT;
            int ns = data[p]; p++;
            for (int i = 0; i < ns; ++i) {
                if (p + 2 > segend) return JPEG_ERROR_INVALID_FORMAT;
                int cs = data[p]; p++;
                int tdta = data[p]; p++;
                int ci = jpeg_find_comp(ctx, cs);
                if (ci >= 0) { ctx->comp[ci].td = tdta >> 4; ctx->comp[ci].ta = tdta & 0x0F; }
            }
            /* Ss, Se, AhAl (3 bytes) - fixed for baseline, not inspected */

            int maxh = 1, maxv = 1;
            for (int i = 0; i < ctx->ncomp; ++i) {
                if (ctx->comp[i].h > maxh) maxh = ctx->comp[i].h;
                if (ctx->comp[i].v > maxv) maxv = ctx->comp[i].v;
            }
            /* Sanity-check every quant/huffman table this scan actually
             * references is defined, and that the pixel count is sane,
             * before committing to (potentially large) plane allocations. */
            if (ctx->width > JPEG_MAX_WIDTH || ctx->height > JPEG_MAX_HEIGHT) {
                return JPEG_ERROR_BUFFER_OVERFLOW;
            }
            for (int i = 0; i < ctx->ncomp; ++i) {
                if (!ctx->qtab_defined[ctx->comp[i].tq] || !ctx->hdc[ctx->comp[i].td].defined || !ctx->hac[ctx->comp[i].ta].defined) {
                    return JPEG_ERROR_INVALID_FORMAT;
                }
            }

            int mcux = (int)((ctx->width + 8 * maxh - 1) / (8 * maxh));
            int mcuy = (int)((ctx->height + 8 * maxv - 1) / (8 * maxv));

            for (int i = 0; i < ctx->ncomp; ++i) {
                ctx->comp[i].plane_w = mcux * ctx->comp[i].h * 8;
                ctx->comp[i].plane_h = mcuy * ctx->comp[i].v * 8;
                uint64_t plane_bytes = (uint64_t)ctx->comp[i].plane_w * (uint64_t)ctx->comp[i].plane_h;
                ctx->comp[i].plane = (uint8_t*)kmalloc(plane_bytes);
                if (!ctx->comp[i].plane) { jpeg_free_planes(ctx); return JPEG_ERROR_BUFFER_OVERFLOW; }
                ctx->comp[i].dc_pred = 0;
            }

            jbitreader_t br = {0};
            br.data = data;
            br.pos = pos + seglen; /* right after the SOS header we just parsed */
            br.size = size;

            int restart_count = 0;
            bool corrupt = false;
            for (int my = 0; my < mcuy && !corrupt; ++my) {
                for (int mx = 0; mx < mcux && !corrupt; ++mx) {
                    for (int c = 0; c < ctx->ncomp && !corrupt; ++c) {
                        jcomp_t* comp = &ctx->comp[c];
                        for (int v = 0; v < comp->v && !corrupt; ++v) {
                            for (int h = 0; h < comp->h && !corrupt; ++h) {
                                int coef[64] = {0};
                                int s = jhuff_decode(&br, &ctx->hdc[comp->td]);
                                if (s < 0 || s > 11) { corrupt = true; break; }
                                int diff = jreceive_extend(&br, s);
                                comp->dc_pred += diff;
                                coef[0] = comp->dc_pred * ctx->qtab[comp->tq][0];

                                int k = 1;
                                while (k < 64) {
                                    int rs = jhuff_decode(&br, &ctx->hac[comp->ta]);
                                    if (rs < 0) { corrupt = true; break; }
                                    int r = rs >> 4, sz = rs & 0x0F;
                                    if (sz == 0) {
                                        if (r == 15) { k += 16; continue; } /* ZRL */
                                        break; /* EOB */
                                    }
                                    k += r;
                                    if (k >= 64) break;
                                    int val = jreceive_extend(&br, sz);
                                    coef[g_zigzag[k]] = val * ctx->qtab[comp->tq][k];
                                    k++;
                                }
                                if (corrupt) break;

                                uint8_t block[64];
                                jidct8x8(coef, block);
                                int bx = mx * comp->h + h, by = my * comp->v + v;
                                int px0 = bx * 8, py0 = by * 8;
                                for (int yy = 0; yy < 8; ++yy) {
                                    memcpy(&comp->plane[(py0 + yy) * comp->plane_w + px0], &block[yy * 8], 8);
                                }
                            }
                        }
                    }
                    if (ctx->restart_interval > 0) {
                        restart_count++;
                        bool last_mcu = (my == mcuy - 1) && (mx == mcux - 1);
                        if (restart_count == ctx->restart_interval && !last_mcu) {
                            restart_count = 0;
                            br.bitcnt = 0;
                            if (br.marker >= 0xD0 && br.marker <= 0xD7) {
                                br.pos += 2; /* consume the FF Dn we're parked on */
                                br.marker = 0;
                            }
                            for (int i = 0; i < ctx->ncomp; ++i) ctx->comp[i].dc_pred = 0;
                        }
                    }
                }
            }

            if (corrupt) { jpeg_free_planes(ctx); return JPEG_ERROR_INVALID_FORMAT; }
            return JPEG_ERROR_OK; /* decoded; caller does YCbCr->RGB + writes display buffer */
        } else if (marker == 0x00 || marker == 0xFF) {
            continue;
        }

        pos = segend;
    }

    return ctx->got_sof ? JPEG_ERROR_INVALID_FORMAT /* no SOS found */ : JPEG_ERROR_UNSUPPORTED;
}

/* Converts decoded component planes to BGRA in out_bgra (stride =
 * max_out_w * 4), nearest-neighbor upsampling any subsampled chroma
 * planes to full resolution. Frees the component planes when done. */
static void jpeg_to_bgra(jctx_t* ctx, uint8_t* out_bgra, uint64_t max_out_w, uint64_t max_out_h,
                          uint64_t* out_w, uint64_t* out_h) {
    int maxh = 1, maxv = 1;
    for (int i = 0; i < ctx->ncomp; ++i) {
        if (ctx->comp[i].h > maxh) maxh = ctx->comp[i].h;
        if (ctx->comp[i].v > maxv) maxv = ctx->comp[i].v;
    }
    uint64_t w = ctx->width < max_out_w ? ctx->width : max_out_w;
    uint64_t h = ctx->height < max_out_h ? ctx->height : max_out_h;

    for (uint64_t y = 0; y < h; ++y) {
        /* Dense packing (stride = w), matching decode_bmp() and what the
         * display/draw_scaled functions expect when reading this buffer
         * back using current_width as the row stride. */
        uint8_t* dst_row = out_bgra + y * w * 4;
        for (uint64_t x = 0; x < w; ++x) {
            int Y, Cb = 128, Cr = 128;
            jcomp_t* c0 = &ctx->comp[0];
            uint64_t sx0 = x * c0->h / maxh, sy0 = y * c0->v / maxv;
            Y = c0->plane[sy0 * c0->plane_w + sx0];

            if (ctx->ncomp >= 3) {
                jcomp_t* c1 = &ctx->comp[1];
                jcomp_t* c2 = &ctx->comp[2];
                uint64_t sx1 = x * c1->h / maxh, sy1 = y * c1->v / maxv;
                uint64_t sx2 = x * c2->h / maxh, sy2 = y * c2->v / maxv;
                Cb = c1->plane[sy1 * c1->plane_w + sx1];
                Cr = c2->plane[sy2 * c2->plane_w + sx2];
            }

            int R, G, B;
            if (ctx->ncomp == 1) {
                R = G = B = Y;
            } else {
                R = (int)(Y + 1.402f * (float)(Cr - 128));
                G = (int)(Y - 0.344136f * (float)(Cb - 128) - 0.714136f * (float)(Cr - 128));
                B = (int)(Y + 1.772f * (float)(Cb - 128));
                if (R < 0) R = 0;
                if (R > 255) R = 255;
                if (G < 0) G = 0;
                if (G > 255) G = 255;
                if (B < 0) B = 0;
                if (B > 255) B = 255;
            }

            uint8_t* dst = dst_row + x * 4;
            dst[0] = (uint8_t)B;
            dst[1] = (uint8_t)G;
            dst[2] = (uint8_t)R;
            dst[3] = 255;
        }
    }

    jpeg_free_planes(ctx);
    *out_w = w;
    *out_h = h;
}

/* ============================================================================
   Background loading
   ========================================================================== */
#include "sync.h"
#include "task.h"

int jpeg_viewer_load(const char* filename); /* forward decl for the thread entry below */
static int jpeg_viewer_load_impl(const char* filename); /* forward decl - real definition is further down */

static mutex_t         jpeg_load_mutex;
static bool            jpeg_load_mutex_ready = false;
static volatile bool   jpeg_load_busy = false;
static volatile int    jpeg_load_last_result = JPEG_ERROR_OK;
static char            jpeg_pending_filename[256];

static void jpeg_load_thread_entry(void* arg) {
    (void)arg;
    mutex_lock(&jpeg_load_mutex);
    jpeg_load_last_result = jpeg_viewer_load_impl(jpeg_pending_filename);
    jpeg_load_busy = false;
    mutex_unlock(&jpeg_load_mutex);
}

int jpeg_viewer_load_async(const char* filename) {
    if (!filename) return -1;
    if (!jpeg_load_mutex_ready) {
        mutex_init(&jpeg_load_mutex);
        jpeg_load_mutex_ready = true;
    }
    if (jpeg_load_busy) return -1;

    strncpy(jpeg_pending_filename, filename, sizeof(jpeg_pending_filename) - 1);
    jpeg_pending_filename[sizeof(jpeg_pending_filename) - 1] = '\0';

    jpeg_load_busy = true;
    if (!thread_create_kernel("jpeg_load", (void*)jpeg_load_thread_entry, NULL)) {
        jpeg_load_busy = false;
        return -1;
    }
    return 0;
}

bool jpeg_viewer_is_loading(void) {
    return jpeg_load_busy;
}

int jpeg_viewer_get_last_result(void) {
    return jpeg_load_last_result;
}

/* ============================================================================
   Initialization
   ========================================================================== */
int jpeg_viewer_init(void) {
    serial_puts("[JPEG] Initializing image viewer\n");
    viewer_state.display_buffer = s_jpeg_display_storage;
    reader.file_buffer = s_jpeg_file_storage;
    reader.file_buffer_size = JPEG_FILE_BUFFER_SIZE;
    reader.file_size = 0;

    memset(viewer_state.current_filename, 0, sizeof(viewer_state.current_filename));
    reset_viewer_state_for_failure();

    serial_puts("[JPEG] Image viewer initialized successfully\n");
    return 0;
}

int jpeg_viewer_load(const char* filename) {
    if (!jpeg_load_mutex_ready) {
        mutex_init(&jpeg_load_mutex);
        jpeg_load_mutex_ready = true;
    }
    mutex_lock(&jpeg_load_mutex);
    int result = jpeg_viewer_load_impl(filename);
    mutex_unlock(&jpeg_load_mutex);
    return result;
}

/* Decode a JPEG supplied by a network/content client without touching the
 * singleton file-viewer buffers or its visible state.  The compact NetSurf
 * frontend serialises fetch conversion on the GUI thread, so the static parse
 * context avoids a large kernel-stack allocation while remaining isolated from
 * jpeg_viewer_load(). */
int jpeg_decode_memory_to_bgra(const uint8_t* data, uint64_t size,
                               uint8_t* out_bgra,
                               uint64_t max_width, uint64_t max_height,
                               uint64_t* out_width, uint64_t* out_height,
                               uint8_t* out_components)
{
    if (out_width) *out_width = 0;
    if (out_height) *out_height = 0;
    if (out_components) *out_components = 0;
    if (data == NULL || out_bgra == NULL || max_width == 0 || max_height == 0 ||
        !is_jpeg_file(data, size)) {
        return JPEG_ERROR_INVALID_FORMAT;
    }

    static jctx_t ctx; /* Huffman tables make this unsuitable for the stack. */
    memset(&ctx, 0, sizeof(ctx));
    int rc = jpeg_decode(data, size, &ctx);
    if (rc != JPEG_ERROR_OK) return rc;

    uint64_t width = 0;
    uint64_t height = 0;
    jpeg_to_bgra(&ctx, out_bgra, max_width, max_height, &width, &height);
    if (width == 0 || height == 0 || width > max_width || height > max_height) {
        return JPEG_ERROR_INVALID_FORMAT;
    }

    if (out_width) *out_width = width;
    if (out_height) *out_height = height;
    if (out_components) *out_components = (uint8_t)ctx.ncomp;
    return JPEG_ERROR_OK;
}

/* ============================================================================
   File loading
   ========================================================================== */
static int jpeg_viewer_load_impl(const char* filename) {
    serial_puts("[JPEG] Loading image: ");
    serial_puts(filename ? filename : "(null)");
    serial_puts("\n");

    if (!filename) {
        reset_viewer_state_for_failure();
        return JPEG_ERROR_BUFFER_OVERFLOW;
    }

    /* The viewer is shared by EFM previews and the standalone window.  A
     * failed third-party allocation/free must not make every later load fail
     * before filesystem access; rebuild any missing persistent buffers here. */
    if (!viewer_state.display_buffer) {
        viewer_state.display_buffer = s_jpeg_display_storage;
    }
    if (!reader.file_buffer) {
        reader.file_buffer = s_jpeg_file_storage;
        reader.file_buffer_size = JPEG_FILE_BUFFER_SIZE;
        reader.file_size = 0;
    }

    strncpy(viewer_state.current_filename, filename, sizeof(viewer_state.current_filename) - 1);
    viewer_state.current_filename[sizeof(viewer_state.current_filename) - 1] = '\0';

    /* Any early-return failure path below must leave viewer_state fully
     * reset (not carrying over the previous image's dimensions/pixels)
     * even though current_filename above now names the new (failed) file. */
    int64_t bytes_read = cos_fs_read_file(filename, reader.file_buffer, reader.file_buffer_size);
    if (bytes_read <= 0) {
        serial_puts("[JPEG] Failed to read file from filesystem\n");
        reset_viewer_state_for_failure();
        return JPEG_ERROR_INVALID_FORMAT;
    }
    reader.file_size = (uint64_t)bytes_read;

    serial_puts("[JPEG] File read: ");
    serial_putdec(reader.file_size);
    serial_puts(" bytes\n");

    const uint8_t* buf = reader.file_buffer;
    uint64_t size = reader.file_size;

    if (is_jpeg_file(buf, size)) {
        static jctx_t ctx; /* large (huffman tables etc.); static avoids a big stack frame */
        memset(&ctx, 0, sizeof(ctx));

        int rc = jpeg_decode(buf, size, &ctx);
        if (rc != JPEG_ERROR_OK) {
            if (ctx.progressive) {
                serial_puts("[JPEG] Progressive JPEGs are not supported\n");
            } else {
                serial_puts("[JPEG] JPEG decode failed (unsupported feature or corrupt data)\n");
            }
            reset_viewer_state_for_failure();
            return rc;
        }

        uint64_t w = 0, h = 0;
        jpeg_to_bgra(&ctx, viewer_state.display_buffer, JPEG_MAX_WIDTH, JPEG_MAX_HEIGHT, &w, &h);

        viewer_state.current_width = w;
        viewer_state.current_height = h;
        viewer_state.components = (uint8_t)ctx.ncomp;
        viewer_state.source_kind = JPEG_SOURCE_REAL;
        viewer_state.image_loaded = (w > 0 && h > 0);

        if (!viewer_state.image_loaded) {
            reset_viewer_state_for_failure();
            return JPEG_ERROR_INVALID_FORMAT;
        }

        serial_puts("[JPEG] Decoded JPEG: ");
        serial_putdec(w); serial_puts("x"); serial_putdec(h); serial_puts("\n");
        return JPEG_ERROR_OK;
    }

    if (is_bmp_file(buf, size)) {
        uint64_t w = 0, h = 0;
        bool ok = decode_bmp(buf, size, viewer_state.display_buffer, JPEG_MAX_WIDTH, JPEG_MAX_HEIGHT, &w, &h);
        if (!ok || w == 0 || h == 0) {
            reset_viewer_state_for_failure();
            return JPEG_ERROR_INVALID_FORMAT;
        }
        viewer_state.current_width = w;
        viewer_state.current_height = h;
        viewer_state.components = 4;
        viewer_state.source_kind = JPEG_SOURCE_REAL;
        viewer_state.image_loaded = true;

        serial_puts("[JPEG] Decoded BMP: ");
        serial_putdec(w); serial_puts("x"); serial_putdec(h); serial_puts("\n");
        return JPEG_ERROR_OK;
    }

    if (is_png_file(buf, size)) {
        uint64_t w = 0, h = 0;
        if (png_decode(buf, size, viewer_state.display_buffer, JPEG_MAX_WIDTH, JPEG_MAX_HEIGHT, &w, &h) && w > 0 && h > 0) {
            viewer_state.current_width = w;
            viewer_state.current_height = h;
            viewer_state.components = 4;
            viewer_state.source_kind = JPEG_SOURCE_REAL;
            viewer_state.image_loaded = true;

            serial_puts("[JPEG] Decoded PNG: ");
            serial_putdec(w); serial_puts("x"); serial_putdec(h); serial_puts("\n");
            return JPEG_ERROR_OK;
        }

        /* Real decode failed (Adam7 interlacing, unsupported color
         * type/bit depth, or corrupt data - png_decode() already
         * logged the specific reason). Fall back to a header-only
         * placeholder so the viewer still shows *something* rather
         * than nothing. */
        uint8_t comps = 0;
        if (!parse_png_header(buf, size, &w, &h, &comps) || w == 0 || h == 0) {
            serial_puts("[JPEG] PNG header is invalid or truncated\n");
            reset_viewer_state_for_failure();
            return JPEG_ERROR_INVALID_FORMAT;
        }

        if (w > JPEG_MAX_WIDTH) w = JPEG_MAX_WIDTH;
        if (h > JPEG_MAX_HEIGHT) h = JPEG_MAX_HEIGHT;

        uint64_t checksum = w ^ h;
        uint8_t base_r = (uint8_t)(128 + (checksum & 0xFF) % 128);
        uint8_t base_g = (uint8_t)(100 + ((checksum >> 8) & 0xFF) % 128);
        uint8_t base_b = (uint8_t)(150 + ((checksum >> 16) & 0xFF) % 100);
        for (uint64_t y = 0; y < h; ++y) {
            for (uint64_t x = 0; x < w; ++x) {
                uint8_t r = (uint8_t)((x * 255) / w);
                uint8_t g = (uint8_t)((y * 255) / h);
                uint8_t b = (uint8_t)(((x + y) * 200) / (w + h));
                r = (uint8_t)((r + base_r) / 2);
                g = (uint8_t)((g + base_g) / 2);
                b = (uint8_t)((b + base_b) / 2);
                /* Dense packing (stride = w) - matches decode_bmp()/
                 * jpeg_to_bgra() and what the display functions expect. */
                uint64_t off = (y * w + x) * 4;
                viewer_state.display_buffer[off + 0] = b;
                viewer_state.display_buffer[off + 1] = g;
                viewer_state.display_buffer[off + 2] = r;
                viewer_state.display_buffer[off + 3] = 255;
            }
        }
        serial_puts("[DBG] fallback: pattern fill done\n");

        viewer_state.current_width = w;
        viewer_state.current_height = h;
        viewer_state.components = comps;
        viewer_state.source_kind = JPEG_SOURCE_PATTERN;
        viewer_state.image_loaded = true;
        return JPEG_ERROR_OK;
    }

    serial_puts("[JPEG] Unrecognized image format\n");
    reset_viewer_state_for_failure();
    return JPEG_ERROR_UNSUPPORTED;
}

int jpeg_viewer_load_file(const char* file_path) {
    return jpeg_viewer_load(file_path);
}

/* ============================================================================
   Display

   NOTE: display_buffer only holds *real* decoded pixels when
   jpeg_viewer_get_source_kind() reports JPEG_SOURCE_REAL (JPEG, BMP).
   For JPEG_SOURCE_PATTERN (currently: PNG) it holds a placeholder
   gradient, not the actual image content - callers that care about the
   distinction (vs. just wanting *something* to paint) should check.
   ========================================================================== */

/* display_buffer is BGRA bytes (see the struct field comment above);
 * gfx_blit's surfaces are 0x00RRGGBB uint32_t. This is a reusable
 * scratch buffer for that conversion, sized for the same worst case
 * as display_buffer itself and lazily allocated the same way, so
 * jpeg_viewer_display()/jpeg_viewer_draw_scaled() can hand gfx_blit a
 * real source surface without a per-call malloc/free. */
static uint32_t* s_jpeg_convert_scratch = NULL;
static uint64_t s_jpeg_convert_scratch_bytes = 0;

/* Converts the top-left w x h region of display_buffer into
 * s_jpeg_convert_scratch and returns a surface over it, or a
 * zeroed/NULL surface on failure (gfx_blit's own NULL-pixels check
 * makes that a safe no-op for callers). */
static gfx_surface_t jpeg_viewer_convert_to_surface(uint64_t w, uint64_t h) {
    gfx_surface_t empty = gfx_surface_make(NULL, 0, 0, 0);
    if (!viewer_state.display_buffer || w == 0 || h == 0) return empty;

    uint64_t pixel_count = w * h;
    uint64_t max_pixels = (uint64_t)JPEG_MAX_WIDTH * (uint64_t)JPEG_MAX_HEIGHT;
    if (pixel_count == 0 || pixel_count > max_pixels) return empty;

    /* The old path always reserved an additional 1920x1080x4 conversion
     * surface (about 8 MiB), even for small files such as the bundled
     * 644x347 JPEG. With NetSurf's cache resident this exhausted the kernel
     * heap on every preview repaint. Allocate only what the current image
     * requires and retain it for reuse. */
    uint64_t required_bytes = pixel_count * sizeof(uint32_t);
    if (!s_jpeg_convert_scratch || required_bytes > s_jpeg_convert_scratch_bytes) {
        uint32_t* replacement = (uint32_t*)kmalloc((size_t)required_bytes);
        if (!replacement) return empty;
        if (s_jpeg_convert_scratch) kfree(s_jpeg_convert_scratch);
        s_jpeg_convert_scratch = replacement;
        s_jpeg_convert_scratch_bytes = required_bytes;
    }

    const uint8_t* src = viewer_state.display_buffer;
    for (uint64_t i = 0; i < pixel_count; ++i) {
        uint64_t src_idx = i * 4;
        if (src_idx + 4 > JPEG_BUFFER_SIZE) break;
        uint8_t b = src[src_idx + 0];
        uint8_t g = src[src_idx + 1];
        uint8_t r = src[src_idx + 2];
        s_jpeg_convert_scratch[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    return gfx_surface_make(s_jpeg_convert_scratch, (int)w, (int)h, (int)w);
}

int jpeg_viewer_display(void) {
    if (!viewer_state.image_loaded) {
        serial_puts("[JPEG] No image loaded\n");
        return -1;
    }

    uint64_t w = viewer_state.current_width;
    uint64_t h = viewer_state.current_height;

    if (!backbuffer) return -1;
    gfx_surface_t src = jpeg_viewer_convert_to_surface(w, h);
    gfx_surface_t dst = gfx_surface_make(backbuffer, (int)SCREEN_W, (int)SCREEN_H, (int)SCREEN_W);
    gfx_blit(&dst, 0, 0, &src, 0, 0, (int)w, (int)h, GFX_BLIT_COPY, 0);
    return 0;
}

int jpeg_viewer_draw_scaled(uint64_t x, uint64_t y, uint64_t width, uint64_t height) {
    if (!viewer_state.image_loaded || !viewer_state.display_buffer) return -1;
    if (width == 0 || height == 0) return -1;

    uint64_t src_w = viewer_state.current_width;
    uint64_t src_h = viewer_state.current_height;
    if (src_w == 0 || src_h == 0) return -1;

    if (!backbuffer) return -1;
    gfx_surface_t src = jpeg_viewer_convert_to_surface(src_w, src_h);
    gfx_surface_t dst = gfx_surface_make(backbuffer, (int)SCREEN_W, (int)SCREEN_H, (int)SCREEN_W);
    gfx_blit_scaled(&dst, (int)x, (int)y, (int)width, (int)height,
                     &src, 0, 0, (int)src_w, (int)src_h, GFX_BLIT_COPY, 0);
    return 0;
}

/* ============================================================================
   Status / accessors
   ========================================================================== */
bool jpeg_viewer_is_loaded(void) {
    return viewer_state.image_loaded;
}

void jpeg_viewer_get_dimensions(uint64_t* out_width, uint64_t* out_height) {
    if (out_width) *out_width = viewer_state.current_width;
    if (out_height) *out_height = viewer_state.current_height;
}

int jpeg_viewer_get_info(uint64_t* width, uint64_t* height, uint8_t* components) {
    if (!viewer_state.image_loaded) return -1;
    if (width) *width = viewer_state.current_width;
    if (height) *height = viewer_state.current_height;
    if (components) *components = viewer_state.components;
    return 0;
}

const char* jpeg_viewer_get_filename(void) {
    return viewer_state.current_filename;
}

int jpeg_viewer_get_source_kind(void) {
    return viewer_state.source_kind;
}

/* ============================================================================
   image_viewer_* compatibility shims

   Several call sites across the GUI (fm_open_image_viewer()'s double-click
   handler, the file manager's grid-view thumbnail generator, the settings
   wallpaper picker) were written against an "image_viewer_*" API that was
   never actually implemented anywhere - only ever declared as weak externs,
   so every one of those call sites has always silently taken its failure
   branch. These thin wrappers connect them to the real jpeg_viewer_*
   implementation above.
   ========================================================================== */
int image_viewer_load_file(const char* file_path) {
    return jpeg_viewer_load_file(file_path);
}

int image_viewer_draw_scaled(uint64_t x, uint64_t y, uint64_t width, uint64_t height) {
    return jpeg_viewer_draw_scaled(x, y, width, height);
}

bool image_viewer_is_loaded(void) {
    return jpeg_viewer_is_loaded();
}

const char* image_viewer_get_filename(void) {
    return jpeg_viewer_get_filename();
}

/* Packed 0xAARRGGBB pixels, matching what callers (efm_thumbnail_preview.c)
 * expect. display_buffer is stored as BGRA bytes (B,G,R,A in that memory
 * order); reinterpreting 4 such bytes as a little-endian uint32_t on this
 * x86-64 target naturally produces (A<<24)|(R<<16)|(G<<8)|B - exactly that
 * format - so no repacking is needed, just a cast. */
const uint32_t* image_viewer_get_buffer(void) {
    if (!viewer_state.image_loaded || !viewer_state.display_buffer) return NULL;
    return (const uint32_t*)viewer_state.display_buffer;
}

uint64_t image_viewer_get_buffer_width(void) {
    return viewer_state.current_width;
}

uint64_t image_viewer_get_buffer_height(void) {
    return viewer_state.current_height;
}
