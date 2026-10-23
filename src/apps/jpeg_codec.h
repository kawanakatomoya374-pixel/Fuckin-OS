/*
 * jpeg_codec.h - full JPEG decoder (ITU-T T.81 / ISO 10918-1), Huffman coding
 *
 * Supported
 *   - SOF0 baseline, SOF1 extended sequential (8-bit and 12-bit precision;
 *     12-bit samples are reduced to 8 bit right after the IDCT), SOF2
 *     progressive (spectral selection, successive approximation, DC/AC first
 *     and refinement scans, EOB runs, interleaved and non-interleaved scans,
 *     any number of scans).  Multi-scan sequential files are handled too.
 *   - Any sampling factors 1..4 per axis (4:4:4, 4:2:2, 4:2:0, 4:4:0, 4:1:1,
 *     odd factors); non-interleaved scans use component block counts, not MCU
 *     padding; image sizes that are not a multiple of the MCU.
 *   - Grayscale, YCbCr, RGB (Adobe transform 0 or component ids 'R','G','B'),
 *     CMYK and YCCK (Adobe APP14 transform 0/2, inverted Adobe CMYK).  The
 *     colour space is chosen exactly like libjpeg (JFIF / Adobe / ids).
 *   - Restart intervals with libjpeg's resynchronisation rules (DC predictor
 *     and EOB-run reset, byte alignment, tolerance for missing / corrupted /
 *     out-of-order RSTn markers); DNL for height 0; 0xFF fill bytes; COM/APPn
 *     skipping; several DQT/DHT in one segment; 16-bit DQT; over-subscribed
 *     Huffman tables rejected; standard (Annex K) Huffman tables are used when
 *     a stream (e.g. an MJPEG frame) has no DHT at all.
 *   - Truncated / damaged entropy data: what could be decoded is returned and
 *     the rest is gray, like libjpeg (info->truncated is set).  An error is
 *     returned only if no scan could be started.
 *   - Bit-exact libjpeg "islow" integer IDCT (jidctint.c), libjpeg's reduced
 *     size IDCTs for 1/2, 1/4, 1/8 scaling (jidctred.c), libjpeg's "fancy"
 *     triangle chroma upsampling (h2v1, h2v2, h1v2) and its integer YCbCr->RGB
 *     tables, so results match libjpeg / libjpeg-turbo (Pillow) to +-1.
 *   - EXIF orientation (APP1) is parsed, reported in info->orientation and
 *     applied to the output unless JPEG_DEC_NO_ORIENT is given.
 *   - Fast table-driven Huffman decoding (9-bit lookup + canonical fallback).
 *
 * NOT supported (reported as JPEG_ERROR_UNSUPPORTED): arithmetic coding
 * (SOF9/SOF10 - no way to produce test streams, so not implemented), lossless
 * (SOF3), hierarchical/differential frames, precision other than 8/12 bit.
 *
 * Memory: one kmalloc block for the whole decode (component planes, coefficient
 * buffers for multi-scan files, line buffers) plus one small context block,
 * both freed before returning.  Limit: 256 MiB workspace; dimensions <= 65535.
 */
#ifndef JPEG_CODEC_H
#define JPEG_CODEC_H

#include <stdint.h>
#include <stdbool.h>

#ifndef JPEG_ERROR_OK
#define JPEG_ERROR_OK 0
#define JPEG_ERROR_INVALID_FORMAT -1
#define JPEG_ERROR_UNSUPPORTED -2
#define JPEG_ERROR_BUFFER_OVERFLOW -3
#endif

/* cos_jpeg_codec_decode() flags */
#define JPEG_DEC_FIT       0x01  /* shrink to fit max_w x max_h instead of failing */
#define JPEG_DEC_NO_ORIENT 0x02  /* do not apply the EXIF orientation */

enum {
    JPEG_CS_UNKNOWN = 0,
    JPEG_CS_GRAY,
    JPEG_CS_YCBCR,
    JPEG_CS_RGB,
    JPEG_CS_CMYK,
    JPEG_CS_YCCK
};

typedef struct {
    uint32_t src_width, src_height;  /* size stored in the file (before EXIF rotation) */
    uint32_t width, height;          /* size of the decoded output (after scaling / rotation) */
    uint8_t  ncomp;                  /* colour components in the file (1, 3, 4) */
    uint8_t  precision;              /* 8 or 12 */
    uint8_t  progressive;
    uint8_t  orientation;            /* EXIF orientation 1..8 found in the file (1 if none) */
    uint8_t  orientation_applied;    /* 1 if the output was rotated/flipped accordingly */
    uint8_t  scale_denom;            /* DCT-domain scale used: 1, 2, 4 or 8 */
    uint8_t  colorspace;             /* JPEG_CS_* */
    uint8_t  truncated;              /* 1 if data was missing/corrupt and part of the image is gray */
} jpeg_info_t;

/* Parses the headers only (no entropy decoding).  Returns JPEG_ERROR_OK and
 * fills src_width/src_height/ncomp/precision/progressive/orientation/colorspace;
 * width/height are set to the oriented source size. */
int cos_jpeg_codec_probe(const uint8_t* data, uint64_t size, jpeg_info_t* info);

/* Decodes into out_bgra (densely packed BGRA, stride = info->width * 4, must
 * hold max_w * max_h * 4 bytes).
 *   without JPEG_DEC_FIT: the full-size image or JPEG_ERROR_BUFFER_OVERFLOW if
 *     it (after rotation) is larger than max_w x max_h;
 *   with JPEG_DEC_FIT: DCT scaling by 1/2, 1/4, 1/8 (smallest divisor that
 *     fits), followed by an exact area-average reduction if even 1/8 is too big.
 * info may be NULL.  info->width/height are set even on BUFFER_OVERFLOW. */
int cos_jpeg_codec_decode(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                      uint64_t max_w, uint64_t max_h, unsigned flags,
                      jpeg_info_t* info);

#endif /* JPEG_CODEC_H */
