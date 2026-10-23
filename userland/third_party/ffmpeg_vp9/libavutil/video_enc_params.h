/* Stand-in for libavutil/video_enc_params.h. This port never sets
 * AVCodecContext.export_side_data (it stays 0, see avcodec.h), so every
 * call site guarded by "avctx->export_side_data & ..." in vp9.c is dead
 * code - these just need matching declarations/types to compile, plus a
 * harmless stub implementation (returns NULL => the dead code's own
 * ENOMEM-and-skip path, never actually reached) in cos_ffmpeg_shim.c. */
#ifndef COS_FFMPEG_VP9_VIDEO_ENC_PARAMS_H
#define COS_FFMPEG_VP9_VIDEO_ENC_PARAMS_H

#include <stdint.h>
#include "avcodec.h"

#define AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS (1 << 0)
#define AV_VIDEO_ENC_PARAMS_VP9 1

typedef struct AVVideoEncParams {
    int32_t qp;
    int32_t delta_qp[4][2];
} AVVideoEncParams;

typedef struct AVVideoBlockParams {
    int src_x, src_y, w, h;
    int32_t delta_qp;
} AVVideoBlockParams;

AVVideoEncParams *av_video_enc_params_create_side_data(AVFrame *frame, int type, unsigned int nb_blocks);
AVVideoBlockParams *av_video_enc_params_block(AVVideoEncParams *par, unsigned int idx);

#endif
