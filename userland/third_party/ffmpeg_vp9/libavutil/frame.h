/* Stand-in for libavutil/frame.h. This port's avcodec.h already defines
 * AVFrame directly (see its own file comment), so there is nothing left
 * for this header to add - it exists only so "#include
 * libavutil/frame.h" resolves. */
#ifndef COS_FFMPEG_VP9_FRAME_H
#define COS_FFMPEG_VP9_FRAME_H
#include "avcodec.h"
#endif
