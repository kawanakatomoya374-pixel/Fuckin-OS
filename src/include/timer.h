#ifndef TIMER_H
#define TIMER_H

#include "types.h"

// Timer frequency
#define TIMER_FREQUENCY 1193180
#define TIMER_DIVISOR 100

// Timer callback type
typedef void (*timer_callback_t)(void);

// Timer settings
#define TIMER_MAX_CALLBACKS 10

// Called by the IRQ wrapper on each PIT tick
void timer_tick(void);

// Get timer ticks
uint64_t get_timer_ticks(void);

// Wait for specified number of ticks
void timer_wait(uint64_t ticks);

// Initialize timer
void timer_init(void);

// Update timer
void timer_update(void);

#endif

#ifndef TIMER_TICKS_PER_SEC
#define TIMER_TICKS_PER_SEC 1000
#endif
