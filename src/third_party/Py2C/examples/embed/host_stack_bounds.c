/*
 * Host-only helper: report the current thread's stack bounds so the embedded
 * tests can declare them via p2c_embed_config_use_stack().
 *
 * This file is deliberately compiled WITHOUT PYTHON_CODE_TO_C_NO_STDLIB: it is
 * the "kernel" side of the boundary, i.e. what a hobby OS provides from its
 * task structure (task->stack_base / task->stack_size).
 */
#define _GNU_SOURCE
#include "host_stack_bounds.h"
#include <pthread.h>

void p2c_host_stack_bounds(void **stack_lo, void **stack_hi) {
    if (stack_lo) *stack_lo = NULL;
    if (stack_hi) *stack_hi = NULL;
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return;
    void *addr = NULL;
    size_t size = 0;
    pthread_attr_getstack(&attr, &addr, &size);
    pthread_attr_destroy(&attr);
    if (!addr || size == 0) return;
    if (stack_lo) *stack_lo = addr;
    if (stack_hi) *stack_hi = (char*)addr + size;
}
