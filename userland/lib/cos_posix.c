/* cos_posix.c - the POSIX layer of libcos: file descriptors with the usual numbers, sockets, poll/select, pthreads, signals,
 * getopt, environment, a regex engine and the many small functions ported programs expect.
 *
 * Everything sits on the cos_* calls. File descriptors here are the process's own table (0, 1, 2 are the console; a socket or a
 * pipe is a descriptor like a file), so read/write/close/poll work the same on all of them.
 */
#include "cos.h"
#include "libc/_cos_libc_cfg.h"
#include "libc/errno.h"
#include "libc/string.h"
#include "libc/strings.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"
#include "libc/time.h"
#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/sys/stat.h"
#include "libc/dirent.h"
#include "libc/sys/time.h"
#include "libc/sys/select.h"
#include "libc/poll.h"
#include "libc/sys/socket.h"
#include "libc/netinet/in.h"
#include "libc/arpa/inet.h"
#include "libc/netdb.h"
#include "libc/pthread.h"
#include "libc/semaphore.h"
#include "libc/signal.h"
#include "libc/sys/mman.h"
#include "libc/getopt.h"
#include "libc/libgen.h"
#include "libc/locale.h"
#include "libc/sys/utsname.h"
#include "libc/regex.h"
#include "libc/ctype.h"

#define SET_ERR(e) (errno = (e))
static int fail(int e) { errno = e; return -1; }
static int neg_errno(long r) { errno = (int)(-r); return -1; }

/* ====================================================================== descriptor table */
enum { T_FREE = 0, T_CON, T_FILE, T_DIR, T_SOCK, T_PIPE_R, T_PIPE_W };
#define NFD 256
typedef struct pipe_s pipe_t;
typedef struct { int type, kfd, flags; long long pos, high; char path[256]; int sock_type; int rcv_ms, snd_ms; pipe_t *pipe; int refs; } fdent_t;
static fdent_t s_fd[NFD];
static int s_fd_init;

static void fd_boot(void) {
    if (s_fd_init) return;
    s_fd_init = 1;
    for (int i = 0; i < 3; ++i) s_fd[i].type = T_CON;
}
static int fd_alloc(void) {
    fd_boot();
    for (int i = 3; i < NFD; ++i) if (s_fd[i].type == T_FREE) { memset(&s_fd[i], 0, sizeof s_fd[i]); s_fd[i].refs = 1; return i; }
    return fail(EMFILE);
}
static fdent_t *fd_get(int fd) {
    fd_boot();
    if (fd < 0 || fd >= NFD || s_fd[fd].type == T_FREE) { errno = EBADF; return NULL; }
    return &s_fd[fd];
}

/* ====================================================================== pipes (inside one process: between threads) */
struct pipe_s { char buf[8192]; int head, count; int readers, writers; cos_mutex_t m; cos_cond_t c; };

int pipe(int fds[2]) {
    int r = fd_alloc(); if (r < 0) return -1;
    s_fd[r].type = T_PIPE_R;
    int w = fd_alloc(); if (w < 0) { s_fd[r].type = T_FREE; return -1; }
    s_fd[w].type = T_PIPE_W;
    pipe_t *p = (pipe_t *)cos_calloc(1, sizeof *p);
    if (!p) { s_fd[r].type = s_fd[w].type = T_FREE; return fail(ENOMEM); }
    cos_mutex_init(&p->m); cos_cond_init(&p->c);
    p->readers = p->writers = 1;
    s_fd[r].pipe = s_fd[w].pipe = p;
    fds[0] = r; fds[1] = w;
    return 0;
}

/* ====================================================================== files */
static int to_cos_flags(int flags, const char *path, int *append) {
    int acc = flags & O_ACCMODE;
    *append = (flags & O_APPEND) != 0;
    if (flags & O_TRUNC) cos_unlink(path);                       /* the read-write open does not truncate: start from an empty file instead */
    if (acc == O_RDONLY) return COS_O_RDONLY;
    return COS_O_RDWR_CREATE;
}

int open(const char *path, int flags, ...) {
    if (!path) return fail(EINVAL);
    cos_stat_t st; bool exists = cos_stat(path, &st) == 0;
    if (exists && (flags & O_CREAT) && (flags & O_EXCL)) return fail(EEXIST);
    if (!exists && !(flags & O_CREAT)) return fail(ENOENT);
    if (exists && st.is_dir) {
        if ((flags & O_ACCMODE) != O_RDONLY) return fail(EISDIR);
        int kd = cos_opendir(path); if (kd < 0) return fail(ENOENT);
        int fd = fd_alloc(); if (fd < 0) { cos_close(kd); return -1; }
        s_fd[fd].type = T_DIR; s_fd[fd].kfd = kd; return fd;
    }
    int append; int cf = to_cos_flags(flags, path, &append);
    int kfd = cos_open(path, (uint32_t)cf);
    if (kfd < 0) return fail(exists ? EACCES : ENOENT);
    int fd = fd_alloc(); if (fd < 0) { cos_close(kfd); return -1; }
    fdent_t *e = &s_fd[fd];
    e->type = T_FILE; e->kfd = kfd; e->flags = flags;
    strlcpy(e->path, path, sizeof e->path);
    e->high = (exists && !(flags & O_TRUNC)) ? (long long)st.size : 0;
    if (append) { e->pos = e->high; cos_lseek(kfd, e->pos); }
    return fd;
}
int creat(const char *path, unsigned mode) { (void)mode; return open(path, O_WRONLY | O_CREAT | O_TRUNC); }

static ssize_t pipe_read(fdent_t *e, void *buf, size_t n) {
    pipe_t *p = e->pipe;
    cos_mutex_lock(&p->m);
    while (p->count == 0) {
        if (p->writers == 0) { cos_mutex_unlock(&p->m); return 0; }
        if (e->flags & O_NONBLOCK) { cos_mutex_unlock(&p->m); return fail(EAGAIN); }
        cos_cond_wait(&p->c, &p->m);
    }
    size_t k = n < (size_t)p->count ? n : (size_t)p->count;
    for (size_t i = 0; i < k; ++i) ((char *)buf)[i] = p->buf[(p->head + (int)i) % (int)sizeof p->buf];
    p->head = (p->head + (int)k) % (int)sizeof p->buf; p->count -= (int)k;
    cos_cond_broadcast(&p->c);
    cos_mutex_unlock(&p->m);
    return (ssize_t)k;
}
static ssize_t pipe_write(fdent_t *e, const void *buf, size_t n) {
    pipe_t *p = e->pipe; size_t done = 0;
    cos_mutex_lock(&p->m);
    while (done < n) {
        if (p->readers == 0) { cos_mutex_unlock(&p->m); return fail(EPIPE); }
        while (p->count == (int)sizeof p->buf) {
            if (e->flags & O_NONBLOCK) { cos_mutex_unlock(&p->m); return done ? (ssize_t)done : fail(EAGAIN); }
            cos_cond_wait(&p->c, &p->m);
            if (p->readers == 0) { cos_mutex_unlock(&p->m); return fail(EPIPE); }
        }
        while (done < n && p->count < (int)sizeof p->buf) { p->buf[(p->head + p->count) % (int)sizeof p->buf] = ((const char *)buf)[done++]; p->count++; }
        cos_cond_broadcast(&p->c);
    }
    cos_mutex_unlock(&p->m);
    return (ssize_t)done;
}

static ssize_t sock_recv(fdent_t *e, void *buf, size_t n, int flags, cos_sockaddr_t *from);

