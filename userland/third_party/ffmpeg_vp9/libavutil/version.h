/* Stand-in for libavutil/version.h - nothing in this port reads any
 * version macro, this file exists only so #include "version.h" (pulled
 * in transitively by common.h) resolves. */
#ifndef COS_FFMPEG_WEBP_VERSION_H
#define COS_FFMPEG_WEBP_VERSION_H
#define LIBAVUTIL_VERSION_INT ((59<<16)|(8<<8)|100)
#endif
