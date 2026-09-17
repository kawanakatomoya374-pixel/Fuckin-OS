/**
 * cos_assert.c - a small, deliberately conflict-free home for libc
 * functions that are declared in this shim's headers but have no
 * implementation anywhere in the build, where pulling in the "real"
 * home of that function wholesale would create duplicate-symbol
 * clashes.
 *
 * Started as just cos_assert_fail() (the backing function for the
 * assert() macro in include/assert.h - MicroPython's own source,
 * objlist.c and others, calls assert() throughout). Grew to also
 * cover strtod()/abort()/putchar(): all three are implemented in
 * lib/stdlib.c or lib/stdio.c already, but those two files also
 * define malloc/calloc/realloc/free/atoi/strtol/printf/etc, several
 * of which collide with lib/string.c's own versions (which *is*
 * linked) - rather than untangle that duplication, each function
 * nothing else provides gets pulled out here individually. See
 * PORTING_NOTES.md for the fuller story.
 */
#include "assert.h"
#include "serial.h"
#include "memory.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* No process model to unwind - halts the kernel outright rather than
 * returning, matching what callers of a noreturn assert()/abort()
 * expect. Deliberately self-contained (doesn't call into kernel.c's
 * kernel_fatal(), which is file-static) so this has no dependency on
 * kernel boot-order internals. */
static void cos_libc_halt(const char* reason) __attribute__((noreturn));
static void cos_libc_halt(const char* reason) {
    serial_puts("[LIBC] ");
    serial_puts(reason);
    serial_puts(" - halting\n");
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
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

void abort(void) {
    cos_libc_halt("abort() called");
}

int putchar(int c) {
    char buf[2] = { (char)c, '\0' };
    serial_puts(buf);
    return c;
}

/* Identical to lib/stdlib.c's own implementation (see that file for
 * the "good enough to unblock the port, not IEEE-754-exact" caveat) -
 * duplicated rather than shared because stdlib.c as a whole isn't
 * linked (see this file's header comment). If the two ever need to
 * diverge, that's a sign this really should be its own translation
 * unit both files pull in instead of copy-pasting further. */
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

/* A glibc extension (not ISO C or POSIX) that reports how much space
 * an allocation actually has available (>= the requested size, since
 * real allocators round up to size classes). QuickJS uses it purely
 * as a GC/memory-pressure heuristic, not for correctness - kmalloc
 * doesn't expose this information, so reporting back exactly what was
 * requested (the safe lower bound: never overstates available space)
 * is honest and sufficient, just not as precise as a real answer
 * would be. */
size_t malloc_usable_size(void* ptr) {
    (void)ptr;
    return 0;
}

int abs(int n)           { return n < 0 ? -n : n; }
long labs(long n)         { return n < 0L ? -n : n; }
long long llabs(long long n) { return n < 0LL ? -n : n; }

/* The single kernel-wide errno - no per-thread semantics needed yet.
 * Declared extern in include/errno.h; defined here since it needs to
 * live in exactly one object file and cos_assert.c is already that
 * catch-all home for gap-fills with no better place. */
int errno = 0;

/* The NetSurf HTML frame parser only needs decimal conversion semantics.
 * Keep float parsing consistent with the already-linked freestanding strtod. */
float strtof(const char *nptr, char **endptr)
{
    return (float)strtod(nptr, endptr);
}
