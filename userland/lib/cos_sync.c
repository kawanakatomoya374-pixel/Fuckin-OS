/**
 * cos_sync.c - thread synchronisation for .c-os programs.
 *
 * WHAT WAS MISSING
 * ----------------
 * `cos_thread_create()` existed. Nothing to synchronise those threads
 * with existed: no mutex, no condition variable, no semaphore, not even
 * a documented atomic. A program with two threads touching the same data
 * had no correct way to be written, and the obvious workaround - a
 * spin loop around a volatile flag - is both wrong (no memory ordering)
 * and actively harmful on a single-core schedule, where the spinner
 * holds the CPU that the thread it is waiting for needs in order to make
 * progress.
 *
 * THE DESIGN
 * ----------
 * Every primitive here follows the same shape: the state is a 32-bit
 * word in the program's own memory, manipulated with atomic
 * instructions, and the kernel is only entered when a thread genuinely
 * has to sleep. An uncontended lock or unlock is one `lock cmpxchg` and
 * no syscall.
 *
 * The mutex is the three-state design (0 free, 1 held, 2 held with
 * waiters) rather than the two-state one. The difference is that
 * unlocking a two-state mutex cannot tell whether anyone is waiting, so
 * it must issue a wake syscall every time - paying the ring transition
 * on every unlock in a program that never contends. With three states,
 * the unlock path only calls the kernel when the word says someone is
 * actually parked.
 *
 * SPURIOUS WAKEUPS
 * ----------------
 * Every wait here is inside a loop that re-checks its condition. That is
 * not defensive padding: the futex is allowed to return early (a hash
 * bucket is shared between futexes, so an unrelated wake can reach this
 * waiter), and a condition variable is specified to allow it regardless.
 * Code that waits once and assumes the condition holds is wrong on every
 * threading platform, not just this one.
 */
#include "cos.h"

/* Atomics via the compiler's builtins rather than hand-written asm: the
 * builtins emit the same instructions and, unlike inline asm, also tell
 * the OPTIMISER about the ordering, so it cannot hoist a load of the
 * protected data above the lock acquisition. */
#define ATOMIC_LOAD(p)            __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ATOMIC_STORE(p, v)        __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define ATOMIC_XCHG(p, v)         __atomic_exchange_n((p), (v), __ATOMIC_ACQ_REL)
#define ATOMIC_ADD(p, v)          __atomic_fetch_add((p), (v), __ATOMIC_ACQ_REL)
#define ATOMIC_SUB(p, v)          __atomic_fetch_sub((p), (v), __ATOMIC_ACQ_REL)
#define ATOMIC_CAS(p, exp, des)   __atomic_compare_exchange_n(              \
                                      (p), (exp), (des), false,             \
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)

/* ---- mutex ---------------------------------------------------------------
 * 0 = free, 1 = held with no waiters, 2 = held and at least one waiter
 * is (or was) parked in the kernel. */

void cos_mutex_init(cos_mutex_t *m) { m->state = 0; m->owner = 0; }

/* Spinning briefly before sleeping is worth it because the syscall pair
 * costs far more than a short critical section takes to finish. It is
 * kept SHORT and ends in a yield rather than a longer spin: on a
 * single-core schedule, spinning past a few dozen iterations cannot
 * help, because the holder needs this CPU to make progress. */
#define COS_SPIN_TRIES 64

int cos_mutex_trylock(cos_mutex_t *m)
{
    uint32_t expected = 0;
    if (ATOMIC_CAS(&m->state, &expected, 1u)) {
        m->owner = (uint64_t)cos_gettid();
        return 0;
    }
    return -1;
}

int cos_mutex_lock(cos_mutex_t *m)
{
    uint32_t expected = 0;
    if (ATOMIC_CAS(&m->state, &expected, 1u)) {
        m->owner = (uint64_t)cos_gettid();
        return 0;
    }

    for (int i = 0; i < COS_SPIN_TRIES; ++i) {
        if (ATOMIC_LOAD(&m->state) == 0) {
            expected = 0;
            if (ATOMIC_CAS(&m->state, &expected, 1u)) {
                m->owner = (uint64_t)cos_gettid();
                return 0;
            }
        }
        __builtin_ia32_pause();
    }

    /* Slow path. The exchange to 2 both claims the lock (if it was free)
     * and records that a waiter exists (if it was not) in one operation.
     * Doing it as a separate load and store would reopen the lost-wakeup
     * race the futex compare exists to close. */
    while (ATOMIC_XCHG(&m->state, 2u) != 0) {
        (void)cos_futex_wait(&m->state, 2u, 0);
        /* No error check: every futex outcome - woken, timed out,
         * EAGAIN because the word changed underneath - means the same
         * thing here, which is "look again". */
    }
    m->owner = (uint64_t)cos_gettid();
    return 0;
}

