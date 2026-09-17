#ifndef COS_STDIO_H
#define COS_STDIO_H

#include <stdarg.h>
#include "types.h"

/* Opaque - nothing dereferences the fields, this only exists so
 * `FILE*` has a type and stdout/stderr have something to point at.
 * All output goes to the kernel serial console regardless of which
 * of these is passed in - see the printf family's doc comment in
 * stdio.c for why that's the right simplification here. */
typedef struct FILE { int unused; } FILE;

extern FILE* const stdin;
extern FILE* const stdout;
extern FILE* const stderr;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int printf(const char* format, ...);
int fprintf(FILE* stream, const char* format, ...);
int vprintf(const char* format, va_list args);
int vfprintf(FILE* stream, const char* format, va_list args);
int putchar(int c);
/* snprintf/vsnprintf/sprintf are real functions (lib/string.c owns
 * the implementation - shared with string.h, which many ported files
 * expect them from instead), declared identically here too since
 * plenty of real-world code reaches for them via <stdio.h> per the
 * usual POSIX convention instead. */
int snprintf(char* str, size_t size, const char* format, ...);
int vsnprintf(char* str, size_t size, const char* format, va_list args);
int sprintf(char* str, const char* format, ...);
/* No real buffering to flush - every write already goes straight to
 * the serial console (see the FILE* comment above) - but plenty of
 * ported code calls this defensively after writes, so it's a
 * real, harmless no-op rather than left undefined. */
int fflush(FILE* stream);

#endif
