/* pthread on top of SYS_PAR compute jobs.
 *
 * A thread is a compute job on an application processor with its own arena (see cos.h), so it cannot make system
 * calls: every wait is a spin on a word in shared memory. The main thread (which can) yields while it spins.
 * That is enough for the decoders' worker pools (dav1d, edge264): their threads only compute and wait on mutexes
 * and condition variables.
 *
 * pthread_cond_t is a sequence counter. A waiter notes the counter before releasing the mutex and spins until it
 * changes, so a signal sent after it released the mutex cannot be lost; every wake-up is "spurious" in the sense
 * that callers must re-check their predicate, as POSIX already requires. */
#include "pthread.h"
#include "cos_codec_runtime.h"

/* Sizes for new threads; applications set these before the decoder creates its pool. */
size_t cos_pthread_arena_heap  = 96u << 20;
size_t cos_pthread_arena_stack = 2u << 20;

typedef struct cos_thread {
    void *(*fn)(void *);
    void *arg, *ret;
    volatile int done;
    cos_arena_t *arena;
    int job;
} cos_thread_t;

static inline void cpu_wait(unsigned *n) {
    if (cos_idle_work()) return;      /* a spinning core does queued work instead of nothing */
    __builtin_ia32_pause();
    /* the main thread can yield its core to others; a worker has no system calls */
    if (((++*n) & 0xFFu) == 0 && !cos_in_worker()) cos_yield();
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void)a; if (m) *m = 0; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m) {
    unsigned n = 0;
    while (__atomic_exchange_n(m, 1, __ATOMIC_ACQUIRE)) while (__atomic_load_n(m, __ATOMIC_RELAXED)) cpu_wait(&n);
    return 0;
}
int pthread_mutex_unlock(pthread_mutex_t *m) { __atomic_store_n(m, 0, __ATOMIC_RELEASE); return 0; }

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { (void)a; if (c) *c = 0; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    int seen = __atomic_load_n(c, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(m);
    unsigned n = 0;
    while (__atomic_load_n(c, __ATOMIC_ACQUIRE) == seen) cpu_wait(&n);
    return pthread_mutex_lock(m);
}
int pthread_cond_signal(pthread_cond_t *c) { __atomic_fetch_add(c, 1, __ATOMIC_ACQ_REL); return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { __atomic_fetch_add(c, 1, __ATOMIC_ACQ_REL); return 0; }

int pthread_once(pthread_once_t *once, void (*fn)(void)) {
    int expected = 0;
    if (__atomic_compare_exchange_n(once, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (fn) fn();
        __atomic_store_n(once, 2, __ATOMIC_RELEASE);
    } else {
        unsigned n = 0;
        while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != 2) cpu_wait(&n);
    }
    return 0;
}

static void thread_entry(void *p) {
    cos_thread_t *t = (cos_thread_t *)p;
    t->ret = t->fn(t->arg);
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
}

int pthread_create(pthread_t *th, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    (void)a;
    if (th) *th = 0;
    if (cos_in_worker() || cos_par_cpus() < 1) return -1;          /* threads are created by the main thread */
    cos_thread_t *t = (cos_thread_t *)cos_calloc(1, sizeof *t);
    if (!t) return -1;
    t->fn = fn; t->arg = arg; t->job = -1;
    t->arena = cos_arena_create(cos_pthread_arena_heap, cos_pthread_arena_stack);
    if (!t->arena) { cos_puts("[PTHREAD] arena create failed\n"); cos_free(t); return -1; }
    void *stk; size_t ss; cos_arena_stack(t->arena, &stk, &ss);
    t->job = cos_par_start(thread_entry, t, stk, ss);
    if (t->job < 0) { {char b[64];cos_snprintf(b,sizeof b,"[PTHREAD] par_start failed: %d\n",t->job);cos_puts(b);} cos_arena_destroy(t->arena); cos_free(t); return -1; }
    if (th) *th = (pthread_t)(uintptr_t)t;
    return 0;
}

int pthread_join(pthread_t th, void **ret) {
    cos_thread_t *t = (cos_thread_t *)(uintptr_t)th;
    if (!t) return -1;
    unsigned n = 0;
    while (!__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) {
        int st = cos_par_poll(t->job, NULL);
        if (st >= COS_PAR_DONE) break;                              /* faulted or stopped */
        cpu_wait(&n);
    }
    for (int i = 0; i < 2000; i++) { if (cos_par_poll(t->job, NULL) >= COS_PAR_DONE) break; cos_sleep_ms(1); }
    cos_par_free(t->job);
    cos_arena_destroy(t->arena);
    if (ret) *ret = t->ret;
    cos_free(t);
    return 0;
}

/* Stops the thread now (it may be in the middle of anything); pthread_join() then reclaims it. */
int pthread_cancel(pthread_t th) {
    cos_thread_t *t = (cos_thread_t *)(uintptr_t)th;
    if (!t) return 0;
    cos_par_stop(t->job);
    /* The caller does not join a cancelled thread (edge264 frees its decoder right after), so reclaim it here. */
    for (int i = 0; i < 2000; i++) { if (cos_par_poll(t->job, NULL) >= COS_PAR_DONE) break; cos_sleep_ms(1); }
    cos_par_free(t->job);
    cos_arena_destroy(t->arena);
    cos_free(t);
    return 0;
}
pthread_t pthread_self(void) { return 0; }
int pthread_setname_np(pthread_t t, const char *name) { (void)t; (void)name; return 0; }
int pthread_attr_init(pthread_attr_t *a) { if (a) *a = 0; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) { (void)a; (void)size; return 0; }
void cos_assert_fail(const char *expr, const char *file, int line) {
    (void)file; (void)line;
    cos_codec_abort(expr);
}