ssize_t read(int fd, void *buf, size_t n) {
    fdent_t *e = fd_get(fd); if (!e) return -1;
    switch (e->type) {
    case T_CON: { if (fd != 0) return fail(EBADF); size_t i = 0; int c = 0; while (i < n && (c = fgetc(stdin)) != EOF) { ((char *)buf)[i++] = (char)c; if (c == '\n') break; } return (ssize_t)i; }
    case T_FILE: {
        if ((e->flags & O_ACCMODE) == O_WRONLY) return fail(EBADF);
        ssize_t r = cos_fd_read(e->kfd, buf, n);
        if (r < 0) return fail(EIO);
        e->pos += r; return r; }
    case T_DIR: return fail(EISDIR);
    case T_SOCK: return sock_recv(e, buf, n, 0, NULL);
    case T_PIPE_R: return pipe_read(e, buf, n);
    default: return fail(EBADF);
    }
}
static ssize_t sock_send(fdent_t *e, const void *buf, size_t n, int flags);
ssize_t write(int fd, const void *buf, size_t n) {
    fdent_t *e = fd_get(fd); if (!e) return -1;
    switch (e->type) {
    case T_CON: { if (fd == 0) return fail(EBADF); ssize_t w = cos_write((const char *)buf, n); return w < 0 ? fail(EIO) : w; }
    case T_FILE: {
        if ((e->flags & O_ACCMODE) == O_RDONLY) return fail(EBADF);
        if (e->flags & O_APPEND) { e->pos = e->high; cos_lseek(e->kfd, e->pos); }
        ssize_t w = cos_fd_write(e->kfd, buf, n);
        if (w < 0) return fail(ENOSPC);
        e->pos += w; if (e->pos > e->high) e->high = e->pos;
        return w; }
    case T_SOCK: return sock_send(e, buf, n, 0);
    case T_PIPE_W: return pipe_write(e, buf, n);
    default: return fail(EBADF);
    }
}
off_t lseek(int fd, off_t off, int whence) {
    fdent_t *e = fd_get(fd); if (!e) return -1;
    if (e->type != T_FILE) return fail(ESPIPE);
    long long np;
    if (whence == SEEK_SET) np = off;
    else if (whence == SEEK_CUR) np = e->pos + off;
    else if (whence == SEEK_END) { cos_stat_t st; long long sz = e->high; if (cos_stat(e->path, &st) == 0 && (long long)st.size > sz) sz = (long long)st.size; np = sz + off; }
    else return fail(EINVAL);
    if (np < 0) return fail(EINVAL);
    if (cos_lseek(e->kfd, np) < 0) return fail(EINVAL);
    e->pos = np; return (off_t)np;
}
int close(int fd) {
    fdent_t *e = fd_get(fd); if (!e) return -1;
    switch (e->type) {
    case T_FILE: case T_DIR: cos_close(e->kfd); break;
    case T_SOCK: cos_sock_close(e->kfd); break;
    case T_PIPE_R: case T_PIPE_W: {
        pipe_t *p = e->pipe; cos_mutex_lock(&p->m);
        if (e->type == T_PIPE_R) p->readers--; else p->writers--;
        cos_cond_broadcast(&p->c);
        int dead = p->readers == 0 && p->writers == 0;
        cos_mutex_unlock(&p->m);
        if (dead) cos_free(p);
        break; }
    default: break;
    }
    if (e->type != T_CON) e->type = T_FREE;
    return 0;
}
int dup(int fd) {
    fdent_t *e = fd_get(fd); if (!e) return -1;
    if (e->type == T_FILE || e->type == T_DIR || e->type == T_CON) {
        if (e->type == T_CON) { int n = fd_alloc(); if (n < 0) return -1; s_fd[n] = *e; return n; }
        errno = ENOSYS; return -1;                      /* two descriptors would need to share a kernel offset */
    }
    int n = fd_alloc(); if (n < 0) return -1;
    s_fd[n] = *e;
    if (e->type == T_PIPE_R) e->pipe->readers++; else if (e->type == T_PIPE_W) e->pipe->writers++;
    return n;
}
int dup2(int fd, int fd2) {
    fdent_t *e = fd_get(fd); if (!e) return -1;
    if (fd2 < 0 || fd2 >= NFD) return fail(EBADF);
    if (fd == fd2) return fd2;
    if (s_fd[fd2].type != T_FREE) close(fd2);
    s_fd[fd2] = *e;
    if (e->type == T_PIPE_R) e->pipe->readers++; else if (e->type == T_PIPE_W) e->pipe->writers++;
    return fd2;
}
int isatty(int fd) { fdent_t *e = fd_get(fd); return e && e->type == T_CON; }
int fsync(int fd) { return fd_get(fd) ? 0 : -1; }
int ftruncate(int fd, off_t len) {
    fdent_t *e = fd_get(fd); if (!e || e->type != T_FILE) return fail(EBADF);
    long long sz = e->high; cos_stat_t st; if (cos_stat(e->path, &st) == 0 && (long long)st.size > sz) sz = (long long)st.size;
    if (len == sz) return 0;
    /* no truncate call in the kernel: rewrite the file (a few MB at most is fine, it is a rare call) */
    if (len > sz || len < 0) return fail(ENOSYS);
    char *tmp = (char *)cos_malloc(len ? (size_t)len : 1); if (!tmp) return fail(ENOMEM);
    ssize_t got = cos_read_file(e->path, tmp, (size_t)len);
    if (got < 0) { cos_free(tmp); return fail(EIO); }
    cos_close(e->kfd); cos_unlink(e->path);
    ssize_t wr = cos_write_file(e->path, tmp, (size_t)len);
    cos_free(tmp);
    e->kfd = cos_open(e->path, COS_O_RDWR_CREATE);
    if (wr < 0 || e->kfd < 0) return fail(EIO);
    e->high = len; if (e->pos > len) e->pos = len; cos_lseek(e->kfd, e->pos);
    return 0;
}
int fcntl(int fd, int cmd, ...) {
    fdent_t *e = fd_get(fd); if (!e) return -1;
    __builtin_va_list ap; __builtin_va_start(ap, cmd);
    int arg = __builtin_va_arg(ap, int); __builtin_va_end(ap);
    switch (cmd) {
    case F_GETFL: return e->flags;
    case F_SETFL: e->flags = (e->flags & O_ACCMODE) | (arg & ~O_ACCMODE); return 0;
    case F_GETFD: return 0;
    case F_SETFD: return 0;
    }
    return fail(EINVAL);
}
int fileno(FILE *f) {                       /* the standard streams only: a file stream is not a descriptor of this table (use open()) */
    if (f == stdin) return 0;
    if (f == stdout) return 1;
    if (f == stderr) return 2;
    return fail(EBADF);
}