int cos_mutex_timedlock(cos_mutex_t *m, uint64_t timeout_ms)
{
    if (cos_mutex_trylock(m) == 0) return 0;

    uint64_t deadline = cos_time_ms() + timeout_ms;
    while (ATOMIC_XCHG(&m->state, 2u) != 0) {
        uint64_t now = cos_time_ms();
        if (now >= deadline) return -1;
        (void)cos_futex_wait(&m->state, 2u, deadline - now);
    }
    m->owner = (uint64_t)cos_gettid();
    return 0;
}

int cos_mutex_unlock(cos_mutex_t *m)
{
    m->owner = 0;
    /* If the state was 2, someone is parked and must be woken. If it was
     * 1, nobody is, and no syscall happens at all - which is the entire
     * reason for the third state. */
    if (ATOMIC_XCHG(&m->state, 0u) == 2u) {
        cos_futex_wake(&m->state, 1);
    }
    return 0;
}

/* ---- condition variable --------------------------------------------------
 * A sequence counter, not a flag. The waiter reads the counter, drops
 * the mutex, and waits for the counter to CHANGE; a signal increments it
 * and wakes. Using a flag instead loses a signal that arrives between
 * the unlock and the wait, because a flag has no way to say "something
 * happened while you were not looking" - the counter does. */

void cos_cond_init(cos_cond_t *c) { c->seq = 0; c->waiters = 0; }

int cos_cond_wait(cos_cond_t *c, cos_mutex_t *m)
{
    return cos_cond_timedwait(c, m, 0);
}

int cos_cond_timedwait(cos_cond_t *c, cos_mutex_t *m, uint64_t timeout_ms)
{
    uint32_t seen = ATOMIC_LOAD(&c->seq);
    ATOMIC_ADD(&c->waiters, 1);

    /* The mutex MUST be released before sleeping and reacquired after,
     * and the sequence number MUST be read before releasing it. Reading
     * it after would leave a window in which a signal is delivered and
     * missed - which is the classic lost-wakeup, and the reason
     * pthread_cond_wait takes the mutex as an argument at all. */
    cos_mutex_unlock(m);

    int rc = 0;
    int64_t r = cos_futex_wait(&c->seq, seen, timeout_ms);
    if (r == -110 /* ETIMEDOUT */) rc = -1;

    ATOMIC_SUB(&c->waiters, 1);
    cos_mutex_lock(m);
    return rc;
}

int cos_cond_signal(cos_cond_t *c)
{
    ATOMIC_ADD(&c->seq, 1);
    if (ATOMIC_LOAD(&c->waiters) > 0) cos_futex_wake(&c->seq, 1);
    return 0;
}

int cos_cond_broadcast(cos_cond_t *c)
{
    ATOMIC_ADD(&c->seq, 1);
    if (ATOMIC_LOAD(&c->waiters) > 0) cos_futex_wake(&c->seq, 0x7fffffff);
    return 0;
}

/* ---- semaphore ---------------------------------------------------------- */

void cos_sem_init(cos_sem_t *s, uint32_t value) { s->count = value; }

int cos_sem_trywait(cos_sem_t *s)
{
    uint32_t v = ATOMIC_LOAD(&s->count);
    while (v > 0) {
        if (ATOMIC_CAS(&s->count, &v, v - 1)) return 0;
        /* CAS wrote the observed value back into v, so the loop retries
         * against what is actually there rather than re-reading. */
    }
    return -1;
}

int cos_sem_wait(cos_sem_t *s)
{
    for (;;) {
        if (cos_sem_trywait(s) == 0) return 0;
        /* Waits for the count to stop being zero. Passing 0 as the
         * expected value is what makes this safe against a post()
         * landing in the window: if the count already changed, the
         * kernel returns EAGAIN immediately instead of parking. */
        (void)cos_futex_wait(&s->count, 0u, 0);
    }
}

int cos_sem_timedwait(cos_sem_t *s, uint64_t timeout_ms)
{
    uint64_t deadline = cos_time_ms() + timeout_ms;
    for (;;) {
        if (cos_sem_trywait(s) == 0) return 0;
        uint64_t now = cos_time_ms();
        if (now >= deadline) return -1;
        (void)cos_futex_wait(&s->count, 0u, deadline - now);
    }
}

