#include "brotli_decoder.h"

#include "memory.h"
#include "serial.h"
#include <brotli/decode.h>

static void *cos_brotli_alloc(void *opaque, size_t size)
{
    (void)opaque;
    return kmalloc(size);
}

static void cos_brotli_free(void *opaque, void *ptr)
{
    (void)opaque;
    if (ptr != NULL) kfree(ptr);
}

int cos_brotli_decode_http_body(const uint8_t *input, size_t input_len,
                                uint8_t *output, size_t output_cap,
                                size_t *output_len)
{
    if (output_len != NULL) *output_len = 0;
    if (input == NULL || output == NULL || output_len == NULL || output_cap == 0) {
        return -1;
    }

    BrotliDecoderState *state = BrotliDecoderCreateInstance(
        cos_brotli_alloc, cos_brotli_free, NULL);
    if (state == NULL) {
        serial_puts("[HTTP/br] unable to allocate decoder state\n");
        return -1;
    }

    const uint8_t *next_in = input;
    size_t available_in = input_len;
    uint8_t *next_out = output;
    size_t available_out = output_cap;
    size_t total_out = 0;
    int result = -1;

    for (;;) {
        BrotliDecoderResult rc = BrotliDecoderDecompressStream(
            state, &available_in, &next_in, &available_out, &next_out, &total_out);
        if (rc == BROTLI_DECODER_RESULT_SUCCESS) {
            *output_len = total_out;
            result = 0;
            break;
        }
        if (rc == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            /* Preserve the successfully decoded prefix rather than treating a
             * large page as a malformed response. This mirrors the existing
             * gzip path, and the caller emits an explicit clipped diagnostic. */
            *output_len = total_out;
            result = 1;
            break;
        }
        if (rc == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
            serial_puts("[HTTP/br] truncated Brotli response\n");
            result = -1;
            break;
        }
        serial_puts("[HTTP/br] decoder rejected response code=");
        serial_putdec((uint64_t)(int64_t)BrotliDecoderGetErrorCode(state));
        serial_puts("\n");
        result = -1;
        break;
    }

    BrotliDecoderDestroyInstance(state);
    return result;
}
