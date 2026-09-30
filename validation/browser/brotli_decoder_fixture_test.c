#include <brotli/decode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char *data = (unsigned char *)malloc((size_t)n + 1u);
    if (data == NULL) { fclose(f); return NULL; }
    if (fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return data;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s INPUT.br EXPECTED\n", argv[0]);
        return 2;
    }
    size_t input_len = 0, expected_len = 0;
    unsigned char *input = read_file(argv[1], &input_len);
    unsigned char *expected = read_file(argv[2], &expected_len);
    unsigned char *output = (unsigned char *)malloc(expected_len + 1u);
    if (input == NULL || expected == NULL || output == NULL) return 3;
    size_t out_len = expected_len;
    BrotliDecoderResult rc = BrotliDecoderDecompress(input_len, input, &out_len, output);
    int ok = rc == BROTLI_DECODER_RESULT_SUCCESS && out_len == expected_len &&
             memcmp(output, expected, expected_len) == 0;
    printf("brotli fixture: rc=%d output=%zu expected=%zu result=%s\n",
           (int)rc, out_len, expected_len, ok ? "PASS" : "FAIL");
    free(output);
    free(expected);
    free(input);
    return ok ? 0 : 1;
}
