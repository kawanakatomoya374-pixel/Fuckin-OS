/**
 * stdlib.c - see stdlib.h. The allocator functions are thin wrappers
 * over this kernel's own kmalloc/krealloc/kfree (kernel/mm/memory.c) -
 * QuickJS is also given those directly through its own pluggable
 * JSMallocFunctions interface (see quickjs_port.c), so in practice the
 * malloc/realloc/free names here only matter for whatever handful of
 * call sites in quickjs.c/libregexp.c/libunicode.c reach for the bare
 * libc name instead of going through JS_NewRuntime2()'s allocator -
 * both paths end up at the same kmalloc arena either way.
 */
#include "stdlib.h"
#include "memory.h"
#include "serial.h"
#include <string.h>

void* malloc(size_t size) {
    return kmalloc(size);
}

void* calloc(size_t nmemb, size_t size) {
    /* Overflow-check the multiply before allocating - a strict
     * requirement of real calloc(), and cheap insurance here since
     * nmemb/size both come from third-party code we're not auditing
     * line-by-line. */
    if (nmemb != 0 && size > (size_t)-1 / nmemb) return NULL;
    size_t total = nmemb * size;
    void* p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void* realloc(void* ptr, size_t size) {
    return krealloc(ptr, size);
}

void free(void* ptr) {
    kfree(ptr);
}

int abs(int n) { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }
long long llabs(long long n) { return n < 0 ? -n : n; }

/* Parses the decimal/scientific-notation syntax QuickJS's own lexer
 * has already validated a numeric token as (this is only ever called
 * on lexer-confirmed digit/./e/sign/digit sequences - see the call
 * site in quickjs.c's tokenizer - never on arbitrary/untrusted
 * strings), so there's no need to handle malformed input beyond not
 * crashing on it. Accumulates the mantissa as an integer and scales
 * by a power of ten at the end; this is accurate for the digit counts
 * JS source literals realistically contain, but - being a from-
 * scratch implementation with no reference test vectors available in
 * this environment - hasn't been checked against a correctly-rounded
 * reference implementation for last-bit accuracy on adversarial
 * inputs. Good enough to unblock the port; worth a closer look if
 * anyone ever hits number-literal precision issues in practice. */
double strtod(const char* nptr, char** endptr) {
    const char* p = nptr;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    bool neg = false;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }

    double mantissa = 0.0;
    bool any_digits = false;
    while (*p >= '0' && *p <= '9') {
        mantissa = mantissa * 10.0 + (double)(*p - '0');
        p++;
        any_digits = true;
    }

    int frac_digits = 0;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            mantissa = mantissa * 10.0 + (double)(*p - '0');
            frac_digits++;
            p++;
            any_digits = true;
        }
    }

    if (!any_digits) {
        if (endptr) *endptr = (char*)nptr;
        return 0.0;
    }

    int exp = -frac_digits;
    if (*p == 'e' || *p == 'E') {
        const char* exp_start = p;
        p++;
        bool exp_neg = false;
        if (*p == '+' || *p == '-') { exp_neg = (*p == '-'); p++; }
        if (*p >= '0' && *p <= '9') {
            int e = 0;
            while (*p >= '0' && *p <= '9') { e = e * 10 + (*p - '0'); p++; }
            exp += exp_neg ? -e : e;
        } else {
            /* "e"/"e+" with no digits after it isn't a valid exponent -
             * back up, the exponent letter (and any sign) belongs to
             * whatever comes after this number, not to us. */
            p = exp_start;
        }
    }

    double result = mantissa;
    if (exp > 0) {
        for (int i = 0; i < exp; i++) result *= 10.0;
    } else if (exp < 0) {
        for (int i = 0; i < -exp; i++) result /= 10.0;
    }

    if (endptr) *endptr = (char*)p;
    return neg ? -result : result;
}

void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*)) {
    const uint8_t* lo = (const uint8_t*)base;
    size_t count = nmemb;
    while (count > 0) {
        size_t mid = count / 2;
        const uint8_t* candidate = lo + mid * size;
        int cmp = compar(key, candidate);
        if (cmp == 0) return (void*)candidate;
        if (cmp > 0) {
            lo = candidate + size;
            count = count - mid - 1;
        } else {
            count = mid;
        }
    }
    return NULL;
}

/* No process model to unwind - both halt the kernel outright rather
 * than returning, matching QuickJS's own expectation that these never
 * return control to the caller. Deliberately self-contained (doesn't
 * call into kernel.c's kernel_fatal(), which is file-static) so this
 * shim has no dependency on kernel boot-order internals. */
static void cos_libc_halt(const char* reason) {
    serial_puts("[LIBC] ");
    serial_puts(reason);
    serial_puts(" - halting\n");
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void abort(void) {
    cos_libc_halt("abort() called");
}

void exit(int status) {
    (void)status;
    cos_libc_halt("exit() called");
}

void cos_assert_fail(const char* expr, const char* file, int line) {
    serial_puts("[LIBC] assertion failed: ");
    serial_puts(expr);
    serial_puts(" at ");
    serial_puts(file);
    serial_puts(":");
    char linebuf[16];
    utoa((uint64_t)line, linebuf, 10);
    serial_puts(linebuf);
    serial_puts("\n");
    cos_libc_halt("assert() failed");
}