/* ====================================================================== cwd, stat, directories */
char *getcwd(char *buf, size_t size) { if (!buf) { buf = (char *)cos_malloc(256); size = 256; } char *r = cos_getcwd(buf, size); if (!r) errno = ERANGE; return r; }
int chdir(const char *path) { if (cos_chdir(path) < 0) return fail(ENOENT); return 0; }
int access(const char *path, int mode) { (void)mode; cos_stat_t st; return cos_stat(path, &st) == 0 ? 0 : fail(ENOENT); }
int unlink(const char *path) { return cos_unlink(path) == 0 ? 0 : fail(ENOENT); }
int mkdir(const char *path, mode_t mode) { (void)mode; cos_stat_t st; if (cos_stat(path, &st) == 0) return fail(EEXIST); return cos_mkdir(path) == 0 ? 0 : fail(ENOENT); }
int chmod(const char *path, mode_t mode) { (void)mode; return access(path, 0); }
char *realpath(const char *path, char *resolved) {
    char full[512];
    if (path[0] == '/') strlcpy(full, path, sizeof full);
    else { char cwd[256]; cos_getcwd(cwd, sizeof cwd); snprintf(full, sizeof full, "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", path); }
    char out[512]; size_t n = 0; out[n++] = '/';
    const char *p = full;
    while (*p) {
        while (*p == '/') ++p;
        const char *e = p; while (*e && *e != '/') ++e;
        size_t l = (size_t)(e - p);
        if (l == 0) break;
        if (l == 1 && p[0] == '.') { }
        else if (l == 2 && p[0] == '.' && p[1] == '.') { if (n > 1) { --n; while (n > 1 && out[n - 1] != '/') --n; } }
        else { if (n > 1) out[n++] = '/'; if (n + l + 1 >= sizeof out) { errno = ENAMETOOLONG; return NULL; } memcpy(out + n, p, l); n += l; }
        p = e;
    }
    out[n] = 0;
    cos_stat_t st; if (cos_stat(out, &st) != 0 && strcmp(out, "/") != 0) { errno = ENOENT; return NULL; }
    if (!resolved) resolved = (char *)cos_malloc(n + 1);
    if (!resolved) { errno = ENOMEM; return NULL; }
    memcpy(resolved, out, n + 1);
    return resolved;
}
static void fill_stat(struct stat *s, int is_dir, uint64_t size, uint64_t mtime) {
    memset(s, 0, sizeof *s);
    s->st_mode = (is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
    s->st_size = (off_t)size; s->st_mtime = s->st_atime = s->st_ctime = (long)mtime; s->st_nlink = 1; s->st_blksize = 4096; s->st_blocks = (long)((size + 511) / 512);
}
int stat(const char *path, struct stat *st) {
    cos_stat_ex_t x;
    if (cos_stat_ex(path, &x) == 0) { fill_stat(st, x.is_dir, x.size, x.mtime); return 0; }
    cos_stat_t s; if (cos_stat(path, &s) == 0) { fill_stat(st, s.is_dir, s.size, 0); return 0; }
    return fail(ENOENT);
}
int lstat(const char *path, struct stat *st) { return stat(path, st); }
int fstat(int fd, struct stat *st) {
    fdent_t *e = fd_get(fd); if (!e) return -1;
    if (e->type == T_FILE) { if (stat(e->path, st) != 0) return -1; if ((uint64_t)e->high > (uint64_t)st->st_size) st->st_size = (off_t)e->high; return 0; }
    memset(st, 0, sizeof *st); st->st_mode = e->type == T_CON ? S_IFCHR | 0620 : e->type == T_DIR ? S_IFDIR | 0755 : 0010000 | 0600; return 0;
}
struct __cos_DIR { int kfd; struct dirent ent; };
DIR *opendir(const char *path) {
    int kd = cos_opendir(path); if (kd < 0) { errno = ENOENT; return NULL; }
    DIR *d = (DIR *)cos_calloc(1, sizeof *d); if (!d) { cos_close(kd); errno = ENOMEM; return NULL; }
    d->kfd = kd; return d;
}
struct dirent *readdir(DIR *d) {
    cos_dirent_t de;
    if (!d || cos_readdir(d->kfd, &de) != 1) return NULL;
    strlcpy(d->ent.d_name, de.name, sizeof d->ent.d_name); d->ent.d_type = de.is_dir ? DT_DIR : DT_REG; d->ent.d_ino = 1;
    return &d->ent;
}
int closedir(DIR *d) { if (!d) return -1; cos_close(d->kfd); cos_free(d); return 0; }
void rewinddir(DIR *d) { (void)d; }

/* ====================================================================== time */
int gettimeofday(struct timeval *tv, struct timezone *tz) {
    if (tz) { tz->tz_minuteswest = 0; tz->tz_dsttime = 0; }
    if (tv) { uint64_t ms = cos_time_ms(); tv->tv_sec = (time_t)cos_time_unix(); tv->tv_usec = (long)((ms % 1000) * 1000); }
    return 0;
}
int clock_gettime(clockid_t id, struct timespec *ts) {
    uint64_t ms = cos_time_ms();
    if (id == CLOCK_MONOTONIC) { ts->tv_sec = (time_t)(ms / 1000); ts->tv_nsec = (long)(ms % 1000) * 1000000L; }
    else { ts->tv_sec = (time_t)cos_time_unix(); ts->tv_nsec = (long)(ms % 1000) * 1000000L; }
    return 0;
}
int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) return fail(EINVAL);
    cos_sleep_ms((uint64_t)req->tv_sec * 1000u + (uint64_t)(req->tv_nsec + 999999L) / 1000000u);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}
long timezone = 0; int daylight = 0; char *tzname[2] = { "UTC", "UTC" };
void tzset(void) {}
time_t timegm(struct tm *tm) { return mktime(tm); }
char *strptime(const char *s, const char *fmt, struct tm *tm) {
    while (*fmt) {
        if (*fmt != '%') { if (*s != *fmt) return NULL; ++s; ++fmt; continue; }
        ++fmt;
        int *dst = NULL, lo = 0, hi = 99, adj = 0, digits = 2;
        switch (*fmt) {
        case 'Y': dst = &tm->tm_year; lo = 1900; hi = 9999; adj = -1900; digits = 4; break;
        case 'm': dst = &tm->tm_mon; lo = 1; hi = 12; adj = -1; break;
        case 'd': case 'e': dst = &tm->tm_mday; lo = 1; hi = 31; break;
        case 'H': dst = &tm->tm_hour; lo = 0; hi = 23; break;
        case 'M': dst = &tm->tm_min; lo = 0; hi = 59; break;
        case 'S': dst = &tm->tm_sec; lo = 0; hi = 61; break;
        case '%': if (*s != '%') return NULL; ++s; ++fmt; continue;
        case 'T': { char *r = strptime(s, "%H:%M:%S", tm); if (!r) return NULL; s = r; ++fmt; continue; }
        case 'F': { char *r = strptime(s, "%Y-%m-%d", tm); if (!r) return NULL; s = r; ++fmt; continue; }
        default: return NULL;
        }
        while (*s == ' ') ++s;
        int v = 0, k = 0;
        while (k < digits && *s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; ++k; }
        if (!k || v < lo || v > hi) return NULL;
        *dst = v + adj; ++fmt;
    }
    return (char *)s;
}

/* ====================================================================== misc process / system */
void _exit(int status) { cos_exit(status); }
pid_t getppid(void) { return 1; }
int getuid(void) { return 0; } int getgid(void) { return 0; } int geteuid(void) { return 0; }
unsigned alarm(unsigned s) { (void)s; return 0; }
int gethostname(char *name, size_t len) { strlcpy(name, "c-os", len); return 0; }
long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE: return 4096;
    case _SC_NPROCESSORS_ONLN: case _SC_NPROCESSORS_CONF: { int n = cos_par_cpus(); return n > 0 ? n + 1 : 1; }
    case _SC_CLK_TCK: return 100;
    case _SC_OPEN_MAX: return NFD;
    }
    errno = EINVAL; return -1;
}
int uname(struct utsname *u) {
    memset(u, 0, sizeof *u);
    strlcpy(u->sysname, "C-OS", sizeof u->sysname); strlcpy(u->nodename, "c-os", sizeof u->nodename); strlcpy(u->release, "4.0.9", sizeof u->release);
    strlcpy(u->version, "C-OS", sizeof u->version); strlcpy(u->machine, "x86_64", sizeof u->machine);
    return 0;
}
char *setlocale(int cat, const char *loc) { (void)cat; if (loc && loc[0] && strcmp(loc, "C") && strcmp(loc, "POSIX")) return NULL; return (char *)"C"; }

