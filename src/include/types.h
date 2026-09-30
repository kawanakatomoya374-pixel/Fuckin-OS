/**
 * types.h - C-OS 4.0.8 alpha Clean Type System
 * Pure freestanding environment - compatible with standard headers
 */

#ifndef TYPES_H
#define TYPES_H

// Include standard types for compatibility
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Physical/Virtual address types
// Use uintptr_t so address-sized values track the build's native pointer width.
typedef uintptr_t phys_addr_t;
typedef uintptr_t virt_addr_t;

// File offset type
#ifdef COS_KERNEL
#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
typedef int64_t off_t;
typedef int64_t _off_t;
#endif
#ifndef _OFF64_T_DEFINED
#define _OFF64_T_DEFINED
typedef int64_t _off64_t;
#endif
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int64_t ssize_t;
#endif

// Process/Thread IDs
typedef int pid_t;
typedef uint64_t tid_t;

// File/IO types
typedef int64_t  file_t;
typedef unsigned int mode_t;
#else
// Host-side tools may include standard libc headers that already define
// off_t/pid_t/mode_t. Keep the freestanding aliases out of that build.
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int64_t ssize_t;
#endif
#endif

// Page table entry types
typedef uint64_t pml4e_t;
typedef uint64_t pdpte_t;
typedef uint64_t pde_t;
typedef uint64_t pte_t;

// Constants
#ifndef NULL
#define NULL ((void*)0)
#endif

// Memory alignment
#define PAGE_SIZE   4096ULL
#define PAGE_MASK   (~(PAGE_SIZE - 1))

// Compiler attributes
#define PACKED      __attribute__((packed))
#define ALIGN(n)    __attribute__((aligned(n)))
#ifndef NORETURN
#define NORETURN    __attribute__((noreturn))
#endif
#define WEAK        __attribute__((weak))

// Memory barriers
#define MEMORY_BARRIER() __asm__ __volatile__("mfence" ::: "memory")

// Volatile for hardware access
#define VOLATILE volatile


#ifndef TRUE
#define TRUE true
#endif
#ifndef FALSE
#define FALSE false
#endif
// Error codes
typedef enum {
    ERROR_NONE = 0,
    ERROR_INVALID_PARAM,
    ERROR_OUT_OF_MEMORY,
    ERROR_NOT_FOUND,
    ERROR_PERMISSION_DENIED,
    ERROR_IO_ERROR,
    ERROR_NOT_IMPLEMENTED
} error_t;

// Memory protection flags
typedef uint64_t mem_flags_t;
#define MEM_READ    ((mem_flags_t)1ULL << 0)
#define MEM_WRITE   ((mem_flags_t)1ULL << 1)
#define MEM_USER    ((mem_flags_t)1ULL << 2)
#define MEM_EXECUTE ((mem_flags_t)1ULL << 3)
#define MEM_GLOBAL  ((mem_flags_t)1ULL << 8)
#define MEM_NX      ((mem_flags_t)1ULL << 63)

// Page table flags
typedef uint64_t pt_flags_t;
#define PT_PRESENT      ((pt_flags_t)1ULL << 0)
#define PT_WRITE        ((pt_flags_t)1ULL << 1)
#define PT_USER         ((pt_flags_t)1ULL << 2)
#define PT_WRITETHROUGH ((pt_flags_t)1ULL << 3)
#define PT_NOCACHE      ((pt_flags_t)1ULL << 4)
#define PT_GLOBAL       ((pt_flags_t)1ULL << 8)
#define PT_NX           ((pt_flags_t)1ULL << 63)

#endif // TYPES_H
