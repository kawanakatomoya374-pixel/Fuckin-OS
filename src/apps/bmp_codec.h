/*
 * bmp_codec.h - full BMP (DIB) decoder
 *
 * Supported
 *   - Headers: BITMAPCOREHEADER (12, OS/2 1.x, 3-byte palette), OS/2 2.x (16/64
 *     bytes), BITMAPINFOHEADER (40), V2 (52), V3 (56), V4 (108), V5 (124).
 *   - 1/2/4/8-bit palettised (palette size from biClrUsed, clamped to what fits
 *     before the pixel data; indices past the palette render opaque black),
 *     16-bit (555 default, any BI_BITFIELDS masks e.g. 565/4444), 24-bit,
 *     32-bit (BI_RGB, BI_BITFIELDS, BI_ALPHABITFIELDS, V4/V5 masks).
 *   - BI_RLE8 / BI_RLE4 with encoded/absolute runs, EOL, EOB and delta
 *     escapes; pixels never written by the stream are transparent black.
 *   - BI_JPEG / BI_PNG: the embedded payload is decoded by the JPEG / PNG
 *     decoders.
 *   - Bottom-up and top-down (negative height) images, 4-byte row padding.
 *   - 32-bit BI_RGB images: the 4th byte is alpha only if at least one pixel
 *     has a non-zero value there (otherwise the image is opaque), the rule
 *     browsers use.  Masks-based formats use the alpha mask.
 *   - bfSize is ignored (it is wrong in many files).  The pixel data offset is
 *     validated; pixel data that does not fit in the file is an error
 *     (RLE streams that end early are fine).  Everything is bounds checked,
 *     width/height are limited to 65535 and RLE images to 32 Mpixel.
 *   - Channels narrower than 8 bits are scaled with v*255/max (truncating,
 *     the same as Pillow's unpackers).
 *
 * Output: densely packed BGRA bytes, stride = reported width * 4.
 */
#ifndef BMP_CODEC_H
#define BMP_CODEC_H

#include <stdint.h>
#include <stdbool.h>

/* Quick signature + header-size check (does not validate the rest). */
bool cos_bmp_is_bmp(const uint8_t* data, uint64_t size);

/* Reads dimensions only. */
bool cos_bmp_probe(const uint8_t* data, uint64_t size, uint32_t* width, uint32_t* height);

/* Full decode; images larger than max_w x max_h are CROPPED to their top-left
 * part (out_w/out_h = cropped size).  out_bgra needs max_w*max_h*4 bytes. */
bool cos_bmp_decode(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                uint64_t max_w, uint64_t max_h, uint64_t* out_w, uint64_t* out_h);

/* Same, but larger images are area-averaged down to fit (aspect preserved).
 * src_w/src_h (optional) receive the original size. */
bool cos_bmp_decode_fit(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                    uint64_t max_w, uint64_t max_h, uint64_t* out_w, uint64_t* out_h,
                    uint64_t* src_w, uint64_t* src_h);

#endif /* BMP_CODEC_H */
