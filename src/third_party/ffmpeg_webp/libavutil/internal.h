/* Stand-in for libavutil/internal.h. Real FFmpeg's is a large grab-bag of
 * build-internal macros; everything the ported files in this directory
 * actually reference from "internal.h" turned out to already come from
 * one of attributes.h/avassert.h/macros.h (which they also include
 * directly) - this stays empty on purpose rather than growing
 * speculative content nothing here reads. */
#ifndef COS_FFMPEG_WEBP_INTERNAL_H
#define COS_FFMPEG_WEBP_INTERNAL_H
#endif
