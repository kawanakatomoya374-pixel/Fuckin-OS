/**
 * png_decoder.h - Real PNG decoding (RFC 1950/1951 zlib/DEFLATE + PNG chunks)
 *
 * Supports: color types 0 (grayscale), 2 (RGB), 3 (palette + tRNS),
 * 4 (grayscale+alpha), 6 (RGBA); bit depths 1/2/4/8/16; non-interlaced
 * only (Adam7 interlacing is reported as unsupported rather than
 * mis-decoded).
 *
 * Algorithm cross-checked against Pillow's decoder output before being
 * ported here: pixel-perfect (zero difference) on RGB/RGBA/palette/
 * grayscale test images, including a 200x150 random-noise image that
 * exercises dynamic Huffman blocks and long LZ77 back-references.
 */
#ifndef PNG_DECODER_H
#define PNG_DECODER_H

#include "types.h"

/* Decodes `data` (a full PNG file, size bytes) into out_bgra, which
 * must be at least max_out_w * max_out_h * 4 bytes. Writes densely
 * packed BGRA (stride = actual decoded width, matching decode_bmp()'s
 * convention in jpeg_viewer.c) into out_bgra and reports the actual
 * (possibly clamped-to-max) dimensions via out_w/out_h.
 * Returns true on success. On failure, out_w/out_h are left untouched
 * and out_bgra's contents are undefined - the caller should not treat
 * the buffer as valid. */
bool png_decode(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                 uint64_t max_out_w, uint64_t max_out_h,
                 uint64_t* out_w, uint64_t* out_h);

/* Decompresses a gzip (RFC 1952) stream - the Content-Encoding: gzip most
 * real HTTP servers use - into out, which must be out_cap bytes. Unlike
 * png_decode(), the true decompressed size is not known upfront here, so
 * this stops (successfully) once out_cap is filled rather than requiring an
 * exact match, and reports how much it actually produced via *out_len.
 * Shares the same DEFLATE (RFC 1951) bit reader and Huffman decoder this
 * file already has for PNG's IDAT stream (see png_decoder.c) - gzip and
 * zlib wrap the identical compressed bitstream in different envelopes, so
 * decompression itself needs no separate implementation, only gzip's
 * variable-length header/trailer instead of zlib's fixed one. */
bool gzip_inflate(const uint8_t* gzdata, uint64_t gzsize, uint8_t* out,
                  uint64_t out_cap, uint64_t* out_len);

#endif /* PNG_DECODER_H */
