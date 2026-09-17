/**
 * cos.h - the C API for .c-os userland programs.
 *
 * WHY THIS EXISTS
 * ----------------
 * Every .c-os program written before this had to be hand-written x86-64
 * assembly, because there was no C runtime at all: no _start that sets up
 * argc/argv per the ABI, no syscall wrappers, no malloc, no string or
 * printf functions. That made userland effectively write-only - fine for
 * small syscall validation tests, impossible for a real application.
 *
 * This is a freestanding C library (no host libc, no OS headers) built
 * against the kernel's actual syscall numbers. Link with cos_crt0.o and
 * libcos.a using validation/cos_programs/cos.ld.
 *
 * DELIBERATELY NOT A POSIX LIBC
 * ------------------------------
 * The names here mirror POSIX where the semantics genuinely match
 * (open/read/write/close/malloc/printf), but this does NOT aim to be a
 * drop-in libc - the kernel underneath does not provide POSIX semantics
 * for everything (no fd 0/1/2 by default, whole-file helpers alongside
 * streaming fds, spawn instead of fork). Pretending otherwise would make
 * ported code compile and then behave subtly wrong, which is worse than
 * an honestly different API.
 */
#ifndef COS_H
#define COS_H

typedef unsigned long  size_t;
typedef long           ssize_t;
typedef unsigned long  uint64_t;
typedef long           int64_t;
typedef unsigned int   uint32_t;
typedef int            int32_t;
typedef unsigned short uint16_t;
typedef short          int16_t;
typedef unsigned char  uint8_t;
typedef signed char    int8_t;
typedef unsigned long  uintptr_t;
typedef long           intptr_t;
typedef unsigned long  uintmax_t;
typedef _Bool          bool;
#define true  1
#define false 0
#define NULL  ((void*)0)

/* ---- process ---------------------------------------------------------- */
void     cos_exit(int status) __attribute__((noreturn));
int      cos_getpid(void);
void     cos_yield(void);
void     cos_sleep_ms(uint64_t ms);
/* Returns the child's pid, or -1. */
int64_t  cos_spawn(const char *path);
/* Spawn with arguments and an environment, both NULL-terminated arrays.
 * cos_spawn() above cannot pass either - the syscall behind it takes
 * only a path - so a child started that way always saw argc == 1 and an
 * empty environment. */
int64_t  cos_spawn_argv(const char *path, const char *const *argv,
                        const char *const *envp);
/* blocking=false performs a WNOHANG-style single check. Returns the
 * collected child's pid, or -1. */
int64_t  cos_waitpid(int64_t pid, int *status, bool blocking);

/* ---- console (serial) -------------------------------------------------- */
ssize_t  cos_write(const char *buf, size_t len);
void     cos_puts(const char *s);
/* Supports %s %d %i %u %x %c %% and %ld/%lu/%lx. No floating point - the
 * kernel's own vsnprintf has no float support either, and silently
 * printing a literal "%f" (which is what that gap causes) would be worse
 * than not offering the specifier at all. */
/* A complete C99 formatter: flags (-+ #0), field width (including `*`),
 * precision (including `*`), the length modifiers hh h l ll z t j L, and
 * the conversions d i u o x X c s p f F e E g G %%.
 *
 * `%n` is REFUSED rather than ignored. It writes through a pointer from
 * the argument list, which is the classic format-string exploit
 * primitive; a program using it gets a visible marker in the output
 * instead of a silent arbitrary write.
 *
 * Floating point IS supported now. The previous formatter had none, and
 * emitted the literal text "%f" - which is worse than refusing, because
 * it turns a missing feature into corrupted output. */
int      cos_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int      cos_snprintf(char *buf, size_t size, const char *fmt, ...)
             __attribute__((format(printf, 3, 4)));
int      cos_vprintf(const char *fmt, __builtin_va_list ap);
int      cos_vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);

/* ---- memory ------------------------------------------------------------ */
void    *cos_sbrk(long increment);
void    *cos_malloc(size_t size);
void    *cos_calloc(size_t count, size_t size);
void    *cos_realloc(void *ptr, size_t size);
void     cos_free(void *ptr);
/* Anonymous mmap. prot is a COS_PROT_* bitmask. */
#define COS_PROT_READ  1u
#define COS_PROT_WRITE 2u
void    *cos_mmap(size_t length, uint32_t prot);
int      cos_munmap(void *addr, size_t length);

