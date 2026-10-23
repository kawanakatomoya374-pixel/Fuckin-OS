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
#define SYS_MPROTECT      57
#define SYS_WIN_DRAW_TEXT 28
#define SYS_WIN_POLL_KEY  29
#define SYS_SET_TLS       42
#define SYS_GET_TLS       43
#define SYS_IMAGE_DECODE  200
#define SYS_DL_INFO       44
#define SYS_DLOPEN2       45
#define SYS_DLCLOSE       46
#define SYS_DLCTL         248
#define SYS_FUTEX_WAIT    47
#define SYS_FUTEX_WAKE    48
#define SYS_GETTID        49
#define SYS_CPU_STATS     298
#define SYS_GETCPU        299
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
/* a system call by number, for the calls a kernel module (.c-osys) provides (0x1000..0x103F); -1 when nobody provides it or the process may not use it */
long cos_syscall3(long nr, long a, long b, long c) { return sys3(nr, a, b, c); }

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
/* COS_STDOUT=<path> in the environment sends this process's console output to that file
 * instead of the console. C-OS has no pipe syscall; this is how C-OS Studio captures the
 * output of the compiler and of the programs it runs. The file is opened in append mode,
 * which the kernel syncs on every write, so the reader sees output as it is produced. */
static int s_out_fd = -2;               /* -2: not looked up yet, -1: no redirect */
ssize_t cos_write(const char *buf, size_t len) {
    if (s_out_fd == -2) {
        const char *p = cos_getenv("COS_STDOUT");
        s_out_fd = (p && p[0]) ? cos_open(p, COS_O_WRONLY_APPEND) : -1;
    }
    if (s_out_fd >= 0) {
        ssize_t w = cos_fd_write(s_out_fd, buf, len);
        if (w >= 0) return w;
    }
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
/* ---- memory primitives ---------------------------------------------------
 *
 * These used to be byte-at-a-time loops, which made them the single hottest
 * thing in the video decoders (about a quarter of all samples in a profile of
 * H.264 playback): frame and macroblock copies go through memcpy/memset.
 * They now move 16 bytes per instruction using GCC/Clang vector types (plain
 * SSE2 movdqu/movups, so no alignment requirement and nothing newer than the
 * x86-64 baseline). -fno-builtin keeps the compiler from turning these loops
 * back into calls to themselves. */
typedef char          cos_v16 __attribute__((vector_size(16), may_alias, aligned(1)));
typedef unsigned long long cos_u64u __attribute__((may_alias, aligned(1)));
typedef unsigned int  cos_u32u __attribute__((may_alias, aligned(1)));
typedef unsigned short cos_u16u __attribute__((may_alias, aligned(1)));

void *cos_memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    unsigned char b = (unsigned char)c;
    if (n >= 16) {
        cos_v16 v = { b,b,b,b,b,b,b,b,b,b,b,b,b,b,b,b };
        size_t i = 0;
        for (; i + 32 <= n; i += 32) { *(cos_v16 *)(d + i) = v; *(cos_v16 *)(d + i + 16) = v; }
        if (i + 16 <= n) { *(cos_v16 *)(d + i) = v; i += 16; }
        if (i < n) *(cos_v16 *)(d + n - 16) = v;      /* overlapping tail */
        return dst;
    }
    for (size_t i = 0; i < n; ++i) d[i] = b;
    return dst;
}
/* Copies non-overlapping (or forward-safe) regions. The tail is handled by one
 * overlapping store, which is only valid when source and destination do not overlap. */
void *cos_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    if (n >= 16) {
        size_t i = 0;
        for (; i + 32 <= n; i += 32) {
            cos_v16 a = *(const cos_v16 *)(s + i), b = *(const cos_v16 *)(s + i + 16);
            *(cos_v16 *)(d + i) = a; *(cos_v16 *)(d + i + 16) = b;
        }
        if (i + 16 <= n) { *(cos_v16 *)(d + i) = *(const cos_v16 *)(s + i); i += 16; }
        if (i < n) *(cos_v16 *)(d + n - 16) = *(const cos_v16 *)(s + n - 16);
        return dst;
    }
    if (n >= 8)      { cos_u64u a = *(const cos_u64u *)s, b = *(const cos_u64u *)(s + n - 8); *(cos_u64u *)d = a; *(cos_u64u *)(d + n - 8) = b; }
    else if (n >= 4) { cos_u32u a = *(const cos_u32u *)s, b = *(const cos_u32u *)(s + n - 4); *(cos_u32u *)d = a; *(cos_u32u *)(d + n - 4) = b; }
    else if (n >= 2) { cos_u16u a = *(const cos_u16u *)s, b = *(const cos_u16u *)(s + n - 2); *(cos_u16u *)d = a; *(cos_u16u *)(d + n - 2) = b; }
    else if (n)      { *d = *s; }
    return dst;
}
int cos_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        if (*(const cos_u64u *)(x + i) != *(const cos_u64u *)(y + i)) break;
    for (; i < n; ++i) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
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
    /* No overlap in the direction of travel: the fast non-overlapping copy is fine. */
    if (d + n <= s || s + n <= d) return cos_memcpy(dst, src, n);
    if (d < s) {
        /* Forward copy, 16 bytes at a time; each chunk is loaded before it is stored and a
         * store never reaches the next chunk's source (d < s), so this is overlap-safe. */
        size_t i = 0;
        for (; i + 16 <= n; i += 16) *(cos_v16 *)(d + i) = *(const cos_v16 *)(s + i);
        for (; i < n; ++i) d[i] = s[i];
    } else {
        /* Backward copy for d > s. */
        size_t i = n;
        for (; i >= 16; i -= 16) *(cos_v16 *)(d + i - 16) = *(const cos_v16 *)(s + i - 16);
        while (i > 0) { --i; d[i] = s[i]; }
    }
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
/* W^X-enforcing: the kernel refuses a request for PROT_WRITE|PROT_EXEC
 * together (returns -1) - see SYS_MPROTECT's own comment in
 * src/kernel/syscall.c for the correct JIT pattern this requires
 * instead (mmap RW, write code, mprotect to RX). addr/length must
 * exactly match a previous cos_mmap() call - no partial-region support. */
int cos_mprotect(void *addr, size_t length, uint32_t prot) {
    return (int)sys3(SYS_MPROTECT, (long)(uintptr_t)addr, (long)length, (long)prot);
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
/* The header is padded to 16 bytes' alignment (sizeof == 32, not 24). With a 24-byte header the
 * payload of a block that started on a 16-byte boundary sat at +24, i.e. 8 mod 16, and the blocks
 * split off after it alternated between aligned and misaligned - so COS_MALLOC_ALIGN below was
 * a promise the allocator did not keep. gcc assumes malloc() results are 16-byte aligned and emits
 * aligned SSE moves (movaps) on that assumption; TinyCC's SValue (it holds a long double) hit
 * exactly that as a #GP the first time a non-trivial file was compiled on the device. */
typedef struct cos_block {
    size_t            size;   /* usable bytes, not counting this header */
    struct cos_block *next;   /* next FREE block; meaningless when in use */
    bool              free;
    uint64_t          seal;   /* per-process secret ^ address ^ size ^ state: a damaged header, a pointer that is not a block and a double free are caught */
} __attribute__((aligned(16))) cos_block_t;
_Static_assert(sizeof(cos_block_t) % 16 == 0, "block header must keep payloads 16-byte aligned");

#define COS_BLOCK_HDR   (sizeof(cos_block_t))
#define COS_MALLOC_ALIGN 16
#define COS_SBRK_CHUNK  (64u * 1024u)

static cos_block_t *g_heap_head = NULL;

/* Heap integrity. The header of every block carries a seal that depends on a secret chosen when the program starts, so an overflow from the block
 * before it (which can rewrite size and free, but does not know the secret), free() of a pointer that never came from malloc, and a double free
 * stop the program with a message instead of silently corrupting the heap. */
static uint64_t g_heap_cookie;
static void heap_cookie_init(void) {
    uint64_t c = 0; const uint8_t *r = (const uint8_t *)cos_getauxval(25 /* AT_RANDOM */);
    if (r) for (int i = 0; i < 8; i++) c = (c << 8) | r[i];
    c ^= cos_time_ms() * 0x9E3779B97F4A7C15ULL; c ^= (uint64_t)(uintptr_t)&g_heap_cookie; if (!c) c = 0x5DEECE66DULL;
    g_heap_cookie = c;
}
static inline uint64_t blk_value(const cos_block_t *b) {
    return g_heap_cookie ^ (uint64_t)(uintptr_t)b ^ ((uint64_t)b->size * 0x9E3779B97F4A7C15ULL) ^ (b->free ? 0xF5EE0000ULL : 0xA11C0000ULL);
}
static inline void blk_seal(cos_block_t *b) { b->seal = blk_value(b); }
static void heap_corrupt(const char *why) { cos_puts("[HEAP] "); cos_puts(why); cos_puts(" - the program is stopped\n"); cos_exit(134); }
static inline void blk_check(const cos_block_t *b) {
    if (!g_heap_cookie || b->seal != blk_value(b)) heap_corrupt("a heap block header is damaged, or the pointer was not returned by malloc");
}

/* Blocks of the ordinary heap freed from a worker (another core): the heap is not thread-safe, so they are
 * parked on a lock-free stack and returned by the main thread the next time it touches the heap. */
static void *volatile g_deferred_free;
static void heap_drain_deferred(void);

static size_t align_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }


