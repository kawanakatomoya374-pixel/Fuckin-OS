/*
 * mouse_irq.h - PS/2 Mouse Driver with IRQ12 Interrupt Support
 */

#ifndef MOUSE_IRQ_H
#define MOUSE_IRQ_H

#include "types.h"

// IRQ12 handler - called by IDT when mouse generates interrupt
void mouse_irq_handler(void);

// Initialize mouse with interrupt support
void mouse_irq_init(void);

// Update mouse state from interrupt data (call in main loop)
void mouse_irq_update(void);

// Hybrid mode - uses both IRQ and polling
void mouse_hybrid_poll(void);

// Check if IRQ mode is available
bool mouse_irq_available(void);

#endif
