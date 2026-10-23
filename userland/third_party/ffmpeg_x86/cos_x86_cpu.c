/* av_get_cpu_flags() for the x86 DSP init code (FFmpeg's own libavutil/x86/cpu.c needs more of libavutil than this port
 * carries). Reports SSE-class extensions only: nothing here may use AVX, because the kernel saves FPU state with
 * fxsave, which does not preserve the upper halves of the YMM registers. */
#include <cpuid.h>
#include "libavutil/cpu.h"

int av_get_cpu_flags(void)
{
    static int flags = -1;
    unsigned a, b, c, d;
    if (flags >= 0) return flags;
    flags = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return flags;
    if (d & (1u << 23)) flags |= AV_CPU_FLAG_MMX;
    if (d & (1u << 25)) flags |= AV_CPU_FLAG_MMXEXT | AV_CPU_FLAG_SSE;
    if (d & (1u << 26)) flags |= AV_CPU_FLAG_SSE2;
    if (c & (1u << 0))  flags |= AV_CPU_FLAG_SSE3;
    if (c & (1u << 9))  flags |= AV_CPU_FLAG_SSSE3;
    if (c & (1u << 19)) flags |= AV_CPU_FLAG_SSE4;
    if (c & (1u << 20)) flags |= AV_CPU_FLAG_SSE42;
    return flags;
}
