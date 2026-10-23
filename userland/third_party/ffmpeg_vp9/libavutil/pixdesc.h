/* Stand-in for libavutil/pixdesc.h: just enough of AVPixFmtDescriptor
 * (chroma subsampling shifts and component depth) for the HEVC port.
 * av_pix_fmt_desc_get() is implemented in cos_hevc_shim.c. */
#ifndef COS_FFMPEG_VP9_PIXDESC_H
#define COS_FFMPEG_VP9_PIXDESC_H
#include <stdint.h>
#include "pixfmt.h"

typedef struct AVComponentDescriptor { int plane, step, offset, shift, depth; } AVComponentDescriptor;

#define AV_PIX_FMT_FLAG_PLANAR (1 << 4)
#define AV_PIX_FMT_FLAG_RGB    (1 << 5)

typedef struct AVPixFmtDescriptor {
    const char* name;
    uint8_t nb_components;
    uint8_t log2_chroma_w;
    uint8_t log2_chroma_h;
    uint64_t flags;
    AVComponentDescriptor comp[4];
} AVPixFmtDescriptor;

const AVPixFmtDescriptor* av_pix_fmt_desc_get(enum AVPixelFormat pix_fmt);
const char* av_color_transfer_name(enum AVColorTransferCharacteristic trc);
const char* av_color_primaries_name(enum AVColorPrimaries primaries);
const char* av_color_space_name(enum AVColorSpace space);
#endif
