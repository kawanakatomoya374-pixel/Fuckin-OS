/*
 * inter prediction template for HEVC/VVC
 *
 * Copyright (C) 2022 Nuo Mi
 * Copyright (C) 2024 Wu Jianhua
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define CHROMA_EXTRA_BEFORE     1
#define CHROMA_EXTRA            3
#define LUMA_EXTRA_BEFORE       3
#define LUMA_EXTRA              7

static void FUNC(put_pixels)(int16_t *dst,
    const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    const pixel *src            = (const pixel *)_src;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dst[x] = src[x] << (14 - BIT_DEPTH);
        src += src_stride;
        dst += MAX_PB_SIZE;
    }
}

static void FUNC(put_uni_pixels)(uint8_t *_dst, const ptrdiff_t _dst_stride,
    const uint8_t *_src, const ptrdiff_t _src_stride, const int height,
     const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    const pixel *src            = (const pixel *)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);

    for (int y = 0; y < height; y++) {
        memcpy(dst, src, width * sizeof(pixel));
        src += src_stride;
        dst += dst_stride;
    }
}

static void FUNC(put_uni_w_pixels)(uint8_t *_dst, const ptrdiff_t _dst_stride,
    const uint8_t *_src, const ptrdiff_t _src_stride, const int height,
    const int denom, const int wx, const int _ox,  const int8_t *restrict hf, const int8_t *restrict vf,
    const int width)
{
    const pixel *src            = (const pixel *)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const int shift             = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    const int offset            = 1 << (shift - 1);
#else
    const int offset            = 0;
#endif
    const int ox                = _ox * (1 << (BIT_DEPTH - 8));

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int v = (src[x] << (14 - BIT_DEPTH));
            dst[x] = av_clip_pixel(((v * wx + offset) >> shift) + ox);
        }
        src += src_stride;
        dst += dst_stride;
    }
}


/* Specialize the interpolation loops on the three luma fractional positions so the
 * taps become compile-time constants (zero taps and multiplies fold away). Tap values
 * are the HEVC luma filters; other tables fall back to the runtime path. */
#ifndef COS_QPEL_SPEC
#define COS_QPEL_SPEC(filter, ...) do { \
    if ((filter)[3] == 58 && (filter)[0] == -1) { enum {c0=-1,c1=4,c2=-10,c3=58,c4=17,c5=-5,c6=1,c7=0}; __VA_ARGS__ } \
    else if ((filter)[3] == 40) { enum {c0=-1,c1=4,c2=-11,c3=40,c4=40,c5=-11,c6=4,c7=-1}; __VA_ARGS__ } \
    else if ((filter)[3] == 17 && (filter)[0] == 0) { enum {c0=0,c1=1,c2=-5,c3=17,c4=58,c5=-10,c6=4,c7=-1}; __VA_ARGS__ } \
    else { const int c0=(filter)[0],c1=(filter)[1],c2=(filter)[2],c3=(filter)[3],c4=(filter)[4],c5=(filter)[5],c6=(filter)[6],c7=(filter)[7]; __VA_ARGS__ } \
} while (0)
#define QPEL_C(src, stride) \
    (c0 * src[x - 3 * stride] + c1 * src[x - 2 * stride] + c2 * src[x - stride] + c3 * src[x] + \
     c4 * src[x + stride] + c5 * src[x + 2 * stride] + c6 * src[x + 3 * stride] + c7 * src[x + 4 * stride])

/* Sliding-window variants: one new sample is loaded per output sample instead of eight. HROW walks a
 * row; VCOL walks a column. STORE sees v (the raw filter sum), x and (for VCOL) y. */
