#include "baremetal/python_code_to_c_baremetal.h"

static size_t align_size(size_t size) {
    const size_t alignment = sizeof(void *);
    return (size + alignment - 1u) & ~(alignment - 1u);
}

static void *baremetal_alloc(size_t size, void *user) {
    P2C_BaremetalBoard *board = (P2C_BaremetalBoard*)user;
    size_t aligned = align_size(size);
    if (!board || aligned > board->heap_size - board->heap_used) return NULL;
    void *result = board->heap + board->heap_used;
    board->heap_used += aligned;
    return result;
}

static void *baremetal_realloc(void *ptr, size_t old_size, size_t new_size, void *user) {
    unsigned char *src = (unsigned char*)ptr;
    unsigned char *dst = (unsigned char*)baremetal_alloc(new_size, user);
    size_t copy_size = old_size < new_size ? old_size : new_size;
    if (!dst) return NULL;
    for (size_t i = 0; i < copy_size; i++) dst[i] = src[i];
    return dst;
}

static void baremetal_free(void *ptr, size_t size, void *user) {
    (void)ptr;
    (void)size;
    (void)user;
}

void p2c_baremetal_uart_write(const char *data, size_t len) {
    (void)data;
    (void)len;
}

static void baremetal_write(int stream, const char *data, size_t len, void *user) {
    P2C_BaremetalBoard *board = (P2C_BaremetalBoard*)user;
    (void)stream;
    if (!data) return;
    p2c_baremetal_uart_write(data, len);
    if (!board || !board->console || board->console_size == 0) return;
    if (len > board->console_size - 1u - board->console_used) len = board->console_size - 1u - board->console_used;
    for (size_t i = 0; i < len; i++) board->console[board->console_used + i] = data[i];
    board->console_used += len;
    board->console[board->console_used] = '\0';
}

static uint64_t baremetal_clock_ms(void *user) {
    P2C_BaremetalBoard *board = (P2C_BaremetalBoard*)user;
    return board ? board->ticks : 0;
}

void p2c_baremetal_board_init(P2C_BaremetalBoard *board, void *heap, size_t heap_size,
                              char *console, size_t console_size) {
    if (!board) return;
    board->heap = (unsigned char*)heap;
    board->heap_size = heap_size;
    board->heap_used = 0;
    board->console = console;
    board->console_size = console_size;
    board->console_used = 0;
    board->ticks = 0;
    if (console && console_size > 0) console[0] = '\0';
}

void p2c_baremetal_board_tick(P2C_BaremetalBoard *board, uint64_t elapsed_ms) {
    if (board) board->ticks += elapsed_ms;
}

P2C_Platform p2c_baremetal_platform(P2C_BaremetalBoard *board) {
    P2C_Platform platform;
    platform.alloc = baremetal_alloc;
    platform.realloc = baremetal_realloc;
    platform.free = baremetal_free;
    platform.write = baremetal_write;
    platform.clock_ms = baremetal_clock_ms;
    platform.user = board;
    return platform;
}

int p2c_baremetal_run(P2C_BaremetalBoard *board, P2C_BaremetalProgram program) {
    if (!board || !board->heap || !program) return 1;
    P2C_Platform platform = p2c_baremetal_platform(board);
    p2c_platform_set(&platform);
    p2c_runtime_init(board->heap, board->heap_size);
    P2C_GC_ENTER_MAIN();
    (void)program();
    p2c_runtime_shutdown();
    p2c_platform_set(NULL);
    return 0;
}

void p2c_platform_init(void) {
}

void p2c_platform_shutdown(void) {
}

void p2c_platform_write_n(const char *s, size_t len) {
    const P2C_Platform *platform = p2c_platform_current();
    if (platform && platform->write && s && len) platform->write(1, s, len, platform->user);
}

void p2c_platform_write(const char *s) {
    size_t len = 0;
    if (s) while (s[len] != '\0') len++;
    p2c_platform_write_n(s, len);
}

size_t p2c_platform_read_line(char *buf, size_t cap) {
    if (buf && cap > 0) buf[0] = '\0';
    return 0;
}

void p2c_platform_abort(const char *reason) {
    p2c_platform_write(reason);
    for (;;) {
    }
}