/* ---- environment ---- */
char **environ;
static int s_env_owned;
static __attribute__((constructor)) void posix_env_init(void) { environ = cos_environ; }
static int env_count(void) { int n = 0; if (cos_environ) while (cos_environ[n]) ++n; return n; }
static int env_own(void) {
    if (s_env_owned) return 0;
    int n = env_count();
    char **v = (char **)cos_calloc((size_t)n + 2, sizeof *v); if (!v) return -1;
    for (int i = 0; i < n; ++i) v[i] = cos_environ[i];
    cos_environ = v; environ = v; s_env_owned = 1; return 0;
}
int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) return fail(EINVAL);
    size_t nl = strlen(name);
    if (env_own() < 0) return fail(ENOMEM);
    int n = env_count();
    for (int i = 0; i < n; ++i) if (!strncmp(cos_environ[i], name, nl) && cos_environ[i][nl] == '=') { if (!overwrite) return 0; goto set; }
    cos_environ = (char **)cos_realloc(cos_environ, ((size_t)n + 2) * sizeof(char *)); if (!cos_environ) return fail(ENOMEM);
    cos_environ[n + 1] = NULL; cos_environ[n] = NULL; environ = cos_environ;
set:;
    char *e = (char *)cos_malloc(nl + strlen(value) + 2); if (!e) return fail(ENOMEM);
    sprintf(e, "%s=%s", name, value);
    int idx = n; for (int i = 0; i < n; ++i) if (cos_environ[i] && !strncmp(cos_environ[i], name, nl) && cos_environ[i][nl] == '=') { idx = i; break; }
    cos_environ[idx] = e; environ = cos_environ;
    return 0;
}
int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) return fail(EINVAL);
    if (env_own() < 0) return fail(ENOMEM);
    size_t nl = strlen(name); int n = env_count();
    for (int i = 0; i < n; ++i) if (!strncmp(cos_environ[i], name, nl) && cos_environ[i][nl] == '=') { for (int j = i; j < n; ++j) cos_environ[j] = cos_environ[j + 1]; --n; --i; }
    environ = cos_environ; return 0;
}
int putenv(char *s) { char *eq = strchr(s, '='); if (!eq) return unsetenv(s); char nm[128]; size_t l = (size_t)(eq - s); if (l >= sizeof nm) return fail(EINVAL); memcpy(nm, s, l); nm[l] = 0; return setenv(nm, eq + 1, 1); }

/* ---- random ---- */
static unsigned long long s_rs = 0x853c49e6748fea9bULL;
long random(void) { s_rs = s_rs * 6364136223846793005ULL + 1442695040888963407ULL; return (long)((s_rs >> 33) & 0x7fffffff); }
void srandom(unsigned seed) { s_rs = seed * 2862933555777941757ULL + 3037000493ULL; }
int rand_r(unsigned *seed) { unsigned x = *seed * 1103515245u + 12345u; *seed = x; return (int)((x >> 16) & 0x7fff); }
void srand48(long seed) { srandom((unsigned)seed); }
long lrand48(void) { return random(); }
double drand48(void) { return (double)random() / 2147483648.0; }
void qsort_r(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *arg) {
    /* heap sort: no recursion and no quadratic case; not stable, as qsort() is not required to be */
    char *a = (char *)base;
    char *tmp = (char *)cos_malloc(size); if (!tmp) return;
#define EL(i) (a + (i) * size)
#define SWAP(x, y) do { memcpy(tmp, EL(x), size); memcpy(EL(x), EL(y), size); memcpy(EL(y), tmp, size); } while (0)
    if (n > 1) {
        for (size_t s = n / 2; s-- > 0; ) { size_t r = s; for (;;) { size_t c = 2 * r + 1; if (c >= n) break; if (c + 1 < n && cmp(EL(c), EL(c + 1), arg) < 0) ++c; if (cmp(EL(r), EL(c), arg) >= 0) break; SWAP(r, c); r = c; } }
        for (size_t end = n - 1; end > 0; --end) { SWAP(0, end); size_t r = 0; for (;;) { size_t c = 2 * r + 1; if (c >= end) break; if (c + 1 < end && cmp(EL(c), EL(c + 1), arg) < 0) ++c; if (cmp(EL(r), EL(c), arg) >= 0) break; SWAP(r, c); r = c; } }
    }
#undef EL
#undef SWAP
    cos_free(tmp);
}
int mkstemp(char *tmpl) {
    size_t n = strlen(tmpl); if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX")) return fail(EINVAL);
    for (int tries = 0; tries < 100; ++tries) {
        for (int i = 0; i < 6; ++i) tmpl[n - 6 + i] = "abcdefghijklmnopqrstuvwxyz0123456789"[random() % 36];
        cos_stat_t st; if (cos_stat(tmpl, &st) == 0) continue;
        return open(tmpl, O_RDWR | O_CREAT | O_EXCL);
    }
    return fail(EEXIST);
}

/* ---- strings ---- */
char *strsep(char **sp, const char *delim) {
    char *s = *sp; if (!s) return NULL;
    char *e = s + strcspn(s, delim);
    if (*e) { *e = 0; *sp = e + 1; } else *sp = NULL;
    return s;
}
void *memmem(const void *h, size_t hl, const void *n, size_t nl) {
    if (!nl) return (void *)h;
    if (hl < nl) return NULL;
    for (size_t i = 0; i + nl <= hl; ++i) if (!memcmp((const char *)h + i, n, nl)) return (void *)((const char *)h + i);
    return NULL;
}
void *memccpy(void *d, const void *s, int c, size_t n) { unsigned char *dd = (unsigned char *)d; const unsigned char *ss = (const unsigned char *)s; for (size_t i = 0; i < n; ++i) { dd[i] = ss[i]; if (ss[i] == (unsigned char)c) return dd + i + 1; } return NULL; }
char *stpcpy(char *d, const char *s) { size_t n = strlen(s); memcpy(d, s, n + 1); return d + n; }
char *strsignal(int sig) { static char b[24]; snprintf(b, sizeof b, "Signal %d", sig); return b; }
char *basename(char *path) {
    if (!path || !*path) return (char *)".";
    size_t n = strlen(path); while (n > 1 && path[n - 1] == '/') path[--n] = 0;
    char *s = strrchr(path, '/'); return (s && n > 1) ? s + 1 : path;
}
char *dirname(char *path) {
    if (!path || !*path) return (char *)".";
    size_t n = strlen(path); while (n > 1 && path[n - 1] == '/') path[--n] = 0;
    char *s = strrchr(path, '/'); if (!s) return (char *)".";
    if (s == path) { path[1] = 0; return path; }
    while (s > path && s[-1] == '/') --s;
    *s = 0; return path;
}

