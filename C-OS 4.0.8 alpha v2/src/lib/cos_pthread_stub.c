/**
 * cos_pthread_stub.c - implementations for pthread.h's shim. See that
 * file's header comment for what's real (pthread_once, mutex lock/
 * unlock as trivial single-threaded operations) versus an honest
 * no-op (condition variables - this kernel has no blocking wait
 * primitive to implement them against yet).
 */
#include "pthread.h"
#include <stddef.h>

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
    if (once_control == NULL || init_routine == NULL) {
        return -1;
    }
    if (*once_control == 0) {
        *once_control = 1;
        init_routine();
    }
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    (void)attr;
    if (mutex != NULL) *mutex = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    /* Real (if trivial) for a single-threaded kernel: nothing else
     * can be holding it, so "acquire" always succeeds immediately.
     * Tracked so a future real implementation can assert on
     * double-lock bugs rather than just being decorative. */
    if (mutex != NULL) *mutex = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    if (mutex != NULL) *mutex = 0;
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    (void)attr;
    if (cond != NULL) *cond = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    /* Honest no-op, not real blocking - see this file's header
     * comment. Returns immediately rather than actually waiting for
     * a signal that (with no other thread able to run) could never
     * come anyway. */
    (void)cond;
    (void)mutex;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                            const struct timespec *abstime)
{
    (void)cond;
    (void)mutex;
    (void)abstime;
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr)
{
    if (attr != NULL) *attr = 0;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr)
{
    (void)attr;
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, int clock_id)
{
    (void)attr;
    (void)clock_id;
    return 0;
}
