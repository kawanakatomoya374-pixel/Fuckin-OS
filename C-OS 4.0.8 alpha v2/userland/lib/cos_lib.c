/**
 * cos_lib.c - the .c-os C runtime library implementation.
 *
 * Freestanding: no host libc, no OS headers, only this kernel's syscalls.
 * See userland/include/cos.h for the API and why it is not a POSIX libc.
 */
#include "cos.h"

/* ---- raw syscall plumbing ----------------------------------------------
 *
 * The kernel's syscall entry is `int 0x80` with the number in RAX and
 * arguments in RDI/RSI/RDX/R10/R8/R9 - deliberately R10 rather than RCX
 * for the 4th argument, matching the kernel side (syscall.c reads r->r10),
 * because on x86-64 the `syscall` instruction clobbers RCX and this
 * codebase follows that same register convention for its int-0x80 entry
 * to stay consistent with how the kernel already reads its arguments.
 *
 * "memory" in the clobber list is required: without it the compiler may
 * cache a value in a register across a syscall that reads or writes that
 * memory through a pointer argument (e.g. cos_fd_read filling a buffer),
 * and reuse the stale cached value afterward.
 */
#define SYS_WRITE          0
#define SYS_EXIT           1
#define SYS_WIN_CREATE     2
#define SYS_WIN_FILL       3
#define SYS_WIN_CLEAR      4
#define SYS_BRK            5
#define SYS_SBRK           6
#define SYS_READ_FILE      7
#define SYS_WRITE_FILE     8
#define SYS_YIELD          9
#define SYS_SLEEP_MS      10
#define SYS_GETPID        11
#define SYS_SPAWN         12
#define SYS_WAITPID       13
#define SYS_DLOPEN        14
#define SYS_DLSYM         15
#define SYS_FD_OPEN       16
#define SYS_FD_OPENDIR    17
#define SYS_FD_READ       18
#define SYS_FD_WRITE      19
#define SYS_FD_LSEEK      20
#define SYS_FD_READDIR    21
#define SYS_FD_CLOSE      22
#define SYS_SIGACTION     23
#define SYS_KILL          24
#define SYS_MMAP          26
#define SYS_MUNMAP        27
#define SYS_WIN_DRAW_TEXT 28
#define SYS_WIN_POLL_KEY  29
#define SYS_SET_TLS       42
#define SYS_GET_TLS       43
#define SYS_DL_INFO       44
#define SYS_DLOPEN2       45
#define SYS_DLCLOSE       46
#define SYS_FUTEX_WAIT    47
#define SYS_FUTEX_WAKE    48
#define SYS_GETTID        49
#define SYS_SPAWN2        50

static inline long sys0(long n) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n) : "memory", "rcx", "r11");
    return r;
}
static inline long sys1(long n, long a) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a) : "memory", "rcx", "r11");
    return r;
}
static inline long sys2(long n, long a, long b) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "memory", "rcx", "r11");
    return r;
}
static inline long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "memory", "rcx", "r11");
    return r;
}
static inline long sys4(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                      : "memory", "rcx", "r11");
    return r;
}
static inline long sys6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile ("int $0x80" : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                      : "memory", "rcx", "r11");
    return r;
}

/* ---- process ------------------------------------------------------------ */
void cos_exit(int status) { sys1(SYS_EXIT, status); __builtin_unreachable(); }
int  cos_getpid(void) { return (int)sys0(SYS_GETPID); }
void cos_yield(void) { sys0(SYS_YIELD); }
void cos_sleep_ms(uint64_t ms) { sys1(SYS_SLEEP_MS, (long)ms); }
int64_t cos_spawn(const char *path) { return sys1(SYS_SPAWN, (long)path); }

int64_t cos_waitpid(int64_t pid, int *status, bool blocking) {
    return sys3(SYS_WAITPID, (long)pid, (long)status, blocking ? 1 : 0);
}

/* ---- console ------------------------------------------------------------ */
ssize_t cos_write(const char *buf, size_t len) {
    return sys2(SYS_WRITE, (long)buf, (long)len);
}
void cos_puts(const char *s) { cos_write(s, cos_strlen(s)); }

