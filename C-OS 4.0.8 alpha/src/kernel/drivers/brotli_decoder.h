#ifndef COS_BROTLI_DECODER_H
#define COS_BROTLI_DECODER_H

#include "types.h"

/* Decode a complete HTTP Content-Encoding: br body into caller-owned memory.
 * The output buffer is fixed by the HTTP response cap. The decoder never
 * allocates from a shared workspace; its state and ring buffer are allocated
 * through the C-OS heap per request. Returns 0 on a complete decode, 1 when
 * valid output reaches the supplied cap, and -1 on malformed input or OOM. */
int cos_brotli_decode_http_body(const uint8_t *input, size_t input_len,
                                uint8_t *output, size_t output_cap,
                                size_t *output_len);

#endif /* COS_BROTLI_DECODER_H */