int cos_sem_post(cos_sem_t *s)
{
    ATOMIC_ADD(&s->count, 1);
    cos_futex_wake(&s->count, 1);
    return 0;
}

/* ---- reader/writer lock --------------------------------------------------
 * One word: the low 30 bits count active readers, bit 30 means a writer
 * holds it, bit 31 means a writer is waiting.
 *
 * The writer-waiting bit is what stops writer starvation. Without it a
 * steady stream of readers can hold the lock indefinitely and a writer
 * never acquires it - which looks like a hang and is very hard to
 * diagnose, because every individual operation is behaving correctly. */

#define RW_WRITER   (1u << 30)
#define RW_WAITING  (1u << 31)
#define RW_READERS  (RW_WRITER - 1u)

void cos_rwlock_init(cos_rwlock_t *l) { l->state = 0; }

int cos_rwlock_rdlock(cos_rwlock_t *l)
{
    for (;;) {
        uint32_t v = ATOMIC_LOAD(&l->state);
        /* New readers stand aside for a waiting writer. */
        if (!(v & (RW_WRITER | RW_WAITING))) {
            uint32_t want = v + 1;
            if ((want & RW_READERS) == 0) return -1;   /* reader count overflow */
            if (ATOMIC_CAS(&l->state, &v, want)) return 0;
            continue;
        }
        (void)cos_futex_wait(&l->state, v, 0);
    }
}

int cos_rwlock_tryrdlock(cos_rwlock_t *l)
{
    uint32_t v = ATOMIC_LOAD(&l->state);
    if (v & (RW_WRITER | RW_WAITING)) return -1;
    return ATOMIC_CAS(&l->state, &v, v + 1) ? 0 : -1;
}

int cos_rwlock_wrlock(cos_rwlock_t *l)
{
    for (;;) {
        uint32_t v = ATOMIC_LOAD(&l->state);
        if (v == 0) {
            uint32_t zero = 0;
            if (ATOMIC_CAS(&l->state, &zero, RW_WRITER)) return 0;
            continue;
        }
        /* Announce the intent before sleeping, so readers arriving from
         * now on queue behind rather than in front. */
        if (!(v & RW_WAITING)) {
            uint32_t want = v | RW_WAITING;
            if (!ATOMIC_CAS(&l->state, &v, want)) continue;
            v = want;
        }
        (void)cos_futex_wait(&l->state, v, 0);
    }
}

int cos_rwlock_trywrlock(cos_rwlock_t *l)
{
    uint32_t zero = 0;
    return ATOMIC_CAS(&l->state, &zero, RW_WRITER) ? 0 : -1;
}

int cos_rwlock_unlock(cos_rwlock_t *l)
{
    uint32_t v = ATOMIC_LOAD(&l->state);
    if (v & RW_WRITER) {
        ATOMIC_STORE(&l->state, 0u);
        cos_futex_wake(&l->state, 0x7fffffff);
        return 0;
    }
    uint32_t prev = ATOMIC_SUB(&l->state, 1);
    /* The last reader out wakes whoever is waiting - and only the last
     * one, so a busy read-heavy workload does not issue a syscall per
     * unlock. */
    if ((prev & RW_READERS) == 1 && (prev & RW_WAITING)) {
        ATOMIC_STORE(&l->state, 0u);
        cos_futex_wake(&l->state, 0x7fffffff);
    }
    return 0;
}

/* ---- one-time initialisation --------------------------------------------
 * 0 = not started, 1 = in progress, 2 = done.
 *
 * The in-progress state matters: a second thread arriving mid-init must
 * WAIT for the initialiser to finish, not skip past it and use
 * half-built state. A plain "if (!done) { init(); done = 1; }" gets that
 * wrong in a way that only shows up under load. */

int cos_once(cos_once_t *o, void (*fn)(void))
{
    uint32_t v = ATOMIC_LOAD(&o->state);
    if (v == 2) return 0;

    uint32_t zero = 0;
    if (ATOMIC_CAS(&o->state, &zero, 1u)) {
        fn();
        ATOMIC_STORE(&o->state, 2u);
        cos_futex_wake(&o->state, 0x7fffffff);
        return 0;
    }

    while (ATOMIC_LOAD(&o->state) != 2) {
        (void)cos_futex_wait(&o->state, 1u, 0);
    }
    return 0;
}