#define QPEL_SUM (c0*a0 + c1*a1 + c2*a2 + c3*a3 + c4*a4 + c5*a5 + c6*a6 + c7*a7)
#define QPEL_HROW(sp, W, STORE) do { \
    const __typeof__(*(sp)) *p_ = (sp); \
    int a0 = p_[-3], a1 = p_[-2], a2 = p_[-1], a3 = p_[0], a4 = p_[1], a5 = p_[2], a6 = p_[3]; \
    for (int x = 0; x < (W); x++) { \
        int a7 = p_[x + 4]; \
        int v = QPEL_SUM; \
        STORE; \
        a0 = a1; a1 = a2; a2 = a3; a3 = a4; a4 = a5; a5 = a6; a6 = a7; \
    } \
} while (0)
#define QPEL_VCOL(base, stride, H, W, STORE) do { \
    for (int x = 0; x < (W); x++) { \
        const __typeof__(*(base)) *p_ = (base) + x; \
        int a0 = p_[-3 * (stride)], a1 = p_[-2 * (stride)], a2 = p_[-(stride)], a3 = p_[0], \
            a4 = p_[(stride)], a5 = p_[2 * (stride)], a6 = p_[3 * (stride)]; \
        for (int y = 0; y < (H); y++) { \
            int a7 = p_[(y + 4) * (stride)]; \
            int v = QPEL_SUM; \
            STORE; \
            a0 = a1; a1 = a2; a2 = a3; a3 = a4; a4 = a5; a5 = a6; a6 = a7; \
        } \
    } \
} while (0)
#endif

#define LUMA_FILTER(src, stride)                                               \
    (filter[0] * src[x - 3 * stride] +                                         \
     filter[1] * src[x - 2 * stride] +                                         \
     filter[2] * src[x -     stride] +                                         \
     filter[3] * src[x             ] +                                         \
     filter[4] * src[x +     stride] +                                         \
     filter[5] * src[x + 2 * stride] +                                         \
     filter[6] * src[x + 3 * stride] +                                         \
     filter[7] * src[x + 4 * stride])

static void FUNC(put_luma_h)(int16_t *dst, const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    const pixel *src           = (const pixel*)_src;
    const ptrdiff_t src_stride = _src_stride / sizeof(pixel);
    const int8_t *restrict filter       = hf;

    COS_QPEL_SPEC(filter,
    for (int y = 0; y < height; y++) {
        QPEL_HROW(src, width, dst[x] = v >> (BIT_DEPTH - 8));
        src += src_stride;
        dst += MAX_PB_SIZE;
    });
}

static void FUNC(put_luma_v)(int16_t *dst, const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    const pixel *src           = (pixel*)_src;
    const ptrdiff_t src_stride = _src_stride / sizeof(pixel);
    const int8_t *restrict filter       = vf;

    COS_QPEL_SPEC(filter,
    QPEL_VCOL(src, src_stride, height, width, dst[y * MAX_PB_SIZE + x] = v >> (BIT_DEPTH - 8)););
}

static void FUNC(put_luma_hv)(int16_t *dst, const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    int16_t tmp_array[(MAX_PB_SIZE + LUMA_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp                = tmp_array;
    const pixel *src            = (const pixel*)_src;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const int8_t *restrict filter        = hf;

    src   -= LUMA_EXTRA_BEFORE * src_stride;
    COS_QPEL_SPEC(filter,
    for (int y = 0; y < height + LUMA_EXTRA; y++) {
        QPEL_HROW(src, width, tmp[x] = v >> (BIT_DEPTH - 8));
        src += src_stride;
        tmp += MAX_PB_SIZE;
    });

    tmp    = tmp_array + LUMA_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = vf;
    COS_QPEL_SPEC(filter,
    QPEL_VCOL(tmp, MAX_PB_SIZE, height, width, dst[y * MAX_PB_SIZE + x] = v >> 6););
}

static void FUNC(put_uni_luma_h)(uint8_t *_dst,  const ptrdiff_t _dst_stride,
    const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    const pixel *src           = (const pixel*)_src;
    pixel *dst                 = (pixel *)_dst;
    const ptrdiff_t src_stride = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride = _dst_stride / sizeof(pixel);
    const int8_t *restrict filter       = hf;
    const int shift            = 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    const int offset           = 1 << (shift - 1);
#else
    const int offset           = 0;
#endif

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int val = LUMA_FILTER(src, 1) >> (BIT_DEPTH - 8);
            dst[x]        = av_clip_pixel((val + offset) >> shift);
        }
        src   += src_stride;
        dst   += dst_stride;
    }
}

static void FUNC(put_uni_luma_v)(uint8_t *_dst,  const ptrdiff_t _dst_stride,
    const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{

    const pixel *src            = (const pixel*)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const int8_t *restrict filter        = vf;
    const int shift             = 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    const int offset            = 1 << (shift - 1);
#else
    const int offset            = 0;
#endif

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int val = LUMA_FILTER(src, src_stride) >> (BIT_DEPTH - 8);
            dst[x]        = av_clip_pixel((val + offset) >> shift);
        }
        src   += src_stride;
        dst   += dst_stride;
    }
}

