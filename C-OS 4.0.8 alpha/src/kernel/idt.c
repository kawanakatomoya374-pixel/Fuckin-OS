/**
 * idt.c - Interrupt Descriptor Table (64-bit)
 * C-OS 4.0.8 alpha x86-64 IDT Management
 * 
 * Fixed: Removed legacy remnants, proper 64-bit IDT structure - fully converted to 64-bit
 */

#include "types.h"
#include "io.h"
#include "serial.h"
#include "idt.h"

// Forward declarations
void page_fault_handler(uint64_t error_code, uint64_t int_no);
void page_fault_handler_wrapper(struct regs *r);

// Forward declarations for freestanding environment
void* memset(void* ptr, int value, size_t num);
char* strncpy(char* dest, const char* src, size_t n);

/* 64-bit IDT table (256 entries x 16 bytes = 4096 bytes) */
static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t   idtp;

/* Interrupt handler function table */
static void (*interrupt_handlers[IDT_ENTRIES])(struct regs *r);

/* External ISR/IRQ stubs from isr.asm / irq.asm */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);
extern void isr128(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

/* Set a 64-bit IDT gate */
void idt_set_gate(uint8_t num, uint64_t base, uint64_t sel, uint8_t flags) {
    idt[num].base_low  = (uint64_t)(base & 0xFFFF);
    idt[num].base_mid  = (uint64_t)((base >> 16) & 0xFFFF);
    idt[num].base_high = (uint64_t)((base >> 32) & 0xFFFFFFFF);
    idt[num].selector  = sel;
    idt[num].ist       = 0;
    idt[num].flags     = flags;
    idt[num].reserved  = 0;
}

/* Default handler for unregistered interrupts.
 *
 * Previously this suppressed the vector number specifically for
 * int_no == 13 (#GP) and never printed the error code or faulting
 * RIP at all - so a General Protection Fault (the single most useful
 * exception to see the address for) showed up in the log as a bare,
 * repeating "[EXCEPTION] Unhandled interrupt: " with no number,
 * making it impossible to tell which vector was firing or where.
 * Since this handler doesn't fix the fault or skip the faulting
 * instruction, an unhandled CPU exception (unlike a hardware IRQ)
 * will IRET straight back into the same faulting instruction and
 * fault again immediately - hence the infinite flood. Printing
 * int_no/err_code/rip/cs here is what actually lets that be diagnosed
 * instead of just observed. */
static void default_handler(struct regs *r) {
    serial_puts("[EXCEPTION] Unhandled interrupt: ");
    serial_putdec((uint64_t)r->int_no);
    serial_puts(" err_code=0x");
    serial_puthex(r->err_code);
    serial_puts(" rip=0x");
    serial_puthex(r->rip);
    serial_puts(" cs=0x");
    serial_puthex(r->cs);
    serial_puts("\n");
    serial_puts("[EXCEPTION] Fatal unhandled CPU exception - halting\n");
    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/* Install the IDT */
static void idt_install(void) {
    idtp.limit = (uint16_t)(sizeof(idt_entry_t) * IDT_ENTRIES - 1);
    idtp.base  = (uint64_t)&idt;
    __asm__ volatile("lidt %0" : : "m"(idtp));
}

/* Initialize IDT */
void idt_init(void) {
    serial_puts("[IDT] Initializing 64-bit IDT\n");

    /* Clear IDT */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i].base_low  = 0;
        idt[i].base_mid  = 0;
        idt[i].base_high = 0;
        idt[i].selector  = 0;
        idt[i].ist       = 0;
        idt[i].flags     = 0;
        idt[i].reserved  = 0;
        interrupt_handlers[i] = default_handler;
    }

    /* Install exception handlers (ISR 0-31) */
    idt_set_gate(0,  (uint64_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint64_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint64_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint64_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint64_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint64_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint64_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint64_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint64_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint64_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint64_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint64_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint64_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint64_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint64_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint64_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint64_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint64_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint64_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint64_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint64_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint64_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint64_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint64_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint64_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint64_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint64_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint64_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint64_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint64_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint64_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint64_t)isr31, 0x08, 0x8E);

    /* Syscall gate for ring3 usermode (int 0x80). DPL=3 (0xEE, vs 0x8E for
     * the exception gates above) is required or the CPU rejects the
     * software interrupt from CPL3 with #GP before ever reaching this
     * handler. */
    idt_set_gate(128, (uint64_t)isr128, 0x08, 0xEE);

    /* Register page fault handler wrapper for interrupt 14.
       The page fault entry itself must still use the ISR stub isr14. */
    register_interrupt_handler(14, page_fault_handler_wrapper);

    /* Install hardware IRQ handlers (32-47) - use irq.asm stubs */
    idt_set_gate(32, (uint64_t)irq0,  0x08, 0x8E);  /* Timer */
    idt_set_gate(33, (uint64_t)irq1,  0x08, 0x8E);  /* Keyboard */
    idt_set_gate(34, (uint64_t)irq2,  0x08, 0x8E);  /* Cascade */
    idt_set_gate(35, (uint64_t)irq3,  0x08, 0x8E);  /* COM2 */
    idt_set_gate(36, (uint64_t)irq4,  0x08, 0x8E);  /* COM1 */
    idt_set_gate(37, (uint64_t)irq5,  0x08, 0x8E);  /* LPT2 */
    idt_set_gate(38, (uint64_t)irq6,  0x08, 0x8E);  /* Floppy */
    idt_set_gate(39, (uint64_t)irq7,  0x08, 0x8E);  /* LPT1 */
    idt_set_gate(40, (uint64_t)irq8,  0x08, 0x8E);  /* RTC */
    idt_set_gate(41, (uint64_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint64_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint64_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint64_t)irq12, 0x08, 0x8E);  /* PS/2 Mouse */
    idt_set_gate(45, (uint64_t)irq13, 0x08, 0x8E);  /* FPU */
    idt_set_gate(46, (uint64_t)irq14, 0x08, 0x8E);  /* Primary ATA */
    idt_set_gate(47, (uint64_t)irq15, 0x08, 0x8E);  /* Secondary ATA */

    idt_install();
    serial_puts("[IDT] 64-bit IDT initialized\n");
}

