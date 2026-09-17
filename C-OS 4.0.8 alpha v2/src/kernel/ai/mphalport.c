#include <string.h>
#include "py/mpconfig.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "../../include/serial.h"
#include "../../include/timer.h"
#include "../../include/memory.h"

// MicroPython HAL implementation

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        serial_putc(str[i]);
    }
    return (mp_uint_t)len;
}

int mp_hal_stdin_rx_chr(void) {
    return (int)serial_getc();
}

mp_uint_t mp_hal_ticks_ms(void) {
    return (mp_uint_t)get_timer_ticks();
}

void mp_hal_delay_ms(mp_uint_t ms) {
    uint64_t start = get_timer_ticks();
    while (get_timer_ticks() - start < ms) {
        __asm__ volatile ("pause");
    }
}


void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
    mp_hal_stdout_tx_strn(str, len);
}

void mp_hal_delay_us(mp_uint_t us) {
    uint64_t start = get_timer_ticks();
    while ((get_timer_ticks() - start) < us) {
        __asm__ volatile ("pause");
    }
}

mp_uint_t mp_hal_ticks_us(void) {
    return (mp_uint_t)get_timer_ticks();
}


// Memory management for MicroPython (GC)
static char mp_heap[128 * 1024] __attribute__((aligned(4096))); // 128KB for Python heap

void *mp_hal_get_heap_start(void) {
    return mp_heap;
}

void *mp_hal_get_heap_end(void) {
    return mp_heap + sizeof(mp_heap);
}

