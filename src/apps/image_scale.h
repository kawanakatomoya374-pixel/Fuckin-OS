/*
 * image_scale.h - exact area-average ("box") downscaler for BGRA images.
 *
 * Shared by the PNG, BMP and JPEG decoders' *_fit() entry points so a photo
 * larger than the viewer buffer is shown scaled instead of failing/cropped.
 *
 * The scaler is row-streaming: the decoder pushes source rows top to bottom
 * and each destination row is written as soon as the last source row that
 * contributes to it has been seen, so no full-size intermediate image is
 * needed.  Pixels are averaged weighted by alpha (i.e. in premultiplied
 * space) so transparent pixels do not bleed their colour into neighbours;
 * for opaque images this is a plain average, rounded to nearest.
 *
 * Only downscaling (dst <= src in both axes) is supported.
 */
#ifndef IMAGE_SCALE_H
#define IMAGE_SCALE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct cos_img_scaler cos_img_scaler_t;

/* Largest size with the source's aspect ratio that fits in max_w x max_h and
 * is never larger than the source (floor, minimum 1x1). */
void cos_img_fit_dims(uint64_t src_w, uint64_t src_h, uint64_t max_w, uint64_t max_h,
                  uint64_t* dst_w, uint64_t* dst_h);

/* out: densely packed BGRA, dst_w*dst_h*4 bytes, owned by the caller.
 * Returns NULL on bad arguments (zero size, dst > src) or allocation failure. */
cos_img_scaler_t* cos_img_scaler_create(uint32_t src_w, uint32_t src_h,
                                uint32_t dst_w, uint32_t dst_h, uint8_t* out);
/* Push the next source row (src_w BGRA pixels).  Rows beyond src_h are ignored. */
void cos_img_scaler_push_row(cos_img_scaler_t* s, const uint8_t* bgra_row);
/* True once every destination row has been written. */
bool cos_img_scaler_done(const cos_img_scaler_t* s);
void cos_img_scaler_destroy(cos_img_scaler_t* s);

#endif /* IMAGE_SCALE_H */
