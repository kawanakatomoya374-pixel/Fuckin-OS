; nasm counterpart of ../ffmpeg_vp9/config.h for FFmpeg's x86 asm (what FFmpeg's configure generates).
; SSE-class code only: the kernel saves FPU state with fxsave, which keeps just the low 128 bits of each vector register.
%define ARCH_X86_32 0
%define ARCH_X86_64 1
%define HAVE_ALIGNED_STACK 1
%define HAVE_AVX2_EXTERNAL 0
%define HAVE_AVX512ICL_EXTERNAL 0
%define PIC 1
%define STACK_ALIGNMENT 16
%define private_prefix ff
%define FORCE_VEX_ENCODING 0
