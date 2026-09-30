#ifndef COS_STDLIB_H
#define COS_STDLIB_H

#include "types.h"

void* malloc(size_t size);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
void  free(void* ptr);

int   abs(int n);
long  labs(long n);
long long llabs(long long n);

double strtod(const char* nptr, char** endptr);
float  strtof(const char* nptr, char** endptr);

void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*));

/* No real process model to exit from - both route to the same
 * kernel halt as assert()/cos_assert_fail() (see assert.h). QuickJS's
 * core engine only calls abort() on an internal invariant failure
 * (out-of-memory paths return NULL/throw rather than aborting); exit()
 * is a CLI-only (qjs.c) concept not reachable from the embedded
 * engine, but is provided in case a future binding path needs it. */
void abort(void) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));

/* Same story: nothing to register against, since nothing ever calls
 * exit(). Real, harmless no-op (always "succeeds") rather than left
 * undefined, since ported code (e.g. talloc's cleanup registration)
 * calls this defensively without checking whether it's meaningful in
 * a given environment. */
int atexit(void (*func)(void));

#endif