static void FUNC(put_uni_luma_hv)(uint8_t *_dst, const ptrdiff_t _dst_stride,
    const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    int16_t tmp_array[(MAX_PB_SIZE + LUMA_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp                = tmp_array;
    const pixel *src            = (const pixel*)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const int8_t *restrict filter        = hf;
    const int shift             =  14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    const int offset            = 1 << (shift - 1);
#else
    const int offset            = 0;
#endif

    src   -= LUMA_EXTRA_BEFORE * src_stride;
    for (int y = 0; y < height + LUMA_EXTRA; y++) {
        for (int x = 0; x < width; x++)
            tmp[x] = LUMA_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += src_stride;
        tmp += MAX_PB_SIZE;
    }

    tmp    = tmp_array + LUMA_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = vf;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int val = LUMA_FILTER(tmp, MAX_PB_SIZE) >> 6;
            dst[x]  = av_clip_pixel((val  + offset) >> shift);
        }
        tmp += MAX_PB_SIZE;
        dst += dst_stride;
    }

}

static void FUNC(put_uni_luma_w_h)(uint8_t *_dst,  const ptrdiff_t _dst_stride,
    const uint8_t *_src, const ptrdiff_t _src_stride, int height,
    const int denom, const int wx, const int _ox, const int8_t *restrict hf, const int8_t *restrict vf,
    const int width)
{
    const pixel *src            = (const pixel*)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const int8_t *restrict filter        = hf;
    const int ox                = _ox * (1 << (BIT_DEPTH - 8));
    const int shift             = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    const int offset            = 1 << (shift - 1);
#else
    const int offset            = 0;
#endif

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dst[x] = av_clip_pixel((((LUMA_FILTER(src, 1) >> (BIT_DEPTH - 8)) * wx + offset) >> shift) + ox);
        src += src_stride;
        dst += dst_stride;
    }
}

static void FUNC(put_uni_luma_w_v)(uint8_t *_dst,  const ptrdiff_t _dst_stride,
    const uint8_t *_src, const ptrdiff_t _src_stride, const int height,
    const int denom, const int wx, const int _ox, const int8_t *restrict hf, const int8_t *restrict vf,
    const int width)
{
    const pixel *src            = (const pixel*)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const int8_t *restrict filter        = vf;
    const int ox                = _ox * (1 << (BIT_DEPTH - 8));
    const int shift             = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    const int offset            = 1 << (shift - 1);
#else
    const int offset            = 0;
#endif

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dst[x] = av_clip_pixel((((LUMA_FILTER(src, src_stride) >> (BIT_DEPTH - 8)) * wx + offset) >> shift) + ox);
        src += src_stride;
        dst += dst_stride;
    }
}

