/**
 * gdt.h - Global Descriptor Table
 * 
 * x86 segmentation support with flat memory model.
 * Modern OS uses paging primarily, but GDT is required for x86.
 */

#ifndef GDT_H
#define GDT_H

#include "types.h"

/* Segment selectors */
#define GDT_KERNEL_CODE 0x08  // Kernel code segment
#define GDT_KERNEL_DATA 0x10  // Kernel data segment
#define GDT_USER_CODE   0x1B  // User code segment (RPL 3)
#define GDT_USER_DATA   0x23  // User data segment (RPL 3)
#define GDT_AP_CODE32   0x38  // Temporary 32-bit code segment for AP bootstrap
#define GDT_TSS         0x40  // Task State Segment (entries 8-9)

/* Entries 0..7 are ordinary descriptors; TSS occupies entries 8 and 9. */
#define GDT_ENTRIES     10

/* Access flags */
#define SEG_ACCESS_ACCESSED     0x01
#define SEG_ACCESS_READ_WRITE   0x02
#define SEG_ACCESS_CONFORMING   0x04
#define SEG_ACCESS_EXECUTABLE   0x08
#define SEG_ACCESS_DESCRIPTOR   0x10  // 1=code/data, 0=system
#define SEG_ACCESS_PRIV_RING0   0x00
#define SEG_ACCESS_PRIV_RING1   0x20
#define SEG_ACCESS_PRIV_RING2   0x40
#define SEG_ACCESS_PRIV_RING3   0x60
#define SEG_ACCESS_PRESENT      0x80

/* Flags */
#define SEG_FLAG_16BIT          0x00
#define SEG_FLAG_32BIT          0x40
#define SEG_FLAG_64BIT          0x20
#define SEG_FLAG_GRAN_BYTE      0x00
#define SEG_FLAG_GRAN_4K       0x80  // 4KB granularity
#define SEG_FLAG_LONG_MODE      0x20  // Long mode flag

/* GDT entry structure (8 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;      // Limit bits 0-15
    uint16_t base_low;       // Base bits 0-15
    uint8_t  base_middle;    // Base bits 16-23
    uint8_t  access;         // Access flags
    uint8_t  granularity;    // Granularity + limit bits 16-19
    uint8_t  base_high;      // Base bits 24-31
} gdt_entry_t;

/* GDT pointer structure (10 bytes in long mode) */
typedef struct __attribute__((packed)) {
    uint16_t limit;          /* Size of GDT - 1 */
    uint64_t base;           /* Address of GDT (64-bit) */
} gdt_ptr_t;

/* TSS (Task State Segment) structure for x86-64 */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t esp0;           // RSP0
    uint64_t esp1;           // RSP1
    uint64_t esp2;           // RSP2
    uint64_t reserved1;
    uint64_t ist[7];         // IST1-7
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss_t;

/* Function prototypes */

/* GDT initialization */
void gdt_init(void);
void gdt_install(void);

/* GDT entry manipulation */
void gdt_set_entry(int index, uint64_t base, uint64_t limit, 
                   uint8_t access, uint8_t granularity);
void gdt_set_kernel_code(void);
void gdt_set_kernel_data(void);
void gdt_set_user_code(void);
void gdt_set_user_data(void);
void gdt_set_tss(uint64_t base);

/* TSS management */
void tss_init(void);
void tss_set_kernel_stack(uint64_t esp0);
void tss_flush(void);
void gdt_enable_syscall(void* entry);

/* Segment selector helpers */
static inline void set_kernel_segments(void) {
    // Set DS, ES, FS, GS to kernel data segment
    __asm__ volatile(
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        : : : "ax"
    );
}

static inline void set_user_segments(void) {
    // Set segments to user data segment
    __asm__ volatile(
        "movw $0x23, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        : : : "ax"
    );
}

/* Far jump to reload CS */
static inline void gdt_reload_cs(void) {
    __asm__ volatile(
        "pushq $0x08\n\t"
        "leaq 1f(%rip), %rax\n\t"
        "pushq %rax\n\t"
        "lretq\n\t"
        "1:\n\t"
    );
}

#endif /* GDT_H */