/* ---- whole-file I/O ---------------------------------------------------- */
ssize_t  cos_read_file(const char *path, void *buf, size_t len);
ssize_t  cos_write_file(const char *path, const void *buf, size_t len);

/* ---- streaming file descriptors ---------------------------------------- */
#define COS_O_RDONLY        1
#define COS_O_WRONLY_CREATE 2
#define COS_O_WRONLY_APPEND 3
#define COS_O_RDWR_CREATE   4

#define COS_DIRENT_NAME_MAX 255
typedef struct {
    char     name[COS_DIRENT_NAME_MAX + 1];
    uint8_t  is_dir;
    uint64_t size;
} cos_dirent_t;

int      cos_open(const char *path, uint32_t flags);
int      cos_opendir(const char *path);
ssize_t  cos_fd_read(int fd, void *buf, size_t len);
ssize_t  cos_fd_write(int fd, const void *buf, size_t len);
int64_t  cos_lseek(int fd, int64_t offset);
/* Returns 1 (entry filled), 0 (end of directory - a real outcome, not an
 * error), or -1. */
int      cos_readdir(int fd, cos_dirent_t *out);
int      cos_close(int fd);

/* ---- shared libraries (.c-osll) ---------------------------------------- */
int64_t  cos_dlopen(const char *path);
void    *cos_dlsym(int64_t handle, const char *name);

/* ---- signals ----------------------------------------------------------- */
typedef void (*cos_sighandler_t)(int);
#define COS_SIG_DFL ((cos_sighandler_t)0)
#define COS_SIG_IGN ((cos_sighandler_t)1)
int      cos_signal(int signum, cos_sighandler_t handler);
int      cos_kill(int pid, int signum);

/* ---- GUI windows ------------------------------------------------------- */
#define COS_KEY_NONE      0
#define COS_KEY_ENTER     1
#define COS_KEY_BACKSPACE 2
#define COS_KEY_ESC       3
#define COS_KEY_UP        4
#define COS_KEY_DOWN      5
#define COS_KEY_LEFT      6
#define COS_KEY_RIGHT     7
#define COS_KEY_TAB       8
#define COS_KEY_DELETE    9

#define COS_MOD_SHIFT 0x01u
#define COS_MOD_CTRL  0x02u
#define COS_MOD_ALT   0x04u

typedef struct {
    char    ascii;
    uint8_t special;
    uint8_t modifiers;
} cos_key_event_t;

