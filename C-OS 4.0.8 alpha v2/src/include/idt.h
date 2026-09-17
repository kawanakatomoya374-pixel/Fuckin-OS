#ifndef IDT_H
#define IDT_H
#include "types.h"

#define IDT_ENTRIES 256

/* 64-bit register frame pushed by ISR/IRQ stubs */
struct regs {
    /* General purpose registers (pushed by stub, reverse order) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* Pushed by stub: interrupt number and error code */
    uint64_t int_no, err_code;
    /* Pushed by CPU on interrupt */
    uint64_t rip, cs, rflags, rsp, ss;
};

/* 64-bit IDT gate descriptor (16 bytes) */
typedef struct {
    uint16_t base_low;      /* bits 0-15 of handler address */
    uint16_t selector;      /* code segment selector */
    uint8_t  ist;           /* interrupt stack table (0 = none) */
    uint8_t  flags;         /* type and attribute flags */
    uint16_t base_mid;      /* bits 16-31 of handler address */
    uint32_t base_high;     /* bits 32-63 of handler address */
    uint32_t reserved;      /* must be zero */
} __attribute__((packed)) idt_entry_t;

/* 64-bit IDT pointer */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

typedef void (*isr_t)(struct regs *);

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint64_t sel, uint8_t flags);
void register_interrupt_handler(uint8_t n, void (*handler)(struct regs *));
void isr_handler(struct regs *r);
bool task_handle_page_fault(uint64_t fault_addr, uint64_t error_code);

#endif /* IDT_H */
