#ifndef _COS_UNISTD_H
#define _COS_UNISTD_H
#include "_cos_libc_cfg.h"
#include "sys/types.h"
_COS_BEGIN
unsigned sleep(unsigned seconds);
int      usleep(unsigned usec);
pid_t    getpid(void);
int      rmdir(const char *path);
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0
#define _SC_PAGESIZE 30
#define _SC_NPROCESSORS_ONLN 84
#define _SC_NPROCESSORS_CONF 83
#define _SC_CLK_TCK 2
#define _SC_OPEN_MAX 4
ssize_t  read(int fd, void *buf, size_t n);
ssize_t  write(int fd, const void *buf, size_t n);
int      close(int fd);
off_t    lseek(int fd, off_t off, int whence);
int      unlink(const char *path);
int      access(const char *path, int mode);
int      isatty(int fd);
char    *getcwd(char *buf, size_t size);
int      chdir(const char *path);
int      dup(int fd);
int      dup2(int fd, int fd2);
int      pipe(int fds[2]);
int      ftruncate(int fd, off_t len);
int      fsync(int fd);
long     sysconf(int name);
pid_t    getppid(void);
unsigned alarm(unsigned seconds);
int      gethostname(char *name, size_t len);
int      getuid(void); int getgid(void); int geteuid(void);
extern char **environ;
void     _exit(int status) __attribute__((noreturn));
_COS_END
#endif
