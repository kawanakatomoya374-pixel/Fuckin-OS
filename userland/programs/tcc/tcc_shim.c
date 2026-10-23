/* tcc_shim.c - the POSIX calls TinyCC makes, implemented on the cos_* syscalls.
 *
 * TinyCC (src/third_party/tinyc, unmodified upstream) expects open/read/write/
 * close/lseek/unlink/getcwd, fdopen, and a few others. libcos has streaming file
 * descriptors and whole-file I/O but not these names, so they are provided here.
 * Only tcc.c-os links this file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "cos.h"

/* fd -> path/flags, so lseek(SEEK_END) and fdopen() can be answered: the syscall layer
 * seeks only from the start and libc's FILE is opaque. fds below FDBASE are stdio. */
#define FDBASE 3
#define NFD 256
static struct { char *path; int wr; } s_fd[NFD];

int open(const char *path, int flags, ...) {
    uint32_t f;
    int wr = (flags & 3) != O_RDONLY;
    if (!wr) f = COS_O_RDONLY;
    else f = (flags & O_APPEND) ? COS_O_WRONLY_APPEND : ((flags & 3) == O_RDWR ? COS_O_RDWR_CREATE : COS_O_WRONLY_CREATE);
    int fd = cos_open(path, f);
    if (fd >= 0 && fd < NFD) { free(s_fd[fd].path); s_fd[fd].path = strdup(path); s_fd[fd].wr = wr; }
    return fd;
}
int close(int fd) {
    if (fd >= 0 && fd < NFD) { free(s_fd[fd].path); s_fd[fd].path = NULL; }
    return cos_close(fd);
}
ssize_t read(int fd, void *b, size_t n) { return cos_fd_read(fd, b, n); }
ssize_t write(int fd, const void *b, size_t n) { return cos_fd_write(fd, b, n); }
off_t lseek(int fd, off_t off, int whence) {
    if (whence == 0) return (off_t)cos_lseek(fd, off);
    if (whence == 2 && fd >= 0 && fd < NFD && s_fd[fd].path) {
        cos_stat_t st;
        if (cos_stat(s_fd[fd].path, &st) != 0) return -1;
        off_t end = (off_t)st.size + off;
        return (off_t)cos_lseek(fd, end);
    }
    return -1;
}
/* TinyCC opens the output with open() then wraps it with fdopen(fd, "wb"). Reopen the same
 * path through the libc so the FILE is a real one; the file was already created/truncated. */
FILE *fdopen(int fd, const char *mode) {
    if (fd < 0 || fd >= NFD || !s_fd[fd].path) return NULL;
    char *p = s_fd[fd].path; s_fd[fd].path = NULL;
    cos_close(fd);
    FILE *f = fopen(p, mode);
    free(p);
    return f;
}
int unlink(const char *p) { return cos_unlink(p); }
int mkdir(const char *p, unsigned m) { (void)m; return cos_mkdir(p); }
int chmod(const char *p, unsigned m) { (void)p; (void)m; return 0; }   /* no permission bits on FAT */
int stat(const char *p, struct stat *st) {
    cos_stat_t s;
    if (cos_stat(p, &s) != 0) return -1;
    st->st_size = (unsigned long)s.size; st->st_mode = s.is_dir ? S_IFDIR : 0100000;
    return 0;
}
int fstat(int fd, struct stat *st) { (void)fd; st->st_size = 0; st->st_mode = 0100000; return 0; }
char *getcwd(char *b, size_t n) { if (n < 2) return NULL; b[0] = '/'; b[1] = 0; return b; }
char *realpath(const char *p, char *r) {
    if (!r) { r = (char *)malloc(strlen(p) + 1); if (!r) return NULL; }
    strcpy(r, p);
    return r;
}
int execvp(const char *f, char *const a[]) { (void)f; (void)a; return -1; }
int gettimeofday(struct timeval *tv, void *tz) { (void)tz; uint64_t ms = cos_time_ms(); tv->tv_sec = (long)(ms / 1000); tv->tv_usec = (long)(ms % 1000) * 1000; return 0; }

/* `tcc -run` JIT-compiles into the compiler's own process and jumps straight to the
 * result, which needs memory it can write code into and then execute. The kernel
 * supports exactly that (src/kernel/syscall.c's SYS_MMAP/SYS_MPROTECT, enforced W^X:
 * a single mprotect asking for PROT_WRITE|PROT_EXEC together is refused, matching the
 * mmap-RW-then-mprotect-RX sequence tccrun.c actually uses). PROT_READ/WRITE/EXEC and
 * COS_PROT_READ/WRITE/EXEC share the same bit values (see compat/sys/mman.h and cos.h),
 * so the flags need no translation - only MAP_ANONYMOUS is meaningful here (there is no
 * file-backed mmap on C-OS; fd and offset are ignored, matching every other caller in
 * this codebase, which always passes MAP_ANONYMOUS). addr is a hint only, real mmap()
 * elsewhere is free to ignore it, but tccrun.c never relies on getting that exact
 * address back - it uses whatever pointer mmap() returns. */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    (void)addr; (void)flags; (void)fd; (void)off;
    void *p = cos_mmap(len, (uint32_t)prot);
    return p ? p : MAP_FAILED;
}
int munmap(void *addr, size_t len) { return cos_munmap(addr, len); }
int mprotect(void *addr, size_t len, int prot) { return cos_mprotect(addr, len, (uint32_t)prot); }

/* Normally a Windows-only API; config.h forces tccrun.c's CONFIG_RUNMEM_VIRTUALALLOC path
 * on unconditionally so -run's one big relocation buffer comes from a real, kernel-tracked
 * cos_mmap() instead of tcc_malloc()'s plain heap memory - see the comment there for why
 * that distinction is what makes the later mprotect() (still the ordinary POSIX one, a few
 * lines above) succeed rather than fail outright. alloc_type/protect are ignored: rt_mem()
 * only ever calls this one way (MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE). */
void *VirtualAlloc(void *addr, size_t size, unsigned alloc_type, unsigned protect) {
    (void)addr; (void)alloc_type; (void)protect;
    return cos_mmap(size, COS_PROT_READ | COS_PROT_WRITE);
}
int VirtualFree(void *addr, size_t size, unsigned free_type) { (void)free_type; return cos_munmap(addr, size) == 0; }

/* tccrun.c calls this only for `tcc -run -stdin=file`-style redirection, which nothing
 * in this environment uses (there is no shell driving tcc.c-os that way); returning
 * failure is correct here, not a stand-in for something unimplemented. */
FILE *freopen(const char *p, const char *m, FILE *f) { (void)p; (void)m; (void)f; return NULL; }

/* long double ldexp, for floating constants in the source being compiled */
long double ldexpl(long double x, int e) {
    while (e > 0) { x *= 2.0L; --e; }
    while (e < 0) { x *= 0.5L; ++e; }
    return x;
}

void (longjmp)(unsigned long env[8], int val) { cos_longjmp(env, val); }
