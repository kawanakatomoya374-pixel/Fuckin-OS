/*
 * pthread.h - minimal shim for QuickJS's cutils.h threading abstraction
 * (js_once, js_mutex_lock/unlock, js_cond_wait etc.) which unconditionally
 * uses POSIX pthreads on any non-Windows build. Not a real pthreads
 * implementation.
 *
 * This kernel is single-threaded so far. pthread_once and mutex ops are
 * real (trivially correct for one thread). Condition variable waits are
 * honest no-ops - their only caller in QuickJS is Atomics.wait(), unused
 * in this build. See PORTING_NOTES.md.
 */
#ifndef COS_PTHREAD_H
#define COS_PTHREAD_H

typedef volatile int pthread_once_t;
#define PTHREAD_ONCE_INIT 0

typedef int pthread_mutex_t;
typedef int pthread_mutexattr_t;
typedef int pthread_cond_t;
typedef int pthread_condattr_t;
typedef int pthread_t;

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);

struct timespec; /* from sys/time.h - forward declared to avoid
                   * forcing every includer of this header to also
                   * pull that one in */
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                            const struct timespec *abstime);

int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);
int pthread_condattr_setclock(pthread_condattr_t *attr, int clock_id);

#endif /* COS_PTHREAD_H */
