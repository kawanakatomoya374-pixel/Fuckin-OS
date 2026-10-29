#include "io.h"
#include "serial.h"

#define SERIAL_PORT 0x3F8

static volatile int g_serial_verbose;
int serial_is_verbose(void) { return g_serial_verbose; }

void serial_init(void) {
#ifdef COS_SERIAL_VERBOSE
    g_serial_verbose = 1;      /* make SERIAL_VERBOSE=1: the development/test build logs everything; releases do not (see k_chatter below) */
#endif
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x80);
    /* Divisor latch: 115200 / divisor = baud.
     *
     * This was 3, i.e. 38400 baud, i.e. ~260us per byte transmitted.
     * serial_putc() below busy-waits for the transmit holding register,
     * so every logged byte is a quarter of a millisecond of spinning -
     * a 45-character diagnostic line costs ~12ms. That is not a
     * background cost: several hot paths log while holding interrupts
     * disabled (see the note on serial_diag_*), so the whole system
     * stops for the duration.
     *
     * Divisor 1 is 115200 baud, the fastest the standard 16550 divisor
     * scheme reaches and what every host terminal and QEMU's -serial
     * default already expect. Three times cheaper for free. */
    outb(SERIAL_PORT + 0, 0x01);
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03);
    outb(SERIAL_PORT + 2, 0xC7);
    outb(SERIAL_PORT + 4, 0x0B);
}

void serial_putc(char c) {
    while (!(inb(SERIAL_PORT + 5) & 0x20));
    outb(SERIAL_PORT, c);
}

/* Several CPUs log: one call's text stays together. Interrupts are off while the lock is held, so a handler on the same CPU cannot wait on its own CPU. */
static volatile int g_serial_lock;
static inline uint64_t serial_lock(void) {
    uint64_t f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    while (__atomic_exchange_n(&g_serial_lock, 1, __ATOMIC_ACQUIRE)) { while (g_serial_lock) __asm__ volatile("pause"); }
    return f;
}
static inline void serial_unlock(uint64_t f) {
    __atomic_store_n(&g_serial_lock, 0, __ATOMIC_RELEASE);
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
}

/* Hot-path chatter. Every byte is a busy wait on the UART (and the lines are mirrored by VMs and serial-to-file setups, which are slower still), and Firefox
 * produces thousands of these lines (a demand-page line per 256 faults per process, a fork line, the periodic statistics). They are dropped unless the file
 * /system/serial_verbose exists (serial_set_verbose, called at boot): start of a line decides, the rest of the line follows it. Everything else is logged as before. */
static const char *const k_chatter[] = {
    "[TASK] demand page", "[LINUX] fork copied", "[TERMINAL] out:", "[PCACHE]", "[LOCK]", "[MEM]", "[GFXPERF]", "[GUIPERF]", "[SCHED-DBG]", "[MEMPERF]", "[NSPERF]", "[PF]   resolved", "[PF] RIP:", 0
};
static int g_line_start = 1, g_line_drop;
void serial_set_verbose(int on) { g_serial_verbose = on; }

static int chatter_line(const char *s) {
    for (int i = 0; k_chatter[i]; ++i) {
        const char *t = k_chatter[i], *u = s;
        while (*t && *u == *t) { ++t; ++u; }
        if (!*t) return 1;
    }
    return 0;
}
static void serial_put_filtered(char c) {                  /* the caller holds the serial lock */
    if (g_line_drop) { if (c == '\n') { g_line_drop = 0; g_line_start = 1; } return; }
    serial_putc(c); g_line_start = (c == '\n');
}

void serial_puts(const char* s) {
    uint64_t f = serial_lock();
    if (!g_serial_verbose && g_line_start && !g_line_drop && chatter_line(s)) g_line_drop = 1;
    if (g_serial_verbose) { while (*s) serial_putc(*s++); }
    else while (*s) serial_put_filtered(*s++);
    serial_unlock(f);
}

void serial_puthex(uint64_t val) {
    const char* hex = "0123456789ABCDEF";
    uint64_t f = serial_lock();
    if (!g_serial_verbose && g_line_drop) { serial_unlock(f); return; }
    g_line_start = 0;
    for (int i = 60; i >= 0; i -= 4) {
        serial_putc(hex[(val >> i) & 0xF]);
    }
    serial_unlock(f);
}

