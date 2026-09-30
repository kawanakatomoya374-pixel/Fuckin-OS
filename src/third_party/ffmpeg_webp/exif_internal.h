/* Stand-in for libavcodec/exif_internal.h. webp.c includes this but
 * (verified by grep) never actually calls anything it would declare -
 * this port's EXIF handling is the inert ff_decode_exif_attach_buffer()
 * stub in codec_internal.h instead. Left empty rather than populated
 * with unused declarations. */
#ifndef COS_FFMPEG_WEBP_EXIF_INTERNAL_H
#define COS_FFMPEG_WEBP_EXIF_INTERNAL_H
#endif
