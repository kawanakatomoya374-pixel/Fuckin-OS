/* Stand-in for libavcodec/decode.h. In real FFmpeg this and
 * codec_internal.h declare different (adjacent) parts of the decode
 * path; this port put every decode-helper declaration vp8.c/webp.c
 * actually use in codec_internal.h instead of splitting them across
 * two files for no benefit here - this header just makes sure whichever
 * of the two gets #included first, the declarations are visible. */
#ifndef COS_FFMPEG_WEBP_DECODE_H
#define COS_FFMPEG_WEBP_DECODE_H
#include "codec_internal.h"
#include "libavutil/mastering_display_metadata.h"
int ff_decode_mastering_display_new(const AVCodecContext* avctx, AVFrame* frame,
                                    AVMasteringDisplayMetadata** mdm);
int ff_decode_content_light_new(const AVCodecContext* avctx, AVFrame* frame,
                                AVContentLightMetadata** clm);
#endif
