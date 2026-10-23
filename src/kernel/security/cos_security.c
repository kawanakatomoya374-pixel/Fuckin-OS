/**
 * cos_security.c - CPU hardening and randomness for address-space layout randomisation.
 */
#include "cos_security.h"
#include "serial.h"
#include "timer.h"

static volatile uint64_t g_rng_state = 0x9E3779B97F4A7C15ull;

static inline uint64_t rdtsc_u64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool cpu_has_rdrand(void) {
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (c & (1u << 30)) != 0;
}

uint64_t cos_random_u64(void) {
    static int has_rdrand = -1;
    if (has_rdrand < 0) has_rdrand = cpu_has_rdrand() ? 1 : 0;
    if (has_rdrand) {
        for (int i = 0; i < 10; ++i) {
            uint64_t v; uint8_t ok;
            __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
            if (ok) return v;
        }
    }
    /* splitmix64 over a state that is perturbed by the TSC and the tick counter on every call */
    uint64_t z = (g_rng_state += 0x9E3779B97F4A7C15ull) ^ rdtsc_u64() ^ (get_timer_ticks() << 20);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

volatile uint8_t cos_smap_active;       /* read by the interrupt stubs (clac) and the syscall wrapper (stac) */

void cos_security_harden_cpu(void) {
    uint32_t a = 7, b, c = 0, d;
    uint32_t maxleaf;
    __asm__ volatile("cpuid" : "=a"(maxleaf) : "a"(0u) : "ebx", "ecx", "edx");
    if (maxleaf < 7) return;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (b & (1u << 7)) {                                 /* CPUID.7.0:EBX.SMEP */
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ull << 20);
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        serial_puts((cr4 & (1ull << 20)) ? "[SECURITY] SMEP enabled on this CPU\n" : "[SECURITY] SMEP could not be enabled\n");
    } else {
        serial_puts("[SECURITY] SMEP not supported by this CPU\n");
    }
    if (b & (1u << 20)) {                                /* CPUID.7.0:EBX.SMAP */
        uint64_t cr4;
        cos_smap_active = 1;                             /* from now on every entry stub executes clac */
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ull << 21);
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
        serial_puts("[SECURITY] SMAP enabled on this CPU\n");
    } else {
        serial_puts("[SECURITY] SMAP not supported by this CPU\n");
    }
}

/* Kernel stack protector (-fstack-protector-strong, global guard). The guard is randomised once, before any protected
 * function that will later return is on the stack; kernel_main() never returns. A smashed frame ends in a panic
 * instead of a corrupted return address. */
uintptr_t __stack_chk_guard = 0x595E9FBD94FDA766ull;

__attribute__((no_stack_protector)) void cos_stack_guard_init(void) {
    uint64_t v = cos_random_u64();
    v &= ~0xFFull;                       /* a zero low byte stops string-copy overflows from forging it */
    __stack_chk_guard = (uintptr_t)v;
}

__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail(void) {
    serial_puts("\n[SECURITY] *** kernel stack smashing detected - halting ***\n");
    for (;;) __asm__ volatile("cli; hlt");
}