/* ---- strings ------------------------------------------------------------ */
size_t cos_strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }

int cos_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int cos_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
char *cos_strcpy(char *dst, const char *src) {
    char *d = dst; while ((*d++ = *src++)) {} return dst;
}
char *cos_strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; ++i) dst[i] = src[i];
    for (; i < n; ++i) dst[i] = '\0';
    return dst;
}
char *cos_strcat(char *dst, const char *src) {
    char *d = dst + cos_strlen(dst);
    while ((*d++ = *src++)) {}
    return dst;
}
char *cos_strchr(const char *s, int c) {
    for (; *s; ++s) if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}
void *cos_memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    for (size_t i = 0; i < n; ++i) d[i] = (unsigned char)c;
    return dst;
}
void *cos_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
int cos_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; ++i) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
int cos_atoi(const char *s) {
    int sign = 1, v = 0;
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == '-') { sign = -1; ++s; } else if (*s == '+') ++s;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; }
    return v * sign;
}

/* GCC emits calls to these for struct assignment and array init even in a
 * freestanding build with -fno-builtin, so they must exist under their
 * standard names or linking fails with an undefined reference the source
 * never mentions. */
void *memset(void *d, int c, size_t n) { return cos_memset(d, c, n); }
void *memcpy(void *d, const void *s, size_t n) { return cos_memcpy(d, s, n); }
int   memcmp(const void *a, const void *b, size_t n) { return cos_memcmp(a, b, n); }
void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    if (d == s || n == 0) return dst;
    /* Overlap-safe: copy backward only when the destination starts inside
     * the source range, which is the case a forward copy would corrupt. */
    if (d < s) { for (size_t i = 0; i < n; ++i) d[i] = s[i]; }
    else       { for (size_t i = n; i > 0; --i) d[i-1] = s[i-1]; }
    return dst;
}

/* ---- formatted output ---------------------------------------------------
 *
 * The whole printf engine moved to cos_fmt.c. What was here handled
 * `%s %c %d %i %u %x %X %p %%` plus the `l` modifier and nothing else:
 * no flags, no field width, no precision, no floating point. That is
 * enough for a diagnostic line and not enough to port anything, because
 * `%-20s`, `%08x` and `%.3f` are what ordinary C is written with - and
 * this formatter emitted them as literal text, turning a missing
 * feature into corrupted output.
 *
 * cos_fprintf() below shares that engine rather than carrying a second
 * copy, so the buffered-stream path and the console path cannot disagree
 * about what a conversion means. */

/* ---- memory ------------------------------------------------------------- */
void *cos_sbrk(long increment) {
    long r = sys1(SYS_SBRK, increment);
    return (r == -1) ? NULL : (void *)(uintptr_t)r;
}
void *cos_mmap(size_t length, uint32_t prot) {
    long r = sys2(SYS_MMAP, (long)length, (long)prot);
    return (r == -1) ? NULL : (void *)(uintptr_t)r;
}
int cos_munmap(void *addr, size_t length) {
    return (int)sys2(SYS_MUNMAP, (long)(uintptr_t)addr, (long)length);
}

/**
 * A first-fit free-list allocator over sbrk.
 *
 * Deliberately simple rather than a size-class/bin allocator: this runs in
 * small single-purpose programs, not a long-lived server, so the win from
 * a sophisticated allocator is small and the risk of a subtle bug in one
 * is not. Blocks are never returned to the kernel (sbrk is only ever
 * grown) - a freed block goes back on the free list for reuse instead.
 * That is a real, stated limitation: a program with a large transient
 * peak keeps that memory for its lifetime.
 */
typedef struct cos_block {
    size_t            size;   /* usable bytes, not counting this header */
    struct cos_block *next;   /* next FREE block; meaningless when in use */
    bool              free;
} cos_block_t;

#define COS_BLOCK_HDR   (sizeof(cos_block_t))
#define COS_MALLOC_ALIGN 16
#define COS_SBRK_CHUNK  (64u * 1024u)

static cos_block_t *g_heap_head = NULL;

