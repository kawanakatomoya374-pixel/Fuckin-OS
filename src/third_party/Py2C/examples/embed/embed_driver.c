/*
 * Kernel-style driver for a transpiled module (see embed_boot.py).
 *
 * It plays the role of a hobby OS task entry: it owns a fixed heap, the task
 * stack window, the UART/console and the clock, then calls the generated
 * entry point.  Nothing else from the project's platform layer is linked:
 * -DP2C_EMBED_PROVIDE_PLATFORM_COMPAT makes p2c_embed supply the compatibility
 * hooks, and -DP2C_EMBED_PROVIDE_LIBC_HEAP routes malloc/free to the embed heap
 * so that memory released by the GC is actually reusable.
 *
 * The UART hook writes to file descriptor 1 on the host so that
 * `make test-embed-generated` can diff the output against CPython; on a kernel
 * it would poke the real UART.
 */
#include "platform/python_code_to_c_embed.h"
#include "host_stack_bounds.h"

extern P2C_Object *p2c_embed_program(void);
extern long write(int fd, const void *buf, unsigned long count);

#define HEAP_STORAGE_SIZE (48u * 1024u)
#define CONSOLE_SIZE 4096u

static unsigned char heap_storage[HEAP_STORAGE_SIZE + 1u];
static char console_buffer[CONSOLE_SIZE];
static P2C_EmbedHeap boot_heap;
static unsigned long uart_bytes;

static void uart_write(void *user, const char *data, size_t len) {
    (void)user;
    if (!data || len == 0) return;
    uart_bytes += len;
    (void)write(1, data, (unsigned long)len);
}

static void kernel_panic(const char *reason, void *user) {
    (void)user;
    static const char prefix[] = "kernel panic: ";
    (void)write(1, prefix, sizeof(prefix) - 1u);
    for (const char *p = reason; p && *p; p++) (void)write(1, p, 1u);
    (void)write(1, "\n", 1u);
    for (;;) { } /* カーネルではここで停止する */
}

static uint64_t kernel_clock(void *user) {
    (void)user;
    return 0; /* 実機ではタイマのミリ秒カウンタを返す */
}

static size_t kernel_input(char *buf, size_t cap, void *user) {
    (void)user;
    if (buf && cap > 0) buf[0] = '\0';
    return 0; /* 実機ではUART受信行を返す */
}

int main(void) {
    /* malloc を置き換えるため、他のどの確保よりも先にヒープを用意する。 */
    (void)p2c_embed_heap_init(&boot_heap, heap_storage, sizeof(heap_storage));
    p2c_embed_heap_set_default(&boot_heap);

    void *stack_lo = NULL;
    void *stack_hi = NULL;
    p2c_host_stack_bounds(&stack_lo, &stack_hi);
    if (!stack_lo || !stack_hi) return 10;

    P2C_EmbedConfig cfg;
    p2c_embed_config_init(&cfg);
    p2c_embed_config_use_uart(&cfg, uart_write, NULL);
    p2c_embed_config_use_console(&cfg, console_buffer, sizeof(console_buffer));
    p2c_embed_config_use_heap(&cfg, &boot_heap, heap_storage, sizeof(heap_storage));
    p2c_embed_config_use_stack(&cfg, stack_lo, stack_hi);
    cfg.clock_ms = kernel_clock;
    cfg.read_line = kernel_input;
    cfg.panic = kernel_panic;
    cfg.gc_threshold = 8u * 1024u; /* 小さめにして収集を実際に走らせる */

    if (p2c_embed_start(&cfg) != 0) return 11;
    if (!p2c_embed_run_program(p2c_embed_program)) return 12;

    P2C_EmbedStats stats;
    p2c_embed_stats(&stats);
    if (!stats.gc_scan_available) return 13;
    if (stats.gc_collections == 0) return 14;
    if (stats.alloc_failures != 0) return 15;
    if (p2c_embed_heap_check(&boot_heap) != 0) return 16;
    if (uart_bytes == 0) return 17;
    if (uart_bytes != p2c_embed_console_len()) return 18;
    p2c_embed_stop();
    if (p2c_embed_is_active()) return 19;
    return 0;
}
