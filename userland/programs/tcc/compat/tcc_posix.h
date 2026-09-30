/* tcc_posix.h - force-included (-include) when building tcc.c-os. Declares the
 * small POSIX surface TinyCC uses that the C-OS libc headers do not; the
 * implementations are in tcc_shim.c on top of the cos_* syscalls. */
#ifndef _TCC_POSIX_H
#define _TCC_POSIX_H
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <setjmp.h>
/* tccrun.c's tcc_run() reads the real, global `environ` to build the envp it hands the
 * -run'd program (see its "char **envp = environ;" - there is no per-call way to supply
 * one). Aliased straight to cos_environ (libcos's own copy of this process's real
 * environment, set up before main() by __cos_rt_start - see cos_rtld.c) rather than a
 * separate variable of our own that something would have to remember to keep in sync:
 * tcc.c-os's own environment (COS_STDOUT included, when Studio's terminal or Build/Run
 * set it) is then exactly what -run's program receives too. */
extern char **cos_environ;
#define environ cos_environ
FILE *freopen(const char *path, const char *mode, FILE *f);
int close(int fd);
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
off_t lseek(int fd, off_t off, int whence);
int unlink(const char *path);
char *getcwd(char *buf, size_t n);
FILE *fdopen(int fd, const char *mode);
long double ldexpl(long double x, int e);
char *realpath(const char *path, char *resolved);
int execvp(const char *file, char *const argv[]);
/* Declared here because config.h forces CONFIG_RUNMEM_VIRTUALALLOC on for every build,
 * not only _WIN32 ones (see config.h's comment) - tccrun.c calls these expecting the real
 * Windows API to already be in scope from <windows.h>, which we never include. Only
 * VirtualAlloc's arguments and return value carry real meaning here (see tcc_shim.c); the
 * flag values below just need to be valid, distinct integers for tccrun.c to OR together
 * and pass through - our VirtualAlloc ignores all of them and always maps RW. */
#define MEM_RESERVE      0x2000u
#define MEM_COMMIT       0x1000u
#define MEM_RELEASE      0x8000u
#define PAGE_READWRITE   0x04u
void *VirtualAlloc(void *addr, size_t size, unsigned alloc_type, unsigned protect);
int   VirtualFree(void *addr, size_t size, unsigned free_type);
/* libtcc.h passes `longjmp` around as a VALUE, which the function-like macro in
 * <setjmp.h> cannot serve; the parentheses stop macro expansion here. */
void (longjmp)(unsigned long env[8], int val) __attribute__((noreturn));
#endif