/* ---- getopt ---- */
char *optarg; int optind = 1, opterr = 1, optopt;
static int s_optpos;
static int getopt_core(int argc, char *const argv[], const char *opts, const struct option *lo, int *lidx) {
    optarg = NULL;
    if (optind >= argc) return -1;
    char *a = argv[optind];
    if (!s_optpos) {
        if (a[0] != '-' || a[1] == 0) return -1;
        if (a[1] == '-' && a[2] == 0) { ++optind; return -1; }
        if (a[1] == '-' && lo) {
            char *nm = a + 2, *eq = strchr(nm, '='); size_t nl = eq ? (size_t)(eq - nm) : strlen(nm);
            for (int i = 0; lo[i].name; ++i) {
                if (strlen(lo[i].name) != nl || strncmp(lo[i].name, nm, nl)) continue;
                ++optind;
                if (lidx) *lidx = i;
                if (lo[i].has_arg == required_argument) { if (eq) optarg = eq + 1; else if (optind < argc) optarg = argv[optind++]; else return '?'; }
                else if (lo[i].has_arg == optional_argument && eq) optarg = eq + 1;
                if (lo[i].flag) { *lo[i].flag = lo[i].val; return 0; }
                return lo[i].val;
            }
            ++optind; return '?';
        }
        s_optpos = 1;
    }
    int c = (unsigned char)a[s_optpos++];
    const char *p = strchr(opts, c);
    if (!p || c == ':') { optopt = c; if (!a[s_optpos]) { ++optind; s_optpos = 0; } return '?'; }
    if (p[1] == ':') {
        if (a[s_optpos]) optarg = a + s_optpos;
        else if (optind + 1 < argc) optarg = argv[++optind];
        else { ++optind; s_optpos = 0; optopt = c; return opts[0] == ':' ? ':' : '?'; }
        ++optind; s_optpos = 0; return c;
    }
    if (!a[s_optpos]) { ++optind; s_optpos = 0; }
    return c;
}
int getopt(int argc, char *const argv[], const char *opts) { return getopt_core(argc, argv, opts, NULL, NULL); }
int getopt_long(int argc, char *const argv[], const char *opts, const struct option *lo, int *idx) { return getopt_core(argc, argv, opts, lo, idx); }

/* ====================================================================== signals */
static sighandler_t s_sig[32];
sighandler_t signal(int sig, sighandler_t h) {
    if (sig <= 0 || sig >= 32 || sig == SIGKILL) { errno = EINVAL; return SIG_ERR; }
    sighandler_t old = s_sig[sig];
    if (cos_signal(sig, (cos_sighandler_t)h) < 0) { errno = EINVAL; return SIG_ERR; }
    s_sig[sig] = h; return old;
}
int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (old) { memset(old, 0, sizeof *old); if (sig > 0 && sig < 32) old->sa_handler = s_sig[sig]; }
    if (act) return signal(sig, act->sa_handler) == SIG_ERR ? -1 : 0;
    return 0;
}
int raise(int sig) { return cos_kill(cos_getpid(), sig) < 0 ? fail(EINVAL) : 0; }
int kill(int pid, int sig) { return cos_kill(pid, sig) < 0 ? fail(EPERM) : 0; }
int sigemptyset(sigset_t *s) { *s = 0; return 0; }
int sigfillset(sigset_t *s) { *s = ~0UL; return 0; }
int sigaddset(sigset_t *s, int n) { *s |= 1UL << n; return 0; }
int sigdelset(sigset_t *s, int n) { *s &= ~(1UL << n); return 0; }
int sigismember(const sigset_t *s, int n) { return (*s >> n) & 1; }
int sigprocmask(int how, const sigset_t *set, sigset_t *old) { (void)how; (void)set; if (old) *old = 0; return 0; }

/* ====================================================================== mmap */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)addr;
    uint32_t p = 0; if (prot & PROT_READ) p |= COS_PROT_READ; if (prot & PROT_WRITE) p |= COS_PROT_WRITE; if (prot & PROT_EXEC) p |= COS_PROT_EXEC;
    if (!(p & COS_PROT_WRITE) && fd >= 0 && !(flags & MAP_ANONYMOUS)) p |= COS_PROT_WRITE;       /* filled below, protected afterwards */
    void *m = cos_mmap(len, p | COS_PROT_READ);
    if (!m) { errno = ENOMEM; return MAP_FAILED; }
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {                          /* a file mapping is a private copy (MAP_SHARED changes do not reach the file) */
        fdent_t *e = fd_get(fd);
        if (!e || e->type != T_FILE) { cos_munmap(m, len); errno = EBADF; return MAP_FAILED; }
        long long keep = e->pos; cos_lseek(e->kfd, off);
        size_t got = 0; while (got < len) { ssize_t r = cos_fd_read(e->kfd, (char *)m + got, len - got); if (r <= 0) break; got += (size_t)r; }
        cos_lseek(e->kfd, keep);
        if (!(prot & PROT_WRITE)) cos_mprotect(m, len, (uint32_t)((prot & PROT_EXEC) ? COS_PROT_READ | COS_PROT_EXEC : COS_PROT_READ));
    }
    return m;
}
int munmap(void *addr, size_t len) { return cos_munmap(addr, len) == 0 ? 0 : fail(EINVAL); }
int mprotect(void *addr, size_t len, int prot) {
    uint32_t p = 0; if (prot & PROT_READ) p |= COS_PROT_READ; if (prot & PROT_WRITE) p |= COS_PROT_WRITE; if (prot & PROT_EXEC) p |= COS_PROT_EXEC;
    return cos_mprotect(addr, len, p) == 0 ? 0 : fail(EINVAL);
}

/* ====================================================================== sockets */
#define SOCK_TMO_DEFAULT 60000
/* The kernel loops back packets for the interface's own address only, so 127.x.x.x is mapped to that address (when it has one). */
static uint32_t ip_host(const struct sockaddr_in *a) {
    uint32_t ip = ntohl(a->sin_addr.s_addr);
    if ((ip >> 24) == 127) {
        cos_netinfo_t ni; memset(&ni, 0, sizeof ni);
        if (cos_net_info(&ni) == 0 && ni.ip[0]) return ((uint32_t)ni.ip[0] << 24) | ((uint32_t)ni.ip[1] << 16) | ((uint32_t)ni.ip[2] << 8) | ni.ip[3];
    }
    return ip;
}
static void sa_from(struct sockaddr *out, socklen_t *len, uint32_t ip, uint16_t port) {
    if (!out || !len) return;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(ip);
    socklen_t n = *len < sizeof a ? *len : (socklen_t)sizeof a;
    memcpy(out, &a, n); *len = sizeof a;
}
static fdent_t *sock_get(int fd) { fdent_t *e = fd_get(fd); if (!e) return NULL; if (e->type != T_SOCK) { errno = ENOTSOCK; return NULL; } return e; }

