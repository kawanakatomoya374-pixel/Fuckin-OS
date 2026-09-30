/* Concurrency tests for cos_sync.c, run with real host threads against
 * the real Linux futex (see futex_host.c).
 *
 * These are stress tests, not unit tests, because a lock that is subtly
 * wrong passes every single-threaded assertion. What finds a missing
 * memory barrier or a lost wakeup is many threads hammering the same
 * word until the window is hit.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "cos.h"

extern int  g_sync_checks, g_sync_failures;
int g_sync_checks, g_sync_failures;

#define SCHECK(cond, msg, ...) do { g_sync_checks++;                       \
    if (!(cond)) { g_sync_failures++;                                      \
        fprintf(stderr, "FAIL [sync] " msg "\n", ##__VA_ARGS__); } } while (0)

#define NTHREADS 8
#define NITERS   20000

static cos_mutex_t  m_lock;
static long         m_counter;
static long         m_unprotected;

static void *mutex_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < NITERS; ++i) {
        cos_mutex_lock(&m_lock);
        m_counter++;
        /* A second, deliberately racy counter. If the mutex works this
         * one still races (it is incremented non-atomically inside the
         * lock, so it should actually agree) - the point is that any
         * divergence between the two proves the critical section was
         * entered concurrently. */
        long t = m_unprotected;
        t++;
        m_unprotected = t;
        cos_mutex_unlock(&m_lock);
    }
    return NULL;
}

static void test_mutex(void)
{
    cos_mutex_init(&m_lock);
    m_counter = 0; m_unprotected = 0;

    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; ++i) pthread_create(&th[i], NULL, mutex_worker, NULL);
    for (int i = 0; i < NTHREADS; ++i) pthread_join(th[i], NULL);

    SCHECK(m_counter == (long)NTHREADS * NITERS,
           "mutex lost increments: %ld, want %d", m_counter, NTHREADS * NITERS);
    SCHECK(m_unprotected == (long)NTHREADS * NITERS,
           "mutual exclusion was violated: %ld, want %d",
           m_unprotected, NTHREADS * NITERS);

    /* trylock must fail while held and succeed when free. */
    cos_mutex_lock(&m_lock);
    SCHECK(cos_mutex_trylock(&m_lock) == -1, "trylock succeeded on a held mutex");
    cos_mutex_unlock(&m_lock);
    SCHECK(cos_mutex_trylock(&m_lock) == 0, "trylock failed on a free mutex");
    cos_mutex_unlock(&m_lock);

    /* timedlock must actually time out rather than hang. */
    cos_mutex_lock(&m_lock);
    uint64_t t0 = cos_time_ms();
    int rc = cos_mutex_timedlock(&m_lock, 100);
    uint64_t dt = cos_time_ms() - t0;
    SCHECK(rc == -1, "timedlock returned success on a held mutex");
    SCHECK(dt >= 90 && dt < 2000, "timedlock waited %llu ms, expected ~100",
           (unsigned long long)dt);
    cos_mutex_unlock(&m_lock);
}

/* ---- condition variable: a bounded producer/consumer queue ---- */
static cos_mutex_t q_lock;
static cos_cond_t  q_notempty, q_notfull;
static int q_buf[16], q_head, q_tail, q_count;
static long q_produced, q_consumed;
#define QCAP 16
#define QITEMS 20000

static void *producer(void *arg)
{
    long n = (long)(intptr_t)arg;
    for (long i = 0; i < n; ++i) {
        cos_mutex_lock(&q_lock);
        while (q_count == QCAP) cos_cond_wait(&q_notfull, &q_lock);
        q_buf[q_tail] = 1;
        q_tail = (q_tail + 1) % QCAP;
        q_count++;
        q_produced++;
        cos_cond_signal(&q_notempty);
        cos_mutex_unlock(&q_lock);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    long n = (long)(intptr_t)arg;
    for (long i = 0; i < n; ++i) {
        cos_mutex_lock(&q_lock);
        while (q_count == 0) cos_cond_wait(&q_notempty, &q_lock);
        q_head = (q_head + 1) % QCAP;
        q_count--;
        q_consumed++;
        cos_cond_signal(&q_notfull);
        cos_mutex_unlock(&q_lock);
    }
    return NULL;
}

static void test_cond(void)
{
    cos_mutex_init(&q_lock);
    cos_cond_init(&q_notempty);
    cos_cond_init(&q_notfull);
    q_head = q_tail = q_count = 0;
    q_produced = q_consumed = 0;

    pthread_t p[4], c[4];
    for (int i = 0; i < 4; ++i)
        pthread_create(&p[i], NULL, producer, (void *)(intptr_t)(QITEMS / 4));
    for (int i = 0; i < 4; ++i)
        pthread_create(&c[i], NULL, consumer, (void *)(intptr_t)(QITEMS / 4));
    for (int i = 0; i < 4; ++i) pthread_join(p[i], NULL);
    for (int i = 0; i < 4; ++i) pthread_join(c[i], NULL);

    SCHECK(q_produced == QITEMS && q_consumed == QITEMS,
           "producer/consumer stalled or lost items: produced %ld consumed %ld",
           q_produced, q_consumed);
    SCHECK(q_count == 0, "queue left %d items behind", q_count);

    /* timedwait must time out when nothing signals. */
    cos_mutex_lock(&q_lock);
    uint64_t t0 = cos_time_ms();
    int rc = cos_cond_timedwait(&q_notempty, &q_lock, 100);
    uint64_t dt = cos_time_ms() - t0;
    cos_mutex_unlock(&q_lock);
    SCHECK(rc == -1, "cond_timedwait did not report a timeout");
    SCHECK(dt >= 90 && dt < 2000, "cond_timedwait waited %llu ms",
           (unsigned long long)dt);
}

/* ---- semaphore ---- */
static cos_sem_t s_sem;
static long s_count;
static cos_mutex_t s_lock;

static void *sem_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 5000; ++i) {
        cos_sem_wait(&s_sem);
        cos_mutex_lock(&s_lock);
        s_count++;
        cos_mutex_unlock(&s_lock);
        cos_sem_post(&s_sem);
    }
    return NULL;
}