static void FUNC(put_uni_luma_w_hv)(uint8_t *_dst,  const ptrdiff_t _dst_stride,
    const uint8_t *_src, const ptrdiff_t _src_stride, const int height, const int denom,
    const int wx, const int _ox, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    int16_t tmp_array[(MAX_PB_SIZE + LUMA_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp                = tmp_array;
    const pixel *src            = (const pixel*)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const int8_t *restrict filter        = hf;
    const int ox                = _ox * (1 << (BIT_DEPTH - 8));
    const int shift             = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    const int offset            = 1 << (shift - 1);
#else
    const int offset            = 0;
#endif

    src   -= LUMA_EXTRA_BEFORE * src_stride;
    for (int y = 0; y < height + LUMA_EXTRA; y++) {
        for (int x = 0; x < width; x++)
            tmp[x] = LUMA_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += src_stride;
        tmp += MAX_PB_SIZE;
    }

    tmp    = tmp_array + LUMA_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = vf;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dst[x] = av_clip_pixel((((LUMA_FILTER(tmp, MAX_PB_SIZE) >> 6) * wx + offset) >> shift) + ox);
        tmp += MAX_PB_SIZE;
        dst += dst_stride;
    }
}


/* Chroma (4-tap) counterparts of the sliding-window helpers: taps are hoisted into locals once
 * per pass and one new sample is loaded per output sample. */
#ifndef COS_EPEL_SPEC
#define COS_EPEL_SPEC(filter, ...) do { \
    const int c0 = (filter)[0], c1 = (filter)[1], c2 = (filter)[2], c3 = (filter)[3]; \
    __VA_ARGS__ \
} while (0)
#define EPEL_HROW(sp, W, STORE) do { \
    const __typeof__(*(sp)) *p_ = (sp); \
    int a0 = p_[-1], a1 = p_[0], a2 = p_[1]; \
    for (int x = 0; x < (W); x++) { \
        int a3 = p_[x + 2]; \
        int v = c0*a0 + c1*a1 + c2*a2 + c3*a3; \
        STORE; \
        a0 = a1; a1 = a2; a2 = a3; \
    } \
} while (0)
#define EPEL_VCOL(base, stride, H, W, STORE) do { \
    for (int x = 0; x < (W); x++) { \
        const __typeof__(*(base)) *p_ = (base) + x; \
        int a0 = p_[-(stride)], a1 = p_[0], a2 = p_[(stride)]; \
        for (int y = 0; y < (H); y++) { \
            int a3 = p_[(y + 2) * (stride)]; \
            int v = c0*a0 + c1*a1 + c2*a2 + c3*a3; \
            STORE; \
            a0 = a1; a1 = a2; a2 = a3; \
        } \
    } \
} while (0)
#endif

#define CHROMA_FILTER(src, stride)                                             \
    (filter[0] * src[x - stride] +                                             \
     filter[1] * src[x]          +                                             \
     filter[2] * src[x + stride] +                                             \
     filter[3] * src[x + 2 * stride])

static void FUNC(put_chroma_h)(int16_t *dst, const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    const pixel *src            = (const pixel *)_src;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const int8_t *restrict filter        = hf;

    COS_EPEL_SPEC(filter,
    for (int y = 0; y < height; y++) {
        EPEL_HROW(src, width, dst[x] = v >> (BIT_DEPTH - 8));
        src += src_stride;
        dst += MAX_PB_SIZE;
    });
}

static void FUNC(put_chroma_v)(int16_t *dst, const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    const pixel *src            = (const pixel *)_src;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const int8_t *restrict filter        = vf;

    COS_EPEL_SPEC(filter,
    EPEL_VCOL(src, src_stride, height, width, dst[y * MAX_PB_SIZE + x] = v >> (BIT_DEPTH - 8)););
}

static void FUNC(put_chroma_hv)(int16_t *dst, const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    int16_t tmp_array[(MAX_PB_SIZE + CHROMA_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp                = tmp_array;
    const pixel *src            = (const pixel *)_src;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const int8_t *restrict filter        = hf;

    src -= CHROMA_EXTRA_BEFORE * src_stride;

    COS_EPEL_SPEC(filter,
    for (int y = 0; y < height + CHROMA_EXTRA; y++) {
        EPEL_HROW(src, width, tmp[x] = v >> (BIT_DEPTH - 8));
        src += src_stride;
        tmp += MAX_PB_SIZE;
    });

    tmp    = tmp_array + CHROMA_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = vf;

    COS_EPEL_SPEC(filter,
    EPEL_VCOL(tmp, MAX_PB_SIZE, height, width, dst[y * MAX_PB_SIZE + x] = v >> 6););
}

static void FUNC(put_uni_chroma_h)(uint8_t *_dst, const ptrdiff_t _dst_stride,
    const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    const pixel *src            = (const pixel *)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const int8_t *restrict filter        = hf;
    const int shift             = 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    const int offset            = 1 << (shift - 1);
#else
    const int offset            = 0;
#endif

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((CHROMA_FILTER(src, 1) >> (BIT_DEPTH - 8)) + offset) >> shift);
        src += src_stride;
        dst += dst_stride;
    }
}

static void FUNC(put_uni_chroma_v)(uint8_t *_dst, const ptrdiff_t _dst_stride,
    const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    const pixel *src            = (const pixel *)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const int8_t *restrict filter        = vf;
    const int shift             = 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    const int offset            = 1 << (shift - 1);
#else
    const int offset            = 0;
#endif

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((CHROMA_FILTER(src, src_stride) >> (BIT_DEPTH - 8)) + offset) >> shift);
        src += src_stride;
        dst += dst_stride;
    }
}

