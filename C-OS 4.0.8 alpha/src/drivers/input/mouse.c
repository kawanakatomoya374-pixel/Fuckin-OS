#include "types.h"
#include "mouse.h"
#include "serial.h"

void serial_puts(const char* s);
void gui_request_redraw(void) __attribute__((weak));

mouse_state_t mouse = {0};
static minimal_mouse_t mouse_state = {0};
static bool mouse_initialized = false; /* see minimal_mouse_interrupt_handler() */

#include "vga.h"
#include "io.h"
#include "irq.h"

static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[4];
static bool has_wheel = false;

static void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        while (timeout--) {
            if ((inb(0x64) & 1) == 1) return;
        }
    } else {
        while (timeout--) {
            if ((inb(0x64) & 2) == 0) return;
        }
    }
}

static void mouse_write(uint8_t data) {
    mouse_wait(1);
    outb(0x64, 0xD4);
    mouse_wait(1);
    outb(0x60, data);
}

static uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(0x60);
}

static void mouse_clamp_state(void) {
    if (mouse_state.x < 0) mouse_state.x = 0;
    if (mouse_state.y < 0) mouse_state.y = 0;
    if (SCREEN_W > 0 && (uint64_t)mouse_state.x >= SCREEN_W) {
        mouse_state.x = (int64_t)(SCREEN_W - 1);
    }
    if (SCREEN_H > 0 && (uint64_t)mouse_state.y >= SCREEN_H) {
        mouse_state.y = (int64_t)(SCREEN_H - 1);
    }
}

static void sync_global_mouse_state(void) {
    mouse.x = mouse_state.x;
    mouse.y = mouse_state.y;
    mouse.dx = mouse_state.dx;
    mouse.dy = mouse_state.dy;
    mouse.left = mouse_state.left;
    mouse.right = mouse_state.right;
    mouse.middle = mouse_state.middle;
    // Mirror the one-shot click/release flags exactly. These must reflect
    // false once minimal_mouse_clear_clicks() resets mouse_state, otherwise
    // a single click latches on forever (mouse.left_click can only ever
    // become true here and is never mirrored back to false).
    mouse.left_click = mouse_state.left_click;
    mouse.right_click = mouse_state.right_click;
    mouse.left_release = mouse_state.left_release;
    mouse.right_release = mouse_state.right_release;
    mouse.wheel = mouse_state.wheel;
}

static void mouse_reset_packet_state(void) {
    mouse_cycle = 0;
    mouse_packet[0] = 0;
    mouse_packet[1] = 0;
    mouse_packet[2] = 0;
}

static void mouse_apply_packet(const uint8_t bytes[4]) {
    uint8_t b0 = bytes[0];

    /* NOTE: this runs inside minimal_mouse_interrupt_handler(), an
     * interrupt-gate ISR where the CPU hardware-clears IF for the whole
     * duration of the call. serial_putc() busy-waits on the UART's
     * "transmit holding register empty" bit for every single character
     * it sends, so per-byte/per-packet logging here used to block all
     * other interrupts (including the 1kHz timer that drives
     * scheduler_tick()/preemption) for as long as the debug lines took
     * to transmit. During sustained mouse movement -- a steady stream of
     * back-to-back IRQ12 packets -- this starved every other thread,
     * including the one that redraws the cursor, for the entire time the
     * mouse was moving: the position kept updating correctly underneath,
     * but the screen only caught up once the movement (and therefore the
     * logging) stopped. Removed from the hot path for that reason. */

    if ((b0 & 0x80) || (b0 & 0x40)) {
        return; /* overflow */
    }

    int8_t x_rel = (int8_t)bytes[1];
    int8_t y_rel = (int8_t)bytes[2];
    int8_t z_rel = has_wheel ? (int8_t)bytes[3] : 0;

    mouse_state.dx = x_rel;
    mouse_state.dy = (int64_t)(-y_rel); /* PS/2 y is bottom-to-top */

    /* Wheel is an edge-like delta, not an absolute per-packet state.  A
     * motion packet with z=0 can arrive before gui_update() consumes a prior
     * wheel packet; assigning zero here silently lost real scroll events.
     * Accumulate only nonzero deltas until the GUI explicitly consumes and
     * clears mouse_state.wheel once per frame. Clamp defensively so a burst
     * cannot wrap the signed GUI-facing counter. */
    if (z_rel != 0) {
        int64_t next_wheel = mouse_state.wheel + (int64_t)z_rel;
        if (next_wheel > 120) next_wheel = 120;
        if (next_wheel < -120) next_wheel = -120;
        mouse_state.wheel = next_wheel;
    }

    mouse_state.x += mouse_state.dx;
    mouse_state.y += mouse_state.dy;

    mouse_clamp_state();

    bool new_left = (b0 & 1) != 0;
    bool new_right = (b0 & 2) != 0;
    bool new_middle = (b0 & 4) != 0;

    // Only set click flags if they're not already set
    // This preserves the flags until gui_handle_input() processes them
    if (!mouse_state.left_click) {
        mouse_state.left_click = new_left && !mouse_state.left;
    }
    if (!mouse_state.right_click) {
        mouse_state.right_click = new_right && !mouse_state.right;
    }
    if (!mouse_state.left_release) {
        mouse_state.left_release = !new_left && mouse_state.left;
    }
    if (!mouse_state.right_release) {
        mouse_state.right_release = !new_right && mouse_state.right;
    }

    /* Click latches are consumed by gui_handle_input() and cleared through
     * minimal_mouse_clear_clicks() after that frame.  Clearing them here on
     * the release packet races the GUI: a normal press/release gesture can
     * arrive entirely between frames, making right-click context menus (and
     * occasionally left clicks) invisible to the application.  Retain the
     * edge until the consumer explicitly acknowledges it. */

    mouse_state.left = new_left;
    mouse_state.right = new_right;
    mouse_state.middle = new_middle;

    sync_global_mouse_state();
    if (gui_request_redraw) gui_request_redraw();
}

