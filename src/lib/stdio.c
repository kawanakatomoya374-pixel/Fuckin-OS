/**
 * stdio.c - minimal printf/fprintf family for freestanding third-party
 * code (currently: QuickJS) that expects libc stdio to exist.
 *
 * There's no real file I/O behind FILE* here - quickjs.c's own
 * always-compiled code only ever prints short diagnostic/error
 * messages through these (the heavy debug-dump printf traffic found
 * while auditing quickjs.c - DUMP_BYTECODE, DUMP_GC_FREE, etc. - is
 * all behind compile-time #ifdefs that are off by default and were
 * confirmed not to be defined anywhere in this port). Routing
 * everything to the serial console - the same place every other
 * kernel subsystem's log output already goes - is the closest useful
 * equivalent to "standard output" C-OS has.
 */
#include "stdio.h"
#include "serial.h"
#include <stdarg.h>
#include <string.h>

static FILE g_stdin_obj, g_stdout_obj, g_stderr_obj;
FILE* const stdin  = &g_stdin_obj;
FILE* const stdout = &g_stdout_obj;
FILE* const stderr = &g_stderr_obj;

/* Formats into a stack buffer and writes it straight out - fine for
 * the short one-line diagnostics this is actually used for. Anything
 * that would overflow this gets truncated (vsnprintf's normal,
 * bounded, non-overflowing behaviour) rather than risking a stack
 * buffer overrun on a pathological format string. */
#define STDIO_SHIM_BUF_SIZE 1024

int vfprintf(FILE* stream, const char* format, va_list args) {
    (void)stream;
    char buf[STDIO_SHIM_BUF_SIZE];
    int n = vsnprintf(buf, sizeof(buf), format, args);
    serial_puts(buf);
    return n;
}

int vprintf(const char* format, va_list args) {
    return vfprintf(stdout, format, args);
}

int fprintf(FILE* stream, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int n = vfprintf(stream, format, args);
    va_end(args);
    return n;
}

int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int n = vfprintf(stdout, format, args);
    va_end(args);
    return n;
}

int putchar(int c) {
    char buf[2] = { (char)c, '\0' };
    serial_puts(buf);
    return c;
}
