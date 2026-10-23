#ifndef _COS_PTHREAD_H
#define _COS_PTHREAD_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
#include "sched.h"
typedef long pthread_t;
typedef struct { int kind; } pthread_attr_t;
typedef struct { unsigned state; unsigned long owner; int kind; int count; unsigned long rowner; } pthread_mutex_t;
typedef struct { int kind; } pthread_mutexattr_t;
typedef struct { unsigned seq; unsigned waiters; } pthread_cond_t;
typedef struct { int dummy; } pthread_condattr_t;
typedef struct { unsigned state; } pthread_rwlock_t;
typedef struct { int dummy; } pthread_rwlockattr_t;
typedef struct { unsigned state; } pthread_once_t;
typedef unsigned pthread_key_t;
#define PTHREAD_MUTEX_INITIALIZER { 0, 0, 0, 0, 0 }
#define PTHREAD_COND_INITIALIZER { 0, 0 }
#define PTHREAD_RWLOCK_INITIALIZER { 0 }
#define PTHREAD_ONCE_INIT { 0 }
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
int pthread_detach(pthread_t t);
void pthread_exit(void *ret) __attribute__((noreturn));
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_attr_init(pthread_attr_t *a); int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setdetachstate(pthread_attr_t *a, int s); int pthread_attr_setstacksize(pthread_attr_t *a, size_t n);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m); int pthread_mutex_trylock(pthread_mutex_t *m); int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutexattr_init(pthread_mutexattr_t *a); int pthread_mutexattr_destroy(pthread_mutexattr_t *a); int pthread_mutexattr_settype(pthread_mutexattr_t *a, int t);
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a); int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *c); int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a); int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l); int pthread_rwlock_wrlock(pthread_rwlock_t *l); int pthread_rwlock_unlock(pthread_rwlock_t *l);
int pthread_once(pthread_once_t *o, void (*fn)(void));
int pthread_key_create(pthread_key_t *k, void (*dtor)(void *)); int pthread_key_delete(pthread_key_t k);
void *pthread_getspecific(pthread_key_t k); int pthread_setspecific(pthread_key_t k, const void *v);
_COS_END
#endif
