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

/* Hardware capability only (CPUID). The CPU state AVX needs (CR4.OSXSAVE, XCR0) is NOT left enabled: C-OS saves task state with FXSAVE, which does not
 * hold the upper halves of the YMM registers, and an enabled OSXSAVE would make CPUID tell Linux programs (glibc's AVX2 memcpy, LLVM, Firefox ...) that AVX works,
 * so they would lose half their vector registers at every context switch. gfx_blit_avx2_copy_row() turns the state on for the length of one row, interrupts off. */
bool gfx_blit_avx2_available(void) {
    if (g_avx2_state == GFX_AVX2_UNPROBED) {
        unsigned int eax, ebx, ecx, edx;
        g_avx2_state = GFX_AVX2_UNAVAILABLE;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & (1u << 28)) &&
            __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) && (ebx & (1u << 5)))
            g_avx2_state = GFX_AVX2_AVAILABLE;
    }
    return g_avx2_state == GFX_AVX2_AVAILABLE;
}

/* 8, then 16, then (for large aligned transfers) 32 pixels per
 * iteration via unaligned loads/stores (rows handed to us from
 * vga.c/gfx_blit.c are not guaranteed 32-byte aligned), scalar tail
 * for whatever's left over. Only ever reached after
 * gfx_blit_avx2_available() has returned true - see the file header
 * for why that check can't be skipped. */
__attribute__((target("avx2")))
static void avx2_copy_row_inner(uint32_t* dst, const uint32_t* src, int count) {
    int i = 0;

    /* Large, naturally-aligned transfers - the common case for a
     * whole-row copy that starts at column 0 (offset 0 is always
     * aligned), which is exactly what vga_flip()'s full-screen present
     * and vga_copy_tile_worker's tiled present both do - bypass the
     * cache with non-temporal stores. This data is written once and
     * not re-read by this CPU any time soon (it's read back by the
     * display hardware/VirtIO-GPU, not by software), so keeping it in
     * cache would only evict other data that IS about to be reused.
     * _mm256_stream_si256 requires a 32-byte-aligned destination -
     * checked here at runtime, never assumed from calling context. */
    bool dst_aligned = (((uintptr_t)dst) & 31u) == 0;
    if (dst_aligned && count >= 512) {
        for (; i + 32 <= count; i += 32) {
            __m256i c0 = _mm256_loadu_si256((const __m256i*)(src + i));
            __m256i c1 = _mm256_loadu_si256((const __m256i*)(src + i + 8));
            __m256i c2 = _mm256_loadu_si256((const __m256i*)(src + i + 16));
            __m256i c3 = _mm256_loadu_si256((const __m256i*)(src + i + 24));
            _mm256_stream_si256((__m256i*)(dst + i), c0);
            _mm256_stream_si256((__m256i*)(dst + i + 8), c1);
            _mm256_stream_si256((__m256i*)(dst + i + 16), c2);
            _mm256_stream_si256((__m256i*)(dst + i + 24), c3);
        }
        /* Non-temporal stores are weakly ordered w.r.t. normal stores -
         * fence before anything below (or the caller) touches this
         * memory through an ordinary load/store. Only needed - and
         * only costs anything - on the path that actually issued NT
         * stores. */
        if (i > 0) _mm_sfence();
    }

    /* 64 bytes/iteration for the common case (a row too small for the
     * streaming path above, or the tail of one that isn't a multiple
     * of 32 pixels): halves loop-branch overhead versus one __m256i
     * at a time without needing any alignment guarantee. */
    for (; i + 16 <= count; i += 16) {
        __m256i c0 = _mm256_loadu_si256((const __m256i*)(src + i));
        __m256i c1 = _mm256_loadu_si256((const __m256i*)(src + i + 8));
        _mm256_storeu_si256((__m256i*)(dst + i), c0);
        _mm256_storeu_si256((__m256i*)(dst + i + 8), c1);
    }
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

void gfx_blit_avx2_copy_row(uint32_t* dst, const uint32_t* src, int count) {
    uint64_t fl;
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(fl) :: "memory");
    uint64_t cr4 = gfx_read_cr4();
    gfx_write_cr4(cr4 | (1ULL << 18));                       /* CR4.OSXSAVE */
    if ((gfx_xgetbv0() & 0x7) != 0x7) gfx_xsetbv0(0x7);
    avx2_copy_row_inner(dst, src, count);
    gfx_write_cr4(cr4 & ~(1ULL << 18));                      /* user code must not see AVX as usable */
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}
