#ifndef _COS_SIGNAL_H
#define _COS_SIGNAL_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);
typedef unsigned long sigset_t;
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGABRT 6
#define SIGFPE 8
#define SIGKILL 9
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGCHLD 17
struct sigaction { sighandler_t sa_handler; sigset_t sa_mask; int sa_flags; };
sighandler_t signal(int sig, sighandler_t h);
int raise(int sig);
int kill(int pid, int sig);
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int sigemptyset(sigset_t *s); int sigfillset(sigset_t *s); int sigaddset(sigset_t *s, int n); int sigdelset(sigset_t *s, int n); int sigismember(const sigset_t *s, int n);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
_COS_END
#endif