static void FUNC(put_uni_chroma_hv)(uint8_t *_dst, const ptrdiff_t _dst_stride,
    const uint8_t *_src, const ptrdiff_t _src_stride,
    const int height, const int8_t *restrict hf, const int8_t *restrict vf, const int width)
{
    int16_t tmp_array[(MAX_PB_SIZE + CHROMA_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp                = tmp_array;
    const pixel *src            = (const pixel *)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const int8_t *restrict filter        = hf;
    const int shift             = 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    const int offset            = 1 << (shift - 1);
#else
    const int offset            = 0;
#endif

    src -= CHROMA_EXTRA_BEFORE * src_stride;

    for (int y = 0; y < height + CHROMA_EXTRA; y++) {
        for (int x = 0; x < width; x++)
            tmp[x] = CHROMA_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += src_stride;
        tmp += MAX_PB_SIZE;
    }

    tmp    = tmp_array + CHROMA_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = vf;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((CHROMA_FILTER(tmp, MAX_PB_SIZE) >> 6) + offset) >> shift);
        tmp += MAX_PB_SIZE;
        dst += dst_stride;
    }
}

static void FUNC(put_uni_chroma_w_h)(uint8_t *_dst, ptrdiff_t _dst_stride,
    const uint8_t *_src, ptrdiff_t _src_stride, int height, int denom, int wx, int ox,
    const int8_t *restrict hf, const int8_t *restrict vf, int width)
{
    const pixel *src            = (const pixel *)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const int8_t *restrict filter        = hf;
    const int shift             = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    const int offset            = 1 << (shift - 1);
#else
    const int offset            = 0;
#endif

    ox     = ox * (1 << (BIT_DEPTH - 8));
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            dst[x] = av_clip_pixel((((CHROMA_FILTER(src, 1) >> (BIT_DEPTH - 8)) * wx + offset) >> shift) + ox);
        }
        dst += dst_stride;
        src += src_stride;
    }
}

static void FUNC(put_uni_chroma_w_v)(uint8_t *_dst, const ptrdiff_t _dst_stride,
    const uint8_t *_src, const ptrdiff_t _src_stride, const int height,
    const int denom, const int wx, const int _ox, const int8_t *restrict hf, const int8_t *restrict vf,
    const int width)
{
    const pixel *src            = (const pixel *)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const int8_t *restrict filter        = vf;
    const int shift             = denom + 14 - BIT_DEPTH;
    const int ox                = _ox * (1 << (BIT_DEPTH - 8));
#if BIT_DEPTH < 14
    int offset                  = 1 << (shift - 1);
#else
    int offset                  = 0;
#endif

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            dst[x] = av_clip_pixel((((CHROMA_FILTER(src, src_stride) >> (BIT_DEPTH - 8)) * wx + offset) >> shift) + ox);
        }
        dst += dst_stride;
        src += src_stride;
    }
}

static void FUNC(put_uni_chroma_w_hv)(uint8_t *_dst, ptrdiff_t _dst_stride,
     const uint8_t *_src, ptrdiff_t _src_stride,  int height, int denom, int wx, int ox,
     const int8_t *restrict hf, const int8_t *restrict vf, int width)
{
    int16_t tmp_array[(MAX_PB_SIZE + CHROMA_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp                = tmp_array;
    const pixel *src            = (const pixel *)_src;
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const int8_t *restrict filter        = hf;
    const int shift             = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    const int offset            = 1 << (shift - 1);
#else
    const int offset            = 0;
#endif

    src -= CHROMA_EXTRA_BEFORE * src_stride;

    for (int y = 0; y < height + CHROMA_EXTRA; y++) {
        for (int x = 0; x < width; x++)
            tmp[x] = CHROMA_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += src_stride;
        tmp += MAX_PB_SIZE;
    }

    tmp    = tmp_array + CHROMA_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = vf;

    ox     = ox * (1 << (BIT_DEPTH - 8));
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dst[x] = av_clip_pixel((((CHROMA_FILTER(tmp, MAX_PB_SIZE) >> 6) * wx + offset) >> shift) + ox);
        tmp += MAX_PB_SIZE;
        dst += dst_stride;
    }
}
