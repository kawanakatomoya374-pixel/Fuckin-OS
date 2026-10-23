/* Stand-in for libavcodec/hwconfig.h - the macros real FFmpeg codecs use
 * to declare a static table of supported hwaccels. This port declares
 * none (CONFIG_VP8_*_HWACCEL are all 0 in config.h), so these expand to
 * nothing anywhere they're used. */
#ifndef COS_FFMPEG_WEBP_HWCONFIG_H
#define COS_FFMPEG_WEBP_HWCONFIG_H

/* vp9.c (unlike vp8.c) computes its own HWACCEL_MAX from the
 * CONFIG_VP9_*_HWACCEL macros in config_components.h (all 0), so this
 * file does not predefine one for it - see config_components.h. */
#ifndef HWACCEL_MAX
#define HWACCEL_MAX 0
#endif

#endif
