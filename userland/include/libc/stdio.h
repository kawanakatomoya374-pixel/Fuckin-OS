#ifndef _COS_STDIO_H
#define _COS_STDIO_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
typedef struct __cos_libc_FILE FILE;
typedef long fpos_t;
#define EOF (-1)
#define BUFSIZ 1024
#define FILENAME_MAX 256
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
extern FILE *stdin, *stdout, *stderr;
int    printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    vsprintf(char *buf, const char *fmt, va_list ap);
int    vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int    sscanf(const char *s, const char *fmt, ...);
int    vsscanf(const char *s, const char *fmt, va_list ap);
int    puts(const char *s);
int    fputs(const char *s, FILE *f);
int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
int    putchar(int c);
int    fgetc(FILE *f);
int    getc(FILE *f);
int    getchar(void);
char  *fgets(char *buf, int n, FILE *f);
int    ungetc(int c, FILE *f);
FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread(void *p, size_t size, size_t n, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
int    fflush(FILE *f);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    fgetpos(FILE *f, fpos_t *pos);
int    fsetpos(FILE *f, const fpos_t *pos);
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);
int    remove(const char *path);
int    rename(const char *from, const char *to);
void   perror(const char *s);
_COS_END
#endif
