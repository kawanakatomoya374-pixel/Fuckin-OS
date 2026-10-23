/* Stand-in for FFmpeg's generated config_components.h. This port
 * defines every CONFIG_*_DECODER/HWACCEL macro anything here tests
 * directly in config.h instead (see that file) - there is no separate
 * per-component config generation step to stand in for. */
#ifndef COS_FFMPEG_WEBP_CONFIG_COMPONENTS_H
#define COS_FFMPEG_WEBP_CONFIG_COMPONENTS_H
/* vp9.c computes its own HWACCEL_MAX from these directly (unlike vp8.c,
 * which just includes hwconfig.h's canned HWACCEL_MAX 0) - all 0, no
 * hardware accelerators exist in this port (see avcodec.h: hwaccel is
 * always NULL). */
#define CONFIG_VP9_DXVA2_HWACCEL 0
#define CONFIG_VP9_D3D11VA_HWACCEL 0
#define CONFIG_VP9_D3D12VA_HWACCEL 0
#define CONFIG_VP9_NVDEC_HWACCEL 0
#define CONFIG_VP9_VAAPI_HWACCEL 0
#define CONFIG_VP9_VDPAU_HWACCEL 0
#define CONFIG_VP9_VIDEOTOOLBOX_HWACCEL 0
#define CONFIG_HEVC_D3D11VA_HWACCEL 0
#define CONFIG_HEVC_D3D11VA2_HWACCEL 0
#define CONFIG_HEVC_D3D12VA_HWACCEL 0
#define CONFIG_HEVC_DXVA2_HWACCEL 0
#define CONFIG_HEVC_NVDEC_HWACCEL 0
#define CONFIG_HEVC_VAAPI_HWACCEL 0
#define CONFIG_HEVC_VDPAU_HWACCEL 0
#define CONFIG_HEVC_VIDEOTOOLBOX_HWACCEL 0
#define CONFIG_HEVC_VULKAN_HWACCEL 0
#endif
