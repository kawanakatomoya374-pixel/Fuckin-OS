/**
 * pixfmt.h - stand-in for libavutil/pixfmt.h.
 *
 * Real FFmpeg's enum has ~200 members; this lists exactly the ones
 * vp9.c/vp9dec.h/vp9data.c reference (every AV_PIX_FMT_, AVCOL_ and
 * AVCHROMA_LOC_ name grepped out of those files), in the same values
 * real FFmpeg uses only where something here compares a raw int - this
 * port never does, so the enum's actual numeric values only need to be
 * distinct from one another. Hardware-accelerated formats (D3D11,
 * D3D12, DXVA2, VAAPI, VDPAU, VIDEOTOOLBOX, VULKAN, CUDA, CUARRAY) are
 * kept only so vp9.c's dead hwaccel-format-negotiation code compiles;
 * avctx->hwaccel is always NULL in this port (see codec_internal.h), so
 * none of them are ever actually selected.
 */
#ifndef COS_FFMPEG_VP9_PIXFMT_H
#define COS_FFMPEG_VP9_PIXFMT_H

enum AVPixelFormat {
    AV_PIX_FMT_NONE = -1,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUVA420P,
    AV_PIX_FMT_YUV422P,
    AV_PIX_FMT_YUV440P,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUV420P10,
    AV_PIX_FMT_YUV422P10,
    AV_PIX_FMT_YUV440P10,
    AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_YUV420P12,
    AV_PIX_FMT_YUV422P12,
    AV_PIX_FMT_YUV440P12,
    AV_PIX_FMT_YUV444P12,
    AV_PIX_FMT_GBRP,
    AV_PIX_FMT_GBRP10,
    AV_PIX_FMT_GBRP12,
    AV_PIX_FMT_ARGB,
    /* Added for the HEVC port. */
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_GRAY10,
    AV_PIX_FMT_GRAY12,
    AV_PIX_FMT_GRAY9,
    AV_PIX_FMT_YUV420P9,
    AV_PIX_FMT_YUV422P9,
    AV_PIX_FMT_YUV444P9,
    AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUVJ422P,
    AV_PIX_FMT_YUVJ444P,
    /* Hardware-accelerated formats this port never produces - see the
     * file comment above. */
    AV_PIX_FMT_CUDA,
    AV_PIX_FMT_CUARRAY,
    AV_PIX_FMT_VAAPI,
    AV_PIX_FMT_VDPAU,
    AV_PIX_FMT_D3D11,
    AV_PIX_FMT_D3D11VA_VLD,
    AV_PIX_FMT_D3D12,
    AV_PIX_FMT_DXVA2_VLD,
    AV_PIX_FMT_VIDEOTOOLBOX,
    AV_PIX_FMT_VULKAN,
};

/* real FFmpeg names the >8-bit formats ...LE / ...BE (native-endian aliases without a suffix) */
#define AV_PIX_FMT_YUV420P9LE AV_PIX_FMT_YUV420P9
#define AV_PIX_FMT_YUV422P9LE AV_PIX_FMT_YUV422P9
#define AV_PIX_FMT_YUV444P9LE AV_PIX_FMT_YUV444P9
#define AV_PIX_FMT_GRAY9LE AV_PIX_FMT_GRAY9
#define AV_PIX_FMT_YUV420P10LE AV_PIX_FMT_YUV420P10
#define AV_PIX_FMT_YUV422P10LE AV_PIX_FMT_YUV422P10
#define AV_PIX_FMT_YUV444P10LE AV_PIX_FMT_YUV444P10
#define AV_PIX_FMT_YUV420P12LE AV_PIX_FMT_YUV420P12
#define AV_PIX_FMT_YUV422P12LE AV_PIX_FMT_YUV422P12
#define AV_PIX_FMT_YUV444P12LE AV_PIX_FMT_YUV444P12
#define AV_PIX_FMT_GRAY10LE AV_PIX_FMT_GRAY10
#define AV_PIX_FMT_GRAY12LE AV_PIX_FMT_GRAY12
#define AV_PIX_FMT_GBRP10LE AV_PIX_FMT_GBRP10
#define AV_PIX_FMT_GBRP12LE AV_PIX_FMT_GBRP12

enum AVColorSpace {
    AVCOL_SPC_RGB = 0,
    AVCOL_SPC_BT709,
    AVCOL_SPC_UNSPECIFIED,
    AVCOL_SPC_RESERVED,
    AVCOL_SPC_FCC,
    AVCOL_SPC_BT470BG,
    AVCOL_SPC_SMPTE170M,
    AVCOL_SPC_SMPTE240M,
    AVCOL_SPC_YCGCO,
    AVCOL_SPC_BT2020_NCL,
    AVCOL_SPC_BT2020_CL,
};

enum AVColorRange {
    AVCOL_RANGE_UNSPECIFIED = 0,
    AVCOL_RANGE_MPEG = 1,
    AVCOL_RANGE_JPEG = 2,
};

enum AVColorPrimaries {
    AVCOL_PRI_RESERVED0 = 0, AVCOL_PRI_BT709 = 1, AVCOL_PRI_UNSPECIFIED = 2,
    AVCOL_PRI_BT470M = 4, AVCOL_PRI_BT470BG = 5, AVCOL_PRI_SMPTE170M = 6,
    AVCOL_PRI_SMPTE240M = 7, AVCOL_PRI_FILM = 8, AVCOL_PRI_BT2020 = 9,
};

enum AVColorTransferCharacteristic {
    AVCOL_TRC_BT709 = 1, AVCOL_TRC_UNSPECIFIED = 2, AVCOL_TRC_GAMMA22 = 4,
    AVCOL_TRC_GAMMA28 = 5, AVCOL_TRC_SMPTE170M = 6, AVCOL_TRC_SMPTE240M = 7,
    AVCOL_TRC_LINEAR = 8, AVCOL_TRC_BT2020_10 = 14, AVCOL_TRC_BT2020_12 = 15,
    AVCOL_TRC_SMPTE2084 = 16, AVCOL_TRC_SMPTE428 = 17, AVCOL_TRC_ARIB_STD_B67 = 18,
};

enum AVChromaLocation {
    AVCHROMA_LOC_UNSPECIFIED = 0,
    AVCHROMA_LOC_LEFT = 1,
    AVCHROMA_LOC_CENTER = 2,
    AVCHROMA_LOC_TOPLEFT = 3,
    AVCHROMA_LOC_TOP = 4, AVCHROMA_LOC_BOTTOMLEFT = 5, AVCHROMA_LOC_BOTTOM = 6,
};

#endif
