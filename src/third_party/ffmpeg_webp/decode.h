/* Stand-in for libavcodec/decode.h. In real FFmpeg this and
 * codec_internal.h declare different (adjacent) parts of the decode
 * path; this port put every decode-helper declaration vp8.c/webp.c
 * actually use in codec_internal.h instead of splitting them across
 * two files for no benefit here - this header just makes sure whichever
 * of the two gets #included first, the declarations are visible. */
#ifndef COS_FFMPEG_WEBP_DECODE_H
#define COS_FFMPEG_WEBP_DECODE_H
#include "codec_internal.h"
#endif
