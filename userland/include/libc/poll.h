#ifndef _COS_POLL_H
#define _COS_POLL_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
struct pollfd { int fd; short events; short revents; };
#define POLLIN  1
#define POLLPRI 2
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define POLLNVAL 32
typedef unsigned long nfds_t;
int poll(struct pollfd *fds, nfds_t n, int timeout_ms);
_COS_END
#endif