int socket(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET) return fail(EAFNOSUPPORT);
    int t = type & 0xff;
    if (t != SOCK_STREAM && t != SOCK_DGRAM) return fail(EINVAL);
    int sd = cos_socket(t == SOCK_STREAM ? COS_SOCK_STREAM : COS_SOCK_DGRAM);
    if (sd < 0) return neg_errno(sd);
    int fd = fd_alloc(); if (fd < 0) { cos_sock_close(sd); return -1; }
    s_fd[fd].type = T_SOCK; s_fd[fd].kfd = sd; s_fd[fd].sock_type = t; s_fd[fd].rcv_ms = s_fd[fd].snd_ms = SOCK_TMO_DEFAULT;
    if (type & 0x800) s_fd[fd].flags |= O_NONBLOCK;
    return fd;
}
int bind(int fd, const struct sockaddr *a, socklen_t len) {
    fdent_t *e = sock_get(fd); if (!e) return -1;
    if (len < sizeof(struct sockaddr_in)) return fail(EINVAL);
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    int r = cos_bind(e->kfd, ip_host(in), ntohs(in->sin_port)); return r < 0 ? neg_errno(r) : 0;
}
int listen(int fd, int backlog) { fdent_t *e = sock_get(fd); if (!e) return -1; if (backlog < 1) backlog = 1; if (backlog > 8) backlog = 8; int r = cos_listen(e->kfd, backlog); return r < 0 ? neg_errno(r) : 0; }
int connect(int fd, const struct sockaddr *a, socklen_t len) {
    fdent_t *e = sock_get(fd); if (!e) return -1;
    if (len < sizeof(struct sockaddr_in)) return fail(EINVAL);
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    int r = cos_connect(e->kfd, ip_host(in), ntohs(in->sin_port));
    if (r == 0) return 0;
    if (r != -COS_EINPROGRESS) return neg_errno(r);
    if (e->flags & O_NONBLOCK) return fail(EINPROGRESS);
    int w = cos_sock_wait(e->kfd, COS_POLLOUT, 30000);
    if (w < 0) return neg_errno(w);
    if (w & (COS_POLLERR | COS_POLLHUP)) return fail(ECONNREFUSED);
    r = cos_connect(e->kfd, ip_host(in), ntohs(in->sin_port));
    return (r == 0 || r == -COS_EISCONN) ? 0 : neg_errno(r);
}
int accept(int fd, struct sockaddr *a, socklen_t *len) {
    fdent_t *e = sock_get(fd); if (!e) return -1;
    for (;;) {
        cos_sockaddr_t peer; memset(&peer, 0, sizeof peer);
        int sd = cos_accept(e->kfd, &peer);
        if (sd >= 0) {
            int nfd = fd_alloc(); if (nfd < 0) { cos_sock_close(sd); return -1; }
            s_fd[nfd].type = T_SOCK; s_fd[nfd].kfd = sd; s_fd[nfd].sock_type = SOCK_STREAM; s_fd[nfd].rcv_ms = s_fd[nfd].snd_ms = SOCK_TMO_DEFAULT;
            sa_from(a, len, peer.ip, peer.port);
            return nfd;
        }
        if (sd != -COS_EAGAIN) return neg_errno(sd);
        if (e->flags & O_NONBLOCK) return fail(EAGAIN);
        int w = cos_sock_wait(e->kfd, COS_POLLIN, 1000);
        if (w < 0 && w != -COS_ETIMEDOUT) return neg_errno(w);
    }
}
static ssize_t sock_send(fdent_t *e, const void *buf, size_t n, int flags) {
    (void)flags;
    size_t done = 0;
    while (done < n) {
        ssize_t r = cos_send(e->kfd, (const char *)buf + done, n - done);
        if (r > 0) { done += (size_t)r; continue; }
        if (r == -COS_EAGAIN) {
            if (e->flags & O_NONBLOCK || (flags & MSG_DONTWAIT)) return done ? (ssize_t)done : fail(EAGAIN);
            int w = cos_sock_wait(e->kfd, COS_POLLOUT, e->snd_ms);
            if (w < 0) return done ? (ssize_t)done : neg_errno(w);
            continue;
        }
        return done ? (ssize_t)done : neg_errno(r);
    }
    return (ssize_t)done;
}
static ssize_t sock_recv(fdent_t *e, void *buf, size_t n, int flags, cos_sockaddr_t *from) {
    for (;;) {
        ssize_t r = from ? cos_recvfrom(e->kfd, buf, n, from) : cos_recv(e->kfd, buf, n);
        if (r >= 0) return r;
        if (r != -COS_EAGAIN) return neg_errno(r);
        if ((e->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return fail(EAGAIN);
        int w = cos_sock_wait(e->kfd, COS_POLLIN, e->rcv_ms);
        if (w < 0) return w == -COS_ETIMEDOUT ? fail(EAGAIN) : neg_errno(w);
    }
}
ssize_t send(int fd, const void *buf, size_t n, int flags) { fdent_t *e = sock_get(fd); if (!e) return -1; return sock_send(e, buf, n, flags); }
ssize_t recv(int fd, void *buf, size_t n, int flags) { fdent_t *e = sock_get(fd); if (!e) return -1; return sock_recv(e, buf, n, flags, NULL); }
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *a, socklen_t len) {
    fdent_t *e = sock_get(fd); if (!e) return -1;
    if (e->sock_type != SOCK_DGRAM || !a) return sock_send(e, buf, n, flags);
    if (len < sizeof(struct sockaddr_in)) return fail(EINVAL);
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    ssize_t r = cos_sendto(e->kfd, buf, n, ip_host(in), ntohs(in->sin_port));
    return r < 0 ? neg_errno(r) : r;
}
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *a, socklen_t *len) {
    fdent_t *e = sock_get(fd); if (!e) return -1;
    if (e->sock_type != SOCK_DGRAM) { ssize_t r = sock_recv(e, buf, n, flags, NULL); if (r >= 0 && a) getpeername(fd, a, len); return r; }
    cos_sockaddr_t from; memset(&from, 0, sizeof from);
    ssize_t r = sock_recv(e, buf, n, flags, &from);
    if (r >= 0) sa_from(a, len, from.ip, from.port);
    return r;
}
int shutdown(int fd, int how) { (void)how; return sock_get(fd) ? 0 : -1; }
int setsockopt(int fd, int level, int opt, const void *v, socklen_t len) {
    fdent_t *e = sock_get(fd); if (!e) return -1;
    if (level == SOL_SOCKET && (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO) && len >= sizeof(struct timeval)) {
        const struct timeval *tv = (const struct timeval *)v; int ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000); if (ms <= 0) ms = SOCK_TMO_DEFAULT;
        if (opt == SO_RCVTIMEO) e->rcv_ms = ms; else e->snd_ms = ms;
    }
    return 0;                                                          /* SO_REUSEADDR, TCP_NODELAY, SO_KEEPALIVE ...: accepted, nothing to do */
}
int getsockopt(int fd, int level, int opt, void *v, socklen_t *len) {
    if (!sock_get(fd)) return -1;
    (void)level;
    if (opt == SO_ERROR && v && len && *len >= sizeof(int)) { *(int *)v = 0; *len = sizeof(int); return 0; }
    if (v && len && *len >= sizeof(int)) { *(int *)v = 0; *len = sizeof(int); }
    return 0;
}
int getsockname(int fd, struct sockaddr *a, socklen_t *len) { fdent_t *e = sock_get(fd); if (!e) return -1; cos_sockaddr_t s; if (cos_getsockname(e->kfd, &s) < 0) return fail(EINVAL); sa_from(a, len, s.ip, s.port); return 0; }
int getpeername(int fd, struct sockaddr *a, socklen_t *len) { fdent_t *e = sock_get(fd); if (!e) return -1; cos_sockaddr_t s; if (cos_getpeername(e->kfd, &s) < 0) return fail(ENOTCONN); sa_from(a, len, s.ip, s.port); return 0; }

