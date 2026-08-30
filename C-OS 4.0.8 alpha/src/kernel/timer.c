/**
 * timer.c - Programmable Interval Timer Implementation
 * C-OS 4.0.8 alpha Timer Management
 */
#include "timer.h"
#include "io.h"
#include "serial.h"
#include "idt.h"

#include "irq.h"
#include "scheduler.h"

// Timer frequency (Hz) - 1000Hz for smooth 60FPS rendering
#define TIMER_FREQ 1000

// Timer tick counter
static volatile uint64_t timer_ticks = 0;
static volatile uint64_t timer_seconds = 0;

// Timer callback functions
static timer_callback_t timer_callbacks[TIMER_MAX_CALLBACKS];
static uint64_t timer_callback_intervals[TIMER_MAX_CALLBACKS];
static uint64_t timer_callback_next[TIMER_MAX_CALLBACKS];

// Set timer frequency
static void timer_phase(uint64_t hz) {
    uint64_t divisor = 1193180 / hz;
    
    // Send command to PIT
    outb(0x43, 0x36);  // Channel 0, lobyte/hibyte, mode 3
    
    // Send divisor
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

// Timer tick handler (called from IRQ wrapper)
void timer_tick(void) {
    timer_ticks++;
    /* Serial tick log removed for performance (1000Hz would flood serial) */
    
    // Update seconds counter
    if (timer_ticks % TIMER_FREQ == 0) {
        timer_seconds++;
    }
    
    // Process timer callbacks
    for (int i = 0; i < TIMER_MAX_CALLBACKS; i++) {
        if (timer_callbacks[i] && timer_ticks >= timer_callback_next[i]) {
            timer_callbacks[i]();
            timer_callback_next[i] = timer_ticks + timer_callback_intervals[i];
        }
    }
    
    // Call scheduler tick
    scheduler_tick();
}

// Install timer callback
int timer_install_callback(timer_callback_t callback, uint64_t interval_ms) {
    for (int i = 0; i < TIMER_MAX_CALLBACKS; i++) {
        if (!timer_callbacks[i]) {
            timer_callbacks[i] = callback;
            timer_callback_intervals[i] = (interval_ms * TIMER_FREQ) / 1000;
            timer_callback_next[i] = timer_ticks + timer_callback_intervals[i];
            return i;
        }
    }
    return -1;  // No free slots
}

// Uninstall timer callback
void timer_uninstall_callback(int id) {
    if (id >= 0 && id < TIMER_MAX_CALLBACKS) {
        timer_callbacks[id] = NULL;
    }
}

// Get timer ticks
uint64_t get_timer_ticks(void) {
    return timer_ticks;
}

// Get timer seconds
uint64_t get_timer_seconds(void) {
    return timer_seconds;
}

// Wait for specified number of ticks
void timer_wait(uint64_t ticks) {
    uint64_t start = get_timer_ticks();
    uint64_t last = start;
    uint64_t guard = ticks ? (ticks * 250000ULL + 250000ULL) : 250000ULL;

    while (get_timer_ticks() - start < ticks) {
        uint64_t now = get_timer_ticks();

        // If the PIT/tick source is alive, keep using it.
        if (now != last) {
            last = now;
            continue;
        }

        // If ticks are not advancing, never deadlock inside HLT.
        if (guard == 0) {
            break;
        }
        guard--;

        for (int i = 0; i < 256; ++i) {
            __asm__ volatile("pause");
        }
    }
}

// Wait for specified number of milliseconds
void timer_wait_ms(uint64_t ms) {
    uint64_t ticks = (ms * TIMER_FREQ) / 1000;
    timer_wait(ticks);
}

// Sleep for specified number of milliseconds
void timer_sleep(uint64_t ms) {
    scheduler_sleep(ms);
}

// Initialize timer
void timer_init(void) {
    serial_puts("[TIMER] Initializing Programmable Interval Timer\n");
    
    // Clear callback array
    for (int i = 0; i < TIMER_MAX_CALLBACKS; i++) {
        timer_callbacks[i] = NULL;
        timer_callback_intervals[i] = 0;
        timer_callback_next[i] = 0;
    }
    
    // Reset counters
    timer_ticks = 0;
    timer_seconds = 0;
    
    // Set timer frequency
    timer_phase(TIMER_FREQ);
    
    serial_puts("[TIMER] Timer initialized at ");
    serial_putdec(TIMER_FREQ);
    serial_puts(" Hz\n");
}

// Update timer (called from main loop)
void timer_update(void) {
    // Timer is updated by interrupt handler
    // This function can be used for any periodic maintenance
}

// HAL timer abstraction wrappers
uint64_t hal_timer_get_ticks(void) {
    return get_timer_ticks();
}

uint64_t hal_timer_get_ms(void) {
    return (get_timer_ticks() * 1000) / TIMER_FREQ;
}

uint64_t hal_timer_get_seconds(void) {
    return get_timer_seconds();
}

int hal_timer_set_frequency(uint64_t frequency) {
    (void)frequency;
    // Not implemented - PIT frequency is fixed
    return 0;
}

void hal_timer_delay_ms(uint64_t ms) {
    timer_wait_ms(ms);
}

void hal_timer_delay_us(uint64_t us) {
    // Approximate delay
    uint64_t ticks = (us * TIMER_FREQ) / 1000000;
    if (ticks == 0) ticks = 1;
    timer_wait(ticks);
}

uint32_t tusb_time_millis_api(void) {
    return (uint32_t)hal_timer_get_ms();
}
