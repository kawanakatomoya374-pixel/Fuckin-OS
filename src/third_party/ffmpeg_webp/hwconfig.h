/* Stand-in for libavcodec/hwconfig.h - the macros real FFmpeg codecs use
 * to declare a static table of supported hwaccels. This port declares
 * none (CONFIG_VP8_*_HWACCEL are all 0 in config.h), so these expand to
 * nothing anywhere they're used. */
#ifndef COS_FFMPEG_WEBP_HWCONFIG_H
#define COS_FFMPEG_WEBP_HWCONFIG_H

#define HWACCEL_MAX 0

#endif
