/**
 * msr.h - Model Specific Register helpers and constants
 */
#ifndef MSR_H
#define MSR_H

#include "types.h"

#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_CSTAR           0xC0000083
#define MSR_SYSCALL_MASK    0xC0000084
#define MSR_FS_BASE         0xC0000100
#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102
#define MSR_TSC_AUX         0xC0000103
#define MSR_APIC_BASE       0x0000001B
#define MSR_IA32_PAT        0x00000277

#define EFER_SCE            (1ULL << 0)
#define EFER_LME            (1ULL << 8)
#define EFER_LMA            (1ULL << 10)
#define EFER_NXE            (1ULL << 11)

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

#endif /* MSR_H */