void minimal_mouse_init(void) {
    mouse_state.x = (int64_t)(SCREEN_W ? SCREEN_W / 2 : 0);
    mouse_state.y = (int64_t)(SCREEN_H ? SCREEN_H / 2 : 0);
    mouse_state.dx = 0;
    mouse_state.dy = 0;
    mouse_state.left = false;
    mouse_state.right = false;
    mouse_state.middle = false;
    mouse_state.left_click = false;
    mouse_state.right_click = false;
    mouse_state.left_release = false;
    mouse_state.right_release = false;
    mouse_reset_packet_state();
    mouse_clamp_state();

    serial_puts("[MOUSE] Initializing PS/2 mouse...\n");

    uint8_t status;

    mouse_wait(1);
    outb(0x64, 0xA8); /* Enable auxiliary device */

    mouse_wait(1);
    outb(0x64, 0x20); /* Get command byte */
    mouse_wait(0);
    status = (uint8_t)(inb(0x60) | 2);

    mouse_wait(1);
    outb(0x64, 0x60); /* Set command byte */
    mouse_wait(1);
    outb(0x60, status);

    mouse_write(0xF6); /* Set default settings */
    (void)mouse_read();

    /* IntelliMouse wheel detection sequence */
    mouse_write(0xF3); (void)mouse_read(); mouse_write(200); (void)mouse_read();
    mouse_write(0xF3); (void)mouse_read(); mouse_write(100); (void)mouse_read();
    mouse_write(0xF3); (void)mouse_read(); mouse_write(80);  (void)mouse_read();
    mouse_write(0xF2); (void)mouse_read();
    uint8_t device_id = mouse_read();
    if (device_id == 3) {
        has_wheel = true;
        serial_puts("[MOUSE] IntelliMouse with wheel detected.\n");
    } else {
        serial_puts("[MOUSE] Standard PS/2 mouse detected.\n");
    }

    mouse_write(0xF4); /* Enable data reporting */
    (void)mouse_read();

    sync_global_mouse_state();
    mouse_initialized = true;
    irq_clear_mask(IRQ_MOUSE);
    serial_puts("[MOUSE] PS/2 mouse initialized.\n");
}

void minimal_mouse_poll(void) {
    /* Legacy compatibility shim. Input now arrives via IRQ12. */
    sync_global_mouse_state();
}

