/* tinycthread.h - the few C11 thread calls Craft uses, on top of C-OS threads (cos_thread_create, futex mutex/condvar). */
#ifndef CRAFT_TINYCTHREAD_H
#define CRAFT_TINYCTHREAD_H
#include <stdint.h>
#include <stdlib.h>
#include "cos.h"

typedef struct { int64_t id; } thrd_t;
typedef cos_mutex_t mtx_t;
typedef cos_cond_t cnd_t;
typedef int (*thrd_start_t)(void *);
enum { mtx_plain = 0 };
enum { thrd_success = 0, thrd_error = 2 };

typedef struct { thrd_start_t fn; void *arg; } thrd_boot_t;
static void thrd_trampoline(void *p) {
    thrd_boot_t b = *(thrd_boot_t *)p; free(p);
    cos_thread_exit(b.fn(b.arg));
}
static inline int thrd_create(thrd_t *t, thrd_start_t fn, void *arg) {
    thrd_boot_t *b = (thrd_boot_t *)malloc(sizeof *b); if (!b) return thrd_error;
    b->fn = fn; b->arg = arg;
    t->id = cos_thread_create(thrd_trampoline, b);
    if (t->id <= 0) { free(b); return thrd_error; }
    return thrd_success;
}
static inline int mtx_init(mtx_t *m, int type) { (void)type; cos_mutex_init(m); return thrd_success; }
static inline int mtx_lock(mtx_t *m) { return cos_mutex_lock(m); }
static inline int mtx_unlock(mtx_t *m) { return cos_mutex_unlock(m); }
static inline int cnd_init(cnd_t *c) { cos_cond_init(c); return thrd_success; }
static inline int cnd_wait(cnd_t *c, mtx_t *m) { return cos_cond_wait(c, m); }
static inline int cnd_signal(cnd_t *c) { return cos_cond_signal(c); }
#endif