static size_t align_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }

void *cos_malloc(size_t size) {
    if (size == 0) return NULL;
    size = align_up(size, COS_MALLOC_ALIGN);

    for (cos_block_t *b = g_heap_head; b; b = b->next) {
        if (!b->free || b->size < size) continue;
        /* Split only when the remainder can hold a header plus a
         * meaningful allocation - otherwise the leftover is unusable
         * and splitting just fragments the list. */
        if (b->size >= size + COS_BLOCK_HDR + COS_MALLOC_ALIGN) {
            cos_block_t *rest = (cos_block_t *)((char *)b + COS_BLOCK_HDR + size);
            rest->size = b->size - size - COS_BLOCK_HDR;
            rest->free = true;
            rest->next = b->next;
            b->size = size;
            b->next = rest;
        }
        b->free = false;
        return (char *)b + COS_BLOCK_HDR;
    }

    size_t need = align_up(size + COS_BLOCK_HDR, COS_SBRK_CHUNK);
    void *mem = cos_sbrk((long)need);
    if (!mem) return NULL;

    cos_block_t *b = (cos_block_t *)mem;
    b->size = need - COS_BLOCK_HDR;
    b->free = false;
    b->next = g_heap_head;
    g_heap_head = b;

    if (b->size >= size + COS_BLOCK_HDR + COS_MALLOC_ALIGN) {
        cos_block_t *rest = (cos_block_t *)((char *)b + COS_BLOCK_HDR + size);
        rest->size = b->size - size - COS_BLOCK_HDR;
        rest->free = true;
        rest->next = b->next;
        b->size = size;
        b->next = rest;
    }
    return (char *)b + COS_BLOCK_HDR;
}

void cos_free(void *ptr) {
    if (!ptr) return;
    cos_block_t *b = (cos_block_t *)((char *)ptr - COS_BLOCK_HDR);
    b->free = true;
    /* Coalesce forward with any immediately-adjacent free block. Only
     * forward, and only with a directly abutting neighbour: the list is
     * not address-ordered, so a general merge would need a sort this
     * allocator deliberately does not maintain. */
    cos_block_t *n = b->next;
    if (n && n->free && (char *)b + COS_BLOCK_HDR + b->size == (char *)n) {
        b->size += COS_BLOCK_HDR + n->size;
        b->next = n->next;
    }
}

void *cos_calloc(size_t count, size_t size) {
    /* Overflow check before multiplying - a caller-supplied count*size
     * that wraps would otherwise allocate a small block and then be
     * memset (and used) as if it were huge. */
    if (count != 0 && size > (size_t)-1 / count) return NULL;
    size_t total = count * size;
    void *p = cos_malloc(total);
    if (p) cos_memset(p, 0, total);
    return p;
}

void *cos_realloc(void *ptr, size_t size) {
    if (!ptr) return cos_malloc(size);
    if (size == 0) { cos_free(ptr); return NULL; }
    cos_block_t *b = (cos_block_t *)((char *)ptr - COS_BLOCK_HDR);
    if (b->size >= size) return ptr;
    void *n = cos_malloc(size);
    if (!n) return NULL;
    cos_memcpy(n, ptr, b->size);
    cos_free(ptr);
    return n;
}

/* ---- whole-file I/O ------------------------------------------------------ */
ssize_t cos_read_file(const char *path, void *buf, size_t len) {
    return sys3(SYS_READ_FILE, (long)path, (long)buf, (long)len);
}
ssize_t cos_write_file(const char *path, const void *buf, size_t len) {
    return sys3(SYS_WRITE_FILE, (long)path, (long)buf, (long)len);
}

