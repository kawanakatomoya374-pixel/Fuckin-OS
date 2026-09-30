#ifndef _COS_UNISTD_H
#define _COS_UNISTD_H
#include "_cos_libc_cfg.h"
#include "sys/types.h"
_COS_BEGIN
unsigned sleep(unsigned seconds);
int      usleep(unsigned usec);
pid_t    getpid(void);
_COS_END
#endif
