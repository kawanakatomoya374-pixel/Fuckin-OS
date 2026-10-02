#include "platform/python_code_to_c_platform.h"

#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static void *host_alloc(size_t size, void *user) {
    (void)user;
    return malloc(size);
}

static void *host_realloc(void *ptr, size_t old_size, size_t new_size, void *user) {
    (void)old_size;
    (void)user;
    return realloc(ptr, new_size);
}

static void host_free(void *ptr, size_t size, void *user) {
    (void)size;
    (void)user;
    free(ptr);
}

static void host_write(int stream, const char *data, size_t len, void *user) {
    (void)user;
    FILE *f = stream == 2 ? stderr : stdout;
    (void)fwrite(data, 1, len, f);
}

static uint64_t host_clock_ms(void *user) {
    (void)user;
    return (uint64_t)(clock() * 1000 / (CLOCKS_PER_SEC ? CLOCKS_PER_SEC : 1));
}

static const P2C_Platform default_platform = {
    host_alloc, host_realloc, host_free, host_write, host_clock_ms, NULL
};
#else
static void *unconfigured_alloc(size_t size, void *user) {
    (void)size;
    (void)user;
    return NULL;
}

static void *unconfigured_realloc(void *ptr, size_t old_size, size_t new_size, void *user) {
    (void)ptr;
    (void)old_size;
    (void)new_size;
    (void)user;
    return NULL;
}

static void unconfigured_free(void *ptr, size_t size, void *user) {
    (void)ptr;
    (void)size;
    (void)user;
}

static void unconfigured_write(int stream, const char *data, size_t len, void *user) {
    (void)stream;
    (void)data;
    (void)len;
    (void)user;
}

static uint64_t unconfigured_clock_ms(void *user) {
    (void)user;
    return 0;
}

static const P2C_Platform default_platform = {
    unconfigured_alloc, unconfigured_realloc, unconfigured_free, unconfigured_write, unconfigured_clock_ms, NULL
};
#endif

static const P2C_Platform *current_platform = &default_platform;

const P2C_Platform *p2c_platform_default(void) {
    return &default_platform;
}

void p2c_platform_set(const P2C_Platform *platform) {
    current_platform = platform ? platform : &default_platform;
}

const P2C_Platform *p2c_platform_current(void) {
    return current_platform;
}

/* アロケータだけを差し替えるためのプラットフォーム。write/clock_ms は
 * 既定プラットフォームの実装を引き継ぐ（コンソール出力を壊さない）。 */
static P2C_Platform alloc_only_platform;

void p2c_platform_set_allocator(void *(*alloc_fn)(size_t size, void *user),
                                void *(*realloc_fn)(void *ptr, size_t old_size, size_t new_size, void *user),
                                void (*free_fn)(void *ptr, size_t size, void *user),
                                void *user) {
    if (!alloc_fn || !realloc_fn || !free_fn) { p2c_platform_set(NULL); return; }
    const P2C_Platform *base = p2c_platform_default();
    alloc_only_platform.alloc    = alloc_fn;
    alloc_only_platform.realloc  = realloc_fn;
    alloc_only_platform.free     = free_fn;
    alloc_only_platform.write    = base->write;
    alloc_only_platform.clock_ms = base->clock_ms;
    alloc_only_platform.user     = user;
    p2c_platform_set(&alloc_only_platform);
}