/* ---- file descriptors ---------------------------------------------------- */
int cos_open(const char *path, uint32_t flags) {
    return (int)sys2(SYS_FD_OPEN, (long)path, (long)flags);
}
int cos_opendir(const char *path) { return (int)sys1(SYS_FD_OPENDIR, (long)path); }
ssize_t cos_fd_read(int fd, void *buf, size_t len) {
    return sys3(SYS_FD_READ, fd, (long)buf, (long)len);
}
ssize_t cos_fd_write(int fd, const void *buf, size_t len) {
    return sys3(SYS_FD_WRITE, fd, (long)buf, (long)len);
}
int64_t cos_lseek(int fd, int64_t offset) {
    return sys2(SYS_FD_LSEEK, fd, (long)offset);
}
int cos_readdir(int fd, cos_dirent_t *out) {
    return (int)sys2(SYS_FD_READDIR, fd, (long)out);
}
int cos_close(int fd) { return (int)sys1(SYS_FD_CLOSE, fd); }

/* ---- shared libraries ---------------------------------------------------- */
int64_t cos_dlopen(const char *path) { return sys1(SYS_DLOPEN, (long)path); }
void *cos_dlsym(int64_t handle, const char *name) {
    return (void *)(uintptr_t)sys2(SYS_DLSYM, (long)handle, (long)name);
}

/* ---- signals -------------------------------------------------------------- */
int cos_signal(int signum, cos_sighandler_t handler) {
    return (int)sys2(SYS_SIGACTION, signum, (long)(uintptr_t)handler);
}
int cos_kill(int pid, int signum) { return (int)sys2(SYS_KILL, pid, signum); }

/* ---- GUI ------------------------------------------------------------------ */
int64_t cos_win_create(const char *title, int32_t w, int32_t h) {
    return sys4(SYS_WIN_CREATE, (long)title, (long)cos_strlen(title), w, h);
}
int cos_win_fill(int64_t win, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    return (int)sys6(SYS_WIN_FILL, (long)win, x, y, w, h, (long)color);
}
int cos_win_text(int64_t win, int32_t x, int32_t y, uint32_t fg, uint32_t bg, const char *s) {
    return (int)sys6(SYS_WIN_DRAW_TEXT, (long)win, x, y, (long)fg, (long)bg, (long)s);
}
int cos_win_clear(int64_t win) { return (int)sys1(SYS_WIN_CLEAR, (long)win); }
int cos_win_poll_key(int64_t win, cos_key_event_t *out) {
    return (int)sys2(SYS_WIN_POLL_KEY, (long)win, (long)out);
}

/* ---- threads, time, filesystem mutation ---------------------------------- */
#define SYS_THREAD_CREATE 30
#define SYS_THREAD_EXIT   31
#define SYS_THREAD_JOIN   32
#define SYS_TIME_MS       33
#define SYS_TIME_UNIX     34
#define SYS_MKDIR         35
#define SYS_UNLINK        36
#define SYS_RENAME        37
#define SYS_STAT          38

/* Thread entry trampoline.
 *
 * A new thread starts with NO thread pointer: %fs base is per-thread CPU
 * state, and the kernel only builds a TLS block for the MAIN thread at
 * exec time. So a thread that jumped straight to the user's function
 * would read every `__thread` variable through whatever %fs base the
 * previously running thread left behind - silently sharing another
 * thread's storage, which is the exact opposite of what thread-local
 * means. This gives the thread its own block first.
 *
 * The (fn, arg) pair is heap-allocated rather than passed in registers
 * because SYS_THREAD_CREATE takes exactly one argument word and the
 * trampoline needs two. It is freed by the trampoline itself. */
typedef struct { cos_thread_fn fn; void *arg; } cos_thread_start_t;

static void cos_thread_trampoline(void *p) {
    cos_thread_start_t *st = (cos_thread_start_t *)p;
    cos_thread_fn fn = st->fn;
    void *arg = st->arg;
    cos_free(st);

    uint64_t tp = cos_tls_create_block();
    /* 0 means this process has no TLS at all, which is not an error -
     * the thread simply runs without one, exactly as before. */
    if (tp) cos_set_tls(tp);

    fn(arg);
    cos_thread_exit(0);
}

