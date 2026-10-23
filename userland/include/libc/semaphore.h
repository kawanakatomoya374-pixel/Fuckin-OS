#ifndef _COS_SEMAPHORE_H
#define _COS_SEMAPHORE_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
typedef struct { unsigned count; } sem_t;
int sem_init(sem_t *s, int pshared, unsigned value); int sem_destroy(sem_t *s);
int sem_wait(sem_t *s); int sem_trywait(sem_t *s); int sem_post(sem_t *s);
_COS_END
#endif
