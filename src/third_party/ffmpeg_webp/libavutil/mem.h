/* Stand-in for libavutil/mem.h. Real FFmpeg's av_malloc() family adds
 * SIMD alignment and a couple of small hardening checks on top of the
 * platform allocator; this port has no SIMD paths (see config.h - every
 * ARCH_* is 0) so it wraps this kernel's own malloc/free (declared in
 * <stdlib.h>, backed by the kernel heap - see src/kernel/memory.c)
 * directly rather than re-implementing an aligned allocator nothing here
 * needs. Implementations are in cos_ffmpeg_shim.c. */
#ifndef COS_FFMPEG_WEBP_MEM_H
#define COS_FFMPEG_WEBP_MEM_H

#include <stddef.h>
#include "mem_internal.h"

void* av_malloc(size_t size);
void* av_mallocz(size_t size);
void* av_malloc_array(size_t nmemb, size_t size);
void* av_calloc(size_t nmemb, size_t size);
void* av_realloc(void* ptr, size_t size);
/* av_realloc_f(ptr, nmemb, size): like av_realloc(ptr, nmemb*size), but
 * frees ptr and returns NULL on overflow/failure instead of leaving the
 * original allocation dangling with an ambiguous return - vlc.c relies
 * on that "old block is gone either way" contract when growing its
 * table. */
void* av_realloc_f(void* ptr, size_t nmemb, size_t size);
void  av_free(void* ptr);
void  av_freep(void* ptr);

#endif
