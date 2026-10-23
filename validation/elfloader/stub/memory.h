#ifndef STUB_MEMORY_H
#define STUB_MEMORY_H
#include <stddef.h>
void *kmalloc(size_t n);
void  kfree(void *p);
#endif