int64_t cos_thread_create(cos_thread_fn fn, void *arg) {
    if (!fn) return -1;
    cos_thread_start_t *st = (cos_thread_start_t *)cos_malloc(sizeof(*st));
    if (!st) return -1;
    st->fn = fn;
    st->arg = arg;
    int64_t tid = sys2(SYS_THREAD_CREATE,
                       (long)(uintptr_t)cos_thread_trampoline,
                       (long)(uintptr_t)st);
    if (tid < 0) cos_free(st);
    return tid;
}
void cos_thread_exit(int code) { sys1(SYS_THREAD_EXIT, code); __builtin_unreachable(); }
int cos_thread_join(int64_t tid, uint64_t timeout_ms) {
    return (int)sys2(SYS_THREAD_JOIN, (long)tid, (long)timeout_ms);
}

uint64_t cos_time_ms(void)   { return (uint64_t)sys0(SYS_TIME_MS); }
uint64_t cos_time_unix(void) { return (uint64_t)sys0(SYS_TIME_UNIX); }

int cos_mkdir(const char *path)  { return (int)sys1(SYS_MKDIR, (long)path); }
int cos_unlink(const char *path) { return (int)sys1(SYS_UNLINK, (long)path); }
int cos_rename(const char *o, const char *n) {
    return (int)sys2(SYS_RENAME, (long)o, (long)n);
}
int cos_stat(const char *path, cos_stat_t *out) {
    return (int)sys2(SYS_STAT, (long)path, (long)out);
}

/* ---- setjmp / longjmp ----------------------------------------------------
 *
 * Written as inline asm rather than a separate .S file so the whole
 * runtime stays one translation unit, and declared naked so GCC emits no
 * prologue that would move RSP before it is captured.
 *
 * Layout of cos_jmp_buf (8 qwords): rbx, rbp, r12, r13, r14, r15, rsp,
 * return address. RSP is stored as the value it will have AFTER the
 * return address is popped, so longjmp can restore it and jump directly -
 * getting this off by 8 is the classic way a hand-written setjmp appears
 * to work and then corrupts the stack one frame later.
 */
__asm__(
".global cos_setjmp\n"
"cos_setjmp:\n"
"  movq %rbx,  0(%rdi)\n"
"  movq %rbp,  8(%rdi)\n"
"  movq %r12, 16(%rdi)\n"
"  movq %r13, 24(%rdi)\n"
"  movq %r14, 32(%rdi)\n"
"  movq %r15, 40(%rdi)\n"
"  leaq 8(%rsp), %rax\n"      /* RSP as it will be after the ret pops */
"  movq %rax, 48(%rdi)\n"
"  movq (%rsp), %rax\n"       /* return address */
"  movq %rax, 56(%rdi)\n"
"  xorl %eax, %eax\n"         /* setjmp returns 0 on the direct call */
"  ret\n"
);

__asm__(
".global cos_longjmp\n"
"cos_longjmp:\n"
"  movq  0(%rdi), %rbx\n"
"  movq  8(%rdi), %rbp\n"
"  movq 16(%rdi), %r12\n"
"  movq 24(%rdi), %r13\n"
"  movq 32(%rdi), %r14\n"
"  movq 40(%rdi), %r15\n"
"  movq 48(%rdi), %rsp\n"
"  movq 56(%rdi), %rdx\n"     /* saved return address */
"  movl %esi, %eax\n"         /* longjmp's val becomes setjmp's return */
"  testl %eax, %eax\n"
"  jnz 1f\n"
"  movl $1, %eax\n"           /* longjmp(env, 0) must still return 1 */
"1:\n"
"  jmp *%rdx\n"
);

/* ---- networking ---------------------------------------------------------- */
#define SYS_NET_AVAILABLE 39
#define SYS_NET_RESOLVE   40
#define SYS_NET_HTTP_GET  41

bool cos_net_available(void) { return sys0(SYS_NET_AVAILABLE) == 1; }
int  cos_net_resolve(const char *hostname, uint8_t *ip_out) {
    return (int)sys2(SYS_NET_RESOLVE, (long)hostname, (long)ip_out);
}
ssize_t cos_net_http_get(const char *url, void *buf, size_t len) {
    return sys3(SYS_NET_HTTP_GET, (long)url, (long)buf, (long)len);
}