/* ---- addresses, names ---- */
in_addr_t inet_addr(const char *s) { uint32_t ip; if (cos_ip_parse(s, &ip) != 0) return (in_addr_t)-1; return htonl(ip); }
int inet_aton(const char *s, struct in_addr *a) { uint32_t ip; if (cos_ip_parse(s, &ip) != 0) return 0; a->s_addr = htonl(ip); return 1; }
int inet_pton(int af, const char *src, void *dst) { if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; } uint32_t ip; if (cos_ip_parse(src, &ip) != 0) return 0; ((struct in_addr *)dst)->s_addr = htonl(ip); return 1; }
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return NULL; }
    uint32_t ip = ntohl(((const struct in_addr *)src)->s_addr);
    int n = snprintf(dst, size, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    if (n < 0 || (socklen_t)n >= size) { errno = ENOSPC; return NULL; }
    return dst;
}
char *inet_ntoa(struct in_addr a) { static char b[16]; inet_ntop(AF_INET, &a, b, sizeof b); return b; }
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    uint32_t ip = 0;
    if (node) { if (cos_resolve_wait(node, &ip, 8000) < 0) return EAI_NONAME; }
    else if (!(hints && (hints->ai_flags & AI_PASSIVE))) ip = INADDR_LOOPBACK;
    int port = 0;
    if (service) { char *end; long v = strtol(service, &end, 10); if (*end) { if (!strcmp(service, "http")) v = 80; else if (!strcmp(service, "https")) v = 443; else if (!strcmp(service, "ftp")) v = 21; else if (!strcmp(service, "ssh")) v = 22; else if (!strcmp(service, "domain")) v = 53; else return EAI_SERVICE; } port = (int)v; }
    int st = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    struct addrinfo *ai = (struct addrinfo *)cos_calloc(1, sizeof *ai + sizeof(struct sockaddr_in) + 64);
    if (!ai) return EAI_MEMORY;
    struct sockaddr_in *sa = (struct sockaddr_in *)(ai + 1);
    sa->sin_family = AF_INET; sa->sin_port = htons((uint16_t)port); sa->sin_addr.s_addr = htonl(ip);
    ai->ai_family = AF_INET; ai->ai_socktype = st; ai->ai_protocol = st == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    ai->ai_addrlen = sizeof *sa; ai->ai_addr = (struct sockaddr *)sa;
    if (hints && (hints->ai_flags & AI_CANONNAME) && node) { char *c = (char *)(sa + 1); strlcpy(c, node, 64); ai->ai_canonname = c; }
    *res = ai; return 0;
}
void freeaddrinfo(struct addrinfo *res) { while (res) { struct addrinfo *n = res->ai_next; cos_free(res); res = n; } }
const char *gai_strerror(int code) { switch (code) { case EAI_NONAME: return "Name or service not known"; case EAI_SERVICE: return "Service not supported"; case EAI_MEMORY: return "Out of memory"; default: return "Name resolution failed"; } }
struct hostent *gethostbyname(const char *name) {
    static struct hostent h; static char *addrs[2]; static uint32_t ipn; static char nm[128];
    uint32_t ip; if (cos_resolve_wait(name, &ip, 8000) < 0) return NULL;
    ipn = htonl(ip); addrs[0] = (char *)&ipn; addrs[1] = NULL;
    strlcpy(nm, name, sizeof nm); h.h_name = nm; h.h_aliases = NULL; h.h_addrtype = AF_INET; h.h_length = 4; h.h_addr_list = addrs;
    return &h;
}

/* ====================================================================== poll / select */
static int fd_ready(int fd, int want, int *err_out) {
    fdent_t *e = fd_get(fd); if (!e) { *err_out = POLLNVAL; return 0; }
    int rev = 0;
    switch (e->type) {
    case T_SOCK: { cos_pollfd_t p = { e->kfd, (int16_t)(((want & POLLIN) ? COS_POLLIN : 0) | ((want & POLLOUT) ? COS_POLLOUT : 0)), 0 }; if (cos_sock_poll(&p, 1) > 0) { rev = p.revents & (COS_POLLIN | COS_POLLOUT | COS_POLLERR | COS_POLLHUP); } break; }
    case T_PIPE_R: { cos_mutex_lock(&e->pipe->m); if (e->pipe->count > 0) rev |= POLLIN; else if (e->pipe->writers == 0) rev |= POLLHUP | POLLIN; cos_mutex_unlock(&e->pipe->m); break; }
    case T_PIPE_W: { cos_mutex_lock(&e->pipe->m); if (e->pipe->count < (int)sizeof e->pipe->buf) rev |= POLLOUT; cos_mutex_unlock(&e->pipe->m); break; }
    default: rev = want & (POLLIN | POLLOUT); break;             /* files and the console never block */
    }
    return rev;
}
int poll(struct pollfd *fds, nfds_t n, int timeout_ms) {
    uint64_t t0 = cos_time_ms();
    for (;;) {
        int ready = 0;
        for (nfds_t i = 0; i < n; ++i) {
            int err = 0; fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            int r = fd_ready(fds[i].fd, fds[i].events, &err);
            fds[i].revents = (short)(r | err);
            if (fds[i].revents) ++ready;
        }
        if (ready || timeout_ms == 0) return ready;
        if (timeout_ms > 0 && cos_time_ms() - t0 >= (uint64_t)timeout_ms) return 0;
        cos_sleep_ms(1);
    }
}
int select(int nfds, fd_set *r, fd_set *w, fd_set *x, struct timeval *tv) {
    uint64_t t0 = cos_time_ms(); long long limit = tv ? (long long)tv->tv_sec * 1000 + tv->tv_usec / 1000 : -1;
    fd_set rr, ww; if (r) rr = *r; if (w) ww = *w;
    for (;;) {
        int ready = 0; fd_set or_, ow_; FD_ZERO(&or_); FD_ZERO(&ow_);
        for (int fd = 0; fd < nfds && fd < FD_SETSIZE; ++fd) {
            int want = ((r && FD_ISSET(fd, &rr)) ? POLLIN : 0) | ((w && FD_ISSET(fd, &ww)) ? POLLOUT : 0);
            if (!want) continue;
            int err = 0; int rev = fd_ready(fd, want, &err);
            if ((want & POLLIN) && (rev & (POLLIN | POLLHUP | POLLERR))) { FD_SET(fd, &or_); ++ready; }
            if ((want & POLLOUT) && (rev & POLLOUT)) { FD_SET(fd, &ow_); ++ready; }
        }
        if (ready || limit == 0 || (limit > 0 && (long long)(cos_time_ms() - t0) >= limit)) { if (r) *r = or_; if (w) *w = ow_; if (x) FD_ZERO(x); return ready; }
        cos_sleep_ms(1);
    }
}