void minimal_mouse_interrupt_handler(void) {
    if (!mouse_initialized) {
        /* Not set up yet - still drain the controller output byte so
         * we don't leave it sitting there for the next real read, but
         * don't touch any driver state (mouse_state, mouse_cycle,
         * mouse_packet) that minimal_mouse_init() hasn't set up yet. */
        uint8_t status = inb(0x64);
        if ((status & 0x01) != 0) (void)inb(0x60);
        /* Do NOT send EOI here: _irq_handler() (idt.c) already EOIs both
         * PICs for IRQ12 (vector 44 >= 40) before calling this handler.
         * Sending it again is a spurious second EOI that can clear the
         * in-service bit of a different, still-nested interrupt (e.g. the
         * timer), which is what caused mouse interrupts to intermittently
         * stop being delivered under load. */
        return;
    }

    uint8_t status = inb(0x64);

    // Check if data is available
    if ((status & 0x01) == 0) {
        /* Spurious IRQ12 with nothing in the output buffer. _irq_handler()
         * already sent EOI to both PICs before calling us; do not send it
         * again here (see note above). */
        return;
    }

    // Check if this is mouse data (bit 5 = 1 means mouse data)
    if ((status & 0x20) == 0) {
        // This is keyboard data, not mouse data -- just drain it. EOI for
        // this IRQ was already sent by _irq_handler() before it called us.
        (void)inb(0x60);
        return;
    }

    uint8_t data = inb(0x60);

    /* No EOI here: _irq_handler() (idt.c) already EOI's both PICs for
     * IRQ12 (vector 44) unconditionally before dispatching to this
     * handler, for every single byte of the packet. Sending it again
     * from here was a spurious second EOI per byte -- harmless-looking
     * on its own, but a non-specific EOI clears whichever interrupt is
     * *currently* highest-priority in-service, which is not necessarily
     * this one if another IRQ (e.g. the timer) is nested on top of it.
     * That let a mouse burst quietly cancel a nested timer interrupt's
     * in-service state, and vice versa, producing exactly the kind of
     * "mouse randomly stops responding" symptom this driver was seeing. */

    if (mouse_cycle == 0 && (data & 0x08) == 0) {
        return; /* desync: wait for packet start */
    }

    mouse_packet[mouse_cycle++] = data;
    uint8_t packet_size = has_wheel ? 4 : 3;
    if (mouse_cycle < packet_size) {
        return;
    }

    mouse_cycle = 0;
    mouse_apply_packet(mouse_packet);
}

minimal_mouse_t* minimal_mouse_get_state(void) {
    return &mouse_state;
}

/* USB HID boot-mouse reports carry the same shape as a PS/2 packet
 * (button bitmask + relative dx/dy[+wheel]) once translated by the
 * caller, so this reuses mouse_apply_packet()'s edge-detection and
 * global-state sync instead of duplicating it. Unlike
 * minimal_mouse_interrupt_handler(), this runs from usb_poll() in
 * ordinary polled context, not an ISR - and PS/2 IRQ12 can still
 * fire in between, touching the same mouse_state/has_wheel this
 * function does, so the update is wrapped in the same save-EFLAGS/
 * cli/.../restore-EFLAGS critical section task_reap_zombies() uses
 * for the same reason (nests safely regardless of the caller's
 * current interrupt state, unlike an unconditional cli+sti pair).
 * left/right/middle follow HID's standard bit0/1/2 order, matching
 * PS/2's byte0 layout, so no bit-remapping is needed. */
void mouse_apply_usb_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    if (!mouse_initialized) return;
    uint8_t bytes[4];
    bytes[0] = buttons & 0x07;
    bytes[1] = (uint8_t)dx;
    bytes[2] = (uint8_t)(-dy); /* mouse_apply_packet negates y like PS/2's bottom-to-top convention */
    bytes[3] = (uint8_t)wheel;
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    bool had_wheel = has_wheel;
    has_wheel = true; /* USB boot mice always carry a wheel byte */
    mouse_apply_packet(bytes);
    has_wheel = had_wheel;
    __asm__ volatile("push %0\n\tpopfq" :: "r"(flags) : "memory", "cc");
}

void minimal_mouse_clear_clicks(void) {
    mouse_state.left_click = false;
    mouse_state.right_click = false;
    mouse_state.left_release = false;
    mouse_state.right_release = false;
    // Don't sync to global state here - let gui_update() handle that
    // to avoid clearing flags before gui_handle_input() can process them
}

mouse_state_t* mouse_get_state(void) {
    return (mouse_state_t*)&mouse_state;
}