/* ---- idle-work queue ---------------------------------------------------------------------------------
 * Worker cores spend part of their time spinning for each other (a reference picture's rows, a condition
 * variable). Rather than waste that, every spin loop calls cos_idle_work(), which runs one queued job if there
 * is one; the main thread posts small independent jobs (YUV->RGBA conversion of a band of rows) and joins on a
 * counter. A bounded multi-producer multi-consumer ring (per-slot sequence numbers), safe from any core. */
#define COS_JQ_SIZE 256u
typedef struct { volatile uint32_t seq; void (*fn)(void *); void *arg; } cos_jq_slot_t;
static cos_jq_slot_t g_jq[COS_JQ_SIZE];
static volatile uint32_t g_jq_enq, g_jq_deq;
static volatile int g_jq_ready;

static void jq_init(void) {
    if (g_jq_ready) return;
    for (uint32_t i = 0; i < COS_JQ_SIZE; i++) g_jq[i].seq = i;
    __atomic_store_n(&g_jq_ready, 1, __ATOMIC_RELEASE);
}
bool cos_job_push(void (*fn)(void *), void *arg) {
    jq_init();
    uint32_t pos = __atomic_load_n(&g_jq_enq, __ATOMIC_RELAXED);
    for (;;) {
        cos_jq_slot_t *s = &g_jq[pos % COS_JQ_SIZE];
        int32_t diff = (int32_t)(__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) - pos);
        if (diff == 0) {
            if (__atomic_compare_exchange_n(&g_jq_enq, &pos, pos + 1, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                s->fn = fn; s->arg = arg;
                __atomic_store_n(&s->seq, pos + 1, __ATOMIC_RELEASE);
                return true;
            }
        } else if (diff < 0) return false;                    /* full */
        else pos = __atomic_load_n(&g_jq_enq, __ATOMIC_RELAXED);
    }
}
bool cos_idle_work(void) {
    if (!g_jq_ready) return false;
    uint32_t pos = __atomic_load_n(&g_jq_deq, __ATOMIC_RELAXED);
    for (;;) {
        cos_jq_slot_t *s = &g_jq[pos % COS_JQ_SIZE];
        int32_t diff = (int32_t)(__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) - (pos + 1));
        if (diff == 0) {
            if (__atomic_compare_exchange_n(&g_jq_deq, &pos, pos + 1, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                void (*fn)(void *) = s->fn; void *arg = s->arg;
                __atomic_store_n(&s->seq, pos + COS_JQ_SIZE, __ATOMIC_RELEASE);
                fn(arg);
                return true;
            }
        } else if (diff < 0) return false;                    /* empty */
        else pos = __atomic_load_n(&g_jq_deq, __ATOMIC_RELAXED);
    }
}

/* ---- per-worker arenas ---------------------------------------------------
 *
 * Code running as a compute job on another core (SYS_PAR_*) can make no
 * system calls, so it cannot grow the heap, and the first-fit heap above is
 * not thread-safe anyway. A worker therefore owns an arena: one pre-touched
 * block holding its stack and a private heap. cos_malloc()/cos_free() pick it
 * by looking at the stack the caller is running on, so decoder code written
 * against plain malloc() needs no changes.
 *
 * The heap is size-class segregated with a bump pointer: freed blocks go on
 * their class's list and are reused, never split or merged. That is wasteful
 * in the abstract and ideal for a decoder, whose allocations repeat the same
 * few sizes every frame. Another thread may free a block the worker made
 * (the main thread unref'ing a buffer, say); that goes on a lock-free stack
 * the owner drains on its next allocation. */
#define COS_ARENA_MAX     8
#define COS_ARENA_HDR     16
#define COS_ARENA_CLASSES 192
struct cos_arena {
    char *region; size_t region_bytes;
    char *stack_lo, *stack_hi;
    char *base, *bump, *end;
    void *free_list[COS_ARENA_CLASSES];
    void *volatile remote;
};
static cos_arena_t *g_arenas[COS_ARENA_MAX];
static volatile int g_narenas;