/* ====================================================================== pthreads */
typedef struct { void *(*fn)(void *); void *arg; void *ret; int64_t tid; volatile int done; int detached; int used; } thr_t;
#define NTHR 64
static thr_t s_thr[NTHR];
static cos_mutex_t s_thr_lock = COS_MUTEX_INIT;
static thr_t *thr_by_tid(int64_t tid) { for (int i = 0; i < NTHR; ++i) if (s_thr[i].used && s_thr[i].tid == tid) return &s_thr[i]; return NULL; }
static void thr_main(void *p) {
    thr_t *t = (thr_t *)p;
    t->tid = cos_gettid();
    t->ret = t->fn(t->arg);
    t->done = 1;
    if (t->detached) { cos_mutex_lock(&s_thr_lock); t->used = 0; cos_mutex_unlock(&s_thr_lock); }
}
int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    cos_mutex_lock(&s_thr_lock);
    thr_t *t = NULL; for (int i = 0; i < NTHR; ++i) if (!s_thr[i].used) { t = &s_thr[i]; break; }
    if (!t) { cos_mutex_unlock(&s_thr_lock); return EAGAIN; }
    memset(t, 0, sizeof *t); t->used = 1; t->fn = fn; t->arg = arg; t->detached = attr && attr->kind == PTHREAD_CREATE_DETACHED;
    t->tid = -1;
    cos_mutex_unlock(&s_thr_lock);
    int64_t id = cos_thread_create(thr_main, t);
    if (id <= 0) { t->used = 0; return EAGAIN; }
    *out = (pthread_t)id; t->tid = id;                      /* thr_main sets it again (the same number) */
    return 0;
}
int pthread_join(pthread_t th, void **ret) {
    thr_t *t = thr_by_tid((int64_t)th); if (!t) return 3;
    while (!t->done) cos_thread_join((int64_t)th, 50);
    cos_thread_join((int64_t)th, 1000);
    if (ret) *ret = t->ret;
    cos_mutex_lock(&s_thr_lock); t->used = 0; cos_mutex_unlock(&s_thr_lock);
    return 0;
}
int pthread_detach(pthread_t th) { thr_t *t = thr_by_tid((int64_t)th); if (!t) return 3; t->detached = 1; if (t->done) t->used = 0; return 0; }
void pthread_exit(void *ret) {
    thr_t *t = thr_by_tid(cos_gettid());
    if (t) { t->ret = ret; t->done = 1; if (t->detached) t->used = 0; }
    cos_thread_exit(0);
}
pthread_t pthread_self(void) { return (pthread_t)cos_gettid(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
int sched_yield(void) { cos_yield(); return 0; }
int pthread_attr_init(pthread_attr_t *a) { a->kind = 0; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setdetachstate(pthread_attr_t *a, int s) { a->kind = s; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t n) { (void)a; (void)n; return 0; }

int pthread_mutexattr_init(pthread_mutexattr_t *a) { a->kind = 0; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int t) { a->kind = t; return 0; }
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { memset(m, 0, sizeof *m); m->kind = a ? a->kind : 0; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m) {
    unsigned long me = (unsigned long)cos_gettid();
    if (m->kind == PTHREAD_MUTEX_RECURSIVE && m->count > 0 && m->rowner == me) { m->count++; return 0; }
    if (m->kind == PTHREAD_MUTEX_ERRORCHECK && m->count > 0 && m->rowner == me) return EDEADLK;
    cos_mutex_lock((cos_mutex_t *)m);
    m->rowner = me; m->count = 1; return 0;
}
int pthread_mutex_trylock(pthread_mutex_t *m) {
    unsigned long me = (unsigned long)cos_gettid();
    if (m->kind == PTHREAD_MUTEX_RECURSIVE && m->count > 0 && m->rowner == me) { m->count++; return 0; }
    if (cos_mutex_trylock((cos_mutex_t *)m) != 0) return EBUSY;
    m->rowner = me; m->count = 1; return 0;
}
int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (m->kind == PTHREAD_MUTEX_RECURSIVE && m->count > 1) { m->count--; return 0; }
    m->count = 0; m->rowner = 0;
    cos_mutex_unlock((cos_mutex_t *)m); return 0;
}
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { (void)a; c->seq = c->waiters = 0; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    int saved = m->count; unsigned long ro = m->rowner;
    int r = cos_cond_wait((cos_cond_t *)c, (cos_mutex_t *)m);
    m->count = saved; m->rowner = ro; return r == 0 ? 0 : EINVAL;
}
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abst) {
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    long long ms = ((long long)abst->tv_sec - now.tv_sec) * 1000 + (abst->tv_nsec - now.tv_nsec) / 1000000;
    if (ms <= 0) ms = 1;
    int saved = m->count; unsigned long ro = m->rowner;
    int r = cos_cond_timedwait((cos_cond_t *)c, (cos_mutex_t *)m, (uint64_t)ms);
    m->count = saved; m->rowner = ro;
    return r == 0 ? 0 : ETIMEDOUT;
}
int pthread_cond_signal(pthread_cond_t *c) { cos_cond_signal((cos_cond_t *)c); return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { cos_cond_broadcast((cos_cond_t *)c); return 0; }
int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a) { (void)a; l->state = 0; return 0; }
int pthread_rwlock_destroy(pthread_rwlock_t *l) { (void)l; return 0; }
int pthread_rwlock_rdlock(pthread_rwlock_t *l) { return cos_rwlock_rdlock((cos_rwlock_t *)l); }
int pthread_rwlock_wrlock(pthread_rwlock_t *l) { return cos_rwlock_wrlock((cos_rwlock_t *)l); }
int pthread_rwlock_unlock(pthread_rwlock_t *l) { return cos_rwlock_unlock((cos_rwlock_t *)l); }
int pthread_once(pthread_once_t *o, void (*fn)(void)) { return cos_once((cos_once_t *)o, fn); }
int sem_init(sem_t *s, int pshared, unsigned v) { (void)pshared; cos_sem_init((cos_sem_t *)s, v); return 0; }
int sem_destroy(sem_t *s) { (void)s; return 0; }
int sem_wait(sem_t *s) { return cos_sem_wait((cos_sem_t *)s) == 0 ? 0 : fail(EINVAL); }
int sem_trywait(sem_t *s) { return cos_sem_trywait((cos_sem_t *)s) == 0 ? 0 : fail(EAGAIN); }
int sem_post(sem_t *s) { return cos_sem_post((cos_sem_t *)s) == 0 ? 0 : fail(EINVAL); }

/* thread-specific data: a small table of (thread, key) -> value */
#define NKEY 64
#define NTSD 512
static struct { int used; void (*dtor)(void *); } s_key[NKEY];
static struct { int used; unsigned key; int64_t tid; const void *val; } s_tsd[NTSD];
static cos_mutex_t s_tsd_lock = COS_MUTEX_INIT;
int pthread_key_create(pthread_key_t *k, void (*dtor)(void *)) {
    cos_mutex_lock(&s_tsd_lock);
    for (int i = 0; i < NKEY; ++i) if (!s_key[i].used) { s_key[i].used = 1; s_key[i].dtor = dtor; *k = (pthread_key_t)i; cos_mutex_unlock(&s_tsd_lock); return 0; }
    cos_mutex_unlock(&s_tsd_lock); return EAGAIN;
}
int pthread_key_delete(pthread_key_t k) { if (k >= NKEY) return EINVAL; cos_mutex_lock(&s_tsd_lock); s_key[k].used = 0; for (int i = 0; i < NTSD; ++i) if (s_tsd[i].used && s_tsd[i].key == k) s_tsd[i].used = 0; cos_mutex_unlock(&s_tsd_lock); return 0; }
void *pthread_getspecific(pthread_key_t k) { int64_t me = cos_gettid(); void *v = NULL; cos_mutex_lock(&s_tsd_lock); for (int i = 0; i < NTSD; ++i) if (s_tsd[i].used && s_tsd[i].key == k && s_tsd[i].tid == me) { v = (void *)s_tsd[i].val; break; } cos_mutex_unlock(&s_tsd_lock); return v; }
int pthread_setspecific(pthread_key_t k, const void *v) {
    if (k >= NKEY || !s_key[k].used) return EINVAL;
    int64_t me = cos_gettid(); int rc = ENOMEM;
    cos_mutex_lock(&s_tsd_lock);
    int freei = -1;
    for (int i = 0; i < NTSD; ++i) { if (s_tsd[i].used && s_tsd[i].key == k && s_tsd[i].tid == me) { s_tsd[i].val = v; rc = 0; goto out; } if (!s_tsd[i].used && freei < 0) freei = i; }
    if (freei >= 0) { s_tsd[freei].used = 1; s_tsd[freei].key = k; s_tsd[freei].tid = me; s_tsd[freei].val = v; rc = 0; }
out: cos_mutex_unlock(&s_tsd_lock); return rc;
}