/* ---- FILE* stdio ----------------------------------------------------------
 *
 * One buffer per stream, used for reading OR writing but never both at
 * once - `dirty` records which. A stream opened "r+" switches direction
 * by flushing first (see cos_stream_prepare_*), because a single buffer
 * cannot hold pending writes and lookahead reads simultaneously without
 * one silently overwriting the other.
 */
#define COS_STREAM_BUF 1024

struct cos_FILE {
    int      fd;
    bool     used;
    bool     writable;
    bool     at_eof;
    bool     dirty;          /* buffer holds unwritten output */
    long     pos;            /* absolute file offset of buf[0] */
    int      buf_len;        /* valid bytes in buf (read mode) */
    int      buf_pos;        /* next byte to consume (read mode) */
    int      unget;          /* pushed-back char, or -1 */
    char     buf[COS_STREAM_BUF];
};

#define COS_MAX_STREAMS 16
static cos_FILE g_streams[COS_MAX_STREAMS];

static cos_FILE *stream_alloc(void) {
    for (int i = 0; i < COS_MAX_STREAMS; ++i) {
        if (!g_streams[i].used) {
            cos_memset(&g_streams[i], 0, sizeof(g_streams[i]));
            g_streams[i].used = true;
            g_streams[i].unget = -1;
            return &g_streams[i];
        }
    }
    return NULL;
}

/* Writes out any buffered output and resets the buffer. Safe to call on a
 * read-mode stream (nothing is dirty), which is what lets the direction
 * switch in "r+" mode be a single unconditional call. */
static int stream_flush_write(cos_FILE *f) {
    if (!f->dirty || f->buf_len <= 0) { f->dirty = false; f->buf_len = 0; return 0; }
    cos_lseek(f->fd, f->pos);
    ssize_t w = cos_fd_write(f->fd, f->buf, (size_t)f->buf_len);
    if (w < 0) return COS_EOF;
    f->pos += w;
    f->buf_len = 0;
    f->buf_pos = 0;
    f->dirty = false;
    return 0;
}

/* Refills the read buffer from the current position. */
static int stream_fill_read(cos_FILE *f) {
    if (stream_flush_write(f) != 0) return COS_EOF;
    cos_lseek(f->fd, f->pos);
    ssize_t n = cos_fd_read(f->fd, f->buf, COS_STREAM_BUF);
    if (n <= 0) { f->at_eof = true; f->buf_len = 0; f->buf_pos = 0; return COS_EOF; }
    f->buf_len = (int)n;
    f->buf_pos = 0;
    return 0;
}

cos_FILE *cos_fopen(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
    uint32_t flags;
    bool writable;
    if (mode[0] == 'r' && mode[1] == '+') { flags = COS_O_RDWR_CREATE;   writable = true;  }
    else if (mode[0] == 'r')              { flags = COS_O_RDONLY;        writable = false; }
    else if (mode[0] == 'w')              { flags = COS_O_WRONLY_CREATE; writable = true;  }
    else if (mode[0] == 'a')              { flags = COS_O_WRONLY_APPEND; writable = true;  }
    else return NULL;

    int fd = cos_open(path, flags);
    if (fd < 0) return NULL;
    cos_FILE *f = stream_alloc();
    if (!f) { cos_close(fd); return NULL; }
    f->fd = fd;
    f->writable = writable;
    return f;
}

int cos_fclose(cos_FILE *f) {
    if (!f || !f->used) return COS_EOF;
    int rc = stream_flush_write(f);
    cos_close(f->fd);
    f->used = false;
    return rc;
}

int cos_fflush(cos_FILE *f) {
    if (!f || !f->used) return COS_EOF;
    return stream_flush_write(f);
}

int cos_fgetc(cos_FILE *f) {
    if (!f || !f->used) return COS_EOF;
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (f->buf_pos >= f->buf_len) {
        if (stream_fill_read(f) != 0) return COS_EOF;
    }
    unsigned char c = (unsigned char)f->buf[f->buf_pos++];
    /* pos tracks the offset of buf[0]; advance it as the buffer drains so
     * ftell/fseek stay correct without a separate counter. */
    if (f->buf_pos >= f->buf_len) { f->pos += f->buf_len; f->buf_len = 0; f->buf_pos = 0; }
    return (int)c;
}