void serial_putdec(uint64_t val) {
    if (!g_serial_verbose && g_line_drop) return;
    g_line_start = 0;
    if (val == 0) {
        serial_putc('0');
        return;
    }
    
    /* UINT64_MAX (18446744073709551615) is 20 digits - this buffer must
     * hold the worst case. It was previously only 11 bytes, which any
     * caller passing a negative int (implicitly converted to a huge
     * unsigned value close to UINT64_MAX) would overflow by up to 9
     * bytes, smashing adjacent stack contents including, in the crash
     * this fixes, the return address itself. */
    char buffer[20];
    int i = 0;
    
    while (val > 0 && i < (int)sizeof(buffer)) {
        buffer[i++] = '0' + (val % 10);
        val /= 10;
    }
    
    while (i > 0) {
        serial_putc(buffer[--i]);
    }
}

char serial_getc(void) {
    while (!(inb(SERIAL_PORT + 5) & 0x01));
    return inb(SERIAL_PORT);
}

/* ====================================================================
 * Rate-limited diagnostic channel.
 *
 * WHY THIS EXISTS
 * ---------------
 * serial_putc() busy-waits on the UART. Even at 115200 baud that is
 * ~87us per byte, so a 45-character diagnostic line costs ~4ms of
 * spinning. Several hot paths were logging unconditionally:
 *
 *   - kmalloc() printed "[MEMPERF] long single walk" whenever a
 *     first-fit walk exceeded 400 blocks, from INSIDE its
 *     sync_irq_save() critical section. A fragmented heap - which is
 *     exactly what a browser produces - makes that fire on allocation
 *     after allocation, each one stopping the machine with interrupts
 *     off.
 *
 *   - cos_netsurf_browser_poll() printed "[NSPERF] poll" whenever a
 *     poll exceeded 25ms. That poll runs every GUI frame, and the log
 *     itself costs milliseconds, so a slow frame guaranteed the next
 *     frame was also slow enough to log. A self-sustaining slowdown
 *     that gets worse the heavier the page is - which is why browsing
 *     made the whole OS crawl rather than just being slow itself.
 *
 * Both are genuinely useful diagnostics, so the answer is not to delete
 * them but to stop them being able to dominate the workload they are
 * measuring. A token bucket caps output at a handful of lines per
 * second no matter how often the condition trips, and the channel is
 * off by default so a normal boot pays nothing at all.
 *
 * Deliberately NOT a ring buffer flushed later: a diagnostic that
 * appears only if the machine survives to flush it is useless for
 * diagnosing the machine not surviving.
 * ==================================================================== */

/* Millisecond clock, provided by the timer. Declared rather than
 * included to keep serial.c free of a dependency on timer.h - this file
 * is linked into very early boot paths where the timer is not up yet,
 * and get_timer_ticks() returns 0 there, which the logic below treats
 * as "no time has passed" and therefore rate-limits normally. */
extern uint64_t get_timer_ticks(void);

static int      serial_diag_on = 0;     /* off by default */
static uint64_t serial_diag_tokens = 8;
static uint64_t serial_diag_last_ms = 0;

#define SERIAL_DIAG_MAX_TOKENS   8ULL   /* burst allowance             */
#define SERIAL_DIAG_REFILL_MS    250ULL /* one token per quarter second */

void serial_diag_enable(int on)
{
    serial_diag_on = on ? 1 : 0;
}

int serial_diag_enabled(void)
{
    return serial_diag_on;
}

/* Returns non-zero if the caller may emit one diagnostic line now.
 *
 * Callers must gate the WHOLE line on a single call - checking once per
 * serial_puts() would let a multi-part message be truncated halfway,
 * which is worse than dropping it entirely. */
int serial_diag_allow(void)
{
    if (!serial_diag_on) return 0;

    uint64_t now = get_timer_ticks();
    if (now > serial_diag_last_ms) {
        uint64_t elapsed = now - serial_diag_last_ms;
        uint64_t refill = elapsed / SERIAL_DIAG_REFILL_MS;
        if (refill > 0) {
            serial_diag_tokens += refill;
            if (serial_diag_tokens > SERIAL_DIAG_MAX_TOKENS) {
                serial_diag_tokens = SERIAL_DIAG_MAX_TOKENS;
            }
            serial_diag_last_ms = now;
        }
    } else if (now < serial_diag_last_ms) {
        /* Clock went backwards (or was reset). Re-anchor rather than
         * letting the subtraction above produce a huge refill. */
        serial_diag_last_ms = now;
    }

    if (serial_diag_tokens == 0) return 0;
    --serial_diag_tokens;
    return 1;
}
