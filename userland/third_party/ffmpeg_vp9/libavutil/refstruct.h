/* Stand-in for libavutil/refstruct.h. Real FFmpeg's version supports a
 * custom free callback and opaque per-object flags; vp8.c only ever
 * calls the three functions declared below (allocz/replace/unref) on a
 * plain byte buffer (the segmentation map), so this port implements
 * just that: a small fixed header placed immediately before the
 * returned pointer holding a refcount, incremented/decremented with
 * plain (non-atomic) arithmetic since this is a single-threaded decode
 * (see config.h - HAVE_THREADS is 0, so there is never a second thread
 * that could race these). Implementation in cos_ffmpeg_shim.c. */
#ifndef COS_FFMPEG_WEBP_REFSTRUCT_H
#define COS_FFMPEG_WEBP_REFSTRUCT_H

#include <stddef.h>

void* av_refstruct_allocz(size_t size);
void  av_refstruct_unref(void* objp);
void  av_refstruct_replace(void* dstp, const void* src);

/* ff_refstruct_* - vp9.c's (newer, internal-only) names for the exact
 * same three entry points above, plus a small fixed-size POOL on top:
 * ff_refstruct_pool_alloc() creates a pool of same-sized refcounted
 * buffers (used for VP9's per-superblock-row motion-vector reference
 * data, reallocated every frame in real FFmpeg's frame-threaded decode
 * to avoid a malloc storm); ff_refstruct_pool_get() hands out a buffer
 * (reusing one on the pool's free list when available); its normal
 * ff_refstruct_unref() return, instead of actually freeing the memory,
 * puts it back on the free list - that's the one real behavioural
 * difference from a plain av_refstruct_allocz() object, implemented in
 * cos_ffmpeg_shim.c via a free-callback stored in the hidden header. */
typedef struct FFRefStructPool FFRefStructPool;
#define FF_REFSTRUCT_POOL_FLAG_ZERO_EVERY_TIME (1 << 0)
FFRefStructPool* ff_refstruct_pool_alloc(size_t size, unsigned flags);
void   ff_refstruct_pool_uninit(FFRefStructPool** poolp);
void*  ff_refstruct_pool_get(FFRefStructPool* pool);
void   ff_refstruct_unref(void* objp);
void   ff_refstruct_replace(void* dstp, const void* src);

/* ---- additions for the HEVC port ---- */
typedef union { void* nc; const void* c; } FFRefStructOpaque;
#define FF_REFSTRUCT_FLAG_NO_ZEROING (1 << 0)
void* ff_refstruct_alloc_ext_c(size_t size, unsigned flags, FFRefStructOpaque opaque,
                               void (*free_cb)(FFRefStructOpaque opaque, void* obj));
#define ff_refstruct_alloc_ext(size, flags, opaque, free_cb) \
    ff_refstruct_alloc_ext_c(size, flags, (FFRefStructOpaque){ .nc = (opaque) }, free_cb)
#define ff_refstruct_allocz(size) ff_refstruct_alloc_ext(size, 0, NULL, NULL)
void* ff_refstruct_ref(void* obj);
const void* ff_refstruct_ref_c(const void* obj);

#endif
