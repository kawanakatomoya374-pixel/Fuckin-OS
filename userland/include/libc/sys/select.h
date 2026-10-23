#ifndef _COS_SYS_SELECT_H
#define _COS_SYS_SELECT_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
#include "time.h"
typedef struct { unsigned long fds_bits[16]; } fd_set;
#define FD_SETSIZE 1024
#define FD_ZERO(s)  do { for (int _i = 0; _i < 16; ++_i) (s)->fds_bits[_i] = 0; } while (0)
#define FD_SET(n, s)   ((s)->fds_bits[(n) / 64] |= 1UL << ((n) % 64))
#define FD_CLR(n, s)   ((s)->fds_bits[(n) / 64] &= ~(1UL << ((n) % 64)))
#define FD_ISSET(n, s) (((s)->fds_bits[(n) / 64] >> ((n) % 64)) & 1)
int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout);
_COS_END
#endif
