#ifndef SCREENSHOT_ENC_H
#define SCREENSHOT_ENC_H
#include <stdint.h>
#include <stddef.h>
/* px: w*h pixels, 0x00RRGGBB. The result is malloc'ed (free it); NULL on failure. */
uint8_t *ss_encode_png(const uint32_t *px, int w, int h, size_t *n);
uint8_t *ss_encode_jpeg(const uint32_t *px, int w, int h, int quality, size_t *n);
uint8_t *ss_encode_webp(const uint32_t *px, int w, int h, size_t *n);
#endif
