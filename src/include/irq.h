#ifndef IRQ_H
#define IRQ_H

#include "types.h"

// IRQ numbers
#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1
#define IRQ_MOUSE    12

// Forward declarations
struct regs;

// Install IRQ handler
void irq_install_handler(int irq, void (*handler)(struct regs *));

// Mask/unmask an individual IRQ line at the PIC. Used to keep a line
// disabled until its driver has actually finished initializing (see
// irq_init()/irq.c) - installing a handler is not enough by itself,
// since the PIC may already have that line unmasked from whatever
// state the BIOS/bootloader left it in, letting a self-test or spurious
// interrupt reach the handler before the driver's own state exists.
void irq_set_mask(uint8_t irq);
void irq_clear_mask(uint8_t irq);

// Common IRQ handler
void irq_handler(struct regs *r);

// Handle pending IRQs
void irq_handle_pending(void);

// Initialize IRQ system
void irq_init(void);

#endif