int cos_ungetc(int c, cos_FILE *f) {
    if (!f || !f->used || c == COS_EOF) return COS_EOF;
    f->unget = c & 0xFF;
    f->at_eof = false;
    return c;
}

int cos_fputc(int c, cos_FILE *f) {
    if (!f || !f->used || !f->writable) return COS_EOF;
    if (!f->dirty) { f->buf_len = 0; f->dirty = true; }
    if (f->buf_len >= COS_STREAM_BUF) {
        if (stream_flush_write(f) != 0) return COS_EOF;
        f->dirty = true;
    }
    f->buf[f->buf_len++] = (char)c;
    return c;
}

size_t cos_fread(void *ptr, size_t size, size_t count, cos_FILE *f) {
    if (!ptr || !f || !f->used || size == 0) return 0;
    unsigned char *out = ptr;
    size_t total = size * count, done = 0;
    while (done < total) {
        int c = cos_fgetc(f);
        if (c == COS_EOF) break;
        out[done++] = (unsigned char)c;
    }
    return done / size;
}

size_t cos_fwrite(const void *ptr, size_t size, size_t count, cos_FILE *f) {
    if (!ptr || !f || !f->used || size == 0) return 0;
    const unsigned char *in = ptr;
    size_t total = size * count, done = 0;
    while (done < total) {
        if (cos_fputc(in[done], f) == COS_EOF) break;
        done++;
    }
    return done / size;
}

char *cos_fgets(char *buf, int size, cos_FILE *f) {
    if (!buf || size <= 1 || !f || !f->used) return NULL;
    int i = 0;
    while (i < size - 1) {
        int c = cos_fgetc(f);
        if (c == COS_EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;   /* fgets keeps the newline, like the real one */
    }
    if (i == 0) return NULL;
    buf[i] = '\0';
    return buf;
}

int cos_fputs(const char *s, cos_FILE *f) {
    if (!s) return COS_EOF;
    while (*s) if (cos_fputc(*s++, f) == COS_EOF) return COS_EOF;
    return 0;
}

int cos_fprintf(cos_FILE *f, const char *fmt, ...) {
    char tmp[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = cos_vsnprintf(tmp, sizeof(tmp), fmt, ap);
    __builtin_va_end(ap);
    /* cos_fputs() stops at the first NUL, so a formatted string
     * containing one would be silently cut short. Written by length
     * instead. */
    size_t emit = ((size_t)n < sizeof(tmp) - 1) ? (size_t)n : sizeof(tmp) - 1;
    cos_fwrite(tmp, 1, emit, f);
    return n;
}

int cos_fseek(cos_FILE *f, long offset) {
    if (!f || !f->used) return COS_EOF;
    if (stream_flush_write(f) != 0) return COS_EOF;
    f->buf_len = 0; f->buf_pos = 0; f->unget = -1; f->at_eof = false;
    f->pos = offset;
    return (cos_lseek(f->fd, offset) < 0) ? COS_EOF : 0;
}

long cos_ftell(cos_FILE *f) {
    if (!f || !f->used) return -1;
    return f->dirty ? (f->pos + f->buf_len) : (f->pos + f->buf_pos);
}

int cos_feof(cos_FILE *f) { return (f && f->used && f->at_eof) ? 1 : 0; }

/* ---- stdlib staples ------------------------------------------------------- */
static int digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

unsigned long cos_strtoul(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
    if (*p == '+') ++p;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2; base = 16;
    } else if (base == 0) {
        base = (p[0] == '0' && p[1]) ? 8 : 10;
        if (base == 8) ++p;
    }
    unsigned long v = 0;
    bool any = false;
    for (;;) {
        int d = digit_val(*p);
        if (d < 0 || d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
        any = true;
        ++p;
    }
    /* On no valid digits, `end` must report the ORIGINAL string - that is
     * how a caller distinguishes "parsed 0" from "parsed nothing". */
    if (end) *end = (char *)(any ? p : s);
    return any ? v : 0;
}

long cos_strtol(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
    bool neg = (*p == '-');
    if (neg || *p == '+') ++p;
    char *e = NULL;
    unsigned long v = cos_strtoul(p, &e, base);
    if (e == p) { if (end) *end = (char *)s; return 0; }
    if (end) *end = e;
    return neg ? -(long)v : (long)v;
}

/* Insertion sort. O(n^2), and chosen knowingly: it needs no scratch
 * allocation and no recursion (this runs on a bounded ring3 stack), and
 * the data a program here sorts is small. A caller sorting a large array
 * should say so and this can become a proper quicksort. */
void cos_qsort(void *base, size_t n, size_t size,
               int (*cmp)(const void *, const void *)) {
    if (!base || !cmp || size == 0 || n < 2) return;
    unsigned char *a = base;
    for (size_t i = 1; i < n; ++i) {
        for (size_t j = i; j > 0; --j) {
            unsigned char *x = a + (j - 1) * size;
            unsigned char *y = a + j * size;
            if (cmp(x, y) <= 0) break;
            for (size_t k = 0; k < size; ++k) {
                unsigned char t = x[k]; x[k] = y[k]; y[k] = t;
            }
        }
    }
}

char *cos_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = cos_strlen(s) + 1;
    char *p = cos_malloc(n);
    if (p) cos_memcpy(p, s, n);
    return p;
}

char *cos_strstr(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    if (!*needle) return (char *)hay;
    for (; *hay; ++hay) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { ++h; ++n; }
        if (!*n) return (char *)hay;
    }
    return NULL;
}

