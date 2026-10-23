#include <stddef.h>
typedef int pthread_mutex_t, pthread_mutexattr_t, pthread_cond_t, pthread_condattr_t;
int pthread_mutex_init(pthread_mutex_t *a,const pthread_mutexattr_t*b){(void)b;if(a)*a=0;return 0;}
int pthread_mutex_destroy(pthread_mutex_t*a){(void)a;return 0;}
int pthread_mutex_lock(pthread_mutex_t*a){(void)a;return 0;}
int pthread_mutex_unlock(pthread_mutex_t*a){(void)a;return 0;}
int pthread_cond_init(pthread_cond_t*a,const pthread_condattr_t*b){(void)b;if(a)*a=0;return 0;}
int pthread_cond_destroy(pthread_cond_t*a){(void)a;return 0;}
int pthread_cond_wait(pthread_cond_t*a,pthread_mutex_t*b){(void)a;(void)b;return 0;}
int pthread_cond_signal(pthread_cond_t*a){(void)a;return 0;}
int pthread_cond_broadcast(pthread_cond_t*a){(void)a;return 0;}
void cos_assert_fail(const char *expr, const char *file, int line){(void)expr;(void)file;(void)line;for(;;){} }
