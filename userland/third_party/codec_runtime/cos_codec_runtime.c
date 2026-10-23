#include "cos_codec_runtime.h"
#include <limits.h>

void *cos_codec_malloc(size_t size) { return cos_malloc(size); }
void *cos_codec_calloc(size_t count, size_t size) { return cos_calloc(count, size); }
void *cos_codec_realloc(void *ptr, size_t size) { return cos_realloc(ptr, size); }
void cos_codec_free(void *ptr) { cos_free(ptr); }
void *cos_codec_memcpy(void *dst, const void *src, size_t n) { return cos_memcpy(dst, src, n); }
void *cos_codec_memmove(void *dst, const void *src, size_t n) { return memmove(dst, src, n); }
void *cos_codec_memset(void *dst, int value, size_t n) { return cos_memset(dst, value, n); }
int cos_codec_memcmp(const void *a, const void *b, size_t n) { return cos_memcmp(a, b, n); }
void cos_codec_qsort(void *base, size_t count, size_t size,
                    int (*compare)(const void *, const void *)) {
    cos_qsort(base, count, size, compare);
}
void cos_codec_log(const char *message) {
    /* A compute job on another core cannot make system calls. */
    if (message && !cos_in_worker()) cos_write(message, cos_strlen(message));
}
void cos_codec_abort(const char *reason) {
    if (cos_in_worker()) __builtin_trap();   /* surfaces as a job fault with the faulting address */
    if (reason) cos_codec_log(reason);
    cos_codec_log("\n");
    cos_exit(134);
}
