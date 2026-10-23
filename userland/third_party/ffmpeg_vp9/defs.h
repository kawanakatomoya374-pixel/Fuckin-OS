/* Stand-in for libavcodec/defs.h - just the picture-type enum values
 * vp8.c/webp.c tag decoded frames with (informational only; nothing in
 * this port's display path reads pict_type back). */
#ifndef COS_FFMPEG_WEBP_DEFS_H
#define COS_FFMPEG_WEBP_DEFS_H

/* Real FFmpeg defines this one in defs.h too (avcodec.h just includes
 * it) - moved here, rather than duplicated, so get_bits.h (which
 * #includes "defs.h" directly, not avcodec.h) sees it regardless of
 * which header a .c file happens to include first. */
#define AV_INPUT_BUFFER_PADDING_SIZE 64

enum AVPictureType {
    AV_PICTURE_TYPE_NONE = 0,
    AV_PICTURE_TYPE_I,
    AV_PICTURE_TYPE_P,
    AV_PICTURE_TYPE_B,
};


#define AV_PROFILE_UNKNOWN -99
#define AV_PROFILE_HEVC_MAIN 1
#define AV_PROFILE_HEVC_MAIN_10 2
#define AV_PROFILE_HEVC_MAIN_STILL_PICTURE 3
#define AV_PROFILE_HEVC_REXT 4
#define AV_PROFILE_HEVC_SCC 9
#endif