/* Register a C-level interrupt handler */
void register_interrupt_handler(uint8_t n, void (*handler)(struct regs *)) {
    interrupt_handlers[n] = handler;
}

/* Called from isr_common_stub in isr.asm */
void _isr_handler(struct regs *r) {
    uint64_t int_no = r->int_no;
    if (int_no < IDT_ENTRIES && interrupt_handlers[int_no]) {
        interrupt_handlers[int_no](r);
    } else {
        serial_puts("[ISR] Unhandled exception: ");
        serial_putdec((uint64_t)int_no);
        serial_puts("\n");
    }
}

void _irq_handler(struct regs *r) {
    uint64_t irq_no = r->int_no;

    /* IMPORTANT: send EOI to the PIC(s) BEFORE dispatching the handler.
     *
     * The timer handler may call into the scheduler and perform a real
     * CPU context switch (hal_context_switch). That switch does not
     * "return" in the normal sense for the outgoing thread - execution
     * only resumes here, at this exact point, much later when this
     * thread is scheduled back in. If EOI were sent AFTER the handler
     * call (as before), it would never be sent for the thread being
     * switched away from, and the PIC would consider IRQ0 (and IRQ8-15)
     * still "in service" forever - masking all further timer interrupts
     * and permanently halting preemption after the very first context
     * switch. Sending EOI first guarantees the interrupt controller is
     * always re-armed regardless of what the handler does with the CPU. */
    if (irq_no >= 40) {
        __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0x20), "Nd"((uint64_t)0xA0));
    }
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0x20), "Nd"((uint64_t)0x20));

    if (irq_no < IDT_ENTRIES && interrupt_handlers[irq_no]) {
        interrupt_handlers[irq_no](r);
    }
}

/* Page fault handler */
void page_fault_handler_wrapper(struct regs *r) {
    uint64_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
    /* Retain the faulting instruction address in serial diagnostics.  CR2
       identifies only the accessed address; RIP is required to distinguish
       allocator validation from an upstream NetSurf/libcss access. */
    serial_puts("[PF] RIP: 0x");
    serial_puthex(r->rip);
    serial_puts("\n");
    page_fault_handler(r->err_code, fault_addr);
}

/* Legacy compatibility stub */
void isr_handler(struct regs *r) {
    _isr_handler(r);
}
