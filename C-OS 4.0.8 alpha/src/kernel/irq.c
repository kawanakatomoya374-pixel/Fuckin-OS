/**
 * irq.c - IRQ Management (64-bit)
 * C-OS 4.0.8 alpha Interrupt Request Handler
 *
 * FIX: keyboard_interrupt_handler() was an empty stub.
 *      Now properly dispatches to keyboard_irq_handler().
 * FIX: irq_handlers[] is now used by _irq_handler in idt.c
 *      via register_interrupt_handler(). No duplicate dispatch.
 */
#include "types.h"
#include "serial.h"
#include "io.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"
#include "mouse.h"

/* Forward declarations */
extern void timer_irq_handler(struct regs *r);
void keyboard_handler(struct regs *r);
void mouse_handler(struct regs *r);

/* Remap PIC: IRQ 0-7 -> INT 32-39, IRQ 8-15 -> INT 40-47 */
static void irq_remap(void) {
    uint8_t mask1 = inb(0x21);
    uint8_t mask2 = inb(0xA1);

    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);   /* Master PIC vector offset: 32 */
    outb(0xA1, 0x28);   /* Slave PIC vector offset: 40 */
    outb(0x21, 0x04);   /* Master: slave at IRQ2 */
    outb(0xA1, 0x02);   /* Slave: cascade identity */
    outb(0x21, 0x01);   /* 8086 mode */
    outb(0xA1, 0x01);

    outb(0x21, mask1);
    outb(0xA1, mask2);
}

/* Mask/unmask a single IRQ line at the PIC without touching its
 * installed handler. See irq.h for why this exists: irq_install_handler()
 * unconditionally unmasks the line the moment the handler is wired up,
 * which for keyboard (IRQ1) and mouse (IRQ12) happens during irq_init()
 * - long before keyboard_init()/minimal_mouse_init() run and give those
 * drivers' internal state anything valid to work with. On real PS/2
 * controllers (and QEMU's emulation of them) a self-test or stray byte
 * can raise IRQ1/IRQ12 during that window, driving keyboard_handler()/
 * mouse_handler() into driver state that hasn't been set up yet. */
void irq_set_mask(uint8_t irq) {
    uint64_t port;
    if (irq < 8) {
        port = 0x21;
    } else {
        port = 0xA1;
        irq -= 8;
    }
    outb(port, inb(port) | (uint8_t)(1 << irq));
}

void irq_clear_mask(uint8_t irq) {
    uint64_t port;
    if (irq < 8) {
        port = 0x21;
    } else {
        port = 0xA1;
        irq -= 8;
    }
    outb(port, inb(port) & (uint8_t)(~(1 << irq)));
}

/* Install a handler for a specific IRQ line */
void irq_install_handler(int irq, void (*handler)(struct regs *r)) {
    serial_puts("[IRQ] Installing handler for IRQ ");
    serial_putdec(irq);
    serial_puts("\n");

    /* Register at interrupt vector (IRQ 0 = INT 32, etc.) */
    register_interrupt_handler((uint8_t)(32 + irq), handler);

    /* Unmask the IRQ line in PIC */
    uint64_t port;
    uint8_t value;
    if (irq < 8) {
        port = 0x21;
    } else {
        port = 0xA1;
        irq -= 8;
    }
    value = inb(port) & (uint8_t)(~(1 << irq));
    outb(port, value);
}

/* Uninstall a handler for a specific IRQ line */
void irq_uninstall_handler(int irq) {
    /* Mask the IRQ line */
    uint64_t port;
    uint8_t value;
    if (irq < 8) {
        port = 0x21;
    } else {
        port = 0xA1;
        irq -= 8;
    }
    value = inb(port) | (uint8_t)(1 << irq);
    outb(port, value);
}

/* Initialize IRQ system */
static void ide_irq_handler(struct regs *regs)
{
    (void)regs;
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

/* Empty handler for unused IRQs to prevent crashes */
static void dummy_irq_handler(struct regs *r) {
    (void)r;
}

void irq_init(void) {
    serial_puts("[IRQ] Initializing IRQ system\n");

    irq_remap();

    irq_install_handler(0, timer_irq_handler);
    serial_puts("[IRQ] Timer handler registered at IRQ0\n");

    irq_install_handler(1, keyboard_handler);
    irq_install_handler(12, mouse_handler);
    irq_install_handler(14, ide_irq_handler);

    /* Install dummy handlers for unused IRQs to prevent crashes */
    irq_install_handler(2, dummy_irq_handler);  /* Cascade */
    irq_install_handler(3, dummy_irq_handler);  /* COM2 */
    irq_install_handler(4, dummy_irq_handler);  /* COM1 */
    irq_install_handler(5, dummy_irq_handler);  /* LPT2 */
    irq_install_handler(6, dummy_irq_handler);  /* Floppy */
    irq_install_handler(7, dummy_irq_handler);  /* LPT1 */
    irq_install_handler(8, dummy_irq_handler);  /* RTC */
    irq_install_handler(9, dummy_irq_handler);
    irq_install_handler(10, dummy_irq_handler);
    irq_install_handler(11, dummy_irq_handler);
    irq_install_handler(13, dummy_irq_handler);  /* FPU */
    irq_install_handler(15, dummy_irq_handler);  /* Secondary ATA */

    /* Mask unused IRQs. NOTE: IRQ2 is intentionally NOT in this list.
     * IRQ2 is the master PIC's cascade line to the slave PIC - it is not
     * a real device and must stay unmasked at all times, otherwise every
     * slave-PIC interrupt (IRQ8-15) is silently blocked from ever
     * reaching the CPU, including IRQ12 (mouse) and IRQ14 (IDE) which
     * are actively used. Masking it here was the actual reason mouse
     * interrupts never fired: irq_remap()/irq_install_handler() leave
     * IRQ2 unmasked by default, but this code was re-masking it as if
     * it were just another unused device line. */
    irq_set_mask(3);
    irq_set_mask(4);
    irq_set_mask(5);
    irq_set_mask(6);
    irq_set_mask(7);
    irq_set_mask(8);
    irq_set_mask(9);
    irq_set_mask(10);
    irq_set_mask(11);
    irq_set_mask(13);
    irq_set_mask(15);
    irq_clear_mask(2); /* ensure the cascade line is unmasked no matter
                         * what state the PIC came up in */

    /* irq_install_handler() just unmasked these two lines, but
     * keyboard_init()/minimal_mouse_init() haven't run yet - re-mask
     * them here so a stray/self-test byte from the PS/2 controller
     * can't reach keyboard_handler()/mouse_handler() before those
     * drivers' state actually exists. keyboard_init() and
     * minimal_mouse_init() each call irq_clear_mask() themselves once
     * they're done setting up. */
    irq_set_mask(1);
    irq_set_mask(12);

    serial_puts("[IRQ] Mouse handler registered at IRQ12\n");
    serial_puts("[IRQ] IRQ system initialized\n");
}

/* Keyboard IRQ wrapper - dispatches to the real keyboard driver */
void keyboard_handler(struct regs *r) {
    (void)r;
    /* Read scancode and process it via the keyboard driver */
    keyboard_interrupt_handler();
}

void mouse_handler(struct regs *r) {
    (void)r;
    minimal_mouse_interrupt_handler();
}

/* Handle pending IRQs (called from main kernel loop for polling fallback) */
void irq_handle_pending(void) {
    /* Deferred processing placeholder */
}