int64_t  cos_win_create(const char *title, int32_t w, int32_t h);
int      cos_win_fill(int64_t win, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
int      cos_win_text(int64_t win, int32_t x, int32_t y, uint32_t fg, uint32_t bg, const char *s);
int      cos_win_clear(int64_t win);
/* Returns 1 if a key was popped, 0 if none pending, -1 on error. */
int      cos_win_poll_key(int64_t win, cos_key_event_t *out);

/* ---- threads ------------------------------------------------------------
 * The kernel always supported multiple threads per process; nothing
 * exposed it to ring3 until now. */
typedef void (*cos_thread_fn)(void *arg);
/* Returns a thread id (>0) or -1. */
int64_t  cos_thread_create(cos_thread_fn fn, void *arg);
void     cos_thread_exit(int code) __attribute__((noreturn));
/* Returns 0 once the thread has finished, -1 on timeout. */
int      cos_thread_join(int64_t tid, uint64_t timeout_ms);

/* ---- time --------------------------------------------------------------- */
uint64_t cos_time_ms(void);     /* monotonic milliseconds since boot */
uint64_t cos_time_unix(void);   /* wall-clock seconds from the RTC */

/* ---- filesystem mutation ------------------------------------------------- */
typedef struct {
    uint64_t size;
    uint8_t  is_dir;
} cos_stat_t;

int cos_mkdir(const char *path);
int cos_unlink(const char *path);
int cos_rename(const char *oldp, const char *newp);
int cos_stat(const char *path, cos_stat_t *out);

/* ---- setjmp / longjmp ----------------------------------------------------
 * Pure userland - no syscall involved. Saves the callee-saved registers,
 * RSP and the return address, which is the complete set the System V
 * x86-64 ABI requires a function to preserve; caller-saved registers are
 * deliberately NOT saved, exactly as in a real setjmp, because the ABI
 * already permits any call to clobber them. */
typedef unsigned long cos_jmp_buf[8];
int  cos_setjmp(cos_jmp_buf env) __attribute__((returns_twice));
void cos_longjmp(cos_jmp_buf env, int val) __attribute__((noreturn));

/* ---- networking ----------------------------------------------------------
 * Exposed at the HTTP/DNS level, which is what the kernel's network stack
 * actually provides - NOT BSD sockets. A socket API would be a new
 * abstraction layered on top with nothing proving it works; this is the
 * surface that genuinely exists and is already exercised by the bundled
 * browser. */
bool     cos_net_available(void);
/* Writes 4 bytes of IPv4 into ip_out. Returns 0 on success, -1 otherwise. */
int      cos_net_resolve(const char *hostname, uint8_t *ip_out);
/* Returns the number of body bytes received, or -1. */
ssize_t  cos_net_http_get(const char *url, void *buf, size_t len);

/* ---- FILE* stdio ----------------------------------------------------------
 * Buffered stream I/O over the fd syscalls. This is the single biggest
 * thing standing between "runs programs written for C-OS" and "a small
 * external C program can be ported with modest changes" - essentially
 * every real C program uses fopen/fgets/fprintf rather than raw
 * descriptors.
 *
 * Streams are buffered, so fflush()/fclose() matter: unflushed output is
 * lost if a program exits without closing, exactly as with a real stdio.
 */
typedef struct cos_FILE cos_FILE;

#define COS_EOF (-1)

cos_FILE *cos_fopen(const char *path, const char *mode);  /* "r", "w", "a", "r+" */
int       cos_fclose(cos_FILE *f);
size_t    cos_fread(void *ptr, size_t size, size_t count, cos_FILE *f);
size_t    cos_fwrite(const void *ptr, size_t size, size_t count, cos_FILE *f);
int       cos_fgetc(cos_FILE *f);
int       cos_fputc(int c, cos_FILE *f);
char     *cos_fgets(char *buf, int size, cos_FILE *f);
int       cos_fputs(const char *s, cos_FILE *f);
int       cos_fprintf(cos_FILE *f, const char *fmt, ...);
int       cos_fflush(cos_FILE *f);
int       cos_fseek(cos_FILE *f, long offset);
long      cos_ftell(cos_FILE *f);
int       cos_feof(cos_FILE *f);
int       cos_ungetc(int c, cos_FILE *f);

/* ---- stdlib staples -------------------------------------------------------
 * The handful of functions almost any ported C program reaches for. */
long      cos_strtol(const char *s, char **end, int base);
unsigned long cos_strtoul(const char *s, char **end, int base);
void      cos_qsort(void *base, size_t n, size_t size,
                    int (*cmp)(const void *, const void *));
char     *cos_strdup(const char *s);
char     *cos_strstr(const char *hay, const char *needle);
char     *cos_strrchr(const char *s, int c);
int       cos_abs(int v);
/* Deterministic PRNG - cos_srand() with cos_time_ms() for a varying seed.
 * Not cryptographically secure, and not presented as such. */
void      cos_srand(unsigned seed);
int       cos_rand(void);
#define COS_RAND_MAX 0x7fffffff

/* ---- strings ----------------------------------------------------------- */
size_t   cos_strlen(const char *s);
int      cos_strcmp(const char *a, const char *b);
int      cos_strncmp(const char *a, const char *b, size_t n);
char    *cos_strcpy(char *dst, const char *src);
char    *cos_strncpy(char *dst, const char *src, size_t n);
char    *cos_strcat(char *dst, const char *src);
char    *cos_strchr(const char *s, int c);
void    *cos_memset(void *dst, int c, size_t n);
void    *cos_memcpy(void *dst, const void *src, size_t n);
int      cos_memcmp(const void *a, const void *b, size_t n);
int      cos_atoi(const char *s);


/* ---- runtime, link map and thread-local storage --------------------------
 *
 * Provided by cos_rtld.c, which is the ring3 half of the loader: it walks
 * the auxiliary vector the kernel built, applies the ifunc relocations
 * the kernel deliberately did not (they call ring3 resolver functions),
 * runs constructors, and calls main().
 *
 * main() now takes three arguments - main(argc, argv, envp) - because the
 * initial stack finally carries an environment. A main() declared with
 * two still works: the third argument is simply ignored, exactly as on
 * any other System V system. */

extern int    cos_argc;
extern char **cos_argv;
extern char **cos_environ;

/* Auxiliary vector lookup. Useful tags: 3 (AT_PHDR), 5 (AT_PHNUM),
 * 6 (AT_PAGESZ), 9 (AT_ENTRY), 25 (AT_RANDOM), 31 (AT_EXECFN). Returns 0
 * for a tag the kernel did not supply. */
uint64_t cos_getauxval(uint64_t tag);
char    *cos_getenv(const char *name);

/* Destructors. Registered automatically for every loaded object's
 * DT_FINI/DT_FINI_ARRAY; a program may add its own. Run in reverse
 * registration order when main() returns. */
int  cos_atexit(void (*fn)(void));
void cos_run_atexit(void);

/* Builds a fresh thread-local storage block for the CALLING thread's use
 * and returns its thread pointer, or 0 if this process has no TLS.
 * cos_thread_create() does this for you; call it directly only if you
 * are starting a thread some other way. */
uint64_t cos_tls_create_block(void);

/* Installs `tp` as this thread's %fs base. Only ring0 can write that
 * MSR, so this is a syscall. Passing 0 clears it. */
int      cos_set_tls(uint64_t tp);
uint64_t cos_get_tls(void);

/* ---- dynamic loading, extended ------------------------------------------
 * cos_dlopen() above is kept as-is. These add what it could not express. */
#define COS_RTLD_LAZY     0x0001   /* accepted and ignored: binding is eager */
#define COS_RTLD_NOW      0x0002
#define COS_RTLD_GLOBAL   0x0100   /* join the process-wide symbol scope     */
#define COS_RTLD_LOCAL    0x0000
#define COS_RTLD_DEFAULT  ((int64_t)0)   /* dlsym across the whole scope     */

int64_t  cos_dlopen_flags(const char *path, uint32_t flags);
int      cos_dlclose(int64_t handle);
/* Address of the read-only link map the kernel published, or 0. Same
 * value the auxiliary vector carries; this is for code that never saw
 * the initial stack. */
uint64_t cos_dl_info(void);


/* ---- math (cos_math.c) ---------------------------------------------------
 * There was no math library at all before this - not a reduced one, none
 * - so any program doing geometry, statistics, signal processing or
 * plotting simply could not be ported.
 *
 * Accuracy is a few ULP across the normal range, established by fuzzing
 * every function against the host libm in validation/userland rather
 * than asserted. No errno and no FP exception flags: this runtime has
 * neither, and a function that pretended to set errno would be lying.
 * Domain errors return NaN. */
int    cos_isnan(double x);
int    cos_isinf(double x);
int    cos_isfinite(double x);
int    cos_signbit(double x);
double cos_nan(void);
double cos_inf(void);

double cos_fabs(double x);
double cos_copysign(double x, double y);
double cos_fmin(double a, double b);
double cos_fmax(double a, double b);

double cos_floor(double x);
double cos_ceil(double x);
double cos_trunc(double x);
/* Ties away from zero, per round(). NOT the ties-to-even the printf
 * formatter uses - the two genuinely differ, and conflating them is a
 * classic source of off-by-one output. */
double cos_round(double x);
double cos_fmod(double x, double y);
double cos_modf(double x, double *ipart);
double cos_frexp(double x, int *exp_out);
double cos_ldexp(double x, int n);

double cos_sqrt(double x);
double cos_cbrt(double x);
double cos_hypot(double a, double b);
double cos_exp(double x);
double cos_exp2(double x);
double cos_log(double x);
double cos_log2(double x);
double cos_log10(double x);
double cos_pow(double x, double y);

double cos_sin(double x);
double cos_cos(double x);
double cos_tan(double x);
double cos_asin(double x);
double cos_acos(double x);
double cos_atan(double x);
double cos_atan2(double y, double x);
double cos_sinh(double x);
double cos_cosh(double x);
double cos_tanh(double x);


/* ---- thread synchronisation (cos_sync.c) ---------------------------------
 *
 * cos_thread_create() existed with nothing to synchronise those threads
 * with - no mutex, no condition variable, no semaphore, not even a
 * documented atomic. A program with two threads touching the same data
 * had no correct way to be written, and the obvious workaround (a spin
 * loop on a volatile flag) is both wrong - no memory ordering - and
 * actively harmful on a single-core schedule, where the spinner holds
 * the CPU that the thread it waits for needs to make progress.
 *
 * Every primitive is futex-backed: the state is a word in your own
 * memory manipulated with atomic instructions, and the kernel is entered
 * only when a thread genuinely has to sleep. An uncontended lock is one
 * atomic instruction and no syscall.
 *
 * ALL WAITS CAN RETURN SPURIOUSLY. Re-check your condition in a loop.
 * That is not defensive padding - a futex hash bucket is shared, so an
 * unrelated wake can reach your waiter, and condition variables are
 * specified to allow it regardless. */

typedef struct { uint32_t state; uint64_t owner; } cos_mutex_t;
typedef struct { uint32_t seq; uint32_t waiters; } cos_cond_t;
typedef struct { uint32_t count; } cos_sem_t;
typedef struct { uint32_t state; } cos_rwlock_t;
typedef struct { uint32_t state; } cos_once_t;

#define COS_MUTEX_INIT   { 0, 0 }
#define COS_COND_INIT    { 0, 0 }
#define COS_RWLOCK_INIT  { 0 }
#define COS_ONCE_INIT    { 0 }

void cos_mutex_init(cos_mutex_t *m);
int  cos_mutex_lock(cos_mutex_t *m);
int  cos_mutex_trylock(cos_mutex_t *m);           /* 0 on success, -1 if held */
int  cos_mutex_timedlock(cos_mutex_t *m, uint64_t timeout_ms);
int  cos_mutex_unlock(cos_mutex_t *m);

void cos_cond_init(cos_cond_t *c);
/* Must be called with `m` held. Releases it while waiting and reacquires
 * it before returning - which is why it takes the mutex at all: reading
 * the wait state and dropping the lock have to happen together, or a
 * signal in the window is lost. */
int  cos_cond_wait(cos_cond_t *c, cos_mutex_t *m);
int  cos_cond_timedwait(cos_cond_t *c, cos_mutex_t *m, uint64_t timeout_ms);
int  cos_cond_signal(cos_cond_t *c);
int  cos_cond_broadcast(cos_cond_t *c);

void cos_sem_init(cos_sem_t *s, uint32_t value);
int  cos_sem_wait(cos_sem_t *s);
int  cos_sem_trywait(cos_sem_t *s);
int  cos_sem_timedwait(cos_sem_t *s, uint64_t timeout_ms);
int  cos_sem_post(cos_sem_t *s);

/* Writers are not starved: a waiting writer blocks new readers from
 * acquiring, so a steady stream of readers cannot hold the lock forever. */
void cos_rwlock_init(cos_rwlock_t *l);
int  cos_rwlock_rdlock(cos_rwlock_t *l);
int  cos_rwlock_tryrdlock(cos_rwlock_t *l);
int  cos_rwlock_wrlock(cos_rwlock_t *l);
int  cos_rwlock_trywrlock(cos_rwlock_t *l);
int  cos_rwlock_unlock(cos_rwlock_t *l);

/* Runs `fn` exactly once. A thread arriving while another is still
 * inside `fn` WAITS for it to finish rather than proceeding with
 * half-built state. */
int  cos_once(cos_once_t *o, void (*fn)(void));

/* The raw primitive, exposed because a program may want to build
 * something these do not cover (a barrier, a channel, a work queue).
 *
 * cos_futex_wait() sleeps only if *addr still equals `expected`; that
 * comparison is what closes the lost-wakeup race, and it is the reason
 * you must pass the value you based your decision on rather than
 * re-reading it inside. Returns 0 on wake, -11 if the word had already
 * changed, -110 on timeout. timeout_ms of 0 waits forever. */
int64_t cos_futex_wait(volatile uint32_t *addr, uint32_t expected,
                       uint64_t timeout_ms);
int64_t cos_futex_wake(volatile uint32_t *addr, uint32_t count);

/* This thread's id. Distinct from cos_getpid(), which names the
 * process. */
int64_t cos_gettid(void);

#endif /* COS_H */
