/**
 * png_decoder.h - full PNG decoder + zlib/gzip inflate (RFC 1950/1951/1952)
 *
 * PNG support
 *   - All colour types (0 gray, 2 RGB, 3 palette, 4 gray+alpha, 6 RGBA) with
 *     every legal bit depth (1/2/4/8/16); illegal colour-type/bit-depth
 *     combinations are rejected.
 *   - Adam7 interlacing (decoded straight into the output, no full-size
 *     scratch image unless the result must also be downscaled).
 *   - tRNS: palette alpha, and the colour-key of gray/RGB images (all bit
 *     depths, 16-bit keys compared on the full 16-bit sample).
 *   - 16-bit samples are reduced to 8 bit with rounding, v8 = round(v*255/65535)
 *     (libpng's png_set_scale_16), not by dropping the low byte.
 *   - Palette indices >= the PLTE size render as opaque black (what libpng and
 *     Pillow show) - they are not remapped to entry 0.
 *   - CRC-32 of IHDR/PLTE/IDAT is verified (a bad critical chunk rejects the
 *     file; a bad ancillary chunk such as tRNS is ignored) and so is the zlib
 *     Adler-32.  Data after the final IDAT sample is tolerated (not verified).
 *   - APNG: the default image (the IDAT chunks) is decoded; acTL/fcTL/fdAT are
 *     ignored.  gAMA/sRGB/iCCP/cHRM etc. are ignored (no colour management).
 *   - Streaming: the inflater has a 32 KiB window, rows are un-filtered one at
 *     a time, so memory is O(row) except for Adam7+downscale (full BGRA copy).
 *   - Limits: width/height <= 65535; decompression-bomb guard (a header that
 *     claims more pixel data than DEFLATE can possibly produce from the bytes
 *     present is rejected before anything is allocated).
 *
 * Output is densely packed BGRA bytes (B,G,R,A in memory order), straight
 * (non-premultiplied) alpha, stride = reported width * 4.
 */
#ifndef PNG_DECODER_H
#define PNG_DECODER_H

#include "types.h"

/* Decodes `data` (a full PNG file, size bytes) into out_bgra, which
 * must be at least max_out_w * max_out_h * 4 bytes. Writes densely
 * packed BGRA (stride = actual decoded width, matching decode_bmp()'s
 * convention in jpeg_viewer.c) into out_bgra and reports the actual
 * (possibly CROPPED-to-max: the top-left max_out_w x max_out_h region is
 * returned, the rest is dropped) dimensions via out_w/out_h.
 * Returns true on success. On failure, out_w/out_h are left untouched
 * and out_bgra's contents are undefined - the caller should not treat
 * the buffer as valid. */
bool png_decode(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                 uint64_t max_out_w, uint64_t max_out_h,
                 uint64_t* out_w, uint64_t* out_h);

/* Same, but an image larger than max_out_w x max_out_h is DOWNSCALED
 * (exact area average, aspect ratio preserved, alpha-weighted) instead of
 * cropped, so out_w <= max_out_w and out_h <= max_out_h describe the whole
 * picture.  src_w/src_h (either may be NULL) receive the original size. */
bool cos_png_decode_fit(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                    uint64_t max_out_w, uint64_t max_out_h,
                    uint64_t* out_w, uint64_t* out_h,
                    uint64_t* src_w, uint64_t* src_h);

/* Reads only the signature + IHDR (dimensions); no CRC/chunk walk. */
bool cos_png_probe(const uint8_t* data, uint64_t size, uint32_t* width, uint32_t* height);

/* Decompresses a gzip (RFC 1952) stream - the Content-Encoding: gzip most
 * real HTTP servers use - into out, which must be out_cap bytes. Unlike
 * png_decode(), the true decompressed size is not known upfront here, so
 * this stops (successfully) once out_cap is filled rather than requiring an
 * exact match, and reports how much it actually produced via *out_len.
 *
 * Robustness: the DEFLATE stream is fully validated (over-subscribed or
 * incomplete Huffman sets, bad stored-block length checks, distances before
 * the start, reserved block types are rejected).  When the stream ends before
 * out_cap is reached, the CRC-32 and ISIZE trailer are verified if present
 * (a mismatch fails; a trailer cut off by truncation is tolerated); further
 * concatenated gzip members are decompressed as well.  Shares the DEFLATE core
 * with png_decode(). */
bool gzip_inflate(const uint8_t* gzdata, uint64_t gzsize, uint8_t* out,
                  uint64_t out_cap, uint64_t* out_len);

#endif /* PNG_DECODER_H */
