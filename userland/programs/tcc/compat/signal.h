#ifndef _COS_SIGNAL_H
#define _COS_SIGNAL_H
typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);
#define SIGINT 2
#define SIGSEGV 11
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
static inline sighandler_t signal(int s, sighandler_t h) { (void)s; return h; }
#endif