static void test_sem(void)
{
    cos_sem_init(&s_sem, 3);
    cos_mutex_init(&s_lock);
    s_count = 0;

    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; ++i) pthread_create(&th[i], NULL, sem_worker, NULL);
    for (int i = 0; i < NTHREADS; ++i) pthread_join(th[i], NULL);

    SCHECK(s_count == (long)NTHREADS * 5000, "semaphore lost work: %ld", s_count);

    cos_sem_init(&s_sem, 0);
    SCHECK(cos_sem_trywait(&s_sem) == -1, "trywait succeeded on an empty semaphore");
    cos_sem_post(&s_sem);
    SCHECK(cos_sem_trywait(&s_sem) == 0, "trywait failed after a post");
    uint64_t t0 = cos_time_ms();
    SCHECK(cos_sem_timedwait(&s_sem, 100) == -1, "sem_timedwait did not time out");
    SCHECK(cos_time_ms() - t0 >= 90, "sem_timedwait returned too early");
}

/* ---- rwlock ---- */
static cos_rwlock_t rw;
static long rw_shared;
static volatile int rw_readers_seen_writer;
static volatile int rw_writer_active;

static void *rw_reader(void *arg)
{
    (void)arg;
    for (int i = 0; i < 20000; ++i) {
        cos_rwlock_rdlock(&rw);
        /* If a writer is in its critical section while a reader is in
         * its own, the lock is broken. */
        if (rw_writer_active) rw_readers_seen_writer = 1;
        (void)rw_shared;
        cos_rwlock_unlock(&rw);
    }
    return NULL;
}

static void *rw_writer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 4000; ++i) {
        cos_rwlock_wrlock(&rw);
        rw_writer_active = 1;
        rw_shared++;
        rw_writer_active = 0;
        cos_rwlock_unlock(&rw);
    }
    return NULL;
}

static void test_rwlock(void)
{
    cos_rwlock_init(&rw);
    rw_shared = 0; rw_readers_seen_writer = 0; rw_writer_active = 0;

    pthread_t r[6], w[2];
    for (int i = 0; i < 6; ++i) pthread_create(&r[i], NULL, rw_reader, NULL);
    for (int i = 0; i < 2; ++i) pthread_create(&w[i], NULL, rw_writer, NULL);
    for (int i = 0; i < 6; ++i) pthread_join(r[i], NULL);
    for (int i = 0; i < 2; ++i) pthread_join(w[i], NULL);

    SCHECK(!rw_readers_seen_writer,
           "a reader ran concurrently with a writer");
    SCHECK(rw_shared == 8000, "writers lost updates: %ld, want 8000", rw_shared);
    /* A writer completing at all under constant reader pressure is the
     * starvation check: without the writer-waiting bit this hangs. */
}

/* ---- once ---- */
static cos_once_t once_ctl;
static int once_calls;
static int once_observed_incomplete;
static volatile int once_in_progress;

static void once_fn(void)
{
    once_in_progress = 1;
    __sync_fetch_and_add(&once_calls, 1);
    usleep(2000);                 /* widen the window a second thread could hit */
    once_in_progress = 0;
}

static void *once_worker(void *arg)
{
    (void)arg;
    cos_once(&once_ctl, once_fn);
    /* On return the initialiser must be COMPLETE, not merely started. */
    if (once_in_progress) once_observed_incomplete = 1;
    return NULL;
}

static void test_once(void)
{
    memset(&once_ctl, 0, sizeof(once_ctl));
    once_calls = 0; once_observed_incomplete = 0;

    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; ++i) pthread_create(&th[i], NULL, once_worker, NULL);
    for (int i = 0; i < NTHREADS; ++i) pthread_join(th[i], NULL);

    SCHECK(once_calls == 1, "cos_once ran the initialiser %d times", once_calls);
    SCHECK(!once_observed_incomplete,
           "cos_once returned while the initialiser was still running");
}

int main(void)
{
    test_mutex();
    test_cond();
    test_sem();
    test_rwlock();
    test_once();
    printf("\n%d checks, %d failures\n", g_sync_checks, g_sync_failures);
    return g_sync_failures ? 1 : 0;
}