static inline cos_arena_t *arena_for_stack(void) {
    char *sp = (char *)__builtin_frame_address(0);
    for (int i = 0; i < COS_ARENA_MAX; i++) {
        cos_arena_t *a = g_arenas[i];
        if (a && sp >= a->stack_lo && sp < a->stack_hi) return a;
    }
    return NULL;
}
static inline cos_arena_t *arena_for_ptr(const void *p) {
    const char *c = (const char *)p;
    for (int i = 0; i < COS_ARENA_MAX; i++) {
        cos_arena_t *a = g_arenas[i];
        if (a && c >= a->base && c < a->end) return a;
    }
    return NULL;
}
/* Size classes: 16-byte steps up to 1KiB, then four steps per power of two. */
static unsigned arena_class(size_t n, size_t *csize) {
    if (n <= 1024) {
        n = (n + 15) & ~(size_t)15; if (n < 16) n = 16;
        *csize = n; return (unsigned)(n / 16 - 1);
    }
    int msb = 63 - __builtin_clzl((unsigned long)(n - 1));
    size_t step = (size_t)1 << (msb - 2);
    size_t cs = ((n + step - 1) / step) * step;
    *csize = cs;
    return 64u + (unsigned)(msb - 10) * 4u + (unsigned)(cs / step - 5);
}
static void arena_drain_remote(cos_arena_t *a) {
    void *list = __atomic_exchange_n(&a->remote, NULL, __ATOMIC_ACQ_REL);
    while (list) {
        void *next = *(void **)list;
        size_t cs; unsigned c = arena_class(*(size_t *)((char *)list - COS_ARENA_HDR), &cs);
        *(void **)list = a->free_list[c]; a->free_list[c] = list;
        list = next;
    }
}
static void *arena_alloc(cos_arena_t *a, size_t size) {
    if (a->remote) arena_drain_remote(a);
    size_t cs; unsigned c = arena_class(size, &cs);
    void *p = a->free_list[c];
    if (p) { a->free_list[c] = *(void **)p; return p; }
    if ((size_t)(a->end - a->bump) < cs + COS_ARENA_HDR) return NULL;
    char *blk = a->bump; a->bump += cs + COS_ARENA_HDR;
    *(size_t *)blk = cs;
    return blk + COS_ARENA_HDR;
}
static void arena_free(cos_arena_t *a, void *ptr) {
    char *sp = (char *)__builtin_frame_address(0);
    if (sp >= a->stack_lo && sp < a->stack_hi) {
        size_t cs; unsigned c = arena_class(*(size_t *)((char *)ptr - COS_ARENA_HDR), &cs);
        *(void **)ptr = a->free_list[c]; a->free_list[c] = ptr;
    } else {
        void *head = a->remote;
        do { *(void **)ptr = head; }
        while (!__atomic_compare_exchange_n(&a->remote, &head, ptr, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    }
}

/* Maps every page the main thread has already handed out, plus the program's own data and bss, so a worker never
 * takes a first-touch fault on them. Called when the first arena appears (no worker is running yet). */
/* GNU ld defines __bss_start, _edata and _end; TinyCC (tcc.c-os) defines only _edata and _end. __bss_start is therefore a weak reference (0 when the
 * linker has none) and _edata, which both define at the end of the data, stands in for it: a program built on the device must still link. */
extern char __bss_start[] __attribute__((weak));
extern char _edata[], _end[];
static void cos_prefault_process(void) {
    for (cos_block_t *b = g_heap_head; b; b = b->next) {
        char *lo = (char *)b + COS_BLOCK_HDR, *hi = lo + b->size;   /* payload only: never the header */
        for (char *p = (char *)(((uintptr_t)lo + 4095) & ~(uintptr_t)4095); p < hi; p += 4096)
            if (b->free) *(volatile char *)p = 0;       /* free payload is unused */
            else (void)*(volatile char *)p;              /* in use: read only, never disturb the contents */
    }
    char *bss_lo = __bss_start ? __bss_start : _edata;
    for (char *p = (char *)(((uintptr_t)bss_lo) & ~(uintptr_t)4095); p < _end; p += 4096) {
        volatile char *q = (volatile char *)p;
        char v = *q; *q = v;                              /* writable mapping, contents unchanged */
    }
}

cos_arena_t *cos_arena_create(size_t heap_bytes, size_t stack_bytes) {
    if (g_narenas == 0) cos_prefault_process();
    heap_bytes = (heap_bytes + 4095) & ~(size_t)4095;
    stack_bytes = (stack_bytes + 4095) & ~(size_t)4095;
    cos_arena_t *a = (cos_arena_t *)cos_calloc(1, sizeof *a);
    if (!a) return NULL;
    a->region_bytes = stack_bytes + heap_bytes;
    a->region = (char *)cos_mmap(a->region_bytes, COS_PROT_READ | COS_PROT_WRITE);
    if (!a->region) { cos_free(a); return NULL; }
    /* The other core cannot take a page fault: map every page now. */
    for (size_t o = 0; o < a->region_bytes; o += 4096) a->region[o] = 0;
    a->stack_lo = a->region; a->stack_hi = a->region + stack_bytes;
    a->base = a->bump = a->stack_hi; a->end = a->region + a->region_bytes;
    for (int i = 0; i < COS_ARENA_MAX; i++) {
        if (!g_arenas[i]) { g_arenas[i] = a; __atomic_fetch_add(&g_narenas, 1, __ATOMIC_RELEASE); return a; }
    }
    cos_munmap(a->region, a->region_bytes); cos_free(a);
    return NULL;
}
void cos_arena_stack(cos_arena_t *a, void **base, size_t *size) {
    if (base) *base = a->stack_lo;
    if (size) *size = (size_t)(a->stack_hi - a->stack_lo);
}
void cos_arena_destroy(cos_arena_t *a) {
    if (!a) return;
    for (int i = 0; i < COS_ARENA_MAX; i++) if (g_arenas[i] == a) { g_arenas[i] = NULL; __atomic_fetch_sub(&g_narenas, 1, __ATOMIC_RELEASE); }
    cos_munmap(a->region, a->region_bytes);
    cos_free(a);
}
bool cos_in_worker(void) { return g_narenas && arena_for_stack() != NULL; }

/* The main heap is shared by every thread of the process and its free list is not safe to walk from two threads at once (a thread's own startup
 * frees and allocates while the creator allocates). Once a second thread exists the heap takes a lock; before that nothing changes, and no system call
 * is made. The lock is re-entrant: free() of an aligned block, the deferred-free drain and realloc() call back into the public functions.
 * Workers (compute jobs on other cores) have their own arenas, and never reach the main heap. */
static volatile int g_threaded;
static volatile int g_heap_lock;
static volatile int64_t g_heap_owner;
static int g_heap_depth;
static void heap_lock(void) {
    int64_t me = (int64_t)cos_gettid();
    if (g_heap_owner == me) { g_heap_depth++; return; }
    while (__atomic_exchange_n(&g_heap_lock, 1, __ATOMIC_ACQUIRE)) cos_yield();
    g_heap_owner = me; g_heap_depth = 1;
}
static void heap_unlock(void) {
    if (--g_heap_depth == 0) { g_heap_owner = 0; __atomic_store_n(&g_heap_lock, 0, __ATOMIC_RELEASE); }
}
static void *cos_malloc_impl(size_t size);
static void cos_free_impl(void *ptr);
static void *cos_realloc_impl(void *ptr, size_t size);
void *cos_malloc(size_t size) {
    if (!g_threaded) return cos_malloc_impl(size);
    heap_lock(); void *p = cos_malloc_impl(size); heap_unlock(); return p;
}
void cos_free(void *ptr) {
    if (!g_threaded) { cos_free_impl(ptr); return; }
    heap_lock(); cos_free_impl(ptr); heap_unlock();
}
void *cos_realloc(void *ptr, size_t size) {
    if (!g_threaded) return cos_realloc_impl(ptr, size);
    heap_lock(); void *p = cos_realloc_impl(ptr, size); heap_unlock(); return p;
}

static void *cos_malloc_impl(size_t size) {
    if (size == 0) return NULL;
    if (g_narenas) {
        cos_arena_t *a = arena_for_stack();
        if (a) return arena_alloc(a, size);
        if (g_deferred_free) heap_drain_deferred();
    }
    size = align_up(size, COS_MALLOC_ALIGN);
    if (!g_heap_cookie) heap_cookie_init();

    for (cos_block_t *b = g_heap_head; b; b = b->next) {
        blk_check(b);
        if (!b->free || b->size < size) continue;
        /* Split only when the remainder can hold a header plus a
         * meaningful allocation - otherwise the leftover is unusable
         * and splitting just fragments the list. */
        if (b->size >= size + COS_BLOCK_HDR + COS_MALLOC_ALIGN) {
            cos_block_t *rest = (cos_block_t *)((char *)b + COS_BLOCK_HDR + size);
            rest->size = b->size - size - COS_BLOCK_HDR;
            rest->free = true;
            rest->next = b->next;
            blk_seal(rest);
            b->size = size;
            b->next = rest;
        }
        b->free = false; blk_seal(b);
        return (char *)b + COS_BLOCK_HDR;
    }

    size_t need = align_up(size + COS_BLOCK_HDR, COS_SBRK_CHUNK);
    void *mem = cos_sbrk((long)need);
    if (!mem) return NULL;
    /* With workers running, a page first touched by one of them would fault on a core that cannot service it. */
    if (g_narenas) for (size_t o = 0; o < need; o += 4096) ((volatile char *)mem)[o] = 0;

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
        blk_seal(rest);
        b->size = size;
        b->next = rest;
    }
    blk_seal(b);
    return (char *)b + COS_BLOCK_HDR;
}

/* Over-aligned allocations (aligned_alloc with align > 16).
 *
 * The block is carved out of an ordinary cos_malloc() allocation ("raw"), and the
 * pointer handed to the caller sits further in, at the requested alignment. Directly
 * below that pointer, where cos_free()/cos_realloc() look for a cos_block_t, we put a
 * small header of our own instead. It is recognised by a magic value in the slot that
 * normally holds the block size (a real size can never be that large), and it records
 * the raw pointer and the bytes usable from the aligned address. Without it free() would
 * interpret arbitrary payload bytes as a block header and corrupt the heap. */
#define COS_ALIGNED_MAGIC 0xA11C0DEDC0FFEE16ULL
typedef struct cos_aligned_hdr {
    size_t magic;
    void  *raw;       /* the pointer cos_malloc() returned */
    size_t usable;    /* bytes available from the aligned address */
} cos_aligned_hdr_t;
_Static_assert(sizeof(cos_aligned_hdr_t) <= sizeof(cos_block_t), "aligned header must fit in a block header slot");

static bool cos_is_aligned_block(const void *ptr) {
    const cos_aligned_hdr_t *h = (const cos_aligned_hdr_t *)((const char *)ptr - COS_BLOCK_HDR);
    return h->magic == COS_ALIGNED_MAGIC;
}

void *cos_aligned_alloc(size_t align, size_t size) {
    if (align == 0 || (align & (align - 1))) return NULL;          /* must be a power of two */
    if (size == 0) return NULL;
    if (align <= COS_MALLOC_ALIGN) return cos_malloc(size);        /* already satisfied */
    if (size > (size_t)-1 - align - COS_BLOCK_HDR) return NULL;
    char *raw = cos_malloc(size + align + COS_BLOCK_HDR);
    if (!raw) return NULL;
    /* Leave room for our header below the aligned address. */
    uintptr_t a = ((uintptr_t)raw + COS_BLOCK_HDR + align - 1) & ~(uintptr_t)(align - 1);
    cos_aligned_hdr_t *h = (cos_aligned_hdr_t *)(a - COS_BLOCK_HDR);
    h->magic  = COS_ALIGNED_MAGIC;
    h->raw    = raw;
    h->usable = size;
    return (void *)a;
}

static void cos_free_impl(void *ptr) {
    if (!ptr) return;
    if (cos_is_aligned_block(ptr)) {
        void *raw = ((cos_aligned_hdr_t *)((char *)ptr - COS_BLOCK_HDR))->raw;
        ((cos_aligned_hdr_t *)((char *)ptr - COS_BLOCK_HDR))->magic = 0; /* catch double free */
        cos_free(raw);
        return;
    }
    if (g_narenas) {
        cos_arena_t *a = arena_for_ptr(ptr);
        if (a) { arena_free(a, ptr); return; }
        if (arena_for_stack()) {                 /* a worker freeing a main-heap block: let the main thread do it */
            void *head = g_deferred_free;
            do { *(void **)ptr = head; }
            while (!__atomic_compare_exchange_n(&g_deferred_free, &head, ptr, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
            return;
        }
        if (g_deferred_free) heap_drain_deferred();
    }
    cos_block_t *b = (cos_block_t *)((char *)ptr - COS_BLOCK_HDR);
    blk_check(b);
    if (b->free) heap_corrupt("double free");
    b->free = true; blk_seal(b);
    /* Coalesce forward with any immediately-adjacent free block. Only
     * forward, and only with a directly abutting neighbour: the list is
     * not address-ordered, so a general merge would need a sort this
     * allocator deliberately does not maintain. */
    cos_block_t *n = b->next;
    if (n && n->free && (char *)b + COS_BLOCK_HDR + b->size == (char *)n) {
        blk_check(n);
        b->size += COS_BLOCK_HDR + n->size;
        b->next = n->next;
        blk_seal(b);
    }
}

static void heap_drain_deferred(void) {
    void *list = __atomic_exchange_n(&g_deferred_free, NULL, __ATOMIC_ACQ_REL);
    while (list) { void *next = *(void **)list; cos_free(list); list = next; }
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

static void *cos_realloc_impl(void *ptr, size_t size) {
    if (!ptr) return cos_malloc(size);
    if (size == 0) { cos_free(ptr); return NULL; }
    size_t old_size;
    if (cos_is_aligned_block(ptr)) {
        old_size = ((cos_aligned_hdr_t *)((char *)ptr - COS_BLOCK_HDR))->usable;
    } else if (g_narenas && arena_for_ptr(ptr)) {
        old_size = *(size_t *)((char *)ptr - COS_ARENA_HDR);
    } else {
        blk_check((cos_block_t *)((char *)ptr - COS_BLOCK_HDR));
        old_size = ((cos_block_t *)((char *)ptr - COS_BLOCK_HDR))->size;
    }
    if (old_size >= size) return ptr;
    void *n = cos_malloc(size);
    if (!n) return NULL;
    cos_memcpy(n, ptr, old_size);
    cos_free(ptr);
    return n;
}

static size_t strlen_local(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static void cos_memcpy_local(char *d, const char *s, size_t n) { for (size_t i = 0; i < n; ++i) d[i] = s[i]; }

/* ---- path normalisation ---------------------------------------------------------------------------------------------------------------
 * The kernel refuses a path with a "." or ".." component (a process must not be able to climb out of a directory it was given). Programs written for
 * a host - a compiler following "#include \"../gui/x.c\"" - use them all the time, so the library collapses them lexically before the call: "a/b/../c"
 * becomes "a/c", "/x/.." becomes "/" (never above the root). What remains is checked by the kernel as before. */
static char g_cwd[256] = "/";                                  /* the working directory of this process (chdir/getcwd; the kernel has none, relative paths are made absolute here) */
static const char *cos_pathfix_abs(const char *p, char *buf, size_t cap);
static const char *cos_pathfix(const char *p, char *buf, size_t cap) {
    if (!p) return p;
    if (p[0] != '/' && p[0] && g_cwd[1]) {                      /* relative, and the cwd is not the root: join them */
        size_t cl = strlen_local(g_cwd);
        char tmp[320];
        if (cl + 1 + strlen_local(p) + 1 < sizeof tmp) {
            size_t k = 0; for (size_t i = 0; i < cl; ++i) tmp[k++] = g_cwd[i];
            tmp[k++] = '/'; for (const char *q = p; *q; ++q) tmp[k++] = *q;
            tmp[k] = 0;
            return cos_pathfix_abs(tmp, buf, cap) == tmp ? (cos_memcpy_local(buf, tmp, k + 1), buf) : buf;
        }
    }
    return cos_pathfix_abs(p, buf, cap);
}
static const char *cos_pathfix_abs(const char *p, char *buf, size_t cap) {
    if (!p) return p;
    int dots = 0;
    for (const char *q = p; *q; ++q) if (*q == '.' && (q == p || q[-1] == '/') && (q[1] == 0 || q[1] == '/' || (q[1] == '.' && (q[2] == 0 || q[2] == '/')))) { dots = 1; break; }
    if (!dots) return p;
    size_t n = 0; bool abs = p[0] == '/';
    if (abs) buf[n++] = '/';
    size_t floor_n = n;
    const char *q = p;
    while (*q) {
        while (*q == '/') ++q;
        if (!*q) break;
        const char *e = q; while (*e && *e != '/') ++e;
        size_t cl = (size_t)(e - q);
        if (cl == 1 && q[0] == '.') { q = e; continue; }
        if (cl == 2 && q[0] == '.' && q[1] == '.') {
            if (n > floor_n) { --n; while (n > floor_n && buf[n - 1] != '/') --n; }          /* drop the last component (n is after its slash, or at the start) */
            q = e; continue;
        }
        if (n + cl + 2 >= cap) return p;                                  /* too long: let the kernel judge the original */
        if (n > floor_n || (n == floor_n && n > 0 && buf[n - 1] != '/')) { if (n > floor_n && buf[n - 1] != '/') buf[n++] = '/'; }
        for (size_t i = 0; i < cl; ++i) buf[n++] = q[i];
        buf[n++] = '/';
        q = e;
    }
    if (n > 1 && buf[n - 1] == '/') --n;
    if (n == 0) buf[n++] = '.';
    buf[n] = 0;
    return buf;
}
#define PATHFIX(var) char var##_b[320]; var = cos_pathfix(var, var##_b, sizeof var##_b)
int cos_chdir(const char *path) {
    char b[320]; const char *p = cos_pathfix(path, b, sizeof b);
    if (!p || !p[0]) return -1;
    char full[320];
    if (p[0] == '/') { size_t n = strlen_local(p); if (n >= sizeof full) return -1; cos_memcpy_local(full, p, n + 1); }
    else { size_t n = strlen_local(p); if (n + 2 >= sizeof full) return -1; full[0] = '/'; cos_memcpy_local(full + 1, p, n + 1); }
    int fd = cos_opendir(full);                                 /* must exist and be a directory */
    if (fd < 0) return -1;
    cos_close(fd);
    size_t n = strlen_local(full); if (n >= sizeof g_cwd) return -1;
    cos_memcpy_local(g_cwd, full, n + 1);
    return 0;
}
char *cos_getcwd(char *buf, size_t size) {
    size_t n = strlen_local(g_cwd);
    if (!buf || size < n + 1) return 0;
    cos_memcpy_local(buf, g_cwd, n + 1);
    return buf;
}

/* ---- whole-file I/O ------------------------------------------------------ */
ssize_t cos_read_file(const char *path, void *buf, size_t len) {
    PATHFIX(path);
    return sys3(SYS_READ_FILE, (long)path, (long)buf, (long)len);
}
ssize_t cos_write_file(const char *path, const void *buf, size_t len) {
    PATHFIX(path);
    return sys3(SYS_WRITE_FILE, (long)path, (long)buf, (long)len);
}

/* ---- file descriptors ---------------------------------------------------- */
int cos_open(const char *path, uint32_t flags) {
    PATHFIX(path);
    return (int)sys2(SYS_FD_OPEN, (long)path, (long)flags);
}
int cos_opendir(const char *path) { PATHFIX(path); return (int)sys1(SYS_FD_OPENDIR, (long)path); }
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
int64_t cos_dlopen(const char *path) { return cos_dlopen_flags(path, 0x0100 /* COS_RTLD_GLOBAL */); }   /* the dl runtime (cos_rtld.c) runs the library's constructors */
__attribute__((noinline)) void *cos_dlsym(int64_t handle, const char *name) {
    if (handle == -1) return cos_dlsym_next(__builtin_return_address(0), name);                         /* RTLD_NEXT: the caller is what counts */
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
#define SYS_WIN_POLL_MOUSE 58
int cos_win_poll_mouse(int64_t win, cos_mouse_event_t *out) {
    return (int)sys2(SYS_WIN_POLL_MOUSE, (long)win, (long)out);
}

/* ---- threads, time, filesystem mutation ---------------------------------- */
#define SYS_THREAD_CREATE 30
#define SYS_THREAD_EXIT   31
#define SYS_THREAD_JOIN   32
#define SYS_TIME_MS       33
#define SYS_RTC_NOW       197
#define SYS_SETTINGS_GET  198
#define SYS_SETTINGS_SET  199
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

/* Every thread runs on a stack of its own. The kernel would otherwise start all of them on the process stack top, where they overwrite each other's
 * frames (and the main thread's). A stack is freed once its thread has ended: when it is joined, or when a later cos_thread_create() finds it dead. */
#define COS_THREAD_STACK_SIZE (256u * 1024u)
#define COS_THREAD_STACKS 64
static struct { int64_t tid; void *stk; } g_thread_stacks[COS_THREAD_STACKS];

static void thread_stack_release(int64_t tid) {
    for (int i = 0; i < COS_THREAD_STACKS; i++)
        if (g_thread_stacks[i].stk && g_thread_stacks[i].tid == tid) { cos_free(g_thread_stacks[i].stk); g_thread_stacks[i].stk = 0; }
}
static void thread_stacks_reap(void) {
    for (int i = 0; i < COS_THREAD_STACKS; i++)
        if (g_thread_stacks[i].stk && sys2(SYS_THREAD_JOIN, (long)g_thread_stacks[i].tid, 0) == 0) { cos_free(g_thread_stacks[i].stk); g_thread_stacks[i].stk = 0; }
}

int64_t cos_thread_create(cos_thread_fn fn, void *arg) {
    if (!fn) return -1;
    thread_stacks_reap();
    g_threaded = 1;                                          /* from now on the heap is locked */
    cos_thread_start_t *st = (cos_thread_start_t *)cos_malloc(sizeof(*st));
    if (!st) return -1;
    st->fn = fn;
    st->arg = arg;
    int slot = -1;
    for (int i = 0; i < COS_THREAD_STACKS; i++) if (!g_thread_stacks[i].stk) { slot = i; break; }
    char *stk = slot >= 0 ? (char *)cos_malloc(COS_THREAD_STACK_SIZE) : 0;
    if (!stk) { cos_free(st); return -1; }
    stk[COS_THREAD_STACK_SIZE - 1] = 0;                      /* the top of the stack must be mapped before the kernel checks it */
    int64_t tid = sys3(SYS_THREAD_CREATE,
                       (long)(uintptr_t)cos_thread_trampoline,
                       (long)(uintptr_t)st,
                       (long)(uintptr_t)(stk + COS_THREAD_STACK_SIZE));
    if (tid < 0) { cos_free(st); cos_free(stk); return tid; }
    g_thread_stacks[slot].tid = tid; g_thread_stacks[slot].stk = stk;
    return tid;
}
void cos_thread_exit(int code) { sys1(SYS_THREAD_EXIT, code); __builtin_unreachable(); }
int cos_thread_join(int64_t tid, uint64_t timeout_ms) {
    int r = (int)sys2(SYS_THREAD_JOIN, (long)tid, (long)timeout_ms);
    if (r == 0) thread_stack_release(tid);
    return r;
}


/* ---- compute jobs on other cores -------------------------------------- */
#define SYS_PAR_INFO  206
#define SYS_PAR_START 207
#define SYS_PAR_POLL  208
#define SYS_PAR_STOP  209
#define SYS_PAR_FREE  210
/* A job's function returns here (its return address); `int $0x82` is how a
 * job tells the kernel it finished. */
__asm__(".text\n.global cos_par_exit_stub\ncos_par_exit_stub:\n int $0x82\n ud2\n");
extern void cos_par_exit_stub(void);
int cos_par_cpus(void) { return (int)sys0(SYS_PAR_INFO); }
int cos_par_start(void (*fn)(void *), void *arg, void *stack, size_t stack_size) {
    if (!fn || !stack || stack_size < 4096) return -22;
    uint64_t d[5];
    d[0] = (uint64_t)(uintptr_t)fn;
    d[1] = (uint64_t)(uintptr_t)arg;
    d[2] = ((uint64_t)(uintptr_t)stack + stack_size) & ~0xFull;
    d[3] = (uint64_t)(uintptr_t)cos_par_exit_stub;
    d[4] = cos_tls_create_block();       /* 0 when the process has no TLS */
    return (int)sys1(SYS_PAR_START, (long)(uintptr_t)d);
}
int cos_par_poll(int id, cos_par_fault_t *fault) { return (int)sys2(SYS_PAR_POLL, (long)id, (long)(uintptr_t)fault); }
int cos_par_stop(int id) { return (int)sys1(SYS_PAR_STOP, (long)id); }
int cos_par_free(int id) { return (int)sys1(SYS_PAR_FREE, (long)id); }

uint64_t cos_time_ms(void)   { return (uint64_t)sys0(SYS_TIME_MS); }
int cos_rtc_now(cos_datetime_t *out) { if (!out) return -1; return (int)sys1(SYS_RTC_NOW, (long)out); }
int cos_settings_get(cos_settings_t *out) { if (!out) return -1; return (int)sys1(SYS_SETTINGS_GET, (long)out); }
int cos_settings_set(int key, int value) { return (int)sys2(SYS_SETTINGS_SET, (long)key, (long)value); }
#define SYS_SETTINGS_ACTION 202
#define SYS_PASSWORD_CHANGE 203
int cos_settings_action(int action, int arg) { return (int)sys2(SYS_SETTINGS_ACTION, (long)action, (long)arg); }
int cos_kmod_load(const char *path, const char *password) { return (int)sys2(245, (long)path, (long)password); }
int cos_kmod_unload(const char *name, const char *password) { return (int)sys2(246, (long)name, (long)password); }
int cos_kmod_list(char *buf, uint64_t cap) { return (int)sys2(247, (long)buf, (long)cap); }
int cos_password_change(const char *old_password, const char *new_password) {
    return (int)sys2(SYS_PASSWORD_CHANGE, (long)old_password, (long)new_password);
}
const char *cos_font_family_name(int family) {
    static const char *const names[6] = {
        "Anthropic Serif", "Pixel Arcade", "Gothic Sans", "Noto Sans", "Mono Terminal", "Rounded UI"
    };
    if (family < 0 || family >= 6) family = 0;
    return names[family];
}
uint64_t cos_time_unix(void) { return (uint64_t)sys0(SYS_TIME_UNIX); }

int cos_mkdir(const char *path)  { PATHFIX(path); return (int)sys1(SYS_MKDIR, (long)path); }
int cos_unlink(const char *path) { PATHFIX(path); return (int)sys1(SYS_UNLINK, (long)path); }
int cos_rename(const char *o, const char *n) {
    PATHFIX(o); PATHFIX(n);
    return (int)sys2(SYS_RENAME, (long)o, (long)n);
}
int cos_stat(const char *path, cos_stat_t *out) {
    PATHFIX(path);
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

#define SYS_NET_HTTP_START  218
#define SYS_NET_HTTP_POLL   219
#define SYS_NET_HTTP_TAKE   220
#define SYS_NET_HTTP_CANCEL 221
int cos_net_http_start(const char *url) { return (int)sys1(SYS_NET_HTTP_START, (long)url); }
int cos_net_http_poll(int job, cos_http_result_t *res) { return (int)sys2(SYS_NET_HTTP_POLL, (long)job, (long)res); }
ssize_t cos_net_http_take(int job, void *buf, size_t cap) { return sys3(SYS_NET_HTTP_TAKE, (long)job, (long)buf, (long)cap); }
void cos_net_http_cancel(int job) { (void)sys1(SYS_NET_HTTP_CANCEL, (long)job); }

#define SYS_SOCKET     222
#define SYS_BIND       223
#define SYS_LISTEN     224
#define SYS_ACCEPT     225
#define SYS_CONNECT    226
#define SYS_SEND       227
#define SYS_RECV       228
#define SYS_SENDTO     229
#define SYS_RECVFROM   230
#define SYS_SOCK_POLL  231
#define SYS_SOCK_CLOSE 232
#define SYS_SOCK_NAME  233
#define SYS_DNS_START  234
#define SYS_DNS_POLL   235
#define SYS_NET_INFO   236
int cos_socket(int type) { return (int)sys1(SYS_SOCKET, (long)type); }
int cos_bind(int sd, uint32_t ip, uint16_t port) { return (int)sys3(SYS_BIND, (long)sd, (long)ip, (long)port); }
int cos_listen(int sd, int backlog) { return (int)sys2(SYS_LISTEN, (long)sd, (long)backlog); }
int cos_accept(int sd, cos_sockaddr_t *peer) { return (int)sys2(SYS_ACCEPT, (long)sd, (long)peer); }
int cos_connect(int sd, uint32_t ip, uint16_t port) { return (int)sys3(SYS_CONNECT, (long)sd, (long)ip, (long)port); }
ssize_t cos_send(int sd, const void *buf, size_t len) { return sys3(SYS_SEND, (long)sd, (long)buf, (long)len); }
ssize_t cos_recv(int sd, void *buf, size_t len) { return sys3(SYS_RECV, (long)sd, (long)buf, (long)len); }
ssize_t cos_sendto(int sd, const void *buf, size_t len, uint32_t ip, uint16_t port) {
    return sys6(SYS_SENDTO, (long)sd, (long)buf, (long)len, (long)ip, (long)port, 0);
}
ssize_t cos_recvfrom(int sd, void *buf, size_t len, cos_sockaddr_t *from) { return sys4(SYS_RECVFROM, (long)sd, (long)buf, (long)len, (long)from); }
int cos_sock_poll(cos_pollfd_t *fds, int n) { return (int)sys2(SYS_SOCK_POLL, (long)fds, (long)n); }
int cos_sock_close(int sd) { return (int)sys1(SYS_SOCK_CLOSE, (long)sd); }
int cos_getsockname(int sd, cos_sockaddr_t *out) { return (int)sys3(SYS_SOCK_NAME, (long)sd, 0, (long)out); }
int cos_getpeername(int sd, cos_sockaddr_t *out) { return (int)sys3(SYS_SOCK_NAME, (long)sd, 1, (long)out); }
int cos_dns_start(const char *host, cos_dns_result_t *out) { return (int)sys2(SYS_DNS_START, (long)host, (long)out); }
int cos_dns_poll(cos_dns_result_t *io) { return (int)sys1(SYS_DNS_POLL, (long)io); }
int cos_net_info(cos_netinfo_t *out) { return (int)sys1(SYS_NET_INFO, (long)out); }
#define SYS_SHELL_CREATE 237
#define SYS_WIN_LIST     238
#define SYS_WIN_CTL      239
int64_t cos_shell_create(int layer, cos_win_info_t *out) { return sys2(SYS_SHELL_CREATE, (long)layer, (long)out); }
int cos_win_list(cos_winfo_t *out, int max) { return (int)sys2(SYS_WIN_LIST, (long)out, (long)max); }
#define SYS_SHELL_ICONS  240
int cos_shell_icons(int layer, const cos_shell_icon_t *icons, int n, uint32_t tag) { return (int)sys4(SYS_SHELL_ICONS, (long)layer, (long)icons, (long)n, (long)tag); }
#define SYS_CTX_LIST 244
int cos_ctx_list(cos_ctxsnap_t *out) { return (int)sys1(SYS_CTX_LIST, (long)out); }
#define SYS_NOTIF_LIST 243
#define SYS_NOTIF_SEND 249
#define SYS_KERNEL_UPDATE 250
int cos_kernel_update_status(cos_kupd_status_t *out) { return (int)sys3(SYS_KERNEL_UPDATE, 0, (long)out, 0); }
int cos_kernel_update_install(const void *blob, size_t len) { return (int)sys3(SYS_KERNEL_UPDATE, 1, (long)blob, (long)len); }
int cos_kernel_update_why(char *buf96) { return (int)sys3(SYS_KERNEL_UPDATE, 2, (long)buf96, 0); }
int cos_notify(const char *title, const char *message, int type) { return (int)sys3(SYS_NOTIF_SEND, (long)title, (long)message, (long)type); }
int cos_notif_list(cos_notif_t *out, int max) { return (int)sys2(SYS_NOTIF_LIST, (long)out, (long)max); }
#define SYS_DESKTOP_LIST 241
#define SYS_DESKTOP_CTL  242
int cos_desktop_list(cos_dicon_t *out, int max) { return (int)sys2(SYS_DESKTOP_LIST, (long)out, (long)max); }
int cos_desktop_ctl(int action, int a, int b, int c) { return (int)sys4(SYS_DESKTOP_CTL, (long)action, (long)a, (long)b, (long)c); }
int cos_win_ctl(int action, uint32_t uid, int arg) { return (int)sys3(SYS_WIN_CTL, (long)action, (long)uid, (long)arg); }

int cos_sock_wait(int sd, int events, int timeout_ms) {
    uint64_t end = cos_time_ms() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        cos_pollfd_t p = { sd, (int16_t)events, 0 };
        int n = cos_sock_poll(&p, 1);
        if (n < 0) return n;
        if (n > 0) return p.revents;
        if (cos_time_ms() >= end) return -COS_ETIMEDOUT;
        cos_sleep_ms(2);
    }
}

int cos_tcp_connect(uint32_t ip, uint16_t port, int timeout_ms) {
    int sd = cos_socket(COS_SOCK_STREAM);
    if (sd < 0) return sd;
    uint64_t end = cos_time_ms() + (uint64_t)timeout_ms;
    int rc = cos_connect(sd, ip, port);
    while (rc == -COS_EINPROGRESS) {
        if (cos_time_ms() >= end) { rc = -COS_ETIMEDOUT; break; }
        cos_sleep_ms(2);
        rc = cos_connect(sd, ip, port);
    }
    if (rc < 0) { cos_sock_close(sd); return rc; }
    return sd;
}

int cos_tcp_listen(uint16_t port, int backlog) {
    int sd = cos_socket(COS_SOCK_STREAM);
    if (sd < 0) return sd;
    int rc = cos_bind(sd, 0, port);
    if (rc == 0) rc = cos_listen(sd, backlog);
    if (rc < 0) { cos_sock_close(sd); return rc; }
    return sd;
}

int cos_tcp_accept_wait(int sd, cos_sockaddr_t *peer, int timeout_ms) {
    uint64_t end = cos_time_ms() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        int c = cos_accept(sd, peer);
        if (c != -COS_EAGAIN) return c;
        if (cos_time_ms() >= end) return -COS_ETIMEDOUT;
        cos_sleep_ms(2);
    }
}

ssize_t cos_send_all(int sd, const void *buf, size_t len, int timeout_ms) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t done = 0;
    uint64_t end = cos_time_ms() + (uint64_t)timeout_ms;
    while (done < len) {
        ssize_t n = cos_send(sd, p + done, len - done);
        if (n > 0) { done += (size_t)n; end = cos_time_ms() + (uint64_t)timeout_ms; continue; }
        if (n != -COS_EAGAIN) return n < 0 ? n : (ssize_t)done;
        if (cos_time_ms() >= end) return -COS_ETIMEDOUT;
        cos_sleep_ms(2);
    }
    return (ssize_t)done;
}

ssize_t cos_recv_wait(int sd, void *buf, size_t len, int timeout_ms) {
    uint64_t end = cos_time_ms() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        ssize_t n = cos_recv(sd, buf, len);
        if (n != -COS_EAGAIN) return n;
        if (cos_time_ms() >= end) return -COS_ETIMEDOUT;
        cos_sleep_ms(2);
    }
}

int cos_ip_parse(const char *s, uint32_t *ip) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        unsigned n = 0, digits = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (unsigned)(*s - '0'); s++; digits++; if (n > 255) return -1; }
        if (!digits) return -1;
        v = (v << 8) | n;
        if (part < 3) { if (*s != '.') return -1; s++; }
    }
    if (*s) return -1;
    *ip = v;
    return 0;
}

