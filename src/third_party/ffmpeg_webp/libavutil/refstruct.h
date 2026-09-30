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

#endif
