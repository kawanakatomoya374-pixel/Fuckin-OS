/**
 * config.h - hand-written replacement for FFmpeg's real, configure-script-
 * generated config.h, sized to exactly what vp8.c/vp8dsp.c/h264pred.c/
 * videodsp.c/webp.c (and the small utility headers they pull in) actually
 * test with #if/#ifdef. See CPORT_README.md in this directory for the
 * full picture of what this port is and is not.
 *
 * Deliberate choices:
 *  - Every ARCH_* is 0: this is a portable-C-only build. FFmpeg's x86/ARM/
 *    etc. SIMD C intrinsics and hand-written assembly are all gated behind
 *    their respective ARCH_* macro and never compiled in, which is exactly
 *    what lets this avoid needing yasm/nasm object files or vendor
 *    intrinsics headers that don't exist in this freestanding kernel.
 *  - HAVE_AV_CONFIG_H is intentionally NEVER defined (not here - by
 *    omission). Every FFmpeg header's "#ifdef HAVE_AV_CONFIG_H" block
 *    exists to gate internal-build-only content (arch dispatch, this same
 *    config.h, etc.) behind FFmpeg's own build system; leaving it
 *    undefined makes every header fall through to its public/portable
 *    fallback path, which is the path this port relies on throughout.
 *  - HAVE_THREADS is 0: this is a single-threaded decode (one call in,
 *    one decoded frame out, on whichever thread called it) - see
 *    thread.h and progressframe.h in this directory for what that
 *    simplifies away.
 */
#ifndef COS_FFMPEG_WEBP_CONFIG_H
#define COS_FFMPEG_WEBP_CONFIG_H

#define ARCH_AARCH64 0
#define ARCH_ARM 0
#define ARCH_LOONGARCH 0
#define ARCH_LOONGARCH64 0
#define ARCH_MIPS 0
#define ARCH_PPC 0
#define ARCH_RISCV 0
#define ARCH_X86 0
#define ARCH_X86_32 0
#define ARCH_X86_64 0

#define HAVE_BIGENDIAN 0
#define AV_HAVE_BIGENDIAN 0
#define HAVE_FAST_64BIT 1
#define HAVE_FAST_UNALIGNED 1
#define HAVE_SIMD_ALIGN_32 0
#define HAVE_SIMD_ALIGN_64 0
#define HAVE_THREADS 0
#define HAVE_X86ASM 0
#define HAVE_MMAP 0
#define HAVE_VIRTUALALLOC 0

#define CONFIG_SAFE_BITSTREAM_READER 1
#define CONFIG_SMALL 0
#define CONFIG_VP7_DECODER 1
#define CONFIG_VP8_DECODER 1
#define CONFIG_VP8_NVDEC_HWACCEL 0
#define CONFIG_VP8_NVDEC_CUARRAY_HWACCEL 0
#define CONFIG_VP8_VAAPI_HWACCEL 0
#define CONFIG_WEBP_ANIM_DECODER 0

#endif
