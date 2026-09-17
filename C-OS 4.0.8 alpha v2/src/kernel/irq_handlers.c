/**
 * irq_handlers.c - Dedicated IRQ Handler Implementations (64-bit)
 * C-OS 4.0.8 alpha
 */

#include "types.h"
#include "serial.h"
#include "idt.h"
#include "io.h"
#include "keyboard.h"
#include "timer.h"

void scheduler_capture_interrupt_context(struct regs *r);
void scheduler_end_interrupt_tick(void);

void timer_irq_handler(struct regs *r) {
    scheduler_capture_interrupt_context(r);
    extern void timer_tick(void);
    timer_tick();
    keyboard_timer_tick((uint32_t)get_timer_ticks());
    scheduler_end_interrupt_tick();
}

void irq_dispatch_handler(struct regs *r) {
    uint8_t irq = (uint8_t)(r->int_no - 32);
    switch (irq) {
        case 0:
            timer_irq_handler(r);
            break;
        default:
            break;
    }
}
