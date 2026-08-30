typedef struct __FILE FILE;
#include <stdarg.h>
#include <stdlib.h>

FILE *stderr = (FILE *)0;
FILE *stdout = (FILE *)0;
FILE *stdin  = (FILE *)0;

int printf(const char *fmt, ...) {
    (void)fmt;
    return 0;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    (void)stream;
    (void)fmt;
    return 0;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    (void)stream;
    (void)fmt;
    (void)ap;
    return 0;
}

int fputc(int ch, FILE *stream) {
    (void)stream;
    return ch;
}


void exit(int status) {
    (void)status;
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
