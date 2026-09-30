/* Stand-in for libavcodec/defs.h - just the picture-type enum values
 * vp8.c/webp.c tag decoded frames with (informational only; nothing in
 * this port's display path reads pict_type back). */
#ifndef COS_FFMPEG_WEBP_DEFS_H
#define COS_FFMPEG_WEBP_DEFS_H

enum AVPictureType {
    AV_PICTURE_TYPE_NONE = 0,
    AV_PICTURE_TYPE_I,
    AV_PICTURE_TYPE_P,
    AV_PICTURE_TYPE_B,
};

#endif
