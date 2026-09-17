/**
 * Memory compatibility helpers - C-OS 4.0.8 alpha
 *
 * This file keeps compiler/runtime glue minimal and logs when the fallback
 * path is exercised.
 */

#include "../include/serial.h"
#include "../include/types.h"

#if defined(__GNUC__)
#define WEAK __attribute__((weak))
#else
#define WEAK
#endif

static int memory_stub_warned = 0;

static void memory_stub_warn(const char* what) {
    if (!memory_stub_warned) {
        memory_stub_warned = 1;
        serial_puts("[MEMORY-FALLBACK] compatibility shim active\n");
    }
    serial_puts("[MEMORY-FALLBACK] ");
    serial_puts(what);
    serial_puts("\n");
}

static void memory_stub_itoa(unsigned long long value, unsigned base, char* out, size_t out_size) {
    const char* digits = "0123456789abcdef";
    char tmp[32];
    size_t i = 0, j = 0;
    if (!out || out_size == 0) return;
    if (value == 0) {
        if (out_size > 1) {
            out[0] = '0';
            out[1] = '\0';
        }
        return;
    }
    while (value && i < sizeof(tmp)) {
        tmp[i++] = digits[value % base];
        value /= base;
    }
    while (i && j + 1 < out_size) {
        out[j++] = tmp[--i];
    }
    out[j] = '\0';
}

void* WEAK memset(void* ptr, int c, size_t n) {
    unsigned char* p = (unsigned char*)ptr;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return ptr;
}

void* WEAK memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

int WEAK __mingw_snprintf(char* str, size_t size, const char* format, ...) {
    (void)format;
    if (!str || size == 0) {
        memory_stub_warn("__mingw_snprintf called with invalid buffer");
        return 0;
    }
    str[0] = '\0';
    memory_stub_warn("__mingw_snprintf fallback returned empty string");
    return 0;
}

void WEAK __chkstk_ms(void) {
    memory_stub_warn("__chkstk_ms");
}

void WEAK memory_stub_log_u64(const char* tag, uint64_t value) {
    char buf[32];
    memory_stub_itoa(value, 10, buf, sizeof(buf));
    serial_puts(tag ? tag : "[MEMORY]");
    serial_puts(": ");
    serial_puts(buf);
    serial_puts("\n");
}
