/* Stand-in for libavutil/imgutils.h - just the one bounds-check webp.c
 * uses to reject absurd width/height before allocating anything for
 * them. Implementation in cos_ffmpeg_shim.c. */
#ifndef COS_FFMPEG_WEBP_IMGUTILS_H
#define COS_FFMPEG_WEBP_IMGUTILS_H

struct AVCodecContext;
int av_image_check_size(unsigned w, unsigned h, int log_offset, void* log_ctx);

#endif
