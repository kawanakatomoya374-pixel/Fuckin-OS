/**
 * gfx_blit_avx2.c - optional AVX2-accelerated row copy for
 * gfx_blit()'s GFX_BLIT_COPY path (the memcpy-per-row that backs
 * vga_flip(), vga_fill_rect-adjacent copies, vga_copy_rect, decoded
 * image blits, ...).
 *
 * IMPORTANT - read before touching this file: every function here
 * that uses an AVX2 intrinsic is individually marked
 * __attribute__((target("avx2"))), so *only* those functions ever
 * compile to VEX-encoded instructions; the rest of the kernel
 * (including the rest of gfx_blit.c) keeps compiling exactly as it
 * did before this file existed - no -mavx2 is added to the project's
 * global CFLAGS, so nothing else in the tree can silently pick up an
 * autovectorized AVX2 loop outside of this runtime-gated path.
 *
 * Nothing in this file may run before gfx_blit_avx2_available() has
 * returned true. That matters more than it usually would: C-OS's
 * boot path (boot.asm) enables baseline SSE2 (CR4.OSFXSR /
 * OSXMMEXCPT) but never touches CR4.OSXSAVE or XCR0. Executing any
 * VEX-encoded instruction before OSXSAVE/XCR0 are enabled is a
 * guaranteed #UD (invalid opcode) fault - not a graceful
 * "unsupported", a hard crash - regardless of what CPUID says the
 * hardware is capable of. gfx_blit_avx2_probe() below performs and
 * *verifies* that setup itself (CPUID checks are not enough on their
 * own) before ever reporting AVX2 as available, and permanently
 * falls back to false on the first sign anything didn't take.
 */
#include "gfx_blit.h"
#include <stddef.h>
#include <cpuid.h>
#include <immintrin.h>

typedef enum {
    GFX_AVX2_UNPROBED = 0,
    GFX_AVX2_AVAILABLE,
    GFX_AVX2_UNAVAILABLE,
} gfx_avx2_state_t;

static gfx_avx2_state_t g_avx2_state = GFX_AVX2_UNPROBED;

static inline uint64_t gfx_xgetbv0(void) {
    uint32_t lo, hi;
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" /* xgetbv, ecx=0 selects XCR0 */
                      : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void gfx_xsetbv0(uint64_t value) {
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile(".byte 0x0f, 0x01, 0xd1" /* xsetbv, ecx=0 selects XCR0 */
                      :: "a"(lo), "d"(hi), "c"(0) : "memory");
}

static inline uint64_t gfx_read_cr4(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void gfx_write_cr4(uint64_t v) {
    __asm__ volatile("mov %0, %%cr4" :: "r"(v) : "memory");
}

/* Detect AVX2 *and* enable the CPU state required to use it,
 * verifying every step rather than assuming it worked. Runs at most
 * once - later calls just return the cached result. Called lazily
 * from gfx_blit_avx2_available() on first use rather than from
 * kernel init, so this file has zero boot-order dependencies on
 * anything else. */
static void gfx_blit_avx2_probe(void) {
    if (g_avx2_state != GFX_AVX2_UNPROBED) return;
    g_avx2_state = GFX_AVX2_UNAVAILABLE; /* pessimistic until proven otherwise */

    unsigned int eax, ebx, ecx, edx;

    /* CPUID.1:ECX.AVX (bit 28) - raw hardware capability, independent
     * of whether the OS has enabled XSAVE yet. */
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return;
    if (!(ecx & (1u << 28))) return;

    /* CPUID.7.0:EBX.AVX2 (bit 5). */
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) return;
    if (!(ebx & (1u << 5))) return;

    /* Hardware can do AVX2, but nothing has enabled XSAVE yet - do
     * that now, then verify it actually stuck before trusting it. */
    uint64_t cr4 = gfx_read_cr4();
    if (!(cr4 & (1ULL << 18))) {
        gfx_write_cr4(cr4 | (1ULL << 18)); /* CR4.OSXSAVE */
    }
    /* CPUID.1:ECX.OSXSAVE (bit 27) *reflects* CR4.OSXSAVE - if our
     * write above didn't take for any reason, this reads back 0 and
     * we bail out here instead of proceeding to XSETBV (which would
     * itself fault if OSXSAVE genuinely isn't set). */
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return;
    if (!(ecx & (1u << 27))) return;

    gfx_xsetbv0(0x7); /* enable x87 + SSE + AVX state in XCR0 */
    if ((gfx_xgetbv0() & 0x7) != 0x7) return; /* didn't take - stay disabled */

    g_avx2_state = GFX_AVX2_AVAILABLE;
}

bool gfx_blit_avx2_available(void) {
    gfx_blit_avx2_probe();
    return g_avx2_state == GFX_AVX2_AVAILABLE;
}

/* 32 bytes (8 pixels) per iteration via unaligned loads/stores (rows
 * handed to us from vga.c/gfx_blit.c are not guaranteed 32-byte
 * aligned), scalar tail for whatever's left over. Only ever reached
 * after gfx_blit_avx2_available() has returned true - see the file
 * header for why that check can't be skipped. */
__attribute__((target("avx2")))
void gfx_blit_avx2_copy_row(uint32_t* dst, const uint32_t* src, int count) {
    int i = 0;
    for (; i + 8 <= count; i += 8) {
        __m256i chunk = _mm256_loadu_si256((const __m256i*)(src + i));
        _mm256_storeu_si256((__m256i*)(dst + i), chunk);
    }
    for (; i < count; i++) dst[i] = src[i];
    /* Clear the upper YMM state before returning to code that may use
     * legacy SSE (this kernel's default -msse2 float codegen) - skips
     * the AVX/SSE transition penalty and is the standard thing to do
     * before leaving an AVX2 code path. */
    _mm256_zeroupper();
}