char *cos_strrchr(const char *s, int c) {
    const char *last = NULL;
    for (; *s; ++s) if (*s == (char)c) last = s;
    return (c == 0) ? (char *)s : (char *)last;
}

int cos_abs(int v) { return v < 0 ? -v : v; }

/* Park-Miller. Deterministic and not cryptographically secure - stated in
 * the header so nothing relies on it for anything that matters. */
static unsigned long g_rand_state = 1;
void cos_srand(unsigned seed) { g_rand_state = seed ? seed : 1; }
int cos_rand(void) {
    g_rand_state = (g_rand_state * 48271UL) % 2147483647UL;
    return (int)g_rand_state;
}

/* ---- dynamic loading and TLS syscall wrappers --------------------------- */

int cos_set_tls(uint64_t tp) { return (int)sys1(SYS_SET_TLS, (long)tp); }
uint64_t cos_get_tls(void) { return (uint64_t)sys0(SYS_GET_TLS); }
uint64_t cos_dl_info(void) { return (uint64_t)sys0(SYS_DL_INFO); }

int64_t cos_dlopen_flags(const char *path, uint32_t flags)
{
    return (int64_t)sys2(SYS_DLOPEN2, (long)path, (long)flags);
}

int cos_dlclose(int64_t handle)
{
    return (int)sys1(SYS_DLCLOSE, (long)handle);
}

/* ---- futex and thread identity ------------------------------------------ */

int64_t cos_futex_wait(volatile uint32_t *addr, uint32_t expected,
                       uint64_t timeout_ms)
{
    return sys3(SYS_FUTEX_WAIT, (long)(uintptr_t)addr, (long)expected,
                (long)timeout_ms);
}

int64_t cos_futex_wake(volatile uint32_t *addr, uint32_t count)
{
    return sys2(SYS_FUTEX_WAKE, (long)(uintptr_t)addr, (long)count);
}

int64_t cos_gettid(void) { return sys0(SYS_GETTID); }

/* Spawns a child with arguments and an environment.
 *
 * cos_spawn() remains the argument-less form. It could not pass argv at
 * all: SYS_SPAWN takes a path and nothing else, so every spawned process
 * saw argc == 1 and an empty environment no matter what the parent
 * wanted. Both vectors must be NULL-terminated. */
int64_t cos_spawn_argv(const char *path, const char *const *argv,
                       const char *const *envp)
{
    return sys3(SYS_SPAWN2, (long)(uintptr_t)path,
                (long)(uintptr_t)argv, (long)(uintptr_t)envp);
}