int cos_resolve_wait(const char *host, uint32_t *ip, int timeout_ms) {
    cos_dns_result_t r;
    int rc = cos_dns_start(host, &r);
    if (rc < 0) return rc;
    uint64_t end = cos_time_ms() + (uint64_t)timeout_ms;
    while (r.state == 0) {
        if (cos_time_ms() >= end) return -COS_ETIMEDOUT;
        cos_sleep_ms(5);
        cos_dns_poll(&r);
    }
    if (r.state < 0) return -COS_ENETUNREACH;
    *ip = COS_IP(r.ip[0], r.ip[1], r.ip[2], r.ip[3]);
    return 0;
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
    long     high;           /* the end of the furthest write through this stream: the directory entry (cos_stat) shows the old size until the file is closed */
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
    if (f->pos > f->high) f->high = f->pos;
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
    bool plus = false; for (const char *m = mode + 1; *m; ++m) if (*m == '+') plus = true;       /* "r+", "w+", "wb+", "a+" ... */
    bool trunc = false;
    if (mode[0] == 'r' && plus)           { flags = COS_O_RDWR_CREATE;   writable = true;  }
    else if (mode[0] == 'r')              { flags = COS_O_RDONLY;        writable = false; }
    else if (mode[0] == 'w' && plus)      { flags = COS_O_RDWR_CREATE;   writable = true;  trunc = true; }   /* write, then read it back (tcc -ar does) */
    else if (mode[0] == 'w')              { flags = COS_O_WRONLY_CREATE; writable = true;  }
    else if (mode[0] == 'a' && plus)      { flags = COS_O_RDWR_CREATE;   writable = true;  }
    else if (mode[0] == 'a')              { flags = COS_O_WRONLY_APPEND; writable = true;  }
    else return NULL;
    if (trunc) cos_unlink(path);                                     /* "w+" starts empty; the read-write open does not truncate */

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
    /* 1. bytes already buffered (and a pushed-back one) */
    while (done < total && (f->unget >= 0 || f->buf_pos < f->buf_len)) {
        int c = cos_fgetc(f);
        if (c == COS_EOF) break;
        out[done++] = (unsigned char)c;
    }
    /* 2. bulk: with the buffer empty, big reads go straight from the descriptor (the syscall
     *    moves at most 64 KiB, so loop) instead of one cos_fgetc() call per byte */
    if (done < total && f->buf_len == 0 && !f->dirty && total - done >= COS_STREAM_BUF) {
        while (total - done >= COS_STREAM_BUF) {
            size_t want = total - done; if (want > 65536) want = 65536;
            cos_lseek(f->fd, f->pos);
            ssize_t n = cos_fd_read(f->fd, out + done, want);
            if (n <= 0) { f->at_eof = true; break; }
            done += (size_t)n; f->pos += n;
        }
    }
    /* 3. the tail, through the buffer */
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
    /* bulk: a large write flushes what is buffered, then goes straight to the descriptor in
     * <= 64 KiB syscalls - a 10 MB object file is not 10 million cos_fputc() calls */
    if (total >= 2u * COS_STREAM_BUF && stream_flush_write(f) == 0) {
        while (total - done >= COS_STREAM_BUF) {
            size_t chunk = total - done; if (chunk > 65536) chunk = 65536;
            cos_lseek(f->fd, f->pos);
            ssize_t w = cos_fd_write(f->fd, in + done, chunk);
            if (w <= 0) return done / size;
            done += (size_t)w; f->pos += w;
            if (f->pos > f->high) f->high = f->pos;
        }
    }
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

/* How far this stream has written (after flushing what is buffered); 0 when it has not written. fseek(SEEK_END) uses it next to the size on disk. */
long cos_fhigh(cos_FILE *f) { if (!f || !f->used) return 0; stream_flush_write(f); return f->high; }

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

/* The raw system calls; cos_dlopen_flags / cos_dlclose in cos_rtld.c add the constructors and destructors around them. */
int64_t cos_dl_open_raw(const char *path, uint32_t flags) { return (int64_t)sys2(SYS_DLOPEN2, (long)path, (long)flags); }
int cos_dl_close_raw(int64_t handle) { return (int)sys1(SYS_DLCLOSE, (long)handle); }
int64_t cos_dl_ctl(uint64_t op, uint64_t a, uint64_t b, uint64_t c) { return (int64_t)sys4(SYS_DLCTL, (long)op, (long)a, (long)b, (long)c); }

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
int cos_getcpu(void) { return (int)sys0(SYS_GETCPU); }
int cos_cpu_load(uint8_t *pct, int max) { return (int)sys2(SYS_CPU_STATS, (long)(uintptr_t)pct, (long)max); }

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

/* ---- IPC ---------------------------------------------------------------
 *
 * Thin wrappers over SYS_IPC_* (see src/kernel/syscall.c for the full
 * rationale). These are what make a userspace server possible at all:
 * before them, src/kernel/ipc.c's complete message-passing and
 * shared-memory implementation had no syscall reaching it, so nothing
 * outside the kernel could send or receive a message.
 *
 * All return an IPC_* status (0 == success, negative == error), matching
 * the kernel side exactly rather than translating to errno - a server
 * loop wants to distinguish "timed out" from "queue full" from "bad
 * pid", and flattening those into -1 would throw that away. */
#define SYS_IPC_SEND      51
#define SYS_IPC_RECV      52
#define SYS_IPC_RESPOND   53
#define SYS_IPC_NOTIFY    54
#define SYS_SHM_CREATE    55
#define SYS_SHM_ATTACH    56

int cos_ipc_send(uint64_t dst_pid, uint64_t msg_type,
                 const void *data, uint64_t len)
{
    return (int)sys4(SYS_IPC_SEND, (long)dst_pid, (long)msg_type,
                     (long)(uintptr_t)data, (long)len);
}

int cos_ipc_recv(cos_ipc_msg_t *out, void *buf, uint64_t buf_cap,
                 uint64_t timeout_ms)
{
    /* The kernel fills `out` as four u64s: src_pid, msg_type, the length
     * actually written into buf, and a reserved word. cos_ipc_msg_t is
     * laid out to match exactly - the length comes back through the
     * header rather than the return value so that rax stays free to
     * carry the IPC_* status, keeping "received an empty message" and
     * "timed out" distinguishable. */
    return (int)sys4(SYS_IPC_RECV, (long)(uintptr_t)out,
                     (long)(uintptr_t)buf, (long)buf_cap, (long)timeout_ms);
}

int cos_ipc_respond(uint64_t dst_pid, uint64_t msg_id, uint64_t status,
                    const void *data, uint64_t len)
{
    return (int)sys6(SYS_IPC_RESPOND, (long)dst_pid, (long)msg_id,
                     (long)status, (long)(uintptr_t)data, (long)len, 0);
}

int cos_ipc_notify(uint64_t dst_pid, uint64_t event_type,
                   const void *data, uint64_t len)
{
    return (int)sys4(SYS_IPC_NOTIFY, (long)dst_pid, (long)event_type,
                     (long)(uintptr_t)data, (long)len);
}

int cos_shm_create(uint64_t size, uint64_t *out_shm_id)
{
    return (int)sys2(SYS_SHM_CREATE, (long)size, (long)(uintptr_t)out_shm_id);
}

int cos_shm_attach(uint64_t shm_id, void **out_addr)
{
    return (int)sys2(SYS_SHM_ATTACH, (long)shm_id, (long)(uintptr_t)out_addr);
}

/* ---- audio output ---- */
#define SYS_AUDIO_OPEN   170
#define SYS_AUDIO_WRITE  171
#define SYS_AUDIO_STATUS 172
#define SYS_AUDIO_DRAIN  173
#define SYS_AUDIO_STOP   174
#define SYS_AUDIO_VOLUME 175
#define SYS_AUDIO_CLOSE  176
int cos_audio_open(uint32_t rate, uint32_t channels) { return (int)sys2(SYS_AUDIO_OPEN, (long)rate, (long)channels); }
int64_t cos_audio_write(const int16_t *pcm, uint64_t samples) { return (int64_t)sys2(SYS_AUDIO_WRITE, (long)pcm, (long)samples); }
int cos_audio_status(cos_audio_status_t *out) { return (int)sys1(SYS_AUDIO_STATUS, (long)out); }
int cos_audio_drain(void) { return (int)sys0(SYS_AUDIO_DRAIN); }
int cos_audio_stop(void) { return (int)sys0(SYS_AUDIO_STOP); }
int cos_audio_set_volume(uint32_t v) { return (int)sys1(SYS_AUDIO_VOLUME, (long)v); }
int cos_audio_close(void) { return (int)sys0(SYS_AUDIO_CLOSE); }

/* ---- window API v2 ---- */
#define SYS_WIN2_CREATE    180
#define SYS_WIN2_PRESENT   181
#define SYS_WIN2_INPUT     251
#define SYS_WIN2_RESIZE 254
#define SYS_FILE_DROP 255
#define SYS_DRAG_GHOST 256
#define SYS_WIN2_FULLSCREEN 253
#define SYS_WIN2_GRAB      252
#define SYS_WIN2_POLL      182
#define SYS_WIN2_WAIT      183
#define SYS_WIN2_SET_TITLE 184
#define SYS_WIN2_CLOSE     185
#define SYS_WIN2_SCREEN    186
#define SYS_WIN2_VIDEO     188
int64_t cos_win2_create(const char *title, int32_t w, int32_t h, cos_win_info_t *out) {
    uint64_t wh = (uint64_t)(uint32_t)w | ((uint64_t)(uint32_t)h << 32);
    return (int64_t)sys3(SYS_WIN2_CREATE, (long)title, (long)wh, (long)out);
}
int cos_win2_present(int64_t h) { return (int)sys1(SYS_WIN2_PRESENT, (long)h); }
int cos_win2_video_frame(int64_t h, const uint32_t *p, int32_t w, int32_t hh, int32_t stride, int32_t x, int32_t y, int32_t dw, int32_t dh) {
    uint64_t wh = (uint64_t)(uint32_t)w | ((uint64_t)(uint32_t)hh << 32);
    uint64_t xy = (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32);
    uint64_t dd = (uint64_t)(uint32_t)dw | ((uint64_t)(uint32_t)dh << 32);
    return (int)sys6(188, (long)h, (long)p, (long)wh, (long)stride, (long)xy, (long)dd);
}
#define SYS_WIN2_VIDEO_YUV420 201
#define SYS_WIN2_VIDEO_MAP    211
#define SYS_DEVICES_LIST 214
#define SYS_SANDBOX 215
#define SYS_WIN2_PRESENT_RECTS 213
#define SYS_WIN2_VIDEO_COMMIT 212
uint32_t *cos_win2_video_map(int64_t handle, int32_t width, int32_t height, uint32_t **buf1) {
    uint64_t each = 0;
    long r = sys3(SYS_WIN2_VIDEO_MAP, (long)handle, (long)(((uint64_t)(uint32_t)height << 32) | (uint32_t)width), (long)(uintptr_t)&each);
    if (r == -1) return NULL;
    if (buf1) *buf1 = (uint32_t *)(uintptr_t)((uint64_t)r + each);
    return (uint32_t *)(uintptr_t)r;
}
int cos_win2_video_commit(int64_t handle, int idx, int32_t width, int32_t height, int32_t x, int32_t y, int32_t dst_w, int32_t dst_h) {
    return (int)sys6(SYS_WIN2_VIDEO_COMMIT, (long)handle, (long)idx,
                     (long)(((uint64_t)(uint32_t)height << 32) | (uint32_t)width),
                     (long)(((uint64_t)(uint32_t)y << 32) | (uint32_t)x),
                     (long)(((uint64_t)(uint32_t)dst_h << 32) | (uint32_t)dst_w), 0);
}
int64_t cos_sandbox(uint32_t keep_caps, const char *jail) { return (int64_t)sys2(SYS_SANDBOX, (long)keep_caps, (long)jail); }
#define SYS_SYSTEM_INFO 216
#define SYS_PROCLIST 217
int cos_sysinfo(cos_sysinfo_t *out) { return (int)sys1(SYS_SYSTEM_INFO, (long)out); }
int cos_proclist(cos_procinfo_t *out, int max) { return (int)sys2(SYS_PROCLIST, (long)out, (long)max); }
int cos_devices_list(cos_device_info_t *out, int max) { return (int)sys2(SYS_DEVICES_LIST, (long)out, (long)max); }
int cos_win2_present_rects(int64_t handle, const cos_win_rect_t *rects, uint32_t count) {
    return (int)sys3(SYS_WIN2_PRESENT_RECTS, (long)handle, (long)rects, (long)count);
}
int cos_win2_video_frame_yuv420(int64_t h, const cos_video_yuv420_t *frame) {
    return (int)sys2(SYS_WIN2_VIDEO_YUV420, (long)h, (long)frame);
}
int cos_win2_poll(int64_t h, cos_win_event_t *ev) { return (int)sys2(SYS_WIN2_POLL, (long)h, (long)ev); }
int cos_win2_resize(int64_t h, int32_t w, int32_t ht, cos_win_info_t *info) { return (int)sys3(SYS_WIN2_RESIZE, (long)h, (long)(((uint64_t)(uint32_t)ht << 32) | (uint32_t)w), (long)info); }
int cos_file_drop(const char *path, int copy) { return (int)sys2(SYS_FILE_DROP, (long)path, (long)copy); }
int cos_drag_ghost(const char *label) { return (int)sys1(SYS_DRAG_GHOST, (long)label); }
int cos_win2_fullscreen(int64_t h, int mode) { return (int)sys2(SYS_WIN2_FULLSCREEN, (long)h, (long)mode); }
int cos_win2_grab(int64_t h, int on) { return (int)sys2(SYS_WIN2_GRAB, (long)h, (long)on); }
int cos_win2_input(int64_t h, cos_win_input_t *out) { return (int)sys2(SYS_WIN2_INPUT, (long)h, (long)out); }
int cos_win2_wait(int64_t h, cos_win_event_t *ev, uint32_t t) { return (int)sys3(SYS_WIN2_WAIT, (long)h, (long)ev, (long)t); }
int cos_win2_set_title(int64_t h, const char *t) { return (int)sys2(SYS_WIN2_SET_TITLE, (long)h, (long)t); }
int cos_win2_close(int64_t h) { return (int)sys1(SYS_WIN2_CLOSE, (long)h); }
int cos_win2_screen_size(int32_t *w, int32_t *h) {
    int32_t wh[2] = { 0, 0 };
    int r = (int)sys1(SYS_WIN2_SCREEN, (long)wh);
    if (w) *w = wh[0];
    if (h) *h = wh[1];
    return r;
}
#define SYS_WIN2_GET_PATH  187
int cos_win2_get_path(int64_t h, char *buf, size_t cap) { return (int)sys3(SYS_WIN2_GET_PATH, (long)h, (long)buf, (long)cap); }


/* ---- file-manager support syscalls (kernel: cos_fm_support.c) --------------- */
#define SYS_STAT_EX    190
#define SYS_FS_SPACE   191
#define SYS_OPEN_PATH  192
#define SYS_UI_PREFS   193
#define SYS_LISTDIR_EX 194
#define SYS_CLIP_SET   195
#define SYS_CLIP_GET   196
int cos_stat_ex(const char *path, cos_stat_ex_t *out) { PATHFIX(path); return (int)sys2(SYS_STAT_EX, (long)(uintptr_t)path, (long)(uintptr_t)out); }
#define SYS_SET_ATTR 204
#define SYS_FS_GENERATION 205
int cos_set_attr(const char *path, uint8_t attr, uint8_t mask) {
    long packed = (long)attr | ((long)mask << 8);
    return (int)sys2(SYS_SET_ATTR, (long)(uintptr_t)path, packed);
}
uint64_t cos_fs_generation(void) { return (uint64_t)sys0(SYS_FS_GENERATION); }
int cos_listdir_ex(const char *path, cos_dirent_ex_t *out, int max) { return (int)sys3(SYS_LISTDIR_EX, (long)(uintptr_t)path, (long)(uintptr_t)out, (long)max); }
int cos_fs_space(uint64_t *total, uint64_t *free_bytes) {
    uint64_t v[2] = { 0, 0 };
    int r = (int)sys1(SYS_FS_SPACE, (long)(uintptr_t)v);
    if (total) *total = v[0];
    if (free_bytes) *free_bytes = v[1];
    return r;
}
int cos_open_path(const char *path) { return (int)sys1(SYS_OPEN_PATH, (long)(uintptr_t)path); }
uint32_t cos_ui_prefs(void) { return (uint32_t)sys1(SYS_UI_PREFS, 0); }

/* The Japanese font (/system/fonts/NotoSansJP-Regular.ttf), read the first time a program draws a character the built-in
 * Latin font lacks (cos_ui.c asks for it). It does not depend on the language setting or on when the program started, so
 * Japanese text shows up in every program; a missing file is looked for again every two seconds (early in boot it may not be there yet). */
const void *cos_ui_jp_font(void)
{
    static void *font; static uint64_t retry_at;
    if (font) return font;
    uint64_t now = cos_time_ms();
    if (retry_at && now < retry_at) return NULL;
    retry_at = now + 2000;
    cos_stat_t st;
    const char *path = "/system/fonts/NotoSansJP-Regular.ttf";
    if (cos_stat(path, &st) != 0 || st.size < 1000 || st.size > (8u << 20)) return NULL;
    void *b = cos_malloc((size_t)st.size);
    if (!b) return NULL;
    if (cos_read_file(path, b, (size_t)st.size) != (ssize_t)st.size) { cos_free(b); return NULL; }
    font = b;
    return font;
}
int64_t cos_clip_set(const void *data, size_t len) { return (int64_t)sys2(SYS_CLIP_SET, (long)(uintptr_t)data, (long)len); }
int64_t cos_clip_get(void *out, size_t cap) { return (int64_t)sys2(SYS_CLIP_GET, (long)(uintptr_t)out, (long)cap); }
int cos_image_decode(const char *path, void *rgb_pixels, size_t capacity, cos_image_info_t *out) {
    return (int)sys4(SYS_IMAGE_DECODE, (long)(uintptr_t)path, (long)(uintptr_t)rgb_pixels, (long)capacity, (long)(uintptr_t)out);
}
