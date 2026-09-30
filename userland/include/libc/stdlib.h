#ifndef _COS_STDLIB_H
#define _COS_STDLIB_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
void  *malloc(size_t n);
void  *calloc(size_t n, size_t size);
void  *realloc(void *p, size_t n);
void   free(void *p);
void  *aligned_alloc(size_t align, size_t n);
_Noreturn void abort(void);
_Noreturn void exit(int status);
_Noreturn void _Exit(int status);
int    atexit(void (*fn)(void));
int    abs(int v); long labs(long v); long long llabs(long long v);
div_t  div(int a, int b); ldiv_t ldiv(long a, long b);
int    atoi(const char *s); long atol(const char *s); long long atoll(const char *s);
double atof(const char *s);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float  strtof(const char *s, char **end);
void   qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t n, size_t size,
               int (*cmp)(const void *, const void *));
int    rand(void);
void   srand(unsigned seed);
char  *getenv(const char *name);
int    system(const char *cmd);
_COS_END
#endif
