/* cos_linux.c - Linux x86-64 program support: the loader and the Linux system call layer (docs/LINUX_COMPAT.md).
 *
 * How a call arrives: the program executes `syscall`. EFER.SCE is off on C-OS, so the CPU raises #UD; idt.c passes the frame to cos_linux_try_syscall() when the
 * current process is a Linux process and the faulting bytes are 0F 05. The call (number in rax, arguments in rdi, rsi, rdx, r10, r8, r9) is run against C-OS's own
 * services and the result goes back in rax as a value or -errno; execution resumes after the instruction. The cost is one exception per call, which is fine for
 * a compatibility layer and needs no new entry stub, MSR setup or per-CPU kernel stack.
 *
 * Memory: the program's segments are mapped by the C-OS ELF loader. Everything else (mmap, brk) is tracked in a table of ranges (`lvma`) per process; pages are
 * created on first touch by cos_linux_page_fault(). C-OS user addresses start at COS_ELF_PIE_BASE (below it is the kernel's identity map), so only PIE and
 * static-PIE programs can run; their interpreter, if any, is mapped 64 GiB above them.
 *
 * Files: the descriptors are C-OS descriptors (cos_fd.c) behind a Linux descriptor table. A path is looked up in /linux first (a root file system for the
 * program: /linux/lib/ld-linux-x86-64.so.2, /linux/etc/...), then in the C-OS tree. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "cos_linux.h"
#include "cos_elf.h"
#include "cos_fd.h"
#include "cos_socket.h"
#include "smp.h"
#include "memory_physical.h"
#include "cos_caps.h"
#include "paging.h"
#include "scheduler.h"
#include "serial.h"
#include "sync.h"
#include "memory.h"
#include "hal_api.h"
#include "security/cos_security.h"

extern int cos_fs_read_file(const char *path, void *buffer, uint64_t size);
extern bool cos_fs_stat(const char *path, uint64_t *out_size, uint8_t *out_is_dir);
extern bool cos_fs_mkdir(const char *path);
extern int cos_fs_write_file(const char *path, const void *data, uint64_t size);
extern bool cos_fs_unlink(const char *path);
extern bool cos_fs_rename(const char *oldp, const char *newp);
extern uint64_t cos_random_u64(void);
extern uint64_t rtc_get_seconds_since_epoch(void);
/* Wall-clock time in ms: the RTC gives whole seconds, so it is read once and the uptime clock supplies the rest. Mixing the two on every call made the time jump back by up to a second. */
static uint64_t wall_ms(void) {
    static uint64_t base; static volatile bool set;
    if (!set) { uint64_t b = rtc_get_seconds_since_epoch() * 1000 - hal_timer_get_ms(); base = b; set = true; }
    return base + hal_timer_get_ms();
}
extern void thread_exit(thread_t *t, int code);
extern uint32_t smp_online_cpu_count(void);
#include "cos_futex.h"
void paging_release_user_range(uint64_t virtual_addr, uint64_t count);
extern bool phys_page_is_free(uint64_t addr);
extern uint64_t phys_get_total_memory(void);
extern bool cos_pcache_is_shared(uint64_t phys);

/* ---- Linux ABI constants ------------------------------------------------ */
#define L_EPERM 1
#define L_ENOENT 2
#define L_EBADF 9
#define L_EAGAIN 11
#define L_ENOMEM 12
#define L_EACCES 13
#define L_EFAULT 14
#define L_EEXIST 17
#define L_ENOTDIR 20
#define L_EISDIR 21
#define L_EINVAL 22
#define L_EMFILE 24
#define L_ENOTTY 25
#define L_ESPIPE 29
#define L_ERANGE 34
#define L_ENOSYS 38

#define L_PROT_READ 1
#define L_PROT_WRITE 2
#define L_PROT_EXEC 4
#define L_MAP_SHARED 0x01
#define L_MAP_FIXED 0x10
#define L_MAP_ANON 0x20

#define L_O_ACCMODE 3
#define L_O_WRONLY 1
#define L_O_RDWR 2
#define L_O_CREAT 0x40
#define L_O_TRUNC 0x200
#define L_O_APPEND 0x400
#define L_O_DIRECTORY 0x10000

#define L_AT_FDCWD (-100)

#define PAGE 4096ull
#define LX_MMAP_BASE   0x0000030000000000ull
#define LX_MMAP_LIMIT  0x00007F0000000000ull
#define LX_INTERP_BIAS (COS_ELF_PIE_BASE + 0x1000000000ull)
#define LX_MAX_VMA 65536                  /* a program as big as a browser has thousands of mappings: the table grows on demand */
#define LX_MAX_FD 1024
#define LX_STACK_MAX (8ull << 20)

struct lx_shm;
#define LX_VMA_FILE (1ull << 63)             /* in shm_off of a range that is not shared memory: its pages came from a file (MADV_DONTNEED must leave them alone) */
typedef struct { uint64_t start, end; uint32_t prot; struct lx_shm *shm; uint64_t shm_off; } lvma_t;      /* shm: a range of shared memory (memfd, /dev/shm, MAP_SHARED|MAP_ANONYMOUS), frames shared between processes */
/* An open file description (what dup shares): files and directories are C-OS descriptors (cos_fd.c, owned by `owner`), pipes and eventfds are kernel buffers. */
static int s_trace_all;
static uint32_t s_sigdel[65], s_sigraise[65];     /* signals raised / handled, printed with a crash report */
static uint16_t s_fail[400][134];       /* failures by system call and errno, printed with a crash report */
#define LX_EP_MAX 1024
#define LX_MAX_ANC 16
#define LX_ANC_FDS 16
/* A pipe is also one direction of a unix socket: `packet` frames every message with a 4-byte length (SOCK_DGRAM / SOCK_SEQPACKET), and `anc` holds file descriptors in flight (SCM_RIGHTS), each tagged with the stream offset of the data it travels with. */
typedef struct pipe { uint8_t *buf; uint32_t cap, len, head; int readers, writers; uint8_t packet; uint64_t wtotal, rtotal; int nanc; struct { uint64_t at; int n; struct ofd *f[LX_ANC_FDS]; } anc[LX_MAX_ANC]; } pipe_t;
struct ulis;
typedef struct ofd {
    int ref; uint8_t kind; int64_t cosfd; uint64_t owner; uint64_t pos; uint32_t flags; char path[256];
    uint8_t *mem; uint32_t memlen;       /* FD_MEM: a /proc file, generated when opened */
    struct lx_shm *shm;                  /* FD_MEMFD */
    struct ulis *lis; uint8_t ushut;     /* FD_UPAIR: a listening unix socket; bit 1 = write side shut down, 2 = read side */
    pipe_t *pipe, *pipe2; uint64_t counter, ver; uint32_t efd_flags; uint8_t dead;   /* pipe2: the write side of a socketpair end */
    struct epent { int fd; uint32_t ev; uint64_t data; uint32_t last; uint64_t lastver; } *ep; int nep;     /* ep: allocated when the first descriptor is added (up to LX_EP_MAX) */      /* an epoll instance: what it watches */
} ofd_t;
enum { FD_NONE = 0, FD_CON, FD_FILE, FD_DIR, FD_PIPE_R, FD_PIPE_W, FD_EVENT, FD_EPOLL, FD_SOCK, FD_UPAIR, FD_DEV, FD_MEM, FD_VDIR, FD_MEMFD };   /* FD_VDIR: a listing of /dev or /proc (mem = records "<type><name>\0", pos = offset) */   /* FD_DEV: /dev/null, zero, urandom, tty (counter = which) */      /* FD_SOCK: IPv4 TCP/UDP (cosfd = cos_socket descriptor of `owner`, counter = type); FD_UPAIR: one end of a socketpair */

typedef struct {
    lvma_t *vma; int nvma, vma_cap;               /* sorted by address, never overlapping; adjacent ranges of one protection are merged */
    uint64_t mmap_next;
    uint64_t brk_start, brk_cur, brk_max;
    ofd_t *fd[LX_MAX_FD];
    uint8_t cloexec[LX_MAX_FD];
    int64_t out_fd;                 /* COS_STDOUT redirect (a C-OS descriptor) or -1 */
    uint64_t out_pid;               /* ... and the process that opened it: a whole tree of processes writes through that one handle, so their output does not overwrite each other */
    char cwd[256];
    struct { uint64_t tid, clear_tid; } th[256];   /* the threads of the process: what a thread reads and writes when it ends (CLONE_CHILD_CLEARTID) */
    int nth;
    uint64_t fs_base;
    struct { uint64_t handler, flags, restorer, mask; } sig[65];   /* rt_sigaction */
    uint64_t sig_pending, sig_blocked;
    uint64_t alarm_ms;                                             /* alarm(): when SIGALRM is due (monotonic ms), 0 = none */
    volatile int forking;                                          /* a fork is copying the address space: no file mapping may start being filled */
    volatile int mmap_busy;                                        /* threads inside a file mapping that is still being filled: a fork must not copy it half loaded */
    char out_path[256];                                            /* the COS_STDOUT file (reopened by a forked child) */
    char exe[256];
    uint32_t fpid;                                                 /* whose file-mapping records apply (a forked child uses its parent's) */
} lctx_t;
#define LX_SIG_DFL 0ull
#define LX_SIG_IGN 1ull

static inline lctx_t *ctx_of(process_t *p) { return (lctx_t *)p->linux_ctx; }
static inline uint64_t pg_down(uint64_t v) { return v & ~(PAGE - 1); }
static inline uint64_t pg_up(uint64_t v) { return (v + PAGE - 1) & ~(PAGE - 1); }

/* ---- user memory --------------------------------------------------------- */

static bool ensure_user(uint64_t addr, uint64_t len, bool write) {
    if (len == 0) return true;
    if (addr + len < addr || addr < COS_ELF_PIE_BASE || addr + len > 0x0000800000000000ull) return false;
    for (uint64_t a = pg_down(addr); a < addr + len; a += PAGE) {
        if (!paging_virt_to_phys(a)) {
            if (!task_handle_page_fault(a, 0x4u | (write ? 0x2u : 0))) return false;
            if (!paging_virt_to_phys(a)) return false;
        } else if (write && !paging_user_range_ok(a, 1, true)) task_handle_page_fault(a, 0x7u);       /* a page shared with a forked process: copy it first */
    }
    return paging_user_range_ok(addr, len, write);
}
static bool copy_in(void *dst, uint64_t uaddr, uint64_t len) {
    if (!ensure_user(uaddr, len, false)) return false;
    cos_user_access_begin(); memcpy(dst, (const void *)(uintptr_t)uaddr, len); cos_user_access_end(); return true;
}
static bool copy_out(uint64_t uaddr, const void *src, uint64_t len) {
    if (!ensure_user(uaddr, len, true)) return false;
    cos_user_access_begin(); memcpy((void *)(uintptr_t)uaddr, src, len); cos_user_access_end(); return true;
}
static bool copy_str(char *dst, uint64_t uaddr, size_t cap) {
    for (size_t i = 0; i < cap; ++i) {
        char c;
        if (!copy_in(&c, uaddr + i, 1)) return false;
        dst[i] = c;
        if (!c) return true;
    }
    return false;
}

/* ---- ranges -------------------------------------------------------------- */

static uint64_t page_flags(uint32_t prot) {
    uint64_t f = PAGE_PRESENT | PAGE_USER;
    if (prot & L_PROT_WRITE) f |= PAGE_RW;
    if (!(prot & L_PROT_EXEC) && cos_elf_nx_available()) f |= PAGE_NX;
    return f;
}

/* ---- shared memory objects: memfd_create, /dev/shm, MAP_SHARED anonymous ----------------------------------------------------------------------
 * The pages of an object are physical frames made on first touch and mapped into every process that maps it (so the processes really share them); the object
 * is freed when no descriptor and no mapping holds it. */
typedef struct lx_shm { int ref; uint64_t size, npages; uint64_t *frame; char name[64]; int named; struct lx_shm *next; } lx_shm_t;
static lx_shm_t *s_shm_named; static volatile uint32_t s_shm_lock;
static lx_shm_t *shm_new(const char *name) {
    lx_shm_t *o = (lx_shm_t *)kmalloc(sizeof *o); if (!o) return NULL;
    memset(o, 0, sizeof *o); o->ref = 1; if (name) { snprintf(o->name, sizeof o->name, "%s", name); }
    return o;
}
static void shm_ref(lx_shm_t *o) { if (o) { uint64_t fl = sync_irq_save(); o->ref++; sync_irq_restore(fl); } }
static void shm_unref(lx_shm_t *o) {
    if (!o) return;
    uint64_t fl = sync_irq_save(); int last = --o->ref == 0;
    sync_irq_restore(fl);
    if (!last) return;
    for (uint64_t i = 0; i < o->npages; ++i) if (o->frame[i]) paging_free_physical(o->frame[i]);
    if (o->frame) kfree(o->frame);
    kfree(o);
}
static bool shm_resize(lx_shm_t *o, uint64_t size) {
    uint64_t np = (size + PAGE - 1) / PAGE; uint64_t fl = sync_irq_save();                  /* page faults of other processes read frame[] under the same lock */
    if (np > o->npages) {
        uint64_t *nf = (uint64_t *)kmalloc((np ? np : 1) * sizeof(uint64_t)); if (!nf) { sync_irq_restore(fl); return false; }
        memset(nf, 0, np * sizeof(uint64_t)); if (o->frame) { memcpy(nf, o->frame, o->npages * sizeof(uint64_t)); kfree(o->frame); }
        o->frame = nf; o->npages = np;
    } else if (np < o->npages) { for (uint64_t i = np; i < o->npages; ++i) if (o->frame[i]) { paging_free_physical(o->frame[i]); o->frame[i] = 0; } o->npages = np; }
    o->size = size; sync_irq_restore(fl); return true;
}
static uint64_t shm_frame(lx_shm_t *o, uint64_t idx) {
    uint64_t fl = sync_irq_save();
    if (idx >= o->npages) { sync_irq_restore(fl); return 0; }
    uint64_t f = o->frame[idx];
    if (!f) { f = paging_alloc_physical(); if (f) { memset((void *)(uintptr_t)PHYS_TO_VIRT(f), 0, PAGE); o->frame[idx] = f; } }
    sync_irq_restore(fl);
    return f;
}

/* index of the first range that ends above `addr` (binary search) */
static int vma_lower(const lctx_t *c, uint64_t addr) {
    int lo = 0, hi = c->nvma;
    while (lo < hi) { int mid = (lo + hi) / 2; if (c->vma[mid].end <= addr) lo = mid + 1; else hi = mid; }
    return lo;
}
static lvma_t *vma_find(lctx_t *c, uint64_t addr) {
    int i = vma_lower(c, addr);
    return i < c->nvma && addr >= c->vma[i].start ? &c->vma[i] : NULL;
}
static bool vma_free(lctx_t *c, uint64_t s, uint64_t e) {
    int i = vma_lower(c, s);
    return !(i < c->nvma && c->vma[i].start < e);
}
static bool vma_reserve(lctx_t *c, int extra) {
    if (c->nvma + extra <= c->vma_cap) return true;
    if (c->nvma + extra > LX_MAX_VMA) return false;
    int ncap = c->vma_cap ? c->vma_cap * 2 : 128; while (ncap < c->nvma + extra) ncap *= 2;
    if (ncap > LX_MAX_VMA) ncap = LX_MAX_VMA;
    lvma_t *n = (lvma_t *)kmalloc((size_t)ncap * sizeof(lvma_t)); if (!n) return false;
    if (c->vma) { memcpy(n, c->vma, (size_t)c->nvma * sizeof(lvma_t)); kfree(c->vma); }
    c->vma = n; c->vma_cap = ncap; return true;
}
static void vma_remove_slot(lctx_t *c, int i) { if (c->vma[i].shm) shm_unref(c->vma[i].shm); memmove(&c->vma[i], &c->vma[i + 1], (size_t)(c->nvma - i - 1) * sizeof(lvma_t)); c->nvma--; }
static void vma_insert_slot(lctx_t *c, int i, lvma_t v) { memmove(&c->vma[i + 1], &c->vma[i], (size_t)(c->nvma - i) * sizeof(lvma_t)); c->vma[i] = v; c->nvma++; }
/* joins the range at i with its neighbours when they touch and have the same protection */
static void vma_merge_at(lctx_t *c, int i) {
    if (i + 1 < c->nvma && c->vma[i].end == c->vma[i + 1].start && c->vma[i].prot == c->vma[i + 1].prot && !c->vma[i].shm && !c->vma[i + 1].shm && !((c->vma[i].shm_off ^ c->vma[i + 1].shm_off) & LX_VMA_FILE)) { c->vma[i].end = c->vma[i + 1].end; vma_remove_slot(c, i + 1); }
    if (i > 0 && c->vma[i - 1].end == c->vma[i].start && c->vma[i - 1].prot == c->vma[i].prot && !c->vma[i - 1].shm && !c->vma[i].shm && !((c->vma[i - 1].shm_off ^ c->vma[i].shm_off) & LX_VMA_FILE)) { c->vma[i - 1].end = c->vma[i].end; vma_remove_slot(c, i); }
}
static bool vma_insert_f(lctx_t *c, uint64_t s, uint64_t e, uint32_t prot, bool file) {
    if (!vma_reserve(c, 2)) return false;
    int i = vma_lower(c, s);
    if (i < c->nvma && c->vma[i].start < e) return false;                          /* overlaps something */
    vma_insert_slot(c, i, (lvma_t){ s, e, prot, NULL, file ? LX_VMA_FILE : 0 });
    vma_merge_at(c, i);
    return true;
}
static bool vma_insert(lctx_t *c, uint64_t s, uint64_t e, uint32_t prot) { return vma_insert_f(c, s, e, prot, false); }
static bool vma_insert_shm(lctx_t *c, uint64_t s, uint64_t e, uint32_t prot, lx_shm_t *shm, uint64_t off) {
    if (!vma_reserve(c, 2)) return false;
    int i = vma_lower(c, s);
    if (i < c->nvma && c->vma[i].start < e) return false;
    shm_ref(shm); vma_insert_slot(c, i, (lvma_t){ s, e, prot, shm, off });
    return true;
}
/* cuts [s,e) out of the table (splitting what straddles it) and returns the pages */
static void vma_cut(lctx_t *c, uint64_t s, uint64_t e, bool release) {
    vma_reserve(c, 2);
    for (int i = vma_lower(c, s); i < c->nvma && c->vma[i].start < e; ) {
        lvma_t *v = &c->vma[i];
        uint64_t cs = s > v->start ? s : v->start, ce = e < v->end ? e : v->end;
        if (release) {
            if (v->shm) { for (uint64_t a = cs; a < ce; a += PAGE) if (paging_virt_to_phys(a)) paging_unmap_page(a); }          /* shared frames are only unmapped */
            else paging_release_user_range(cs, (ce - cs) / PAGE);
        }
        if (cs > v->start && ce < v->end) {
            lvma_t tail = { ce, v->end, v->prot, v->shm, v->shm_off + (ce - v->start) };
            v->end = cs;
            if (c->nvma < c->vma_cap) { if (tail.shm) shm_ref(tail.shm); vma_insert_slot(c, i + 1, tail); }
            i += 2;
        } else if (cs > v->start) { v->end = cs; ++i; }
        else if (ce < v->end) { v->shm_off += ce - v->start; v->start = ce; ++i; }
        else vma_remove_slot(c, i);
    }
}
static void vma_protect(lctx_t *c, uint64_t s, uint64_t e, uint32_t prot) {
    vma_reserve(c, 4);
    /* split at the borders, then retag the pieces inside */
    for (int pass = 0; pass < 2; ++pass) {
        uint64_t at = pass ? e : s;
        int i = vma_lower(c, at);
        if (i < c->nvma && at > c->vma[i].start && at < c->vma[i].end && c->nvma < c->vma_cap) { lvma_t tail = { at, c->vma[i].end, c->vma[i].prot, c->vma[i].shm, c->vma[i].shm_off + (at - c->vma[i].start) }; c->vma[i].end = at; if (tail.shm) shm_ref(tail.shm); vma_insert_slot(c, i + 1, tail); }
    }
    int first = vma_lower(c, s), last = first;
    for (int i = first; i < c->nvma && c->vma[i].start < e; ++i) {
        lvma_t *v = &c->vma[i];
        if (v->start >= s && v->end <= e) {
            v->prot = prot; last = i;
            for (uint64_t a = v->start; a < v->end; a += PAGE) if (paging_virt_to_phys(a)) { uint64_t fl = page_flags(prot); if ((fl & PAGE_RW) && paging_cow_is_shared(paging_virt_to_phys(a) & ~(PAGE - 1))) fl &= ~(uint64_t)PAGE_RW; paging_protect_page(a, fl); }
        }
    }
    /* merge the retagged pieces with what is next to them */
    for (int i = last < c->nvma ? last : c->nvma - 1; i >= first && i >= 0; --i) { if (i < c->nvma) vma_merge_at(c, i); }
    if (first > 0) vma_merge_at(c, first - 1);
}

static void trace_dump(void);
/* the file mappings made (all processes), to tell where a crash is: the library and the offset in it */
typedef struct { uint32_t pid; uint64_t start, end, off; char name[72]; } lx_fmap_t;
#define LX_FMAP_N 16384                                         /* the log is a ring: a browser maps thousands of files, the libraries come first and must not be forgotten */
static lx_fmap_t s_fmap[LX_FMAP_N]; static uint32_t s_fmap_n;
static void fmap_note(uint32_t pid, uint64_t start, uint64_t end, uint64_t off, const char *path) {
    lx_fmap_t *m = &s_fmap[s_fmap_n++ & (LX_FMAP_N - 1)]; m->pid = pid; m->start = start; m->end = end; m->off = off;
    size_t n = strlen(path); snprintf(m->name, sizeof m->name, "%s", n > 70 ? path + n - 70 : path);
}
static bool fmap_has(uint32_t pid, uint64_t addr) {
    for (uint32_t k = 0; k < LX_FMAP_N && k < s_fmap_n; ++k) { const lx_fmap_t *m = &s_fmap[(s_fmap_n - 1 - k) & (LX_FMAP_N - 1)]; if (m->pid == pid && addr >= m->start && addr < m->end) return true; }
    return false;
}
static void fmap_where(const char *what, uint32_t pid, uint64_t addr) {
    for (uint32_t k = 0; k < LX_FMAP_N && k < s_fmap_n; ++k) {
        const lx_fmap_t *m = &s_fmap[(s_fmap_n - 1 - k) & (LX_FMAP_N - 1)];
        if (m->pid == pid && addr >= m->start && addr < m->end) {
            serial_puts("[LINUX] "); serial_puts(what); serial_puts(" 0x"); serial_puthex(addr); serial_puts(" is in "); serial_puts(m->name);
            serial_puts(" at file offset 0x"); serial_puthex(m->off + (addr - m->start)); serial_puts(" (mapped at 0x"); serial_puthex(m->start); serial_puts(")\n"); return;
        }
    }
    serial_puts("[LINUX] "); serial_puts(what); serial_puts(" 0x"); serial_puthex(addr); serial_puts(" is not in a file mapping\n");
}

/* A crash report check: the newest file mappings of this program, compared page by page with the file they came from. A page that reads as zeros where the file has
 * bytes (a segment that was never filled) or that differs in a read-only mapping is what a dynamic loader trips over. */
static void crash_verify_maps(process_t *proc, uint32_t fp) {
    static uint8_t fb[PAGE], mb[PAGE];
    uint32_t shown = 0;
    for (uint32_t k = 0; k < LX_FMAP_N && k < s_fmap_n && shown < 10; ++k) {
        const lx_fmap_t *m = &s_fmap[(s_fmap_n - 1 - k) & (LX_FMAP_N - 1)];
        if (m->pid != fp || !strstr(m->name, ".so")) continue;
        shown++;
        const uint64_t vown = 0xFFFFF001ull;                  /* a table of its own: the program's 16 slots may all be taken */
        int64_t h = cos_fd_open(vown, m->name, COS_O_RDONLY); if (h < 0) { serial_puts("[VERIFY] cannot open "); serial_puts(m->name); serial_puts("\n"); continue; }
        uint64_t pages = (m->end - m->start) / PAGE, stride = pages > 1500 ? pages / 1500 : 1, same = 0, zero = 0, diff = 0, absent = 0, firstz = 0, firstd = 0;
        for (uint64_t i = 0; i < pages; i += stride) {
            uint64_t a = m->start + i * PAGE;
            if (!paging_virt_to_phys(a)) { absent++; continue; }
            memset(fb, 0, PAGE); int64_t n = cos_fd_pread(vown, h, (int64_t)(m->off + i * PAGE), fb, PAGE); if (n <= 0) continue;
            if (!copy_in(mb, a, PAGE)) { absent++; continue; }
            if (!memcmp(fb, mb, PAGE)) { same++; continue; }
            bool allz = true; for (int q = 0; q < PAGE; ++q) if (mb[q]) { allz = false; break; }
            if (allz) { if (!zero++) firstz = a; } else { if (!diff++) firstd = a; }
        }
        if (shown <= 1 || strstr(m->name, "libxul")) {
            /* the same record at page granularity (up to 12000 pages): the runs of equal / zero / different pages, and the VMAs that cover it */
            int64_t h2 = cos_fd_open(vown, m->name, COS_O_RDONLY);
            if (h2 >= 0) {
                char prev = 0; uint64_t runstart = 0, runs = 0, lim = pages < 12000 ? pages : 12000;
                for (uint64_t i = 0; i <= lim; ++i) {
                    char st = 'x';
                    if (i < lim) {
                        uint64_t a = m->start + i * PAGE;
                        if (!paging_virt_to_phys(a)) st = 'A';
                        else { memset(fb, 0, PAGE); int64_t n = cos_fd_pread(vown, h2, (int64_t)(m->off + i * PAGE), fb, PAGE);
                               if (n <= 0 || !copy_in(mb, a, PAGE)) st = '?'; else if (!memcmp(fb, mb, PAGE)) st = 'S'; else { bool z = true; for (int q = 0; q < PAGE; q += 8) if (*(uint64_t *)(mb + q)) { z = false; break; } st = z ? 'Z' : 'D'; } }
                    }
                    if (st != prev) { if (prev && runs < 30) { serial_puts("[RUNS]   "); serial_puthex(m->start + runstart * PAGE); serial_puts(" +"); serial_putdec(i - runstart); serial_puts(" pages "); serial_putc(prev); serial_puts("\n"); } if (prev) runs++; prev = st; runstart = i; }
                }
                cos_fd_close(vown, h2);
                lctx_t *cx = ctx_of(proc);
                for (int i = 0; i < cx->nvma; ++i) { const lvma_t *v = &cx->vma[i]; if (v->end > m->start && v->start < m->end) { serial_puts("[RUNS]   vma 0x"); serial_puthex(v->start); serial_puts("-0x"); serial_puthex(v->end); serial_puts(" prot "); serial_putdec(v->prot); serial_puts(v->shm ? " shm" : (v->shm_off & LX_VMA_FILE) ? " file" : " anon"); serial_puts("\n"); } }
            }
        }
        cos_fd_close(vown, h);
        serial_puts("[VERIFY] "); serial_puts(m->name); serial_puts(" 0x"); serial_puthex(m->start); serial_puts("-0x"); serial_puthex(m->end); serial_puts(" file off 0x"); serial_puthex(m->off);
        serial_puts(": same "); serial_putdec(same); serial_puts(" zero "); serial_putdec(zero); serial_puts(" different "); serial_putdec(diff); serial_puts(" absent "); serial_putdec(absent);
        if (zero) { serial_puts(" first zero page 0x"); serial_puthex(firstz); } if (diff) { serial_puts(" first different 0x"); serial_puthex(firstd); } serial_puts("\n");
    }
}
static void crash_threads(process_t *proc);
void cos_linux_crash_report(process_t *proc, uint64_t rip, uint64_t addr) {
    if (!proc || !proc->linux_ctx) return;
    trace_dump();
    serial_puts("[LINUX] signals raised/handled:"); for (int sg = 1; sg <= 64; ++sg) if (s_sigraise[sg] || s_sigdel[sg]) { serial_puts(" "); serial_putdec((uint64_t)sg); serial_puts("="); serial_putdec(s_sigraise[sg]); serial_puts("/"); serial_putdec(s_sigdel[sg]); } serial_puts("\n");
    serial_puts("[LINUX] failed calls (nr:errno=count):");
    for (int n = 0; n < 400; ++n) for (int e = 1; e < 134; ++e) if (s_fail[n][e] && !(n == 202 && (e == 110 || e == 11)) && !(n == 232) ) { serial_puts(" "); serial_putdec((uint64_t)n); serial_puts(":"); serial_putdec((uint64_t)e); serial_puts("="); serial_putdec(s_fail[n][e]); }
    serial_puts("\n");
    uint32_t fp = ctx_of(proc)->fpid ? ctx_of(proc)->fpid : (uint32_t)proc->pid;
    { lctx_t *cx = ctx_of(proc); lvma_t *v = vma_find(cx, rip); serial_puts("[LINUX] pid "); serial_putdec(proc->pid); serial_puts(" fpid "); serial_putdec(fp);
      if (v) { serial_puts(" rip vma 0x"); serial_puthex(v->start); serial_puts("-0x"); serial_puthex(v->end); serial_puts(" prot "); serial_putdec(v->prot); }
      uint8_t ib[16]; if (copy_in(ib, rip, 16)) { serial_puts(" code:"); for (int i = 0; i < 16; ++i) { serial_puts(" "); serial_puthex(ib[i]); } } serial_puts("\n"); }
    { lctx_t *cx = ctx_of(proc); size_t el = strlen(cx->exe);          /* Firefox's MOZ_CRASH leaves its reason in the exported pointer gMozCrashReason (offset in the Ubuntu 20.04 build, 136.0) */
      if (el >= 7 && !strcmp(cx->exe + el - 7, "firefox") && cx->nvma) {
          uint64_t base = 0; for (int i = 0; i < cx->nvma; ++i) if (cx->vma[i].start >= COS_ELF_PIE_BASE && cx->vma[i].start < COS_ELF_PIE_BASE + 0x1000000000ull) { base = cx->vma[i].start; break; }
          uint64_t ptr = 0; char why[160]; why[0] = 0;
          serial_puts("[LINUX] exe base 0x"); serial_puthex(base); { bool okp = base && copy_in(&ptr, base + 0xa9208, 8); serial_puts(" reason ptr 0x"); serial_puthex(ptr); serial_puts(okp ? "\n" : " (unreadable)\n"); }
          if (base && copy_in(&ptr, base + 0xa9208, 8) && ptr) { { size_t k = 0; for (; k < sizeof why - 1; ++k) { char ch; if (!copy_in(&ch, ptr + k, 1)) break; why[k] = ch; if (!ch) break; } why[k] = 0; } serial_puts("[LINUX] MOZ_CRASH reason: "); serial_puts(why); serial_puts("\n"); }
      } }
    { extern struct regs *page_fault_regs; const struct regs *rg = page_fault_regs;           /* a poor man's backtrace: stack words that point into executable file mappings */
      if (rg && rg->rip == rip) {
          serial_puts("[LINUX] regs rax=0x"); serial_puthex(rg->rax); serial_puts(" rbx=0x"); serial_puthex(rg->rbx); serial_puts(" rcx=0x"); serial_puthex(rg->rcx); serial_puts(" rdx=0x"); serial_puthex(rg->rdx);
          serial_puts(" rsi=0x"); serial_puthex(rg->rsi); serial_puts(" rdi=0x"); serial_puthex(rg->rdi); serial_puts(" rbp=0x"); serial_puthex(rg->rbp); serial_puts(" r12=0x"); serial_puthex(rg->r12); serial_puts(" r13=0x"); serial_puthex(rg->r13); serial_puts(" r14=0x"); serial_puthex(rg->r14); serial_puts(" r15=0x"); serial_puthex(rg->r15); serial_puts(" rsp=0x"); serial_puthex(rg->rsp); serial_puts("\n");
          lctx_t *cx = ctx_of(proc); int shown = 0;
          for (uint64_t off = 0; off < 4096 && shown < 40; off += 8) {
              uint64_t w; if (!copy_in(&w, rg->rsp + off, 8)) break;
              lvma_t *v = vma_find(cx, w); if (!v || !(v->prot & L_PROT_EXEC)) continue;
              for (uint32_t k = 0; k < LX_FMAP_N && k < s_fmap_n; ++k) { const lx_fmap_t *m = &s_fmap[(s_fmap_n - 1 - k) & (LX_FMAP_N - 1)]; if (m->pid == fp && w >= m->start && w < m->end) { serial_puts("[LINUX]   bt "); serial_puts(m->name); serial_puts(" +0x"); serial_puthex(m->off + (w - m->start)); serial_puts("\n"); shown++; break; } }
          }
      } }
    { /* frame audit of the dying process: a frame mapped twice by its private memory, or a mapped frame the allocator calls free, is how memory gets corrupted */
      lctx_t *cx = ctx_of(proc); uint64_t total = phys_get_total_memory() + (2ull << 30); uint64_t words = (total / PAGE + 63) / 64;
      uint64_t *seen = (uint64_t *)kmalloc(words * 8); uint64_t *where = NULL; int dup = 0, freed = 0; uint64_t npages = 0;
      if (seen) { memset(seen, 0, words * 8);
        for (int i = 0; i < cx->nvma; ++i) { const lvma_t *v = &cx->vma[i]; if (v->shm) continue;
            for (uint64_t a = v->start; a < v->end; a += PAGE) { uint64_t ph = paging_virt_to_phys(a); if (!ph) continue; ph &= ~(PAGE - 1); npages++; uint64_t pf = ph / PAGE;
                if (pf / 64 < words) { if (seen[pf / 64] >> (pf % 64) & 1) { if (dup++ < 8) { serial_puts("[AUDIT] frame 0x"); serial_puthex(ph); serial_puts(" is mapped again at 0x"); serial_puthex(a); serial_puts(" (vma 0x"); serial_puthex(v->start); serial_puts("-0x"); serial_puthex(v->end); serial_puts(" prot "); serial_putdec(v->prot); serial_puts(")\n"); } } else seen[pf / 64] |= 1ull << (pf % 64); }
                if (phys_page_is_free(ph) && freed++ < 8) { serial_puts("[AUDIT] free frame 0x"); serial_puthex(ph); serial_puts(" mapped at 0x"); serial_puthex(a); serial_puts("\n"); } } }
        serial_puts("[AUDIT] "); serial_putdec(npages); serial_puts(" private pages, "); serial_putdec((uint64_t)dup); serial_puts(" double mappings, "); serial_putdec((uint64_t)freed); serial_puts(" free frames mapped\n"); kfree(seen); }
      (void)where;
      /* ... and across processes: a private frame (not shared by fork or the pcache) that another process maps too */
      { uint64_t *own = (uint64_t *)kmalloc(words * 8); uint8_t *who = (uint8_t *)kmalloc(total / PAGE + 1);
        if (own && who) { memset(own, 0, words * 8); memset(who, 0, total / PAGE + 1); int cross = 0;
          uint64_t irq = sync_irq_save(); page_directory_t *saved = paging_get_current_directory();
          for (int pi = 1; pi < 1024 && cross < 8; ++pi) { process_t *q = process_get_by_pid((uint64_t)pi); if (!q || q->state == TASK_UNUSED || q->state == TASK_ZOMBIE || !q->linux_ctx || !q->page_dir) continue;
            lctx_t *qc = ctx_of(q); paging_switch_directory((page_directory_t *)q->page_dir);
            for (int i = 0; i < qc->nvma && cross < 8; ++i) { const lvma_t *v = &qc->vma[i]; if (v->shm) continue;
              for (uint64_t a = v->start; a < v->end && cross < 8; a += PAGE) { uint64_t ph = paging_virt_to_phys(a); if (!ph) continue; ph &= ~(PAGE - 1); uint64_t pf = ph / PAGE; if (pf / 64 >= words) continue;
                if (own[pf / 64] >> (pf % 64) & 1) { if (who[pf] != (uint8_t)q->pid && !paging_cow_is_shared(ph) && !cos_pcache_is_shared(ph)) { cross++; serial_puts("[AUDIT] frame 0x"); serial_puthex(ph); serial_puts(" is private memory of pid "); serial_putdec(who[pf]); serial_puts(" and of pid "); serial_putdec(q->pid); serial_puts(" at 0x"); serial_puthex(a); serial_puts("\n"); } }
                else { own[pf / 64] |= 1ull << (pf % 64); who[pf] = (uint8_t)q->pid; } } } }
          paging_switch_directory(saved); sync_irq_restore(irq); serial_puts("[AUDIT] cross-process check: "); serial_putdec((uint64_t)cross); serial_puts(" shared private frames\n"); }
        if (own) kfree(own); if (who) kfree(who); }
    }
    crash_threads(proc);
    crash_verify_maps(proc, fp);
    fmap_where("rip", fp, rip);
    if (addr != rip) fmap_where("fault address", fp, addr);
    { lctx_t *cx = ctx_of(proc); lvma_t *va = vma_find(cx, addr); serial_puts("[LINUX] fault address vma: ");
      if (va) { serial_puts("0x"); serial_puthex(va->start); serial_puts("-0x"); serial_puthex(va->end); serial_puts(" prot "); serial_putdec(va->prot); serial_puts(va->shm ? " shm" : " anon/file"); } else serial_puts("none");
      serial_puts(" frame 0x"); serial_puthex(paging_virt_to_phys(addr & ~(PAGE - 1))); serial_puts(" writable "); serial_putdec(paging_user_range_ok(addr, 1, true) ? 1 : 0); serial_puts(" cow-shared "); serial_putdec(paging_cow_is_shared(paging_virt_to_phys(addr & ~(PAGE - 1)) & ~(PAGE - 1)) ? 1 : 0); serial_puts("\n"); }
}
/* a write to a page that fork() left shared and read-only */
bool cos_linux_cow_fault(process_t *proc, uint64_t page) {
    if (!proc || !proc->linux_ctx) return false;
    lctx_t *c = ctx_of(proc); uint32_t prot = 0;
    if (c->brk_start && page >= c->brk_start && page < pg_up(c->brk_cur)) prot = L_PROT_READ | L_PROT_WRITE;
    else if (proc->stack_end && page >= proc->stack_start && page < proc->stack_end) prot = L_PROT_READ | L_PROT_WRITE;
    else { lvma_t *v = vma_find(c, page); if (!v || v->shm) return false; prot = v->prot; }
    if (!(prot & L_PROT_WRITE)) return false;
    return paging_cow_break(page, page_flags(prot));
}
bool cos_linux_page_fault(process_t *proc, uint64_t page) {
    if (!proc || !proc->linux_ctx) return false;
    lctx_t *c = ctx_of(proc);
    if (c->brk_start && page >= c->brk_start && page < pg_up(c->brk_cur)) return task_alloc_page(proc, page, page_flags(L_PROT_READ | L_PROT_WRITE));
    /* the main stack: grows down from stack_end by up to 8 MiB (RLIMIT_STACK); the pages between are mapped on touch */
    if (proc->stack_end && page < proc->stack_end && page >= proc->stack_end - LX_STACK_MAX) {
        uint64_t from = page < proc->stack_start ? page : proc->stack_start;
        if (page < proc->stack_start) {
            for (uint64_t pg = page; pg < proc->stack_start; pg += PAGE) if (!task_alloc_page(proc, pg, page_flags(L_PROT_READ | L_PROT_WRITE))) return false;
            proc->stack_start = from; return true;
        }
        return task_alloc_page(proc, page, page_flags(L_PROT_READ | L_PROT_WRITE));
    }
    lvma_t *v = vma_find(c, page);
    if (!v || v->prot == 0) return false;
    if (v->shm) {                                                                   /* shared memory: map the object's frame (made on the first touch by anyone) */
        uint64_t f = shm_frame(v->shm, (v->shm_off + (page - v->start)) / PAGE); if (!f) return false;
        return paging_map_page(page, f, page_flags(v->prot));
    }
    return task_alloc_page(proc, page, page_flags(v->prot));
}

/* ---- paths and descriptors ----------------------------------------------- */

static void path_normalize(const char *in, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (const char *s = in; *s; ) {
        while (*s == '/') s++;
        const char *e = s; while (*e && *e != '/') e++;
        size_t l = (size_t)(e - s);
        if (l == 0) break;
        if (l == 1 && s[0] == '.') { }
        else if (l == 2 && s[0] == '.' && s[1] == '.') { while (o > 0 && out[o - 1] != '/') o--; if (o > 0) o--; out[o] = 0; }
        else if (o + l + 2 < cap) { out[o++] = '/'; memcpy(out + o, s, l); o += l; out[o] = 0; }
        s = e;
    }
    if (!o) { out[0] = '/'; out[1] = 0; }
}
/* ---- the manifest of the root file system: symbolic links and executable bits ------------------------------------------------------------
 * The root file system of a Linux program lives on the FAT volume (/linux/...), which has neither symbolic links nor permission bits. /linux/.manifest (made by
 * tools/linux_rootfs_disk.py) lists them: "L<TAB>path<TAB>target" and "X<TAB>path" with the paths as the program sees them (without /linux). It is read once,
 * on the first use, into a hash table; resolve() follows the links, stat() reports the executable bit, readlink() answers. */
typedef struct { uint64_t h; uint32_t path_off, target_off; uint8_t kind; } lxm_ent_t;       /* kind 1: link, 2: executable */
static lxm_ent_t *lxm_tab; static uint32_t lxm_cap; static char *lxm_str; static int lxm_state;   /* 0 not read, 1 read (maybe empty), 2 reading */
static uint64_t lxm_hash(const char *s, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; ++i) h = (h ^ (uint8_t)s[i]) * 1099511628211ull; return h | 1; }
static void lxm_load(void) {
    if (lxm_state) return;
    lxm_state = 2;
    uint64_t size = 0; uint8_t isdir = 0;
    if (cos_fs_stat("/linux/.manifest", &size, &isdir) && !isdir && size > 0 && size < (64u << 20)) {
        char *buf = (char *)kmalloc(size + 2);
        if (buf && cos_fs_read_file("/linux/.manifest", buf, size) == (int)size) {
            buf[size] = 0;
            uint32_t lines = 0; for (uint64_t i = 0; i < size; ++i) if (buf[i] == '\n') lines++;
            uint32_t cap = 1024; while (cap < lines * 2 + 16) cap <<= 1;
            lxm_tab = (lxm_ent_t *)kmalloc((size_t)cap * sizeof(lxm_ent_t)); lxm_str = buf;
            if (lxm_tab) {
                memset(lxm_tab, 0, (size_t)cap * sizeof(lxm_ent_t)); lxm_cap = cap;
                char *q = buf;
                while (*q) {
                    char *nl = q; while (*nl && *nl != '\n') nl++;
                    char *end = nl; if (*nl) *nl++ = 0;
                    if ((q[0] == 'L' || q[0] == 'X') && q[1] == '\t') {
                        char *path = q + 2, *tab = path; while (*tab && *tab != '\t') tab++;
                        char *target = NULL; if (*tab == '\t') { *tab = 0; target = tab + 1; }
                        size_t pl = strlen(path);
                        if (pl && (q[0] == 'X' || target)) {
                            uint64_t h = lxm_hash(path, pl); uint32_t i = (uint32_t)h & (cap - 1);
                            while (lxm_tab[i].h) i = (i + 1) & (cap - 1);
                            lxm_tab[i].h = h; lxm_tab[i].path_off = (uint32_t)(path - buf); lxm_tab[i].target_off = target ? (uint32_t)(target - buf) : 0; lxm_tab[i].kind = q[0] == 'L' ? 1 : 2;
                        }
                    }
                    (void)end; q = nl;
                }
            } else { kfree(buf); lxm_str = NULL; }
        } else if (buf) kfree(buf);
    }
    lxm_state = 1;
}
static const lxm_ent_t *lxm_find(const char *path) {
    if (lxm_state != 1 || !lxm_cap) return NULL;
    size_t n = strlen(path); uint64_t h = lxm_hash(path, n);
    for (uint32_t i = (uint32_t)h & (lxm_cap - 1); lxm_tab[i].h; i = (i + 1) & (lxm_cap - 1))
        if (lxm_tab[i].h == h && !strncmp(lxm_str + lxm_tab[i].path_off, path, n) && lxm_str[lxm_tab[i].path_off + n] == 0) return &lxm_tab[i];
    return NULL;
}
/* the program's path with every symbolic link expanded (the last component only when follow_last); `in` is normalized, `out` too */
static void lx_follow(const char *in, char *out, size_t cap, int follow_last, const lxm_ent_t **last_link) {
    static const char dummy = 0; (void)dummy;
    if (last_link) *last_link = NULL;
    if (lxm_state != 1 || !lxm_cap) { snprintf(out, cap, "%s", in); return; }
    char work[640]; snprintf(work, sizeof work, "%s", in);
    for (int depth = 0; depth < 40; ++depth) {
        char cur[320]; cur[0] = 0; size_t cl = 0;
        const char *s = work; int restarted = 0;
        while (*s) {
            while (*s == '/') s++;
            if (!*s) break;
            const char *e = s; while (*e && *e != '/') e++;
            size_t l = (size_t)(e - s);
            if (cl + l + 2 >= sizeof cur) { snprintf(out, cap, "%s", in); return; }
            size_t keep = cl; cur[cl++] = '/'; memcpy(cur + cl, s, l); cl += l; cur[cl] = 0;
            int is_last = (*e == 0) || (e[0] == '/' && e[1] == 0);
            const lxm_ent_t *le = lxm_find(cur);
            if (le && le->kind == 1 && (!is_last || follow_last)) {
                const char *tg = lxm_str + le->target_off;
                char nw[640];
                if (tg[0] == '/') snprintf(nw, sizeof nw, "%s%s%s", tg, *e ? "/" : "", e);
                else { cur[keep] = 0; snprintf(nw, sizeof nw, "%s/%s%s%s", cur, tg, *e ? "/" : "", e); }
                path_normalize(nw, work, sizeof work);
                restarted = 1; break;
            }
            if (le && le->kind == 1 && is_last && last_link) *last_link = le;
            s = e;
        }
        if (!restarted) { snprintf(out, cap, "%s", cl ? cur : "/"); return; }
    }
    snprintf(out, cap, "%s", in);                                                    /* a loop of links: give up */
}

/* the C-OS path of a Linux path: /linux/<path> if it exists, the path itself otherwise; links are followed (also the last one when follow_last) */
static void resolve2(lctx_t *c, int dirfd, const char *p, char *out, size_t cap, int follow_last, const lxm_ent_t **last_link) {
    char full[320];
    if (p[0] == '/') snprintf(full, sizeof full, "%s", p);
    else if (dirfd >= 0 && dirfd < LX_MAX_FD && c->fd[dirfd] && c->fd[dirfd]->kind == FD_DIR) snprintf(full, sizeof full, "%s/%s", c->fd[dirfd]->path, p);
    else snprintf(full, sizeof full, "%s/%s", c->cwd, p);
    if (!strncmp(full, "/linux/", 7)) { char t2[320]; snprintf(t2, sizeof t2, "%s", full + 6); snprintf(full, sizeof full, "%s", t2); }     /* a directory descriptor holds a C-OS path: back to the program's */
    char norm[320]; path_normalize(full, norm, sizeof norm);
    lxm_load();
    char fl[320]; lx_follow(norm, fl, sizeof fl, follow_last, last_link);
    char jail[340]; snprintf(jail, sizeof jail, "/linux%s", fl);
    uint64_t sz; uint8_t isdir;
    if (strncmp(fl, "/linux", 6) != 0 && cos_fs_stat(jail, &sz, &isdir)) snprintf(out, cap, "%s", jail);
    else if (strncmp(fl, "/linux", 6) != 0 && strncmp(fl, "/dev/", 5) != 0 && strncmp(fl, "/proc/", 6) != 0 && lxm_cap) {              /* a path to be created: it goes into the root file system when its directory is there */
        char par[340]; snprintf(par, sizeof par, "%s", jail); char *sl = strrchr(par, '/'); if (sl && sl > par + 6) { *sl = 0; if (cos_fs_stat(par, &sz, &isdir) && isdir) { snprintf(out, cap, "%s", jail); return; } }
        snprintf(out, cap, "%s", fl);
    }
    else snprintf(out, cap, "%s", fl);
}
static void resolve(lctx_t *c, int dirfd, const char *p, char *out, size_t cap) { resolve2(c, dirfd, p, out, cap, 1, NULL); }
static bool fs_ok(process_t *p, uint32_t need) { return p->caps == COS_CAP_ALL || (p->caps & need) == need; }

static inline ofd_t *F(lctx_t *c, int fd) { return fd >= 0 && fd < LX_MAX_FD ? c->fd[fd] : NULL; }
static int fd_alloc_from(lctx_t *c, int min) { for (int i = min; i < LX_MAX_FD; ++i) if (!c->fd[i]) return i; return -1; }
static ofd_t *ofd_new(uint8_t kind, uint32_t flags) {
    ofd_t *o = (ofd_t *)kmalloc(sizeof *o); if (!o) return NULL;
    memset(o, 0, sizeof *o); o->ref = 1; o->kind = kind; o->flags = flags; o->cosfd = -1; return o;
}
static void ofd_unref(ofd_t *o);
static void pipe_free(pipe_t *q) {
    for (int i = 0; i < q->nanc; ++i) for (int k = 0; k < q->anc[i].n; ++k) ofd_unref(q->anc[i].f[k]);
    kfree(q->buf); kfree(q);
}
typedef struct ulis { int listening, nq; ofd_t *q[16]; } ulis_t;
static struct { char name[108]; ulis_t *l; } ubind[64];       /* listening unix sockets by name ('@' + name for the abstract namespace) */
static pipe_t *pipe_new(void) {
    pipe_t *q = (pipe_t *)kmalloc(sizeof *q); if (!q) return NULL;
    memset(q, 0, sizeof *q); q->cap = 65536; q->buf = (uint8_t *)kmalloc(q->cap);
    if (!q->buf) { kfree(q); return NULL; }
    return q;
}
/* Another thread of the process may still hold the pointer of a description that was just closed (an epoll_wait or poll walking its list): the structure is not freed
 * at once but emptied (kind FD_NONE, no buffers) and kept in a ring, and freed only when 32768 later closes have gone by. */
#define LX_OFD_RING 32768
static ofd_t *s_ofd_ring[LX_OFD_RING]; static uint32_t s_ofd_ring_n;
static void ofd_retire(ofd_t *o) {
    o->kind = FD_NONE; o->cosfd = -1; o->pipe = o->pipe2 = NULL; o->lis = NULL; o->mem = NULL; o->memlen = 0; o->ep = NULL; o->nep = 0; o->shm = NULL; o->counter = 0; o->dead = 1;
    uint64_t fl = sync_irq_save(); ofd_t **slot = &s_ofd_ring[s_ofd_ring_n++ % LX_OFD_RING]; ofd_t *old = *slot; *slot = o; sync_irq_restore(fl);
    kfree(old);
}
static void ofd_unref(ofd_t *o) {
    if (!o || o->dead || --o->ref > 0) return;
    if ((o->kind == FD_FILE || o->kind == FD_DIR) && o->cosfd >= 0) cos_fd_close(o->owner, o->cosfd);
    if (o->kind == FD_PIPE_R || o->kind == FD_PIPE_W) {
        pipe_t *q = o->pipe;
        if (o->kind == FD_PIPE_R) q->readers--; else q->writers--;
        if (q->readers <= 0 && q->writers <= 0) pipe_free(q);
    }
    if (o->kind == FD_UPAIR) {
        pipe_t *r = o->pipe, *w = o->pipe2;
        if (r) { if (!(o->ushut & 2)) r->readers--; if (r->readers <= 0 && r->writers <= 0) pipe_free(r); }
        if (w) { if (!(o->ushut & 1)) w->writers--; if (w->readers <= 0 && w->writers <= 0) pipe_free(w); }
        if (o->lis) {
            for (int i = 0; i < 64; ++i) if (ubind[i].l == o->lis) { ubind[i].l = NULL; ubind[i].name[0] = 0; }
            for (int i = 0; i < o->lis->nq; ++i) ofd_unref(o->lis->q[i]);
            kfree(o->lis);
        }
    }
    if (o->kind == FD_SOCK && o->cosfd >= 0) cos_sock_close((uint32_t)o->owner, (int)o->cosfd);
    if (o->kind == FD_MEMFD) shm_unref(o->shm);
    if (o->mem) kfree(o->mem);
    if (o->kind == FD_EPOLL && o->ep) kfree(o->ep);
    ofd_retire(o);
}
static void fd_close_slot(lctx_t *c, int fd) { if (fd < 0 || fd >= LX_MAX_FD) return; ofd_unref(c->fd[fd]); c->fd[fd] = NULL; c->cloexec[fd] = 0; }

static int64_t console_write(process_t *p, lctx_t *c, const char *b, uint64_t n) {
    (void)p;
    if (c->out_fd >= 0) { int64_t w = cos_fd_write(c->out_pid, c->out_fd, b, n); if (w >= 0) return w; }
    for (uint64_t i = 0; i < n; ++i) serial_putc(b[i]);
    return (int64_t)n;
}

/* ---- threads ---------------------------------------------------------------- */

static int th_find(lctx_t *c, uint64_t tid) { for (int i = 0; i < c->nth; ++i) if (c->th[i].tid == tid) return i; return -1; }
static void th_add(lctx_t *c, uint64_t tid, uint64_t clear_tid) {
    int i = th_find(c, tid);
    if (i < 0) { if (c->nth >= 256) return; i = c->nth++; c->th[i].tid = tid; }
    c->th[i].clear_tid = clear_tid;
}
static uint64_t cur_tid(void) { thread_t *t = scheduler_get_current_thread(); return t ? t->tid : 0; }

/* A thread ends: tell whoever waits for it (pthread_join waits on the word CLONE_CHILD_CLEARTID named), then leave. */
static void thread_end(process_t *p, lctx_t *c, int code) {
    thread_t *t = scheduler_get_current_thread();
    int i = t ? th_find(c, t->tid) : -1;
    if (i >= 0) {
        uint64_t ct = c->th[i].clear_tid;
        c->th[i] = c->th[--c->nth];
        if (ct) { uint32_t zero = 0; if (copy_out(ct, &zero, 4)) cos_futex_wake(ct, 0x7FFFFFFFu); }
    }
    (void)p;
    thread_exit(t, code);
    for (;;) __asm__ volatile("hlt");
}

#define L_CLONE_VM 0x100
#define L_CLONE_THREAD 0x10000
#define L_CLONE_SETTLS 0x80000
#define L_CLONE_PARENT_SETTID 0x100000
#define L_CLONE_CHILD_CLEARTID 0x200000
#define L_CLONE_CHILD_SETTID 0x1000000

/* clone() as a thread: a new C-OS thread of the same process that continues after the `syscall` with rax = 0, the given stack and the parent's registers. */
/* the arguments are separate from `r`: the child starts with the caller's registers (clone3's glibc wrapper keeps the thread function in one) */
static int64_t do_clone_thread(process_t *p, lctx_t *c, struct regs *r, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls) {
    if (p->thread_count >= 250 || c->nth >= 255) { static int n1; if (n1++ < 5) { serial_puts("[LINUX] clone: too many threads: thread_count "); serial_putdec(p->thread_count); serial_puts(" nth "); serial_putdec((uint64_t)c->nth); serial_puts("\n"); } return -L_EAGAIN; }
    if (!stack) return -L_EINVAL;
    { extern void task_reap_dead_threads(void); task_reap_dead_threads(); }          /* give the slots and kernel stacks of finished threads back first */
    uint64_t fl = sync_irq_save();                     /* the child must not run before its registers are set */
    thread_t *th = thread_create_user_sp(p, (void *)(uintptr_t)(r->rip + 2), NULL, KERNEL_STACK_SIZE, stack);
    if (!th) { sync_irq_restore(fl); { static int n2; if (n2++ < 5) serial_puts("[LINUX] clone: no thread slot (thread table full)\n"); } return -L_EAGAIN; }
    th->context.r15 = r->r15; th->context.r14 = r->r14; th->context.r13 = r->r13; th->context.r12 = r->r12; th->context.r11 = r->r11;
    th->context.r10 = r->r10; th->context.r9 = r->r9; th->context.r8 = r->r8; th->context.rbp = r->rbp; th->context.rdi = r->rdi;
    th->context.rsi = r->rsi; th->context.rdx = r->rdx; th->context.rcx = r->rcx; th->context.rbx = r->rbx;
    th->context.rax = 0; th->context.rip = r->rip + 2; th->context.rsp = stack; th->context.rflags = (r->rflags & ~0x100ull) | 0x202;
    if (flags & L_CLONE_SETTLS) scheduler_set_thread_tls(th, tls);
    th_add(c, th->tid, (flags & L_CLONE_CHILD_CLEARTID) ? ctid : 0);
    sync_irq_restore(fl);
    uint32_t tid32 = (uint32_t)th->tid;
    if ((flags & L_CLONE_PARENT_SETTID) && ptid) copy_out(ptid, &tid32, 4);
    if ((flags & L_CLONE_CHILD_SETTID) && ctid) copy_out(ctid, &tid32, 4);
    return (int64_t)th->tid;
}

/* ---- futex ------------------------------------------------------------------- */

static int64_t futex_timeout_ms(uint64_t ts_addr, bool absolute, bool realtime, uint64_t *out_ms) {
    *out_ms = 0;
    if (!ts_addr) return 0;
    uint64_t ts[2]; if (!copy_in(ts, ts_addr, 16)) return -L_EFAULT;
    uint64_t ms = ts[0] * 1000 + ts[1] / 1000000;
    if (absolute) {
        uint64_t now = realtime ? wall_ms() : hal_timer_get_ms();
        if (ms <= now) return -110;
        ms -= now;
    }
    *out_ms = ms ? ms : 1;
    return 0;
}

static int64_t do_futex(uint64_t uaddr, uint32_t op, uint32_t val, uint64_t a3, uint64_t uaddr2, uint32_t val3) {
    uint32_t cmd = op & 0x7F;
    switch (cmd) {
    case 0: case 9: {   /* FUTEX_WAIT, FUTEX_WAIT_BITSET */
        uint64_t ms; int64_t e = futex_timeout_ms(a3, cmd == 9, (op & 256) != 0, &ms); if (e) return e;
        if (!ensure_user(uaddr, 4, true)) return -L_EFAULT;                                  /* a page shared with a forked process is copied first */
        return cos_futex_wait(uaddr, val, ms);
    }
    case 1: case 10: return cos_futex_wake(uaddr, val > 0x7FFFFFFFu ? 0x7FFFFFFFu : val);   /* FUTEX_WAKE, FUTEX_WAKE_BITSET */
    case 3: case 4: {   /* FUTEX_REQUEUE, FUTEX_CMP_REQUEUE: the waiters are woken instead of moved; they check their word again and wait where they belong */
        if (cmd == 4) { uint32_t cur; if (!copy_in(&cur, uaddr, 4)) return -L_EFAULT; if (cur != val3) return -L_EAGAIN; }
        uint64_t n = (uint64_t)val + (uint64_t)(uint32_t)a3; if (n > 0x7FFFFFFFu) n = 0x7FFFFFFFu;
        return cos_futex_wake(uaddr, (uint32_t)n);
    }
    case 5: {   /* FUTEX_WAKE_OP: *uaddr2 = op(*uaddr2); wake uaddr; wake uaddr2 if the comparison holds */
        uint32_t op3 = val3, opn = (op3 >> 28) & 7, cmpn = (op3 >> 24) & 15, oparg = (op3 >> 12) & 0xFFF, cmparg = op3 & 0xFFF;
        if (op3 & 0x80000000u) oparg = 1u << (oparg & 31);
        int32_t so = (int32_t)(oparg << 20) >> 20, sc = (int32_t)(cmparg << 20) >> 20; (void)so; (void)sc;
        uint32_t old; if (!copy_in(&old, uaddr2, 4)) return -L_EFAULT;
        uint32_t nw = old;
        switch (opn) { case 0: nw = oparg; break; case 1: nw = old + oparg; break; case 2: nw = old | oparg; break; case 3: nw = old & ~oparg; break; case 4: nw = old ^ oparg; break; }
        if (!copy_out(uaddr2, &nw, 4)) return -L_EFAULT;
        int64_t n = cos_futex_wake(uaddr, val);
        bool hit = false;
        switch (cmpn) { case 0: hit = old == cmparg; break; case 1: hit = old != cmparg; break; case 2: hit = old < cmparg; break; case 3: hit = old <= cmparg; break; case 4: hit = old > cmparg; break; case 5: hit = old >= cmparg; break; }
        if (hit) n += cos_futex_wake(uaddr2, (uint32_t)a3);
        return n;
    }
    default: return -L_ENOSYS;
    }
}

void cos_linux_release_pages(process_t *proc);
static bool lx_load(process_t *proc, lctx_t *c, const char *path, const uint8_t *image, uint64_t len, const char *const *argv, int argc, const char *const *envp, int envc, uint64_t *out_entry, uint64_t *out_rsp);

/* ---- signals --------------------------------------------------------------- */

#define LX_FRAME_SET ((int64_t)0x8000000000000000ll)     /* the call changed the whole register frame (execve, rt_sigreturn): nothing more to set */
#define L_EINTR 4
#define L_EIO 5
#define L_ESRCH 3
#define L_ECHILD 10
#define L_E2BIG 7
#define L_ENOEXEC 8
#define L_SIGKILL 9
#define L_SIGALRM 14
#define L_SIGCHLD 17
#define L_SA_RESTART 0x10000000ull
#define L_SA_NODEFER 0x40000000ull
#define L_SA_RESETHAND 0x80000000ull

static inline uint64_t sig_bit(int s) { return 1ull << (s - 1); }
/* the default action of a signal with no handler: end the process (1), or nothing (0) */
static int sig_default_ends(int s) {
    switch (s) { case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 28: return 0; default: return 1; }
}
static void sig_raise(lctx_t *c, int s) {
    if (s < 1 || s > 64) return;
    s_sigraise[s]++;
    if (c->sig[s].handler == LX_SIG_IGN) return;
    if (c->sig[s].handler == LX_SIG_DFL && !sig_default_ends(s)) return;
    c->sig_pending |= sig_bit(s);
}
static void sig_poll_timers(lctx_t *c) {
    if (c->alarm_ms && hal_timer_get_ms() >= c->alarm_ms) { c->alarm_ms = 0; sig_raise(c, L_SIGALRM); }
}
/* a waiting call looks here: true if a signal that is not blocked is waiting for delivery */
static bool sig_interrupt(lctx_t *c) { sig_poll_timers(c); return (c->sig_pending & ~c->sig_blocked) != 0; }

static void notify_parent(process_t *p) {
    process_t *par = p->parent_pid ? process_get_by_pid(p->parent_pid) : NULL;
    if (par && par->linux_ctx) sig_raise(ctx_of(par), L_SIGCHLD);
}
static void end_by_signal(process_t *p, int sig) {
    serial_puts("[LINUX] pid "); serial_putdec(p->pid); serial_puts(" ended by signal "); serial_putdec((uint64_t)sig); serial_puts("\n");
    notify_parent(p);
    process_exit(p, -(1000 + sig));
    thread_exit(scheduler_get_current_thread(), -(1000 + sig));
    for (;;) __asm__ volatile("hlt");
}

typedef struct { uint64_t retaddr; struct regs saved; uint64_t saved_blocked; uint8_t siginfo[128]; uint8_t uc[512]; uint8_t fx[512]; } sigframe_t;   /* fx: FXSAVE image of the interrupted code; a handler may use SSE */
/* where the exception stub (isr.asm) parked the user's FPU/SSE state below this register frame; it is restored when the frame is left */
static inline uint8_t *fpu_area_of(struct regs *r) { return (uint8_t *)(((uintptr_t)r - 512u) & ~(uintptr_t)15u); }

/* Delivers the first pending, unblocked signal: a handler runs on the user stack (the frame has the registers to come back to), a default that ends the process
 * does so. res/nr: the system call that is just ending, so one that was interrupted can be restarted. */
static void deliver_signals(process_t *p, lctx_t *c, struct regs *r, uint64_t nr, int64_t res) {
    for (;;) {
        uint64_t pend = c->sig_pending & ~c->sig_blocked;
        if (!pend) return;
        int s = __builtin_ctzll(pend) + 1;
        c->sig_pending &= ~sig_bit(s);
        uint64_t h = c->sig[s].handler;
        if (h == LX_SIG_IGN) continue;
        if (h == LX_SIG_DFL) { if (sig_default_ends(s)) end_by_signal(p, s); continue; }
        s_sigdel[s]++;
        sigframe_t *f = (sigframe_t *)kmalloc(sizeof *f); if (!f) return;
        memset(f, 0, sizeof *f);
        f->retaddr = c->sig[s].restorer;
        f->saved = *r;
        if (res == -L_EINTR && (c->sig[s].flags & L_SA_RESTART)) { f->saved.rax = nr; f->saved.rip = r->rip - 2; }
        f->saved_blocked = c->sig_blocked;
        memcpy(f->fx, fpu_area_of(r), 512);
        *(int32_t *)f->siginfo = s;
        uint64_t *g = (uint64_t *)(f->uc + 40);          /* ucontext.uc_mcontext.gregs */
        const struct regs *q = &f->saved;
        g[0] = q->r8; g[1] = q->r9; g[2] = q->r10; g[3] = q->r11; g[4] = q->r12; g[5] = q->r13; g[6] = q->r14; g[7] = q->r15;
        g[8] = q->rdi; g[9] = q->rsi; g[10] = q->rbp; g[11] = q->rbx; g[12] = q->rdx; g[13] = q->rax; g[14] = q->rcx;
        g[15] = q->rsp; g[16] = q->rip; g[17] = q->rflags;
        uint64_t sp = ((r->rsp - 144 - sizeof *f) & ~15ull) + 8;       /* below the red zone; at entry rsp+8 is 16-aligned, as after a call */
        bool ok = copy_out(sp, f, sizeof *f);
        uint64_t newblocked = c->sig_blocked | c->sig[s].mask | ((c->sig[s].flags & L_SA_NODEFER) ? 0 : sig_bit(s));
        if (c->sig[s].flags & L_SA_RESETHAND) c->sig[s].handler = LX_SIG_DFL;
        kfree(f);
        if (!ok) end_by_signal(p, 11);                                 /* no room on the stack: SIGSEGV */
        c->sig_blocked = newblocked;
        r->rsp = sp; r->rip = h; r->rdi = (uint64_t)s;
        r->rsi = sp + offsetof(sigframe_t, siginfo); r->rdx = sp + offsetof(sigframe_t, uc); r->rax = 0;
        return;
    }
}

static int64_t do_sigreturn(lctx_t *c, struct regs *r) {
    uint64_t base = r->rsp - 8;                                        /* the handler's `ret` popped retaddr */
    sigframe_t *f = (sigframe_t *)kmalloc(sizeof *f); if (!f) return -L_ENOMEM;
    if (!copy_in(f, base, sizeof *f)) { kfree(f); return -L_EFAULT; }
    const uint64_t *g = (const uint64_t *)(f->uc + 40);
    r->r8 = g[0]; r->r9 = g[1]; r->r10 = g[2]; r->r11 = g[3]; r->r12 = g[4]; r->r13 = g[5]; r->r14 = g[6]; r->r15 = g[7];
    r->rdi = g[8]; r->rsi = g[9]; r->rbp = g[10]; r->rbx = g[11]; r->rdx = g[12]; r->rax = g[13]; r->rcx = g[14];
    r->rsp = g[15]; r->rip = g[16]; r->rflags = (g[17] & 0x0CD5ull) | 0x202;
    c->sig_blocked = f->saved_blocked & ~(sig_bit(L_SIGKILL));
    memcpy(fpu_area_of(r), f->fx, 512);
    kfree(f);
    return LX_FRAME_SET;
}

/* Linux wait status of a C-OS exit status */
static int wait_status_of(int st) {
    if (st >= 0) return (st & 0xFF) << 8;
    if (st <= -1000) return (-st - 1000) & 0x7F;                       /* ended by that signal */
    int vec = -st - 128;                                               /* ended by a CPU exception */
    int sig = vec == 14 || vec == 13 ? 11 : vec == 6 ? 4 : vec == 0 ? 8 : vec == 3 ? 5 : 6;
    return sig;
}

static int64_t do_kill(process_t *p, lctx_t *c, int64_t pid, int sig) {
    if (sig < 0 || sig > 64) return -L_EINVAL;
    if (pid == 0) pid = (int64_t)p->pid;
    if (pid <= 0) return -L_ESRCH;
    process_t *t = process_get_by_pid((uint64_t)pid);
    if (!t || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) return -L_ESRCH;
    if (sig == 0) return 0;
    if (!t->linux_ctx) {                                               /* a C-OS process: only the program's own children may be ended */
        if (t->parent_pid != p->pid) return -L_EPERM;
        if (sig_default_ends(sig)) process_exit(t, -(1000 + sig));
        return 0;
    }
    lctx_t *tc = ctx_of(t);
    if (t == p) { sig_raise(tc, sig); if (sig == L_SIGKILL) end_by_signal(p, sig); return 0; }
    if (sig == L_SIGKILL || (tc->sig[sig].handler == LX_SIG_DFL && sig_default_ends(sig))) {
        notify_parent(t); process_exit(t, -(1000 + sig)); return 0;    /* no handler to run: it ends now, wherever it is */
    }
    sig_raise(tc, sig);
    return 0;
}

/* ---- processes: fork, execve, wait ---------------------------------------------- */

/* the description a forked child gets: files are opened again (the C-OS descriptor belongs to the process that opened it), the rest is shared */
static ofd_t *ofd_for_fork(ofd_t *o, uint64_t child_pid) {
    if (o->kind != FD_FILE && o->kind != FD_DIR) { o->ref++; return o; }
    ofd_t *n = ofd_new(o->kind, o->flags); if (!n) return NULL;
    n->owner = child_pid; n->pos = o->pos; memcpy(n->path, o->path, sizeof n->path);
    if (o->kind == FD_DIR) n->cosfd = cos_fd_opendir(child_pid, o->path);
    else {
        uint32_t acc = o->flags & L_O_ACCMODE;
        uint32_t mode = acc == 0 ? COS_O_RDONLY : (o->flags & L_O_APPEND) ? COS_O_WRONLY_APPEND : COS_O_RDWR_CREATE;   /* never the truncating mode */
        n->cosfd = cos_fd_open(child_pid, o->path, mode);
    }
    if (n->cosfd < 0) { kfree(n); return NULL; }
    return n;
}

typedef struct { uint64_t virt, phys, flags; } vm_page_t;      /* phys: a private copy or a frame shared with the parent (copy-on-write, paging_cow_share) */
static bool vm_flush(process_t *ch, vm_page_t *b, int n) {
    uint64_t irq = sync_irq_save();
    page_directory_t *saved = paging_get_current_directory();
    paging_switch_directory((page_directory_t *)ch->page_dir);
    bool ok = true;
    for (int i = 0; i < n; ++i) if (!paging_map_page(b[i].virt, b[i].phys, b[i].flags)) { serial_puts("[LINUX] fork: could not map 0x"); serial_puthex(b[i].virt); serial_puts(" (free pages "); serial_putdec(phys_memory_get_stats()->free_pages); serial_puts(") heap used "); serial_putdec(memory_get_heap_used() >> 20); serial_puts("/"); serial_putdec(memory_get_heap_total() >> 20); serial_puts(" MiB\n"); ok = false; if (!paging_cow_drop(b[i].phys)) paging_free_physical(b[i].phys); }
    paging_switch_directory(saved);
    sync_irq_restore(irq);
    return ok;
}
/* copies every page of the ranges of the Linux process `c` (and its stack and brk heap) into the child */
static int s_cowmode; static bool s_cowmode_set;       /* /linux/etc/cos.cowro: only read-only pages are shared, cos.cowrw: only writable ones (debugging) */
static bool s_nocow, s_nocow_set;     /* /linux/etc/cos.nocow: fork copies every page (to tell a copy-on-write bug from another one) */
static bool lx_copy_vm(process_t *p, lctx_t *c, process_t *ch) {
    enum { BATCH = 1024 };
    vm_page_t *b = (vm_page_t *)kmalloc(BATCH * sizeof *b); if (!b) return false;
    int n = 0; bool ok = true;
    struct rg_t { uint64_t s, e; uint32_t prot; } *rg = (struct rg_t *)kmalloc((size_t)(c->nvma + 2) * sizeof(struct rg_t)); int nr = 0;
    if (!rg) { kfree(b); return false; }
    for (int i = 0; i < c->nvma; ++i) { if (c->vma[i].shm) continue; rg[nr].s = c->vma[i].start; rg[nr].e = c->vma[i].end; rg[nr].prot = c->vma[i].prot; nr++; }
    if (c->brk_start && c->brk_cur > c->brk_start) { rg[nr].s = c->brk_start; rg[nr].e = pg_up(c->brk_cur); rg[nr].prot = L_PROT_READ | L_PROT_WRITE; nr++; }
    rg[nr].s = p->stack_start; rg[nr].e = p->stack_end; rg[nr].prot = L_PROT_READ | L_PROT_WRITE; nr++;
    for (int i = 0; i < nr && ok; ++i) for (uint64_t a = rg[i].s; a < rg[i].e && ok; a += PAGE) {
        uint64_t old = paging_virt_to_phys(a);
        if (!old) continue;
        old &= ~(PAGE - 1);
        { static int uaf_n; if (phys_page_is_free(old) && uaf_n < 20) { uaf_n++; serial_puts("[LINUX] fork: page 0x"); serial_puthex(a); serial_puts(" of pid "); serial_putdec(p->pid); serial_puts(" is mapped to the FREE frame 0x"); serial_puthex(old); serial_puts(" prot "); serial_putdec(rg[i].prot); serial_puts("\n"); } }
        uint64_t fl = page_flags(rg[i].prot);
        if (!s_nocow_set) { uint64_t sz; uint8_t d; s_nocow = cos_fs_stat("/linux/etc/cos.nocow", &sz, &d); s_nocow_set = true; }
        if (!s_cowmode_set) { uint64_t sz; uint8_t d; s_cowmode = 0; if (cos_fs_stat("/linux/etc/cos.cowro", &sz, &d)) s_cowmode = 1; else if (cos_fs_stat("/linux/etc/cos.cowrw", &sz, &d)) s_cowmode = 2; s_cowmode_set = true; }
        bool want_share = s_cowmode == 0 || (s_cowmode == 1 && !(rg[i].prot & L_PROT_WRITE)) || (s_cowmode == 2 && (rg[i].prot & L_PROT_WRITE));
        if (!s_nocow && want_share && paging_cow_share(old)) {                                                /* both sides read-only until one writes */
            fl &= ~(uint64_t)PAGE_RW;
            if (rg[i].prot & L_PROT_WRITE) paging_clear_rw_noflush(a);
            b[n].virt = a; b[n].phys = old; b[n].flags = fl; n++;
        } else {
            uint64_t np = paging_alloc_physical(); if (!np) { serial_puts("[LINUX] fork: out of physical memory\n"); ok = false; break; }
            memcpy((void *)(uintptr_t)PHYS_TO_VIRT(np), (const void *)(uintptr_t)PHYS_TO_VIRT(old), PAGE);
            b[n].virt = a; b[n].phys = np; b[n].flags = fl; n++;
        }
        if (n == BATCH) { ok = vm_flush(ch, b, n); n = 0; }
    }
    if (ok && n) ok = vm_flush(ch, b, n);
    paging_flush_all_cpus();                                                       /* the parent's pages are read-only now: no CPU may keep a writable entry */
    kfree(b); kfree(rg);
    return ok;
}

static int64_t do_fork_inner(process_t *p, lctx_t *c, struct regs *r, uint64_t flags, uint64_t child_stack, uint64_t ptid) {
    cos_fd_sync_all();                      /* the child opens its own handles: what the parent wrote must be on the volume */
    process_t *ch = process_create(p->name, TASK_TYPE_USER);
    if (!ch) return -L_EAGAIN;
    lctx_t *cc = (lctx_t *)kmalloc(sizeof *cc);
    if (!cc) { serial_puts("[LINUX] fork: no memory for the context\n"); process_destroy(ch); return -L_ENOMEM; }
    memcpy(cc, c, sizeof *cc);
    cc->vma = NULL; cc->vma_cap = 0; cc->nvma = 0;                                   /* the child gets a table of its own */
    if (c->nvma) { if (!vma_reserve(cc, c->nvma)) { kfree(cc); process_destroy(ch); return -L_ENOMEM; } memcpy(cc->vma, c->vma, (size_t)c->nvma * sizeof(lvma_t)); cc->nvma = c->nvma; for (int k = 0; k < cc->nvma; ++k) if (cc->vma[k].shm) shm_ref(cc->vma[k].shm); }
    cc->fpid = c->fpid ? c->fpid : (uint32_t)p->pid;
    cc->nth = 0; cc->mmap_busy = 0; cc->forking = 0; cc->sig_pending = 0; cc->alarm_ms = 0; cc->out_fd = -1;
    for (int i = 0; i < LX_MAX_FD; ++i) cc->fd[i] = NULL;
    ch->linux_ctx = cc;
    if (ch->stack_end > ch->stack_start) {                      /* the child was created with a stack of its own: give it back, the parent's pages take its place (they may overlap it) */
        uint64_t irq = sync_irq_save(); page_directory_t *saved = paging_get_current_directory();
        paging_switch_directory((page_directory_t *)ch->page_dir);
        paging_release_user_range(ch->stack_start, (ch->stack_end - ch->stack_start + PAGE - 1) / PAGE);
        paging_switch_directory(saved); sync_irq_restore(irq);
    }
    ch->stack_start = p->stack_start; ch->stack_end = p->stack_end;
    bool ok = true;
    for (int i = 0; i < LX_MAX_FD && ok; ++i) if (c->fd[i]) { cc->fd[i] = ofd_for_fork(c->fd[i], ch->pid); if (!cc->fd[i]) { serial_puts("[LINUX] fork: cannot reopen fd "); serial_putdec((uint64_t)i); serial_puts(" kind "); serial_putdec(c->fd[i]->kind); serial_puts(" "); serial_puts(c->fd[i]->path); serial_puts("\n"); ok = false; } }
    cc->out_fd = c->out_fd; cc->out_pid = c->out_pid;        /* the same handle: separate append handles would overwrite each other's data */
    uint32_t pid32 = (uint32_t)ch->pid;
    if ((flags & L_CLONE_PARENT_SETTID) && ptid) copy_out(ptid, &pid32, 4);
    uint64_t t_fork = hal_timer_get_ms();
    if (ok) ok = lx_copy_vm(p, c, ch);
    serial_puts("[LINUX] fork copied the address space in "); serial_putdec(hal_timer_get_ms() - t_fork); serial_puts(" ms\n");
    if (!ok) { serial_puts("[LINUX] fork: files or memory could not be copied\n"); process_destroy(ch); return -L_ENOMEM; }

    uint64_t fl = sync_irq_save();
    thread_t *cur = scheduler_get_current_thread();
    thread_t *th = thread_create_user_sp(ch, (void *)(uintptr_t)(r->rip + 2), NULL, KERNEL_STACK_SIZE, child_stack ? child_stack : r->rsp);
    if (!th) { sync_irq_restore(fl); process_destroy(ch); return -L_EAGAIN; }
    th->context.r15 = r->r15; th->context.r14 = r->r14; th->context.r13 = r->r13; th->context.r12 = r->r12; th->context.r11 = r->r11;
    th->context.r10 = r->r10; th->context.r9 = r->r9; th->context.r8 = r->r8; th->context.rbp = r->rbp; th->context.rdi = r->rdi;
    th->context.rsi = r->rsi; th->context.rdx = r->rdx; th->context.rcx = r->rcx; th->context.rbx = r->rbx;
    th->context.rax = 0; th->context.rip = r->rip + 2; th->context.rsp = child_stack ? child_stack : r->rsp; th->context.rflags = (r->rflags & ~0x100ull) | 0x202;
    if (cur && cur->fs_base) scheduler_set_thread_tls(th, cur->fs_base);
    th_add(cc, th->tid, 0);
    sync_irq_restore(fl);
    return (int64_t)ch->pid;
}
/* A fork copies the address space while other threads may be filling a file mapping page by page: the child would get the library half loaded (zero pages where the
 * file has bytes) - glibc keeps a fork out of a dlopen, but here a mapping takes seconds. A fork closes the gate, waits for the fillers to finish, copies, opens it. */
static int64_t do_fork(process_t *p, lctx_t *c, struct regs *r, uint64_t flags, uint64_t child_stack, uint64_t ptid) {
    __atomic_add_fetch(&c->forking, 1, __ATOMIC_SEQ_CST);
    { uint64_t until = hal_timer_get_ms() + 900000; while (__atomic_load_n(&c->mmap_busy, __ATOMIC_SEQ_CST) && hal_timer_get_ms() < until) scheduler_yield(); }
    int64_t res = do_fork_inner(p, c, r, flags, child_stack, ptid);
    __atomic_sub_fetch(&c->forking, 1, __ATOMIC_SEQ_CST);
    return res;
}

static void strv_free(char **v, int n) { for (int i = 0; i < n; ++i) kfree(v[i]); kfree(v); }
static char **strv_copy(uint64_t uv, int *n_out) {
    char **v = (char **)kmalloc(129 * sizeof(char *)); if (!v) return NULL;
    int n = 0;
    while (uv && n < 128) {
        uint64_t sp; if (!copy_in(&sp, uv + (uint64_t)n * 8, 8)) { strv_free(v, n); return NULL; }
        if (!sp) break;
        char *str = (char *)kmalloc(1024); if (!str) { strv_free(v, n); return NULL; }
        if (!copy_str(str, sp, 1024)) { kfree(str); strv_free(v, n); return NULL; }
        v[n++] = str;
    }
    v[n] = NULL; *n_out = n; return v;
}

static int64_t do_execve(process_t *p, lctx_t *c, struct regs *r, uint64_t upath, uint64_t uargv, uint64_t uenvp) {
    char raw[300], path[256];
    if (!copy_str(raw, upath, sizeof raw)) return -L_EFAULT;
    int argc = 0, envc = 0;
    char **argv = strv_copy(uargv, &argc); if (!argv) return -L_EFAULT;
    char **envp = uenvp ? strv_copy(uenvp, &envc) : NULL;
    if (uenvp && !envp) { strv_free(argv, argc); return -L_EFAULT; }
    if (!envp) { envp = (char **)kmalloc(sizeof(char *)); if (envp) envp[0] = NULL; }
    if (!strcmp(raw, "/proc/self/exe") && c->exe[0]) snprintf(raw, sizeof raw, "%s", c->exe);      /* busybox starts its applets this way */
    resolve(c, L_AT_FDCWD, raw, path, sizeof path);
    int64_t res = -L_ENOENT;
    uint8_t *img = NULL; uint64_t sz = 0; uint8_t isdir = 0;
    char *sb_argv[3 + 130]; int sbn = 0; char sbs[2][130];     /* a #! line: interpreter and one argument */
    char use_path[256]; snprintf(use_path, sizeof use_path, "%s", path);
    for (int depth = 0; depth < 4; ++depth) {
        if (!cos_fs_stat(use_path, &sz, &isdir) || isdir || sz < 4 || sz > (96ull << 20)) { res = -L_ENOENT; goto out; }
        img = (uint8_t *)kmalloc(sz); if (!img) { res = -L_ENOMEM; goto out; }
        int got = cos_fs_read_file(use_path, img, sz); if (got < 4) { res = -L_ENOEXEC; goto out; }
        if (img[0] == '#' && img[1] == '!') {
            char line[130]; int k = 0; for (int i = 2; i < got && i < 128 && img[i] != '\n'; ++i) line[k++] = (char)img[i]; line[k] = 0;
            char *ip = line; while (*ip == ' ') ip++;
            char *e = ip; while (*e && *e != ' ') e++;
            char *arg = NULL; if (*e) { *e++ = 0; while (*e == ' ') e++; if (*e) arg = e; }
            snprintf(sbs[0], sizeof sbs[0], "%s", ip); if (arg) snprintf(sbs[1], sizeof sbs[1], "%s", arg);
            /* new argv: interpreter, [arg], script path, the old argv[1..] */
            char **nv = (char **)kmalloc(132 * sizeof(char *)); if (!nv) { res = -L_ENOMEM; goto out; }
            int m = 0;
            nv[m] = (char *)kmalloc(160); snprintf(nv[m++], 160, "%s", sbs[0]);
            if (arg) { nv[m] = (char *)kmalloc(160); snprintf(nv[m++], 160, "%s", sbs[1]); }
            nv[m] = (char *)kmalloc(160); snprintf(nv[m++], 160, "%s", use_path);
            for (int i = 1; i < argc && m < 130; ++i) { nv[m] = argv[i]; m++; argv[i] = NULL; }
            kfree(argv[0]); kfree(argv); argv = nv; argc = m; argv[m] = NULL;
            resolve(c, L_AT_FDCWD, sbs[0], use_path, sizeof use_path);
            kfree(img); img = NULL; (void)sb_argv; (void)sbn;
            continue;
        }
        break;
    }
    if (!img) goto out;
    {
        cos_elf_info_t info;
        if (!cos_elf_inspect(img, sz, &info) || info.e_type != 3) { res = -L_ENOEXEC; goto out; }
        for (int i = 0; i < LX_MAX_FD; ++i) if (c->fd[i] && c->cloexec[i]) fd_close_slot(c, i);
        cos_linux_release_pages(p);
        /* the loader writes the new argument block straight into the old stack frames (through the physical map, which ignores read-only): frames still shared with the parent after a fork must become private first */
        for (uint64_t a = p->stack_start; a < p->stack_end; a += PAGE) if (paging_virt_to_phys(a) && !paging_user_range_ok(a, 1, true)) paging_cow_break(a, page_flags(L_PROT_READ | L_PROT_WRITE));
        c->nvma = 0; c->brk_start = c->brk_cur = 0;
        for (int sg = 1; sg <= 64; ++sg) if (c->sig[sg].handler != LX_SIG_IGN) { c->sig[sg].handler = LX_SIG_DFL; c->sig[sg].flags = 0; }
        c->alarm_ms = 0;
        uint64_t entry = 0, rsp = 0;
        if (!lx_load(p, c, use_path, img, sz, (const char *const *)argv, argc, (const char *const *)envp, envc, &entry, &rsp)) {
            serial_puts("[LINUX] execve: the new program could not be loaded\n");
            end_by_signal(p, 11);
        }
        r->rip = entry; r->rsp = rsp; r->rflags = 0x202;
        r->rax = r->rbx = r->rcx = r->rdx = r->rsi = r->rdi = r->rbp = 0; r->r8 = r->r9 = r->r10 = r->r11 = r->r12 = r->r13 = r->r14 = r->r15 = 0;
        thread_t *t = scheduler_get_current_thread();
        if (t) scheduler_set_thread_tls(t, 0);
        c->fs_base = 0;
        res = LX_FRAME_SET;
    }
out:
    kfree(img); strv_free(argv, argc); if (envp) strv_free(envp, envc);
    return res;
}

/* ---- the system calls ---------------------------------------------------- */

#define SC_DEBUG 0
static const char *s_unknown_logged;

extern uint64_t s_fill_ms, s_fill_pages;
static int64_t sys_mmap(process_t *p, lctx_t *c, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, int fd, uint64_t off) {
    if (len == 0) return -L_EINVAL;
    uint64_t sz = pg_up(len);
    uint64_t at;
    if (flags & L_MAP_FIXED) {
        if (addr & (PAGE - 1)) return -L_EINVAL;
        if (addr < COS_ELF_PIE_BASE || addr + sz > LX_MMAP_LIMIT || addr + sz < addr) return -L_ENOMEM;
        at = addr;
        vma_cut(c, at, at + sz, true);
    } else {
        at = 0;
        if (addr && !(addr & (PAGE - 1)) && addr >= COS_ELF_PIE_BASE && addr + sz < LX_MMAP_LIMIT && vma_free(c, addr, addr + sz)) at = addr;
        if (!at) {
            at = c->mmap_next;
            for (int skip = 0; skip < 100000; ++skip) {                                      /* a hinted mapping may sit where the bump pointer arrives: go past it */
                if (at + sz > LX_MMAP_LIMIT || at + sz < at) return -L_ENOMEM;
                int vi = vma_lower(c, at);
                if (!(vi < c->nvma && c->vma[vi].start < at + sz)) break;
                at = pg_up(c->vma[vi].end);
            }
            if (at + sz > LX_MMAP_LIMIT || at + sz < at) return -L_ENOMEM;
            c->mmap_next = at + sz;
        }
    }
    if (flags & L_MAP_SHARED) {                                                       /* shared memory: a memfd or /dev/shm object, or an anonymous shared range (fork keeps it shared) */
        lx_shm_t *sh = NULL; int own = 0;
        if (flags & L_MAP_ANON) { sh = shm_new(NULL); if (!sh || !shm_resize(sh, sz)) { if (sh) shm_unref(sh); vma_cut(c, at, at + sz, true); return -L_ENOMEM; } own = 1; off = 0; }
        else { ofd_t *mo = F(c, fd); if (mo && mo->kind == FD_MEMFD) { sh = mo->shm; if (sh->size < off + sz && !shm_resize(sh, off + sz)) { vma_cut(c, at, at + sz, true); return -L_ENOMEM; } } }
        if (sh) {
            bool ok = vma_insert_shm(c, at, at + sz, prot, sh, off);
            if (own) shm_unref(sh);
            if (!ok) return -L_ENOMEM;
            return (int64_t)at;
        }
    }
    bool filemap = !(flags & L_MAP_ANON);
    if (filemap) for (;;) { __atomic_add_fetch(&c->mmap_busy, 1, __ATOMIC_SEQ_CST); if (!__atomic_load_n(&c->forking, __ATOMIC_SEQ_CST)) break; __atomic_sub_fetch(&c->mmap_busy, 1, __ATOMIC_SEQ_CST); scheduler_yield(); }     /* released when the pages are loaded (or on any error) */
    if (!vma_insert_f(c, at, at + sz, prot, filemap)) { if (filemap) __atomic_sub_fetch(&c->mmap_busy, 1, __ATOMIC_SEQ_CST); return -L_ENOMEM; }
    if (!(flags & L_MAP_ANON)) {
        ofd_t *mo = F(c, fd); if (!mo || mo->kind != FD_FILE) { vma_cut(c, at, at + sz, true); __atomic_sub_fetch(&c->mmap_busy, 1, __ATOMIC_SEQ_CST); return -L_EBADF; }
        /* a file mapping is filled now: the pages are made, loaded, then given the requested rights */
        uint8_t *buf = (uint8_t *)kmalloc(PAGE);
        if (!buf) { vma_cut(c, at, at + sz, true); __atomic_sub_fetch(&c->mmap_busy, 1, __ATOMIC_SEQ_CST); return -L_ENOMEM; }
        uint64_t fsz = 0; { uint8_t isd = 0; if (!cos_fs_stat(mo->path, &fsz, &isd)) fsz = ~0ull; }
        uint64_t an_eof = 0, an_zero = 0, an_short = 0, an_first = 0;
        uint64_t fill_t0 = hal_timer_get_ms();       /* what the fill saw: reads that ended early / gave zeros / gave a part of a page */      /* where the file really ends: a read that gives nothing before that is an error */
        for (uint64_t a = at; a < at + sz; a += PAGE) {
            if (!task_alloc_page(p, a, page_flags(L_PROT_READ | L_PROT_WRITE))) { kfree(buf); vma_cut(c, at, at + sz, true); __atomic_sub_fetch(&c->mmap_busy, 1, __ATOMIC_SEQ_CST); return -L_ENOMEM; }
            memset(buf, 0, PAGE);
            uint64_t foff = off + (a - at);
            int64_t n = cos_fd_pread(mo->owner, mo->cosfd, (int64_t)foff, buf, PAGE);
            for (int retry = 0; n < 0 && retry < 5; ++retry) n = cos_fd_pread(mo->owner, mo->cosfd, (int64_t)foff, buf, PAGE);     /* a failed read must not leave a zero page behind: the loader would run on it */
            if (n == (int64_t)PAGE && foff < fsz) {                        /* an all-zero page is read again: a read that "succeeds" with zeros would leave a hole the loader trips over */
                bool allz = true; for (uint64_t q = 0; q < PAGE; q += 8) if (*(uint64_t *)(buf + q)) { allz = false; break; }
                for (int again = 0; allz && again < 3; ++again) {
                    int64_t n2 = cos_fd_pread(mo->owner, mo->cosfd, (int64_t)foff, buf, PAGE);
                    if (n2 != (int64_t)PAGE) break;
                    for (uint64_t q = 0; q < PAGE; q += 8) if (*(uint64_t *)(buf + q)) { allz = false; break; }
                    if (!allz) { static uint32_t rep; if (rep++ < 20) { serial_puts("[LXE] mmap: a zero read of "); serial_puts(mo->path); serial_puts(" at 0x"); serial_puthex(foff); serial_puts(" gave data on read "); serial_putdec((uint64_t)again + 2); serial_puts("\n"); } }
                }
            }
            if (n == 0 && foff + PAGE <= fsz + PAGE) { if (!an_eof++) an_first = foff; }
            if (n > 0 && n < (int64_t)PAGE && foff + (uint64_t)n < fsz) { if (!an_short++ && !an_first) an_first = foff; }
            if (n == (int64_t)PAGE) { bool zz = true; for (uint64_t q = 0; q < PAGE; q += 8) if (*(uint64_t *)(buf + q)) { zz = false; break; } if (zz) { if (!an_zero++ && !an_first) an_first = foff; } }
            if (n < 0 || (n == 0 && foff < fsz)) { serial_puts("[LXE] mmap: reading "); serial_puts(mo->path); serial_puts(" at 0x"); serial_puthex(foff); serial_puts(" failed (got "); serial_putdec((uint64_t)(n < 0 ? 0 : n)); serial_puts(")\n"); kfree(buf); vma_cut(c, at, at + sz, true); __atomic_sub_fetch(&c->mmap_busy, 1, __ATOMIC_SEQ_CST); return -L_EIO; }
            if (n > 0 && !copy_out(a, buf, (uint64_t)n)) { serial_puts("[LXE] mmap: could not store the page at 0x"); serial_puthex(a); serial_puts("\n"); kfree(buf); vma_cut(c, at, at + sz, true); __atomic_sub_fetch(&c->mmap_busy, 1, __ATOMIC_SEQ_CST); return -L_ENOMEM; }
            if (!(prot & L_PROT_WRITE) || (prot & L_PROT_EXEC)) paging_protect_page(a, page_flags(prot));
        }
        __atomic_add_fetch(&s_fill_ms, hal_timer_get_ms() - fill_t0, __ATOMIC_RELAXED); __atomic_add_fetch(&s_fill_pages, sz / PAGE, __ATOMIC_RELAXED);
        if (an_eof || an_short || (an_zero > 64)) { serial_puts("[LXE] mmap: "); serial_puts(mo->path); serial_puts(" pid "); serial_putdec(p->pid); serial_puts(" file size per stat "); serial_putdec(fsz); serial_puts(": reads at EOF "); serial_putdec(an_eof);
            serial_puts(" short "); serial_putdec(an_short); serial_puts(" zero pages "); serial_putdec(an_zero); serial_puts(" first at 0x"); serial_puthex(an_first); serial_puts("\n"); }
        { static int mv = -1; if (mv < 0) { uint64_t zsz; uint8_t zd; mv = cos_fs_stat("/linux/etc/cos.mapverify", &zsz, &zd) ? 1 : 0; }
          if (mv) {                                    /* debugging: is the memory what the file says, right after the fill? */
              static uint8_t vb[PAGE]; uint64_t bad = 0, zero = 0, first = 0;
              for (uint64_t a = at; a < at + sz; a += PAGE) {
                  uint64_t foff = off + (a - at); if (foff >= fsz) break;
                  memset(vb, 0, PAGE); int64_t n = cos_fd_pread(mo->owner, mo->cosfd, (int64_t)foff, vb, PAGE); if (n <= 0) continue;
                  if (!copy_in(buf, a, PAGE)) { bad++; continue; }
                  if (memcmp(vb, buf, n < (int64_t)PAGE ? (size_t)n : PAGE)) { if (bad < 4) { uint64_t fr = paging_virt_to_phys(a); serial_puts("[MAPV]   page 0x"); serial_puthex(a); serial_puts(" frame 0x"); serial_puthex(fr); serial_puts(" free "); serial_putdec(phys_page_is_free(fr & ~(uint64_t)(PAGE - 1)) ? 1 : 0);
                      serial_puts(" cow-shared "); serial_putdec(paging_cow_is_shared(fr & ~(uint64_t)(PAGE - 1)) ? 1 : 0); serial_puts(" writable "); serial_putdec(paging_user_range_ok(a, 1, true) ? 1 : 0); serial_puts(" file-word0 0x"); serial_puthex(*(uint64_t *)vb); serial_puts("\n"); }
                      if (!bad++) first = a; bool z = true; for (int q = 0; q < PAGE; q += 8) if (*(uint64_t *)(buf + q)) { z = false; break; } if (z) zero++; }
              }
              serial_puts("[MAPV] "); serial_puts(mo->path); serial_puts(" 0x"); serial_puthex(at); serial_puts("+0x"); serial_puthex(sz); serial_puts(" pid "); serial_putdec(p->pid);
              serial_puts(" mismatched pages "); serial_putdec(bad); serial_puts(" (zero "); serial_putdec(zero); serial_puts(") first 0x"); serial_puthex(first); serial_puts("\n");
          } }
        kfree(buf);
        fmap_note(c->fpid ? c->fpid : (uint32_t)p->pid, at, at + sz, off, mo->path);
        __atomic_sub_fetch(&c->mmap_busy, 1, __ATOMIC_SEQ_CST);
    }
    return (int64_t)at;
}


/* ---- /dev and /proc: a few synthesized entries (only when the path is not found under /linux) ---- */
enum { VFS_NONE = 0, VFS_DEV, VFS_PROC, VFS_DIR };
enum { PF_MAPS, PF_STATUS, PF_STAT, PF_CPUINFO, PF_MEMINFO, PF_VERSION, PF_UPTIME, PF_CMDLINE, PF_LOADAVG };
static int vfs_classify(const char *p, int *id) {
    *id = 0;
    if (!strcmp(p, "/dev/null")) return VFS_DEV;
    if (!strcmp(p, "/dev/zero")) { *id = 1; return VFS_DEV; }
    if (!strcmp(p, "/dev/urandom") || !strcmp(p, "/dev/random")) { *id = 2; return VFS_DEV; }
    if (!strcmp(p, "/dev/tty") || !strcmp(p, "/dev/console")) { *id = 3; return VFS_DEV; }
    static const char *const vd[] = { "/dev", "/proc", "/proc/self", "/proc/sys", "/dev/shm", "/proc/self/fd" };
    for (int i = 0; i < 6; ++i) if (!strcmp(p, vd[i])) { *id = i; return VFS_DIR; }
    static const struct { const char *n; int id; } pf[] = { {"/proc/self/maps", PF_MAPS}, {"/proc/self/status", PF_STATUS}, {"/proc/self/stat", PF_STAT},
        {"/proc/cpuinfo", PF_CPUINFO}, {"/proc/meminfo", PF_MEMINFO}, {"/proc/version", PF_VERSION}, {"/proc/uptime", PF_UPTIME},
        {"/proc/self/cmdline", PF_CMDLINE}, {"/proc/loadavg", PF_LOADAVG} };
    for (unsigned i = 0; i < sizeof pf / sizeof pf[0]; ++i) if (!strcmp(p, pf[i].n)) { *id = pf[i].id; return VFS_PROC; }
    return VFS_NONE;
}
static uint8_t *proc_content(process_t *p, lctx_t *c, int id, uint32_t *len) {
    const uint32_t cap = 16384 + (uint32_t)c->nvma * 220; char *b = (char *)kmalloc(cap); if (!b) return NULL;
    uint32_t n = 0;
#define PF(...) do { if (n < cap) n += (uint32_t)snprintf(b + n, cap - n, __VA_ARGS__); if (n > cap) n = cap; } while (0)
    switch (id) {
    case PF_MAPS:
        /* sorted by address like Linux's; a file mapping has its offset, a device and an inode number, and the path as the program knows it (programs find their
         * own libraries and parse the lines: a missing field is a crash in them) */
        for (int i = 0; i < c->nvma; ++i) {
            const lvma_t *v = &c->vma[i]; const lx_fmap_t *fm = NULL;
            for (uint32_t k = 0; k < LX_FMAP_N && k < s_fmap_n && !fm; ++k) { const lx_fmap_t *m = &s_fmap[(s_fmap_n - 1 - k) & (LX_FMAP_N - 1)]; if (m->pid == (c->fpid ? c->fpid : (uint32_t)p->pid) && v->start >= m->start && v->start < m->end) fm = m; }
            if (fm) { const char *gp = !strncmp(fm->name, "/linux/", 7) ? fm->name + 6 : fm->name; unsigned long long ino = (unsigned long long)(lxm_hash(fm->name, strlen(fm->name)) & 0xFFFFFF);
                PF("%llx-%llx %c%c%c%c %08llx 08:01 %llu %s\n", (unsigned long long)v->start, (unsigned long long)v->end, v->prot & 1 ? 'r' : '-', v->prot & 2 ? 'w' : '-', v->prot & 4 ? 'x' : '-', 'p',
                   (unsigned long long)(fm->off + (v->start - fm->start)), ino, gp); }
            else PF("%llx-%llx %c%c%c%c 00000000 00:00 0\n", (unsigned long long)v->start, (unsigned long long)v->end, v->prot & 1 ? 'r' : '-', v->prot & 2 ? 'w' : '-', v->prot & 4 ? 'x' : '-', 'p');
        }
        if (c->brk_start && c->brk_cur > c->brk_start) PF("%llx-%llx rw-p 00000000 00:00 0                          [heap]\n", (unsigned long long)c->brk_start, (unsigned long long)pg_up(c->brk_cur));
        PF("%llx-%llx rw-p 00000000 00:00 0                          [stack]\n", (unsigned long long)p->stack_start, (unsigned long long)p->stack_end);
        break;
    case PF_STATUS: PF("Name:\t%s\nState:\tR (running)\nTgid:\t%llu\nPid:\t%llu\nPPid:\t%llu\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nThreads:\t1\n", p->name, (unsigned long long)p->pid, (unsigned long long)p->pid, (unsigned long long)p->parent_pid); break;
    case PF_STAT: PF("%llu (%s) R %llu 0 0 0 0 0 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0\n", (unsigned long long)p->pid, p->name, (unsigned long long)p->parent_pid); break;
    case PF_CPUINFO: { uint32_t k = smp_online_cpu_count(); if (k < 1) k = 1; for (uint32_t i = 0; i < k; ++i) PF("processor\t: %u\nvendor_id\t: C-OS\nmodel name\t: C-OS virtual CPU\ncpu MHz\t\t: 2000.000\nflags\t\t: fpu sse sse2\n\n", i); break; }
    case PF_MEMINFO: { uint64_t tot = phys_get_total_memory() / 1024; phys_mem_stats_t *st = phys_memory_get_stats(); uint64_t fr = st ? st->free_pages * 4 : tot / 2;
        PF("MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\nBuffers:               0 kB\nCached:                0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n", (unsigned long long)tot, (unsigned long long)fr, (unsigned long long)fr); break; }
    case PF_VERSION: PF("Linux version 6.1.0-cos (C-OS compatibility layer) #1 SMP\n"); break;
    case PF_UPTIME: { uint64_t ms = hal_timer_get_ms(); PF("%llu.%02llu 0.00\n", (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000 / 10)); break; }
    case PF_LOADAVG: PF("0.00 0.00 0.00 1/1 %llu\n", (unsigned long long)p->pid); break;
    case PF_CMDLINE: PF("%s", c->exe); if (n < cap) b[n++] = 0; break;
    }
#undef PF
    *len = n; return (uint8_t *)b;
}

/* /proc/<own pid>/... is /proc/self/... */
static void vfs_canon(process_t *p, char *path, size_t cap) {
    if (strncmp(path, "/proc/", 6) != 0) return;
    const char *q = path + 6; uint64_t v = 0; int d = 0;
    while (*q >= '0' && *q <= '9') { v = v * 10 + (uint64_t)(*q - '0'); ++q; ++d; }
    if (d && v == p->pid && (*q == 0 || *q == '/')) { char t[256]; snprintf(t, sizeof t, "/proc/self%s", q); snprintf(path, cap, "%s", t); }
}
static uint8_t *vdir_build(process_t *p, lctx_t *c, int id, uint32_t *len) {
    const uint32_t cap = 4096; char *b = (char *)kmalloc(cap); if (!b) return NULL;
    uint32_t n = 0;
#define VE(t, ...) do { if (n + 40 < cap) { b[n++] = (t); n += (uint32_t)snprintf(b + n, cap - n, __VA_ARGS__) + 1; } } while (0)
    switch (id) {
    case 0: VE('c', "null"); VE('c', "zero"); VE('c', "urandom"); VE('c', "random"); VE('c', "tty"); VE('c', "console"); VE('d', "shm"); break;
    case 1: VE('d', "self"); VE('d', "%llu", (unsigned long long)p->pid); VE('d', "sys"); VE('f', "cpuinfo"); VE('f', "meminfo"); VE('f', "version"); VE('f', "uptime"); VE('f', "loadavg"); break;
    case 2: VE('f', "maps"); VE('f', "status"); VE('f', "stat"); VE('f', "cmdline"); VE('d', "fd"); break;
    case 5: for (int i = 0; i < LX_MAX_FD; ++i) if (c->fd[i]) VE('l', "%d", i); break;
    default: break;
    }
#undef VE
    *len = n; return (uint8_t *)b;
}

static int64_t do_open(process_t *p, lctx_t *c, int dirfd, const char *upath, uint32_t flags) {
    char path[256];
    { char raw[300]; if (!copy_str(raw, (uint64_t)(uintptr_t)upath, sizeof raw)) return -L_EFAULT; resolve(c, dirfd, raw, path, sizeof path); vfs_canon(p, path, sizeof path); }
    uint64_t size = 0; uint8_t isdir = 0;
    bool exists = cos_fs_stat(path, &size, &isdir);
    int slot = fd_alloc_from(c, 0);
    if (slot < 0) return -L_EMFILE;
    if (!strncmp(path, "/proc/self/fd/", 14)) {                                         /* open("/proc/self/fd/N"): another description of the same object (how a read-only copy of a memfd is made) */
        int sfd = 0; for (const char *q = path + 14; *q >= '0' && *q <= '9'; ++q) sfd = sfd * 10 + (*q - '0');
        ofd_t *so = F(c, sfd); if (!so) return -L_ENOENT;
        if (so->kind == FD_MEMFD) { ofd_t *vo = ofd_new(FD_MEMFD, flags | (flags & L_O_ACCMODE ? 0 : 0)); if (!vo) return -L_ENOMEM; shm_ref(so->shm); vo->shm = so->shm; c->fd[slot] = vo; c->cloexec[slot] = (flags & 0x80000) ? 1 : 0; return slot; }
        so->ref++; c->fd[slot] = so; c->cloexec[slot] = (flags & 0x80000) ? 1 : 0; return slot;
    }
    if (!strncmp(path, "/dev/shm/", 9) && path[9]) {                                  /* shm_open(): a named shared memory object */
        const char *nm = path + 9; lx_shm_t *sh = NULL; uint64_t fl = sync_irq_save();
        for (lx_shm_t *q = s_shm_named; q; q = q->next) if (!strcmp(q->name, nm)) { sh = q; break; }
        if (!sh && (flags & L_O_CREAT)) { sh = shm_new(nm); if (sh) { sh->named = 1; sh->next = s_shm_named; s_shm_named = sh; } }      /* the list holds the first reference */
        if (sh) sh->ref++;
        sync_irq_restore(fl);
        if (!sh) return -L_ENOENT;
        ofd_t *vo = ofd_new(FD_MEMFD, flags | L_O_RDWR); if (!vo) { shm_unref(sh); return -L_ENOMEM; }
        vo->shm = sh; if (flags & L_O_TRUNC) shm_resize(sh, 0);
        c->fd[slot] = vo; c->cloexec[slot] = (flags & 0x80000) ? 1 : 0; return slot;
    }
    { int vid, vk = strncmp(path, "/linux", 6) ? vfs_classify(path, &vid) : VFS_NONE;
      if (vk == VFS_DEV || vk == VFS_PROC) {
        ofd_t *vo = ofd_new(vk == VFS_DEV ? FD_DEV : FD_MEM, flags); if (!vo) return -L_ENOMEM;
        if (vk == VFS_DEV) vo->counter = (uint64_t)vid;
        else { vo->mem = proc_content(p, c, vid, &vo->memlen); if (!vo->mem) { kfree(vo); return -L_ENOMEM; } }
        snprintf(vo->path, sizeof vo->path, "%s", path);
        c->fd[slot] = vo; c->cloexec[slot] = (flags & 0x80000) ? 1 : 0; return slot;
      } else if (vk == VFS_DIR) {
        ofd_t *vo = ofd_new(FD_VDIR, flags); if (!vo) return -L_ENOMEM;
        vo->counter = (uint64_t)vid; vo->mem = vdir_build(p, c, vid, &vo->memlen); if (!vo->mem) { kfree(vo); return -L_ENOMEM; }
        snprintf(vo->path, sizeof vo->path, "%s", path);
        c->fd[slot] = vo; c->cloexec[slot] = (flags & 0x80000) ? 1 : 0; return slot;
      } }
    uint32_t acc = flags & L_O_ACCMODE;
    ofd_t *o;
    if (exists && isdir) {
        int64_t h = cos_fd_opendir(p->pid, path);
        if (h < 0) return -L_EACCES;
        o = ofd_new(FD_DIR, flags); if (!o) { cos_fd_close(p->pid, h); return -L_ENOMEM; }
        o->cosfd = h; o->owner = p->pid; snprintf(o->path, sizeof o->path, "%s", path);
    } else {
        if (flags & L_O_DIRECTORY) return exists ? -L_ENOTDIR : -L_ENOENT;
        if (!exists && !(flags & L_O_CREAT)) return -L_ENOENT;
        if (acc != 0 || (flags & L_O_CREAT)) { if (!fs_ok(p, COS_CAP_FS_WRITE)) return -L_EACCES; } else if (!fs_ok(p, COS_CAP_FS_READ)) return -L_EACCES;
        uint32_t mode = COS_O_RDONLY;
        if (acc != 0 || (flags & L_O_CREAT)) {
            if (flags & L_O_APPEND) mode = COS_O_WRONLY_APPEND;
            else if ((flags & L_O_TRUNC) || !exists) mode = acc == L_O_RDWR ? COS_O_RDWR_CREATE : COS_O_WRONLY_CREATE;
            else mode = COS_O_RDWR_CREATE;
        }
        cos_fd_sync_all();
        int64_t h = cos_fd_open(p->pid, path, mode);
        if (h < 0) return -L_ENOENT;
        o = ofd_new(FD_FILE, flags); if (!o) { cos_fd_close(p->pid, h); return -L_ENOMEM; }
        o->cosfd = h; o->owner = p->pid; snprintf(o->path, sizeof o->path, "%s", path);
    }
    c->fd[slot] = o; c->cloexec[slot] = (flags & 0x80000) ? 1 : 0;      /* O_CLOEXEC */
    return slot;
}

static bool sig_interrupt(lctx_t *c);

/* ---- reading, writing and waiting on a description ----------------------------- */

#define L_O_NONBLOCK 0x800
#define L_EPIPE 32
#define L_EOPNOTSUPP 95
#define L_POLLIN 1
#define L_POLLOUT 4
#define L_POLLERR 8
#define L_POLLHUP 0x10

static int64_t upair_send(lctx_t *c, ofd_t *o, const void *src, uint64_t n, bool nb, ofd_t **fds, int nfds);
static int64_t upair_recv(lctx_t *c, ofd_t *o, void *dst, uint64_t n, bool nb, bool peek, ofd_t **fds, int *nfds, bool *trunc);
static int64_t sock_recv(lctx_t *c, ofd_t *o, void *dst, uint64_t n, uint32_t flags, cos_sockaddr6_t *from);
static int64_t sock_send(lctx_t *c, ofd_t *o, const void *src, uint64_t n, uint32_t flags, const cos_sockaddr6_t *to);

/* a count that grows with every new event on a description (data written, an eventfd written): an edge-triggered watch is reported again when it moves */
static uint64_t ofd_version(ofd_t *o) {
    switch (o->kind) {
    case FD_PIPE_R: case FD_UPAIR: return o->pipe ? o->pipe->wtotal + (o->pipe->writers <= 0) : 0;
    case FD_EVENT: return o->ver;
    default: return 0;
    }
}
static uint32_t ofd_ready(ofd_t *o) {
    switch (o->kind) {
    case FD_SOCK: { cos_pollfd_t pf; pf.sd = (int32_t)o->cosfd; pf.events = COS_POLLIN | COS_POLLOUT; pf.revents = 0; cos_sock_poll((uint32_t)o->owner, &pf, 1); return (uint32_t)(uint16_t)pf.revents & (L_POLLIN | L_POLLOUT | L_POLLERR | L_POLLHUP); }
    case FD_DEV: case FD_MEM: case FD_MEMFD: return L_POLLIN | L_POLLOUT;
    case FD_UPAIR:
        if (o->lis) return o->lis->nq ? L_POLLIN : 0;
        if (!o->pipe) return L_POLLOUT | L_POLLHUP;
        return (o->pipe->len ? L_POLLIN : 0) | (o->pipe->writers <= 0 ? (L_POLLHUP | L_POLLIN | 0x2000) : 0) | (o->pipe2->len < o->pipe2->cap && o->pipe2->readers > 0 ? L_POLLOUT : 0) | (o->pipe2->readers <= 0 ? L_POLLERR : 0);
    case FD_CON: return (o->flags & L_O_ACCMODE) == 0 ? (L_POLLIN | L_POLLHUP) : L_POLLOUT;
    case FD_FILE: case FD_DIR: return L_POLLIN | L_POLLOUT;
    case FD_PIPE_R: return (o->pipe->len ? L_POLLIN : 0) | (o->pipe->writers <= 0 ? L_POLLHUP : 0);
    case FD_PIPE_W: return (o->pipe->len < o->pipe->cap ? L_POLLOUT : 0) | (o->pipe->readers <= 0 ? L_POLLERR : 0);
    case FD_EVENT: return (o->counter ? L_POLLIN : 0) | L_POLLOUT;
    default: return 0;
    }
}

/* kernel-buffer read: n bytes at most */
static int64_t ofd_read(process_t *p, lctx_t *c, ofd_t *o, void *dst, uint64_t n, bool use_pos, uint64_t pos) {
    (void)p; (void)c;
    switch (o->kind) {
    case FD_CON: return 0;
    case FD_DIR: return -L_EISDIR;
    case FD_FILE: {
        uint64_t at = use_pos ? pos : o->pos;
        int64_t got = cos_fd_pread(o->owner, o->cosfd, (int64_t)at, dst, n);
        if (got < 0) return -L_EBADF;
        if (!use_pos) o->pos += (uint64_t)got;
        return got;
    }
    case FD_SOCK: return sock_recv(c, o, dst, n, 0, NULL);
    case FD_MEMFD: {
        uint64_t at = use_pos ? pos : o->pos; lx_shm_t *sh = o->shm; if (at >= sh->size) return 0;
        uint64_t k = n < sh->size - at ? n : sh->size - at, done = 0;
        while (done < k) { uint64_t idx = (at + done) / PAGE, po = (at + done) % PAGE, m = PAGE - po < k - done ? PAGE - po : k - done; uint64_t f = shm_frame(sh, idx); if (!f) break;
            memcpy((uint8_t *)dst + done, (uint8_t *)(uintptr_t)PHYS_TO_VIRT(f) + po, m); done += m; }
        if (!use_pos) o->pos += done;
        return (int64_t)done;
    }
    case FD_MEM: {
        uint64_t at = use_pos ? pos : o->pos; if (at >= o->memlen) return 0;
        uint64_t k = n < o->memlen - at ? n : o->memlen - at; memcpy(dst, o->mem + at, k);
        if (!use_pos) o->pos += k;
        return (int64_t)k;
    }
    case FD_DEV:
        if (o->counter == 1) { memset(dst, 0, n); return (int64_t)n; }
        if (o->counter == 2) { for (uint64_t i = 0; i < n; i += 8) { uint64_t v = cos_random_u64(); for (uint64_t k = 0; k < 8 && i + k < n; ++k) ((uint8_t *)dst)[i + k] = (uint8_t)(v >> (8 * k)); } return (int64_t)n; }
        return 0;
    case FD_UPAIR: { int nf = 0; bool tr; return upair_recv(c, o, dst, n, (o->flags & L_O_NONBLOCK) != 0, false, NULL, &nf, &tr); }
    case FD_PIPE_R: {
        pipe_t *q = o->pipe;
        for (;;) {
            if (q->len) {
                uint64_t k = n < q->len ? n : q->len;
                for (uint64_t i = 0; i < k; ++i) ((uint8_t *)dst)[i] = q->buf[(q->head + i) % q->cap];
                q->head = (uint32_t)((q->head + k) % q->cap); q->len -= (uint32_t)k;
                return (int64_t)k;
            }
            if (q->writers <= 0) return 0;
            if (o->flags & L_O_NONBLOCK) return -L_EAGAIN;
            if (sig_interrupt(c)) return -L_EINTR;
            scheduler_sleep(1);
        }
    }
    case FD_EVENT: {
        if (n < 8) return -L_EINVAL;
        for (;;) {
            if (o->counter) {
                uint64_t v = (o->efd_flags & 1) ? 1 : o->counter;
                o->counter -= v; memcpy(dst, &v, 8); return 8;
            }
            if (o->flags & L_O_NONBLOCK) return -L_EAGAIN;
            scheduler_sleep(1);
        }
    }
    default: return -L_EBADF;
    }
}

static int64_t ofd_write(process_t *p, lctx_t *c, ofd_t *o, const void *src, uint64_t n, bool use_pos, uint64_t pos) {
    switch (o->kind) {
    case FD_CON: return console_write(p, c, (const char *)src, n);
    case FD_FILE: {
        int64_t res;
        if (o->flags & L_O_APPEND) res = cos_fd_write(o->owner, o->cosfd, src, n);
        else { res = cos_fd_pwrite(o->owner, o->cosfd, (int64_t)(use_pos ? pos : o->pos), src, n); }
        if (res < 0) return -L_EBADF;
        if (!use_pos) o->pos += (uint64_t)res;
        return res;
    }
    case FD_SOCK: return sock_send(c, o, src, n, 0, NULL);
    case FD_MEMFD: {
        lx_shm_t *sh = o->shm; uint64_t at = (o->flags & L_O_APPEND) ? sh->size : (use_pos ? pos : o->pos);
        if (at + n > sh->size && !shm_resize(sh, at + n)) return -L_ENOMEM;
        uint64_t done = 0;
        while (done < n) { uint64_t idx = (at + done) / PAGE, po = (at + done) % PAGE, m = PAGE - po < n - done ? PAGE - po : n - done; uint64_t f = shm_frame(sh, idx); if (!f) return done ? (int64_t)done : -L_ENOMEM;
            memcpy((uint8_t *)(uintptr_t)PHYS_TO_VIRT(f) + po, (const uint8_t *)src + done, m); done += m; }
        if (!use_pos) o->pos = at + done;
        return (int64_t)done;
    }
    case FD_MEM: return -L_EBADF;
    case FD_DEV: return o->counter == 3 ? console_write(p, c, (const char *)src, n) : (int64_t)n;
    case FD_UPAIR: return upair_send(c, o, src, n, (o->flags & L_O_NONBLOCK) != 0, NULL, 0);
    case FD_PIPE_W: {
        pipe_t *q = o->pipe; uint64_t done = 0;
        while (done < n) {
            if (q->readers <= 0) return done ? (int64_t)done : -L_EPIPE;
            uint32_t space = q->cap - q->len;
            if (space) {
                uint64_t k = n - done < space ? n - done : space;
                for (uint64_t i = 0; i < k; ++i) q->buf[(q->head + q->len + i) % q->cap] = ((const uint8_t *)src)[done + i];
                q->len += (uint32_t)k; done += k;
            } else {
                if (o->flags & L_O_NONBLOCK) return done ? (int64_t)done : -L_EAGAIN;
                if (sig_interrupt(c)) return done ? (int64_t)done : -L_EINTR;
                scheduler_sleep(1);
            }
        }
        return (int64_t)done;
    }
    case FD_EVENT: {
        if (n < 8) return -L_EINVAL;
        uint64_t v; memcpy(&v, src, 8);
        if (v == ~0ull) return -L_EINVAL;
        o->counter += v; o->ver++; return 8;
    }
    default: return -L_EBADF;
    }
}

/* read/write through a user buffer, in pieces */
static int64_t user_read(process_t *p, lctx_t *c, int fd, uint64_t ubuf, uint64_t len, bool use_pos, uint64_t pos) {
    ofd_t *o = F(c, fd); if (!o) return -L_EBADF;
    if (len == 0) return 0;
    if ((o->flags & L_O_ACCMODE) == L_O_WRONLY && o->kind == FD_FILE) return -L_EBADF;
    /* a regular file is read in full (programs that do not loop over short reads rely on it: a zip reader, ld.so); other kinds return what one piece gave */
    int64_t total = 0;
    while ((uint64_t)total < len) {
        uint64_t n = len - (uint64_t)total > 65536 ? 65536 : len - (uint64_t)total;
        uint8_t *buf = (uint8_t *)kmalloc(n); if (!buf) return total ? total : -L_ENOMEM;
        int64_t got = ofd_read(p, c, o, buf, n, use_pos, pos + (uint64_t)total);
        if (got > 0 && !copy_out(ubuf + (uint64_t)total, buf, (uint64_t)got)) got = -L_EFAULT;
        kfree(buf);
        if (got < 0) return total ? total : got;
        total += got;
        if ((uint64_t)got < n || o->kind != FD_FILE) break;
    }
    return total;
}
static int64_t user_write(process_t *p, lctx_t *c, int fd, uint64_t ubuf, uint64_t len, bool use_pos, uint64_t pos) {
    ofd_t *o = F(c, fd); if (!o) return -L_EBADF;
    if (len == 0) return 0;
    if ((o->flags & L_O_ACCMODE) == 0 && o->kind == FD_FILE) return -L_EBADF;
    int64_t total = 0;
    while ((uint64_t)total < len) {
        uint64_t n = len - (uint64_t)total > 65536 ? 65536 : len - (uint64_t)total;
        uint8_t *buf = (uint8_t *)kmalloc(n); if (!buf) return total ? total : -L_ENOMEM;
        if (!copy_in(buf, ubuf + (uint64_t)total, n)) { kfree(buf); return total ? total : -L_EFAULT; }
        int64_t w = ofd_write(p, c, o, buf, n, use_pos, pos + (uint64_t)total);
        kfree(buf);
        if (w < 0) return total ? total : w;
        total += w; if ((uint64_t)w < n) break;
        if (o->kind != FD_PIPE_W && o->kind != FD_FILE && o->kind != FD_CON && o->kind != FD_SOCK && o->kind != FD_UPAIR && o->kind != FD_DEV) break;
    }
    return total;
}

/* poll-style waiting: calls `scan` until it finds something or the time is up (timeout in ms: <0 forever, 0 not at all) */
static void wait_tick(void) { scheduler_sleep(1); }
static int64_t do_stat(process_t *p, lctx_t *c, const char *path, uint8_t isdir_known, uint64_t size, uint64_t ubuf) {
    (void)p; (void)c;
    uint8_t st[144]; memset(st, 0, sizeof st);
    uint64_t ino = 1469598103934665603ull;                 /* a number per path: ld.so tells files apart by (st_dev, st_ino) and would take libm for the libc it has already */
    for (const char *q = path; q && *q; ++q) ino = (ino ^ (uint8_t)*q) * 1099511628211ull;
    *(uint64_t *)(st + 0) = 1;                             /* st_dev */
    uint32_t mode = isdir_known ? (0040000u | 0755u) : (0100000u | 0644u);
    { const char *gp = !strncmp(path, "/linux/", 7) ? path + 6 : path; const lxm_ent_t *xe = lxm_find(gp); if (xe && xe->kind == 2) mode |= 0111; }       /* the executable bit comes from the manifest */
    *(uint64_t *)(st + 8) = ino | 1;                 /* st_ino */
    *(uint64_t *)(st + 16) = 1;                      /* st_nlink */
    *(uint32_t *)(st + 24) = mode;
    *(uint64_t *)(st + 48) = size;
    *(uint64_t *)(st + 56) = 4096;
    *(uint64_t *)(st + 64) = (size + 511) / 512;
    uint64_t t = rtc_get_seconds_since_epoch();
    *(uint64_t *)(st + 72) = t; *(uint64_t *)(st + 88) = t; *(uint64_t *)(st + 104) = t;
    return copy_out(ubuf, st, sizeof st) ? 0 : -L_EFAULT;
}

static uint64_t now_ms(void) { return hal_timer_get_ms(); }

/* ---- sockets: IPv4/IPv6 TCP/UDP over cos_socket.c (non-blocking calls; blocking is done here by polling every 1 ms) ---- */
#define L_MSG_DONTWAIT 0x40
#define L_EINPROGRESS 115
#define L_EAFNOSUPPORT 97
#define L_ENOTSOCK 88
#define L_EPROTONOSUPPORT 93
#define L_AF_INET 2
#define L_AF_INET6 10

static int64_t sock_recv(lctx_t *c, ofd_t *o, void *dst, uint64_t n, uint32_t flags, cos_sockaddr6_t *from) {
    if (n > 65536) n = 65536;
    for (;;) {
        int r = o->counter == COS_SOCK_DGRAM ? cos_sock_recvfrom6((uint32_t)o->owner, (int)o->cosfd, dst, n, from) : cos_sock_recv((uint32_t)o->owner, (int)o->cosfd, dst, n);
        if (r != -COS_EAGAIN) return r;
        if ((o->flags & L_O_NONBLOCK) || (flags & L_MSG_DONTWAIT)) return -L_EAGAIN;
        if (sig_interrupt(c)) return -L_EINTR;
        scheduler_sleep(1);
    }
}
static int64_t sock_send(lctx_t *c, ofd_t *o, const void *src, uint64_t n, uint32_t flags, const cos_sockaddr6_t *to) {
    uint32_t pid = (uint32_t)o->owner; int sd = (int)o->cosfd;
    if (o->counter == COS_SOCK_DGRAM) {
        if (!to) return cos_sock_send(pid, sd, src, n);
        if (to->v6) return cos_sock_sendto6(pid, sd, src, n, to->a, to->port);
        return cos_sock_sendto(pid, sd, src, n, (uint32_t)to->a[0] << 24 | (uint32_t)to->a[1] << 16 | (uint32_t)to->a[2] << 8 | to->a[3], to->port);
    }
    uint64_t done = 0;
    while (done < n) {
        uint64_t k = n - done > 16384 ? 16384 : n - done;
        int r = cos_sock_send(pid, sd, (const uint8_t *)src + done, k);
        if (r > 0) { done += (uint64_t)r; continue; }
        if (r != -COS_EAGAIN) return done ? (int64_t)done : r;
        if ((o->flags & L_O_NONBLOCK) || (flags & L_MSG_DONTWAIT)) return done ? (int64_t)done : -L_EAGAIN;
        if (sig_interrupt(c)) return done ? (int64_t)done : -L_EINTR;
        scheduler_sleep(1);
    }
    return (int64_t)done;
}
/* struct sockaddr_in: u16 family (2), u16 port (big endian), u32 address (big endian).
 * struct sockaddr_in6: u16 family (10), u16 port, u32 flowinfo, u8 address[16], u32 scope_id. An IPv4-mapped address (::ffff:a.b.c.d) is an IPv4 address. */
static int sa_get(uint64_t ua, uint64_t len, cos_sockaddr6_t *a) {
    uint8_t b[28]; if (len < 8) return -L_EINVAL;
    if (!copy_in(b, ua, len < 28 ? len : 28)) return -L_EFAULT;
    memset(a, 0, sizeof *a);
    if (b[0] == L_AF_INET && b[1] == 0) { a->port = (uint16_t)(b[2] << 8 | b[3]); memcpy(a->a, b + 4, 4); return 0; }
    if (b[0] == L_AF_INET6 && b[1] == 0) {
        if (len < 24) return -L_EINVAL;
        a->port = (uint16_t)(b[2] << 8 | b[3]);
        static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
        if (!memcmp(b + 8, mapped, 12)) { memcpy(a->a, b + 20, 4); return 0; }
        memcpy(a->a, b + 8, 16); a->v6 = 1; return 0;
    }
    return -L_EAFNOSUPPORT;
}
static int sa_put(const ofd_t *o, uint64_t ua, uint64_t lenp, const cos_sockaddr6_t *a) {
    if (!ua || !lenp) return 0;
    uint32_t have; if (!copy_in(&have, lenp, 4)) return -L_EFAULT;
    uint8_t b[28]; memset(b, 0, sizeof b); uint32_t want;
    if (o->efd_flags == L_AF_INET6) {
        want = 28; b[0] = L_AF_INET6; b[2] = (uint8_t)(a->port >> 8); b[3] = (uint8_t)a->port;
        if (a->v6) memcpy(b + 8, a->a, 16); else { b[18] = b[19] = 0xFF; memcpy(b + 20, a->a, 4); }
    } else {
        want = 16; b[0] = L_AF_INET; b[2] = (uint8_t)(a->port >> 8); b[3] = (uint8_t)a->port; memcpy(b + 4, a->a, 4);
    }
    if (!copy_out(ua, b, have < want ? have : want)) return -L_EFAULT;
    return copy_out(lenp, &want, 4) ? 0 : -L_EFAULT;
}
static ofd_t *sock_of(lctx_t *c, int fd, int64_t *err) {
    ofd_t *o = F(c, fd); if (!o) { *err = -L_EBADF; return NULL; }
    if (o->kind != FD_SOCK) { *err = o->kind == FD_UPAIR ? -L_EOPNOTSUPP : -L_ENOTSOCK; return NULL; }
    return o;
}
static int64_t sock_new_fd(lctx_t *c, uint64_t pid, int sd, int type, int af, uint32_t sockflags) {
    int n = fd_alloc_from(c, 0);
    ofd_t *o = n >= 0 ? ofd_new(FD_SOCK, (sockflags & 0x800) ? L_O_NONBLOCK : 0) : NULL;
    if (!o) { cos_sock_close((uint32_t)pid, sd); return n < 0 ? -L_EMFILE : -L_ENOMEM; }
    o->owner = pid; o->cosfd = sd; o->counter = (uint64_t)type; o->efd_flags = (uint32_t)af; c->fd[n] = o; c->cloexec[n] = (sockflags & 0x80000) != 0; return n;
}
/* msghdr gather/scatter through one bounded buffer */
static int64_t sock_msg(process_t *p, lctx_t *c, ofd_t *o, uint64_t um, uint32_t flags, bool sending) {
    (void)p;
    uint8_t mh[56]; if (!copy_in(mh, um, sizeof mh)) return -L_EFAULT;
    uint64_t name = *(uint64_t *)mh, iov = *(uint64_t *)(mh + 16), iovn = *(uint64_t *)(mh + 24); uint32_t namelen = *(uint32_t *)(mh + 8);
    if (iovn > 64) return -L_EINVAL;
    uint8_t *buf = (uint8_t *)kmalloc(65536); if (!buf) return -L_ENOMEM;
    uint64_t tot = 0; int64_t res = 0; cos_sockaddr6_t peer; memset(&peer, 0, sizeof peer);
    if (sending) {
        for (uint64_t i = 0; i < iovn && res == 0; ++i) {
            uint64_t v[2]; if (!copy_in(v, iov + i * 16, 16)) { res = -L_EFAULT; break; }
            uint64_t k = v[1] > 65536 - tot ? 65536 - tot : v[1];
            if (k && !copy_in(buf + tot, v[0], k)) { res = -L_EFAULT; break; }
            tot += k;
        }
        if (!res) {
            cos_sockaddr6_t to; bool has = false;
            if (name && namelen) { int e = sa_get(name, namelen, &to); if (e) res = e; else has = true; }
            if (!res) res = sock_send(c, o, buf, tot, flags, has ? &to : NULL);
        }
    } else {
        for (uint64_t i = 0; i < iovn; ++i) { uint64_t v[2]; if (!copy_in(v, iov + i * 16, 16)) { res = -L_EFAULT; break; } tot += v[1]; }
        if (!res) {
            res = sock_recv(c, o, buf, tot > 65536 ? 65536 : tot, flags, &peer);
            if (res > 0) {
                uint64_t left = (uint64_t)res, off = 0;
                for (uint64_t i = 0; i < iovn && left; ++i) {
                    uint64_t v[2]; if (!copy_in(v, iov + i * 16, 16)) break;
                    uint64_t k = v[1] < left ? v[1] : left;
                    if (k && !copy_out(v[0], buf + off, k)) { res = -L_EFAULT; break; }
                    off += k; left -= k;
                }
                if (name && namelen && o->counter == COS_SOCK_DGRAM) { sa_put(o, name, um + 8, &peer); }
                uint32_t zero = 0; copy_out(um + 48, &zero, 4); uint64_t z0 = 0; copy_out(um + 40, &z0, 8);
            }
        }
    }
    kfree(buf); return res;
}

/* ---- unix sockets: socketpair() ends and named listeners -------------------- */

static int64_t upair_send(lctx_t *c, ofd_t *o, const void *src, uint64_t n, bool nb, ofd_t **fds, int nfds) {
    if (!o->pipe2) return -107;                       /* ENOTCONN */
    pipe_t *q = o->pipe2;
    if (o->ushut & 1) return -L_EPIPE;
    if (nfds > LX_ANC_FDS) return -L_EINVAL;
    if (q->packet) {
        if (n + 4 > q->cap) return -90;               /* EMSGSIZE */
        for (;;) {
            uint64_t irq = sync_irq_save();                                   /* the queue is shared by the CPUs of both ends */
            if (q->readers <= 0) { sync_irq_restore(irq); return -L_EPIPE; }
            if (q->cap - q->len >= n + 4 && (!nfds || q->nanc < LX_MAX_ANC)) {
                uint32_t h = (uint32_t)n;
                for (int i = 0; i < 4; ++i) q->buf[(q->head + q->len + i) % q->cap] = (uint8_t)(h >> (8 * i));
                for (uint64_t i = 0; i < n; ++i) q->buf[(q->head + q->len + 4 + i) % q->cap] = ((const uint8_t *)src)[i];
                if (nfds) { int a = q->nanc++; q->anc[a].at = q->wtotal; q->anc[a].n = nfds; for (int i = 0; i < nfds; ++i) q->anc[a].f[i] = fds[i]; }
                q->len += (uint32_t)(n + 4); q->wtotal += n + 4;
                sync_irq_restore(irq);
                return (int64_t)n;
            }
            sync_irq_restore(irq);
            if (nb) return -L_EAGAIN;
            if (sig_interrupt(c)) return -L_EINTR;
            scheduler_sleep(1);
        }
    }
    uint64_t done = 0;
    while (done < n || (n == 0 && nfds && done == 0)) {
        uint64_t irq = sync_irq_save();
        if (q->readers <= 0) { sync_irq_restore(irq); return done ? (int64_t)done : -L_EPIPE; }
        uint32_t space = q->cap - q->len;
        if (space && (done || !nfds || q->nanc < LX_MAX_ANC)) {
            uint64_t k = n - done < space ? n - done : space;
            if (!done && nfds) { int a = q->nanc++; q->anc[a].at = q->wtotal; q->anc[a].n = nfds; for (int i = 0; i < nfds; ++i) q->anc[a].f[i] = fds[i]; nfds = 0; }
            for (uint64_t i = 0; i < k; ++i) q->buf[(q->head + q->len + i) % q->cap] = ((const uint8_t *)src)[done + i];
            q->len += (uint32_t)k; q->wtotal += k; done += k;
            sync_irq_restore(irq);
            if (n == 0) break;
        } else {
            sync_irq_restore(irq);
            if (nb) return done ? (int64_t)done : -L_EAGAIN;
            if (sig_interrupt(c)) return done ? (int64_t)done : -L_EINTR;
            scheduler_sleep(1);
        }
    }
    return (int64_t)done;
}
static int64_t upair_recv(lctx_t *c, ofd_t *o, void *dst, uint64_t n, bool nb, bool peek, ofd_t **fds, int *nfds, bool *trunc) {
    *nfds = 0; *trunc = false;
    if (!o->pipe) return -107;
    pipe_t *q = o->pipe;
    uint64_t irq;
    for (;;) {
        irq = sync_irq_save();
        if (q->len) break;                                                    /* leaves with the kernel lock held */
        bool eof = q->writers <= 0 || (o->ushut & 2);
        sync_irq_restore(irq);
        if (eof) return 0;
        if (nb) return -L_EAGAIN;
        if (sig_interrupt(c)) return -L_EINTR;
        scheduler_sleep(1);
    }
    uint64_t take, adv, skip = 0;
    if (q->packet) {
        uint32_t h = 0; for (int i = 0; i < 4; ++i) h |= (uint32_t)q->buf[(q->head + i) % q->cap] << (8 * i);
        take = n < h ? n : h; *trunc = n < h; adv = 4 + (uint64_t)h; skip = 4;
    } else {
        take = n < q->len ? n : q->len;
        for (int i = 0; i < q->nanc; ++i) if (q->anc[i].at > q->rtotal) { if (q->anc[i].at - q->rtotal < take) take = q->anc[i].at - q->rtotal; break; }   /* data of the next message with descriptors is read separately */
        adv = take;
    }
    for (uint64_t i = 0; i < take; ++i) ((uint8_t *)dst)[i] = q->buf[(q->head + skip + i) % q->cap];
    if (!peek) {
        if (q->nanc && q->anc[0].at == q->rtotal) {
            int cnt = q->anc[0].n; ofd_t *tmp[LX_ANC_FDS];
            for (int i = 0; i < cnt; ++i) tmp[i] = q->anc[0].f[i];
            for (int i = 1; i < q->nanc; ++i) q->anc[i - 1] = q->anc[i];
            q->nanc--;
            for (int i = 0; i < cnt; ++i) { if (fds) fds[(*nfds)++] = tmp[i]; else ofd_unref(tmp[i]); }
        }
        q->head = (uint32_t)((q->head + adv) % q->cap); q->len -= (uint32_t)adv; q->rtotal += adv;
    }
    sync_irq_restore(irq);
    return (int64_t)take;
}
/* sockaddr_un <-> the registry name */
static int un_get(uint64_t ua, uint64_t len, char *name) {
    if (len < 3 || len > 110) return -L_EINVAL;
    uint8_t b[112]; memset(b, 0, sizeof b); if (!copy_in(b, ua, len)) return -L_EFAULT;
    uint64_t pl = len - 2;
    if (b[2] == 0) { name[0] = '@'; uint64_t k = pl - 1 > 106 ? 106 : pl - 1; memcpy(name + 1, b + 3, k); name[1 + k] = 0; }
    else { uint64_t k = 0; while (k < pl && k < 107 && b[2 + k]) { name[k] = (char)b[2 + k]; ++k; } name[k] = 0; }
    return 0;
}
static int64_t un_put(const char *name, uint64_t ua, uint64_t lenp) {
    if (!ua || !lenp) return 0;
    uint8_t b[112]; memset(b, 0, sizeof b); b[0] = 1; uint32_t want = 2;
    if (name && name[0]) { uint32_t nl = (uint32_t)strlen(name); if (name[0] == '@') { memcpy(b + 3, name + 1, nl - 1); want = 2 + nl; } else { memcpy(b + 2, name, nl); want = 3 + nl; } }
    uint32_t have = 0; if (!copy_in(&have, lenp, 4)) return -L_EFAULT;
    if (!copy_out(ua, b, have < want ? have : want)) return -L_EFAULT;
    return copy_out(lenp, &want, 4) ? 0 : -L_EFAULT;
}
static int64_t unix_socket(lctx_t *c, uint32_t type) {
    uint32_t t = type & 0xF; if (t != 1 && t != 2 && t != 5) return -L_EPROTONOSUPPORT;
    int n = fd_alloc_from(c, 0); if (n < 0) return -L_EMFILE;
    ofd_t *o = ofd_new(FD_UPAIR, (type & 0x800) ? L_O_NONBLOCK : 0); if (!o) return -L_ENOMEM;
    o->counter = t; c->fd[n] = o; c->cloexec[n] = (type & 0x80000) != 0; return n;
}
static int64_t unix_bind(ofd_t *o, uint64_t ua, uint64_t len) {
    char name[108]; int e = un_get(ua, len, name); if (e) return e;
    if (o->lis || o->pipe) return -L_EINVAL;
    int slot = -1;
    for (int i = 0; i < 64; ++i) { if (ubind[i].l && !strcmp(ubind[i].name, name)) return -98; if (!ubind[i].l && slot < 0) slot = i; }   /* EADDRINUSE */
    if (slot < 0) return -L_ENOMEM;
    ulis_t *l = (ulis_t *)kmalloc(sizeof *l); if (!l) return -L_ENOMEM;
    memset(l, 0, sizeof *l); o->lis = l; ubind[slot].l = l; strncpy(ubind[slot].name, name, 107); ubind[slot].name[107] = 0;
    strncpy(o->path, name, 255); o->path[255] = 0;
    return 0;
}
static int64_t unix_connect(lctx_t *c, ofd_t *o, uint64_t ua, uint64_t len) {
    (void)c;
    char name[108]; int e = un_get(ua, len, name); if (e) return e;
    if (o->pipe) return -106;                         /* EISCONN */
    ulis_t *l = NULL, *any = NULL;
    for (int i = 0; i < 64; ++i) if (ubind[i].l && !strcmp(ubind[i].name, name)) { any = ubind[i].l; if (any->listening) l = any; }
    if (!any) return -L_ENOENT;
    if (!l || l->nq >= 16) return -111;               /* ECONNREFUSED */
    pipe_t *p1 = pipe_new(), *p2 = pipe_new(); ofd_t *sv = ofd_new(FD_UPAIR, L_O_RDWR);
    if (!p1 || !p2 || !sv) { if (p1) pipe_free(p1); if (p2) pipe_free(p2); if (sv) kfree(sv); return -L_ENOMEM; }
    p1->readers = p1->writers = p2->readers = p2->writers = 1; p1->packet = p2->packet = o->counter != 1;
    o->pipe = p2; o->pipe2 = p1; sv->pipe = p1; sv->pipe2 = p2; sv->counter = o->counter;
    l->q[l->nq++] = sv; return 0;
}
static int64_t unix_accept(lctx_t *c, ofd_t *o, uint32_t flags, uint64_t ua, uint64_t lenp) {
    if (!o->lis || !o->lis->listening) return -L_EINVAL;
    for (;;) {
        if (o->lis->nq) break;
        if (o->flags & L_O_NONBLOCK) return -L_EAGAIN;
        if (sig_interrupt(c)) return -L_EINTR;
        scheduler_sleep(1);
    }
    int n = fd_alloc_from(c, 0); if (n < 0) return -L_EMFILE;
    ofd_t *sv = o->lis->q[0]; for (int i = 1; i < o->lis->nq; ++i) o->lis->q[i - 1] = o->lis->q[i]; o->lis->nq--;
    if (flags & 0x800) sv->flags |= L_O_NONBLOCK;
    c->fd[n] = sv; c->cloexec[n] = (flags & 0x80000) != 0;
    un_put(NULL, ua, lenp); return n;
}
static int64_t upair_msg(process_t *p, lctx_t *c, ofd_t *o, uint64_t um, uint32_t flags, bool sending) {
    (void)p;
    uint8_t mh[56]; if (!copy_in(mh, um, sizeof mh)) return -L_EFAULT;
    uint64_t iov = *(uint64_t *)(mh + 16), iovn = *(uint64_t *)(mh + 24), ctl = *(uint64_t *)(mh + 32), ctll = *(uint64_t *)(mh + 40);
    if (iovn > 64) return -L_EINVAL;
    bool nb = (o->flags & L_O_NONBLOCK) || (flags & 0x40);
    uint8_t *buf = (uint8_t *)kmalloc(65536); if (!buf) return -L_ENOMEM;
    uint64_t tot = 0; int64_t res = 0; ofd_t *fds[LX_ANC_FDS]; int nf = 0;
    if (sending) {
        for (uint64_t i = 0; i < iovn && res == 0; ++i) {
            uint64_t v[2]; if (!copy_in(v, iov + i * 16, 16)) { res = -L_EFAULT; break; }
            uint64_t k = v[1] > 65536 - tot ? 65536 - tot : v[1];
            if (k && !copy_in(buf + tot, v[0], k)) { res = -L_EFAULT; break; }
            tot += k;
        }
        if (!res && ctl && ctll >= 16) {
            uint64_t cl = ctll > 1024 ? 1024 : ctll; uint8_t *cb = (uint8_t *)kmalloc(cl);
            if (!cb || !copy_in(cb, ctl, cl)) res = -L_EFAULT;
            else for (uint64_t off = 0; off + 16 <= cl && !res;) {
                uint64_t len = *(uint64_t *)(cb + off); int32_t lvl = *(int32_t *)(cb + off + 8), ty = *(int32_t *)(cb + off + 12);
                if (len < 16 || off + len > cl) break;
                if (lvl == 1 && ty == 1) for (uint64_t i = 0; i < (len - 16) / 4; ++i) {
                    ofd_t *f = F(c, *(int32_t *)(cb + off + 16 + 4 * i));
                    if (!f) { res = -L_EBADF; break; }
                    if (nf >= LX_ANC_FDS) { res = -L_EINVAL; break; }
                    f->ref++; fds[nf++] = f;
                }
                off += (len + 7) & ~7ull;
            }
            if (cb) kfree(cb);
        }
        if (!res) res = upair_send(c, o, buf, tot, nb, fds, nf);
        if (res < 0) for (int i = 0; i < nf; ++i) ofd_unref(fds[i]);
    } else {
        for (uint64_t i = 0; i < iovn; ++i) { uint64_t v[2]; if (!copy_in(v, iov + i * 16, 16)) { res = -L_EFAULT; break; } tot += v[1]; }
        if (!res) {
            bool tr = false;
            res = upair_recv(c, o, buf, tot > 65536 ? 65536 : tot, nb, (flags & 2) != 0, fds, &nf, &tr);
            uint32_t mf = tr ? 0x20 : 0; uint64_t outl = 0;
            if (res >= 0) {
                uint64_t left = (uint64_t)res, off = 0;
                for (uint64_t i = 0; i < iovn && left; ++i) {
                    uint64_t v[2]; if (!copy_in(v, iov + i * 16, 16)) break;
                    uint64_t k = v[1] < left ? v[1] : left;
                    if (k && !copy_out(v[0], buf + off, k)) { res = -L_EFAULT; break; }
                    off += k; left -= k;
                }
                if (nf) {
                    uint64_t fit = ctl && ctll >= 20 ? (ctll - 16) / 4 : 0; if (fit > (uint64_t)nf) fit = (uint64_t)nf;
                    int got = 0; int32_t ints[LX_ANC_FDS];
                    for (uint64_t i = 0; i < fit; ++i) {
                        int slot = fd_alloc_from(c, 0); if (slot < 0) break;
                        c->fd[slot] = fds[i]; c->cloexec[slot] = (flags & 0x40000000) != 0; ints[got++] = slot;
                    }
                    for (int i = got; i < nf; ++i) { ofd_unref(fds[i]); mf |= 8; }
                    if (got) {
                        uint8_t cm[16 + 4 * LX_ANC_FDS]; uint64_t cl = 16 + 4 * (uint64_t)got; memset(cm, 0, sizeof cm);
                        *(uint64_t *)cm = cl; *(int32_t *)(cm + 8) = 1; *(int32_t *)(cm + 12) = 1; memcpy(cm + 16, ints, 4 * (uint64_t)got);
                        copy_out(ctl, cm, cl); outl = (cl + 7) & ~7ull;
                    }
                }
            }
            uint32_t zero = 0; copy_out(um + 8, &zero, 4); copy_out(um + 40, &outl, 8); copy_out(um + 48, &mf, 4);
        }
    }
    if (s_trace_all > 0) { serial_puts("[LXM] "); serial_putdec(p->pid); serial_puts(sending ? " sendmsg" : " recvmsg"); serial_puts(" bytes="); serial_putdec((uint64_t)res); serial_puts(" nfds="); serial_putdec((uint64_t)nf); serial_puts(" ctl="); serial_putdec(ctll); serial_puts(" ty="); serial_putdec(o->counter); serial_puts(" lvl pk="); serial_putdec(o->pipe ? o->pipe->packet : 9); serial_puts("\n"); }
    kfree(buf); return res;
}

static int64_t linux_syscall(process_t *p, lctx_t *c, struct regs *r) {
    uint64_t nr = r->rax, a0 = r->rdi, a1 = r->rsi, a2 = r->rdx, a3 = r->r10, a4 = r->r8, a5 = r->r9;
    switch (nr) {
    case 0: return user_read(p, c, (int)a0, a1, a2, false, 0);
    case 1: return user_write(p, c, (int)a0, a1, a2, false, 0);
    case 17: return user_read(p, c, (int)a0, a1, a2, true, a3);
    case 18: return user_write(p, c, (int)a0, a1, a2, true, a3);
    case 20: case 19: {  /* writev, readv */
        int64_t total = 0;
        for (uint64_t i = 0; i < a2 && i < 64; ++i) {
            uint64_t iov[2]; if (!copy_in(iov, a1 + i * 16, 16)) return total ? total : -L_EFAULT;
            if (!iov[1]) continue;
            int64_t w = nr == 20 ? user_write(p, c, (int)a0, iov[0], iov[1], false, 0) : user_read(p, c, (int)a0, iov[0], iov[1], false, 0);
            if (w < 0) return total ? total : w;
            total += w; if ((uint64_t)w < iov[1]) break;
        }
        return total;
    }
    case 2: return do_open(p, c, L_AT_FDCWD, (const char *)(uintptr_t)a0, (uint32_t)a1);
    case 257: return do_open(p, c, (int)a0, (const char *)(uintptr_t)a1, (uint32_t)a2);
    case 3: { if (!F(c, (int)a0)) return -L_EBADF; fd_close_slot(c, (int)a0); return 0; }
    case 8: {   /* lseek */
        ofd_t *f = F(c, (int)a0); if (!f) return -L_EBADF;
        if (f->kind == FD_MEM || f->kind == FD_DEV || f->kind == FD_MEMFD) {
            int64_t np2 = a2 == 0 ? (int64_t)a1 : a2 == 1 ? (int64_t)f->pos + (int64_t)a1 : a2 == 2 ? (int64_t)(f->kind == FD_MEMFD ? f->shm->size : f->memlen) + (int64_t)a1 : -1;
            if (np2 < 0) return -L_EINVAL;
            f->pos = (uint64_t)np2; return np2;
        }
        if (f->kind != FD_FILE) return -L_ESPIPE;
        uint64_t size = 0; uint8_t isdir = 0; cos_fs_stat(f->path, &size, &isdir);
        int64_t off = (int64_t)a1, np;
        if (a2 == 0) np = off; else if (a2 == 1) np = (int64_t)f->pos + off; else if (a2 == 2) np = (int64_t)size + off; else return -L_EINVAL;
        if (np < 0) return -L_EINVAL;
        f->pos = (uint64_t)np; return np;
    }
    case 4: case 6: case 262: {  /* stat, lstat, newfstatat */
        char raw[300], path[256]; int dirfd = L_AT_FDCWD; uint64_t up = a0, ub = a1;
        if (nr == 262) { dirfd = (int)a0; up = a1; ub = a2; if (!copy_str(raw, up, sizeof raw)) return -L_EFAULT;
            if (raw[0] == 0 && (a3 & 0x1000)) { /* AT_EMPTY_PATH: fstat of the descriptor */
                struct regs t = *r; t.rax = 5; t.rdi = a0; t.rsi = a2; return linux_syscall(p, c, &t); } }
        else if (!copy_str(raw, up, sizeof raw)) return -L_EFAULT;
        const lxm_ent_t *lk = NULL; int nofollow = nr == 6 || (nr == 262 && (a3 & 0x100));
        resolve2(c, dirfd, raw, path, sizeof path, !nofollow, &lk); vfs_canon(p, path, sizeof path);
        if (lk) {                                                                   /* lstat of a link */
            uint8_t st[144]; memset(st, 0, sizeof st); const char *tg = lxm_str + lk->target_off;
            uint64_t ino = lxm_hash(path, strlen(path));
            *(uint64_t *)(st + 0) = 1; *(uint64_t *)(st + 8) = ino; *(uint64_t *)(st + 16) = 1; *(uint32_t *)(st + 24) = 0120777u; *(uint64_t *)(st + 48) = strlen(tg); *(uint64_t *)(st + 56) = 4096;
            return copy_out(ub, st, sizeof st) ? 0 : -L_EFAULT;
        }
        uint64_t size = 0; uint8_t isdir = 0;
        { int vid, vk = strncmp(path, "/linux", 6) ? vfs_classify(path, &vid) : VFS_NONE;
          if (vk) { int64_t rr = do_stat(p, c, path, vk == VFS_DIR, 0, ub); if (rr == 0 && vk == VFS_DEV) { uint32_t m = 0020666u; copy_out(ub + 24, &m, 4); } if (rr == 0 && vk == VFS_PROC) { uint32_t m = 0100444u; copy_out(ub + 24, &m, 4); } return rr; } }
        if (!cos_fs_stat(path, &size, &isdir)) return -L_ENOENT;
        return do_stat(p, c, path, isdir, size, ub);
    }
    case 332: {  /* statx(dirfd, path, flags, mask, buf): a stat into the buffer, rewritten in statx layout */
        struct regs t = *r; t.rax = 262; t.rdi = a0; t.rsi = a1; t.rdx = a4; t.r10 = a2 & 0x1100;
        int64_t rr = linux_syscall(p, c, &t); if (rr < 0) return rr;
        uint8_t st[144], sx[256]; if (!copy_in(st, a4, sizeof st)) return -L_EFAULT;
        memset(sx, 0, sizeof sx);
        *(uint32_t *)(sx + 0) = 0x7FF; *(uint32_t *)(sx + 4) = *(uint32_t *)(st + 56); *(uint32_t *)(sx + 16) = (uint32_t)*(uint64_t *)(st + 16);
        *(uint32_t *)(sx + 20) = *(uint32_t *)(st + 28); *(uint32_t *)(sx + 24) = *(uint32_t *)(st + 32); *(uint16_t *)(sx + 28) = (uint16_t)*(uint32_t *)(st + 24);
        *(uint64_t *)(sx + 32) = *(uint64_t *)(st + 8); *(uint64_t *)(sx + 40) = *(uint64_t *)(st + 48); *(uint64_t *)(sx + 48) = *(uint64_t *)(st + 64);
        memcpy(sx + 64, st + 72, 8); memcpy(sx + 96, st + 104, 8); memcpy(sx + 112, st + 88, 8);
        *(uint32_t *)(sx + 68) = (uint32_t)*(uint64_t *)(st + 80); *(uint32_t *)(sx + 100) = (uint32_t)*(uint64_t *)(st + 112); *(uint32_t *)(sx + 116) = (uint32_t)*(uint64_t *)(st + 96);
        return copy_out(a4, sx, sizeof sx) ? 0 : -L_EFAULT;
    }
    case 307: case 299: {  /* sendmmsg, recvmmsg: one message at a time */
        int64_t done = 0;
        for (uint64_t i = 0; i < a2 && i < 64; ++i) {
            struct regs t = *r; t.rax = nr == 307 ? 46 : 47; t.rdi = a0; t.rsi = a1 + i * 64; t.rdx = nr == 307 ? a3 : (a3 | (i ? 0x40 : 0));
            int64_t rr = linux_syscall(p, c, &t);
            if (rr < 0) return done ? done : rr;
            uint32_t len = (uint32_t)rr; if (!copy_out(a1 + i * 64 + 56, &len, 4)) return -L_EFAULT;
            done++;
        }
        return done;
    }
    case 5: {   /* fstat */
        ofd_t *f = F(c, (int)a0); if (!f) return -L_EBADF;
        if (f->kind == FD_FILE || f->kind == FD_DIR) { uint64_t size = 0; uint8_t isdir = 0; cos_fs_stat(f->path, &size, &isdir); return do_stat(p, c, f->path, f->kind == FD_DIR, size, a1); }
        uint8_t st[144]; memset(st, 0, sizeof st);
        if (f->kind == FD_MEMFD) { *(uint32_t *)(st + 24) = 0100600u; *(uint64_t *)(st + 16) = 1; *(uint64_t *)(st + 48) = f->shm->size; *(uint64_t *)(st + 56) = 4096; *(uint64_t *)(st + 64) = (f->shm->size + 511) / 512; *(uint64_t *)(st + 8) = (uint64_t)(uintptr_t)f->shm >> 4; return copy_out(a1, st, sizeof st) ? 0 : -L_EFAULT; }
        uint32_t mode = f->kind == FD_VDIR ? 0040555u : f->kind == FD_MEM ? 0100444u : f->kind == FD_SOCK || f->kind == FD_UPAIR ? 0140000u | 0777u : f->kind == FD_PIPE_R || f->kind == FD_PIPE_W ? 0010000u | 0600u : 0020000u | 0620u;      /* a FIFO, or the console: a character device */
        *(uint32_t *)(st + 24) = mode; *(uint64_t *)(st + 16) = 1; *(uint64_t *)(st + 56) = 4096;
        return copy_out(a1, st, sizeof st) ? 0 : -L_EFAULT;
    }
    case 21: case 269: {   /* access, faccessat */
        char raw[300], path[256]; int dirfd = nr == 269 ? (int)a0 : L_AT_FDCWD;
        if (!copy_str(raw, nr == 269 ? a1 : a0, sizeof raw)) return -L_EFAULT;
        resolve(c, dirfd, raw, path, sizeof path); vfs_canon(p, path, sizeof path);
        uint64_t size; uint8_t isdir; { int vid; if (strncmp(path, "/linux", 6) && vfs_classify(path, &vid)) return 0; }
        return cos_fs_stat(path, &size, &isdir) ? 0 : -L_ENOENT;
    }
    case 79: {  /* getcwd */
        size_t n = strlen(c->cwd) + 1; if (a1 < n) return -L_ERANGE;
        return copy_out(a0, c->cwd, n) ? (int64_t)n : -L_EFAULT;
    }
    case 80: {  /* chdir */
        char raw[300], path[256]; if (!copy_str(raw, a0, sizeof raw)) return -L_EFAULT;
        resolve(c, L_AT_FDCWD, raw, path, sizeof path);
        uint64_t size; uint8_t isdir; if (!cos_fs_stat(path, &size, &isdir) || !isdir) return -L_ENOENT;
        char norm[320]; path_normalize(raw[0] == '/' ? raw : path, norm, sizeof norm);
        snprintf(c->cwd, sizeof c->cwd, "%s", raw[0] == '/' ? norm : path); return 0;
    }
    case 83: { char raw[300], path[256]; if (!fs_ok(p, COS_CAP_FS_WRITE)) return -L_EACCES; if (!copy_str(raw, a0, sizeof raw)) return -L_EFAULT; resolve(c, L_AT_FDCWD, raw, path, sizeof path); return cos_fs_mkdir(path) ? 0 : -L_EEXIST; }
    case 87: case 263: { char raw[300], path[256]; if (!fs_ok(p, COS_CAP_FS_WRITE)) return -L_EACCES; if (!copy_str(raw, nr == 263 ? a1 : a0, sizeof raw)) return -L_EFAULT; resolve(c, nr == 263 ? (int)a0 : L_AT_FDCWD, raw, path, sizeof path);
        if (!strncmp(path, "/dev/shm/", 9)) {                                          /* shm_unlink() */
            lx_shm_t *sh = NULL; uint64_t fl = sync_irq_save();
            for (lx_shm_t **pp = &s_shm_named; *pp; pp = &(*pp)->next) if (!strcmp((*pp)->name, path + 9)) { sh = *pp; *pp = sh->next; break; }
            sync_irq_restore(fl);
            if (!sh) return -L_ENOENT;
            shm_unref(sh); return 0;
        }
        return cos_fs_unlink(path) ? 0 : -L_ENOENT; }
    case 84: { char raw[300], path[256]; if (!fs_ok(p, COS_CAP_FS_WRITE)) return -L_EACCES; if (!copy_str(raw, a0, sizeof raw)) return -L_EFAULT; resolve(c, L_AT_FDCWD, raw, path, sizeof path); return cos_fs_unlink(path) ? 0 : -L_ENOENT; }   /* rmdir */
    case 88: case 266: return -L_EPERM;   /* symlink: a FAT volume has none (programs fall back) */
    case 86: case 265: {   /* link / linkat: a copy of a small regular file (enough for X's lock files) */
        char ra[200], rb[200], pa[256], pb[256]; if (!fs_ok(p, COS_CAP_FS_WRITE)) return -L_EACCES;
        uint64_t ua = nr == 86 ? a0 : a1, ub = nr == 86 ? a1 : a3; int da = nr == 86 ? L_AT_FDCWD : (int)a0, db = nr == 86 ? L_AT_FDCWD : (int)a2;
        if (!copy_str(ra, ua, sizeof ra) || !copy_str(rb, ub, sizeof rb)) return -L_EFAULT;
        resolve(c, da, ra, pa, sizeof pa); resolve(c, db, rb, pb, sizeof pb);
        uint64_t sz = 0; uint8_t dir = 0, d2 = 0; uint64_t s2 = 0;
        if (!cos_fs_stat(pa, &sz, &dir)) return -L_ENOENT; if (dir || sz > (1u << 20)) return -L_EPERM;
        if (cos_fs_stat(pb, &s2, &d2)) return -L_EEXIST;
        uint8_t *buf = (uint8_t *)kmalloc(sz ? sz : 1); if (!buf) return -L_ENOMEM;
        int got = sz ? cos_fs_read_file(pa, buf, sz) : 0; int ok = got >= 0 && cos_fs_write_file(pb, buf, (uint64_t)got) >= 0; kfree(buf);
        return ok ? 0 : -L_EPERM;
    }
    case 27: { uint64_t n = (a1 + PAGE - 1) / PAGE; uint8_t one[256]; memset(one, 1, sizeof one); for (uint64_t i = 0; i < n; i += 256) { uint64_t k = n - i < 256 ? n - i : 256; if (!copy_out(a2 + i, one, k)) return -L_EFAULT; } return 0; }   /* mincore: everything is resident */
    case 26: return 0;   /* msync */
    case 105: case 106: case 113: case 114: case 116: case 122: case 123: return 0;      /* setuid, setgid, setreuid, setregid, setgroups, setfsuid, setfsgid: everything is root already */
    case 115: return 0;                                                                       /* getgroups: none */
    case 117: case 119: return 0;                                                             /* setresuid, setresgid */
    case 118: case 120: { uint32_t z[3] = { 0, 0, 0 }; return copy_out(a0, &z[0], 4) && copy_out(a1, &z[1], 4) && copy_out(a2, &z[2], 4) ? 0 : -L_EFAULT; }   /* getresuid, getresgid */
    case 253: case 294: {   /* inotify_init, inotify_init1: a descriptor that never reports anything (nothing in this file system changes behind a program's back) */
        int n = fd_alloc_from(c, 0); if (n < 0) return -L_EMFILE;
        ofd_t *o = ofd_new(FD_EVENT, nr == 294 ? (uint32_t)a0 & L_O_NONBLOCK : 0); if (!o) return -L_ENOMEM;
        o->counter = 0; o->efd_flags = 0; c->fd[n] = o; c->cloexec[n] = nr == 294 && (a0 & 0x80000) != 0; return n;
    }
    case 254: { static int wd = 0; return F(c, (int)a0) ? ++wd : -L_EBADF; }   /* inotify_add_watch */
    case 255: return 0;                                                       /* inotify_rm_watch */
    case 235: case 132: return 0;                                             /* utimes, utime */
    case 187: case 221: return 0;   /* readahead, fadvise64 */
    case 315: { uint32_t sz = (uint32_t)a2; uint8_t b[56]; memset(b, 0, sizeof b); *(uint32_t *)b = 56; if (sz > 56) sz = 56; return copy_out(a1, b, sz) ? 0 : -L_EFAULT; }   /* sched_getattr: SCHED_OTHER */
    case 89: case 267: {   /* readlink: nothing is a link, except /proc/self/exe */
        char raw[300]; uint64_t up = nr == 267 ? a1 : a0, ub = nr == 267 ? a2 : a1, ul = nr == 267 ? a3 : a2;
        if (!copy_str(raw, up, sizeof raw)) return -L_EFAULT;
        { const char *fdp = strncmp(raw, "/proc/self/fd/", 14) == 0 ? raw + 14 : NULL;
          if (fdp || !strcmp(raw, "/proc/self/cwd")) {
            char tgt[200];
            if (fdp) { int n = 0; for (const char *q = fdp; *q >= '0' && *q <= '9'; ++q) n = n * 10 + (*q - '0'); ofd_t *o = F(c, n); if (!o) return -L_ENOENT;
                if (o->path[0]) snprintf(tgt, sizeof tgt, "%s", o->path);
                else snprintf(tgt, sizeof tgt, "%s:[%d]", o->kind == FD_SOCK ? "socket" : o->kind == FD_EVENT || o->kind == FD_EPOLL ? "anon_inode" : "pipe", n); }
            else snprintf(tgt, sizeof tgt, "%s", c->cwd);
            size_t n = strlen(tgt); if (n > ul) n = ul; return copy_out(ub, tgt, n) ? (int64_t)n : -L_EFAULT; } }
        if (!strcmp(raw, "/proc/self/exe") && c->exe[0]) { const char *ex = !strncmp(c->exe, "/linux/", 7) ? c->exe + 6 : c->exe; size_t n = strlen(ex); if (n > ul) n = ul; return copy_out(ub, ex, n) ? (int64_t)n : -L_EFAULT; }
        { char rp[256]; const lxm_ent_t *lk = NULL; resolve2(c, nr == 267 ? (int)a0 : L_AT_FDCWD, raw, rp, sizeof rp, 0, &lk);
          if (lk) { const char *tg = lxm_str + lk->target_off; size_t n = strlen(tg); if (n > ul) n = ul; return copy_out(ub, tg, n) ? (int64_t)n : -L_EFAULT; } }
        return -L_EINVAL;
    }
    case 217: case 78: {   /* getdents64, getdents */
        ofd_t *df = F(c, (int)a0); if (!df || (df->kind != FD_DIR && df->kind != FD_VDIR)) return -L_EBADF;
        uint8_t *out = (uint8_t *)kmalloc(a2 > 8192 ? 8192 : a2 + 1); if (!out) return -L_ENOMEM;
        uint64_t cap = a2 > 8192 ? 8192 : a2, used = 0;
        for (;;) {
            cos_dirent_t d; int64_t rr;
            if (df->kind == FD_VDIR) {
                if (df->pos >= df->memlen) break;
                const char *rec = (const char *)df->mem + df->pos; d.is_dir = rec[0] == 'd'; snprintf(d.name, sizeof d.name, "%s", rec + 1);
                size_t rl0 = (19 + strlen(d.name) + 1 + 7) & ~7ull; if (used + rl0 > cap) break;
                df->pos += 1 + strlen(rec + 1) + 1; rr = 1;
            } else rr = cos_fd_readdir(df->owner, df->cosfd, &d);
            if (rr <= 0) break;
            size_t nl = strlen(d.name); uint64_t rl = (19 + nl + 1 + 7) & ~7ull;
            if (used + rl > cap) { /* does not fit: the entry is lost, so stop before reading it is impossible; keep it simple and cap early */ break; }
            memset(out + used, 0, rl);
            *(uint64_t *)(out + used) = 1; *(int64_t *)(out + used + 8) = (int64_t)(used + rl);
            *(uint16_t *)(out + used + 16) = (uint16_t)rl; out[used + 18] = d.is_dir ? 4 : 8;
            memcpy(out + used + 19, d.name, nl);
            used += rl;
        }
        int64_t res = used ? (copy_out(a1, out, used) ? (int64_t)used : -L_EFAULT) : 0;
        kfree(out); return res;
    }
    case 9: return sys_mmap(p, c, a0, a1, (uint32_t)a2, (uint32_t)a3, (int)a4, a5);
    case 10: {  /* mprotect */
        if (a0 & (PAGE - 1)) return -L_EINVAL;
        uint64_t e = pg_up(a0 + a1);
        for (uint64_t a = a0; a < e; a += PAGE) if (!vma_find(c, a) && !(c->brk_start && a >= c->brk_start && a < pg_up(c->brk_cur))) return -L_ENOMEM;
        vma_protect(c, a0, e, (uint32_t)a2); return 0;
    }
    case 11: {  /* munmap */
        if (a0 & (PAGE - 1) || a1 == 0) return -L_EINVAL;
        vma_cut(c, a0, pg_up(a0 + a1), true); return 0;
    }
    case 25: {  /* mremap: only what a realloc of a mapping needs, by moving */
        lvma_t *v = vma_find(c, a0); if (!v || v->start != a0) return -L_EINVAL;
        uint64_t old = pg_up(a1), nw = pg_up(a2); uint32_t prot = v->prot;
        if (nw <= old) { vma_cut(c, a0 + nw, a0 + old, true); return (int64_t)a0; }
        if (vma_free(c, a0 + old, a0 + nw)) { vma_insert(c, a0 + old, a0 + nw, prot); /* merge not needed: lookups scan */ return (int64_t)a0; }
        if (!(a3 & 1)) return -L_ENOMEM;            /* MREMAP_MAYMOVE */
        int64_t to = sys_mmap(p, c, 0, nw, prot, L_MAP_ANON, -1, 0); if (to < 0) return to;
        for (uint64_t o = 0; o < old; o += PAGE) {
            if (!paging_virt_to_phys(a0 + o)) continue;
            uint8_t *buf = (uint8_t *)kmalloc(PAGE); if (!buf) return -L_ENOMEM;
            copy_in(buf, a0 + o, PAGE); copy_out((uint64_t)to + o, buf, PAGE); kfree(buf);
        }
        vma_cut(c, a0, a0 + old, true); return to;
    }
    case 12: {  /* brk */
        if (a0 == 0 || a0 < c->brk_start) return (int64_t)c->brk_cur;
        if (a0 > c->brk_max) return (int64_t)c->brk_cur;
        if (a0 < c->brk_cur) { uint64_t ne = pg_up(a0), oe = pg_up(c->brk_cur); if (oe > ne) paging_release_user_range(ne, (oe - ne) / PAGE); }
        c->brk_cur = a0; return (int64_t)c->brk_cur;
    }
    case 28: {   /* madvise: MADV_DONTNEED gives anonymous private pages back (they read as zeros afterwards, which malloc relies on); the other advice is accepted */
        if (a2 != 4 || !a1) return 0;
        { static int nomadv = -1; if (nomadv < 0) { uint64_t sz; uint8_t d; nomadv = cos_fs_stat("/linux/etc/cos.nomadv", &sz, &d) ? 1 : 0; } if (nomadv) return 0; }   /* debugging switch */
        uint64_t s0 = pg_down(a0), e0 = pg_up(a0 + a1);
        for (uint64_t at = s0; at < e0; at += PAGE) {
            lvma_t *v = vma_find(c, at); if (!v || v->shm) continue;
            uint64_t end = e0 < v->end ? e0 : v->end;
            if (v->shm_off & LX_VMA_FILE) { at = end - PAGE; continue; }       /* a file mapping: its pages would have to be read again */
            paging_release_user_range(at, (end - at) / PAGE); at = end - PAGE;
        }
        return 0;
    }
    case 131: return 0;   /* sigaltstack: accepted, handlers run on the normal stack */
    case 16: {  /* ioctl */
        ofd_t *f = F(c, (int)a0); if (!f) return -L_EBADF;
        if (a1 == 0x5413 /* TIOCGWINSZ */ && f->kind == FD_CON) { uint16_t ws[4] = { 25, 80, 0, 0 }; return copy_out(a2, ws, 8) ? 0 : -L_EFAULT; }
        if (a1 == 0x5421 /* FIONBIO */) { int v; if (!copy_in(&v, a2, 4)) return -L_EFAULT; if (v) f->flags |= L_O_NONBLOCK; else f->flags &= ~L_O_NONBLOCK; return 0; }
        if (a1 == 0x541B /* FIONREAD */) { int v = f->kind == FD_PIPE_R ? (int)f->pipe->len : 0; return copy_out(a2, &v, 4) ? 0 : -L_EFAULT; }
        return -L_ENOTTY;
    }
    case 137: case 138: {   /* statfs, fstatfs: a large tmpfs-like volume (4 KiB blocks) */
        uint64_t sf[15]; memset(sf, 0, sizeof sf);
        sf[0] = 0x01021994; sf[1] = 4096; sf[2] = 1u << 20; sf[3] = 1u << 19; sf[4] = 1u << 19; sf[5] = 1u << 20; sf[6] = 1u << 19; sf[9] = 255; sf[10] = 4096;
        return copy_out(a1, sf, sizeof sf) ? 0 : -L_EFAULT;
    }
    case 140: case 141: return 0;                                                        /* getpriority, setpriority */
    case 319: {  /* memfd_create */
        char nm[64]; if (!copy_str(nm, a0, sizeof nm)) return -L_EFAULT;
        int n = fd_alloc_from(c, 0); if (n < 0) return -L_EMFILE;
        lx_shm_t *sh = shm_new(nm); if (!sh) return -L_ENOMEM;
        ofd_t *o = ofd_new(FD_MEMFD, L_O_RDWR); if (!o) { shm_unref(sh); return -L_ENOMEM; }
        o->shm = sh; c->fd[n] = o; c->cloexec[n] = (a1 & 1) != 0; return n;
    }
    case 77: case 285: {  /* ftruncate, fallocate */
        ofd_t *f = F(c, (int)a0); if (!f) return -L_EBADF;
        uint64_t nsz = nr == 77 ? a1 : a2 + a3;                          /* fallocate(fd, mode, offset, len) */
        if (f->kind == FD_MEMFD) { if (nr == 285 && nsz < f->shm->size) return 0; return shm_resize(f->shm, nsz) ? 0 : -L_ENOMEM; }
        if (f->kind == FD_FILE) {
            uint64_t size = 0; uint8_t isdir = 0; cos_fs_stat(f->path, &size, &isdir);
            if (nsz > size) { uint8_t z[4096]; memset(z, 0, sizeof z); cos_fd_lseek(f->owner, f->cosfd, (int64_t)size); for (uint64_t at = size; at < nsz; ) { uint64_t m = nsz - at < sizeof z ? nsz - at : sizeof z; if (cos_fd_write(f->owner, f->cosfd, z, m) < 0) return -L_ENOMEM; at += m; } }
            return 0;
        }
        return -L_EINVAL;
    }
    case 72: {  /* fcntl */
        ofd_t *f = F(c, (int)a0); if (!f) return -L_EBADF;
        if (a1 == 1033 /* F_ADD_SEALS */) return 0;
        if (a1 == 1034 /* F_GET_SEALS */) return 0;
        switch (a1) {
        case 0: case 1030: { int n = fd_alloc_from(c, (int)a2); if (n < 0) return -L_EMFILE; c->fd[n] = f; f->ref++; c->cloexec[n] = a1 == 1030; return n; }
        case 1: return c->cloexec[a0];
        case 2: c->cloexec[a0] = (a2 & 1) != 0; return 0;
        case 3: return f->flags;
        case 4: f->flags = (f->flags & ~(uint32_t)(L_O_NONBLOCK | L_O_APPEND)) | ((uint32_t)a2 & (L_O_NONBLOCK | L_O_APPEND)); return 0;
        default: return 0;
        }
    }
    case 32: { ofd_t *f = F(c, (int)a0); if (!f) return -L_EBADF; int n = fd_alloc_from(c, 0); if (n < 0) return -L_EMFILE; c->fd[n] = f; f->ref++; return n; }
    case 33: case 292: {  /* dup2, dup3 */
        ofd_t *f = F(c, (int)a0); if (!f) return -L_EBADF;
        if (a1 >= LX_MAX_FD) return -L_EBADF;
        if (a0 == a1) return nr == 292 ? -L_EINVAL : (int64_t)a1;
        fd_close_slot(c, (int)a1); c->fd[a1] = f; f->ref++; c->cloexec[a1] = nr == 292 && (a2 & 0x80000) ? 1 : 0; return (int64_t)a1;
    }
    case 22: case 293: {  /* pipe, pipe2 */
        int fds[2]; int fr = fd_alloc_from(c, 0); if (fr < 0) return -L_EMFILE;
        c->fd[fr] = (ofd_t *)1; int fw = fd_alloc_from(c, 0); c->fd[fr] = NULL; if (fw < 0) return -L_EMFILE;
        pipe_t *q = pipe_new(); ofd_t *ro = ofd_new(FD_PIPE_R, 0 | (nr == 293 ? (uint32_t)a1 & L_O_NONBLOCK : 0)), *wo = ofd_new(FD_PIPE_W, L_O_WRONLY | (nr == 293 ? (uint32_t)a1 & L_O_NONBLOCK : 0));
        if (!q || !ro || !wo) { if (ro) kfree(ro); if (wo) kfree(wo); return -L_ENOMEM; }
        ro->pipe = wo->pipe = q; q->readers = 1; q->writers = 1;
        c->fd[fr] = ro; c->fd[fw] = wo;
        c->cloexec[fr] = c->cloexec[fw] = nr == 293 && (a1 & 0x80000) ? 1 : 0;
        fds[0] = fr; fds[1] = fw; return copy_out(a0, fds, 8) ? 0 : -L_EFAULT;
    }
    case 284: case 290: {  /* eventfd, eventfd2 */
        int n = fd_alloc_from(c, 0); if (n < 0) return -L_EMFILE;
        uint32_t fl = nr == 290 ? (uint32_t)a1 : 0;
        ofd_t *o = ofd_new(FD_EVENT, fl & L_O_NONBLOCK); if (!o) return -L_ENOMEM;
        o->counter = (uint32_t)a0; o->efd_flags = fl; c->fd[n] = o; c->cloexec[n] = (fl & 0x80000) != 0; return n;
    }
    case 7: case 271: {  /* poll, ppoll */
        uint64_t nfds = a1 > 256 ? 256 : a1; uint8_t pf[256 * 8];
        if (!copy_in(pf, a0, nfds * 8)) return -L_EFAULT;
        int64_t tmo_ms = -1;
        if (nr == 7) tmo_ms = (int32_t)a2;
        else if (a2) { uint64_t ts[2]; if (!copy_in(ts, a2, 16)) return -L_EFAULT; tmo_ms = (int64_t)(ts[0] * 1000 + ts[1] / 1000000); }
        uint64_t start = now_ms();
        for (;;) {
            int ready = 0;
            for (uint64_t i = 0; i < nfds; ++i) {
                int fd = *(int32_t *)(pf + i * 8); uint16_t ev = *(uint16_t *)(pf + i * 8 + 4), rev = 0;
                if (fd >= 0) { ofd_t *o = F(c, fd); if (!o) rev = 0x20 /* POLLNVAL */; else rev = (uint16_t)(ofd_ready(o) & (ev | L_POLLERR | L_POLLHUP)); }
                *(uint16_t *)(pf + i * 8 + 6) = rev; if (rev) ready++;
            }
            if (ready || tmo_ms == 0 || (tmo_ms > 0 && now_ms() - start >= (uint64_t)tmo_ms)) { copy_out(a0, pf, nfds * 8); return ready; }
            if (sig_interrupt(c)) return -L_EINTR;
            wait_tick();
        }
    }
    case 23: case 270: {  /* select, pselect6 */
        int nfds = (int)a0; if (nfds < 0 || nfds > 1024) return -L_EINVAL;
        uint8_t in[3][128], out[3][128]; memset(in, 0, sizeof in);
        uint64_t sets[3] = { a1, a2, a3 };
        uint32_t nb = ((uint32_t)nfds + 7) / 8;
        for (int k = 0; k < 3; ++k) if (sets[k] && !copy_in(in[k], sets[k], nb)) return -L_EFAULT;
        int64_t tmo_ms = -1;
        if (a4) { uint64_t tv[2]; if (!copy_in(tv, a4, 16)) return -L_EFAULT; tmo_ms = (int64_t)(tv[0] * 1000 + (nr == 23 ? tv[1] / 1000 : tv[1] / 1000000)); }
        uint64_t start = now_ms();
        for (;;) {
            int ready = 0; memset(out, 0, sizeof out);
            for (int fd = 0; fd < nfds; ++fd) {
                ofd_t *o = F(c, fd); uint32_t rd = o ? ofd_ready(o) : 0;
                if (((in[0][fd >> 3] >> (fd & 7)) & 1) && (rd & (L_POLLIN | L_POLLHUP))) { out[0][fd >> 3] |= 1u << (fd & 7); ready++; }
                if (((in[1][fd >> 3] >> (fd & 7)) & 1) && (rd & (L_POLLOUT | L_POLLERR))) { out[1][fd >> 3] |= 1u << (fd & 7); ready++; }
            }
            if (ready || tmo_ms == 0 || (tmo_ms > 0 && now_ms() - start >= (uint64_t)tmo_ms)) {
                for (int k = 0; k < 3; ++k) if (sets[k]) copy_out(sets[k], out[k], nb);
                return ready;
            }
            if (sig_interrupt(c)) return -L_EINTR;
            wait_tick();
        }
    }
    case 213: case 291: {  /* epoll_create, epoll_create1 */
        int n = fd_alloc_from(c, 0); if (n < 0) return -L_EMFILE;
        ofd_t *o = ofd_new(FD_EPOLL, 0); if (!o) return -L_ENOMEM; c->fd[n] = o; c->cloexec[n] = nr == 291 && (a0 & 0x80000) ? 1 : 0; return n;
    }
    case 233: {  /* epoll_ctl */
        ofd_t *ep = F(c, (int)a0); if (!ep || ep->kind != FD_EPOLL) return -L_EBADF;
        int tfd = (int)a2; if (!F(c, tfd)) return -L_EBADF;
        int idx = -1; for (int i = 0; i < ep->nep; ++i) if (ep->ep[i].fd == tfd) idx = i;
        if (a1 == 2) { if (idx < 0) return -L_ENOENT; ep->ep[idx] = ep->ep[--ep->nep]; return 0; }
        uint8_t ev[12]; if (!copy_in(ev, a3, 12)) return -L_EFAULT;
        uint32_t events = *(uint32_t *)ev; uint64_t data = *(uint64_t *)(ev + 4);
        if (a1 == 1) { if (idx >= 0) return -L_EEXIST; if (!ep->ep) { ep->ep = (struct epent *)kmalloc(LX_EP_MAX * sizeof(struct epent)); if (!ep->ep) return -L_ENOMEM; } if (ep->nep >= LX_EP_MAX) return -L_ENOMEM; ep->ep[ep->nep].fd = tfd; ep->ep[ep->nep].ev = events; ep->ep[ep->nep].data = data; ep->ep[ep->nep].last = 0; ep->ep[ep->nep].lastver = 0; ep->nep++; return 0; }
        if (a1 == 3) { if (idx < 0) return -L_ENOENT; ep->ep[idx].ev = events; ep->ep[idx].data = data; ep->ep[idx].last = 0; ep->ep[idx].lastver = 0; return 0; }
        return -L_EINVAL;
    }
    case 232: case 281: {  /* epoll_wait, epoll_pwait */
        ofd_t *ep = F(c, (int)a0); if (!ep || ep->kind != FD_EPOLL) return -L_EBADF;
        int maxev = (int)a2; if (maxev <= 0) return -L_EINVAL; if (maxev > 128) maxev = 128;
        int64_t tmo_ms = (int32_t)a3; uint64_t start = now_ms();
        for (;;) {
            uint8_t out[128 * 12]; int n = 0;
            for (int i = 0; i < ep->nep && n < maxev; ++i) {
                ofd_t *t = F(c, ep->ep[i].fd); if (!t) continue;
                uint32_t rd = ofd_ready(t) & (ep->ep[i].ev | L_POLLERR | L_POLLHUP);
                if (ep->ep[i].ev & 0x80000000u) {                         /* EPOLLET: only what is new since the last report: a bit that came up, or fresh activity */
                    uint64_t v = ofd_version(t); uint32_t cur = rd;
                    rd = cur & (~ep->ep[i].last | (v != ep->ep[i].lastver ? ~0u : 0u));
                    ep->ep[i].last = cur; ep->ep[i].lastver = v;
                }
                if (!(ep->ep[i].ev & 0x3FFFFFFFu)) rd = 0;                                                       /* disabled by EPOLLONESHOT */
                if (rd && (ep->ep[i].ev & 0x40000000u)) ep->ep[i].ev &= 0xC0000000u;                      /* EPOLLONESHOT: reported once, off until EPOLL_CTL_MOD */
                if (rd) { *(uint32_t *)(out + n * 12) = rd; *(uint64_t *)(out + n * 12 + 4) = ep->ep[i].data; n++;
                          { extern uint32_t s_epdbg_n; extern uint32_t s_epdbg[8][5]; uint32_t *e = s_epdbg[s_epdbg_n++ & 7]; e[0] = (uint32_t)cur_tid(); e[1] = (uint32_t)ep->ep[i].fd; e[2] = t->kind; e[3] = ep->ep[i].ev; e[4] = rd; } }
            }
            if (n || tmo_ms == 0 || (tmo_ms > 0 && now_ms() - start >= (uint64_t)tmo_ms)) { if (n && !copy_out(a1, out, (uint64_t)n * 12)) return -L_EFAULT; return n; }
            if (sig_interrupt(c)) return -L_EINTR;
            wait_tick();
        }
    }
    case 24: scheduler_yield(); return 0;
    case 35: case 230: {  /* nanosleep, clock_nanosleep */
        uint64_t ts[2]; if (!copy_in(ts, nr == 35 ? a0 : a2, 16)) return -L_EFAULT;
        uint64_t ms = ts[0] * 1000 + ts[1] / 1000000;
        if (nr == 230 && (a1 & 1)) { uint64_t n = now_ms(); ms = ms > n ? ms - n : 0; }
        uint64_t end = now_ms() + ms;
        while (now_ms() < end) { if (sig_interrupt(c)) return -L_EINTR; uint64_t left = end - now_ms(); scheduler_sleep(left > 10 ? 10 : left); }
        return 0;
    }
    case 39: return (int64_t)p->pid;
    case 110: return (int64_t)p->parent_pid;
    case 186: return (int64_t)cur_tid();
    case 102: case 104: case 107: case 108: return 0;   /* uid, gid, euid, egid: root, as on a single-user device */
    case 63: {  /* uname */
        char u[6][65]; memset(u, 0, sizeof u);
        snprintf(u[0], 65, "Linux"); snprintf(u[1], 65, "c-os"); snprintf(u[2], 65, "6.1.0-cos"); snprintf(u[3], 65, "#1 C-OS"); snprintf(u[4], 65, "x86_64"); snprintf(u[5], 65, "(none)");
        return copy_out(a0, u, sizeof u) ? 0 : -L_EFAULT;
    }
    case 96: {  /* gettimeofday */
        if (a0) { uint64_t wm = wall_ms(); uint64_t tv[2] = { wm / 1000, (wm % 1000) * 1000 }; if (!copy_out(a0, tv, 16)) return -L_EFAULT; } return 0;
    }
    case 201: { uint64_t t = wall_ms() / 1000; if (a0) copy_out(a0, &t, 8); return (int64_t)t; }
    case 228: {  /* clock_gettime */
        uint64_t ms = now_ms(); uint64_t ts[2];
        if (a0 == 0 || a0 == 5 || a0 == 8) { uint64_t wm = wall_ms(); ts[0] = wm / 1000; ts[1] = (wm % 1000) * 1000000; }
        else { ts[0] = ms / 1000; ts[1] = (ms % 1000) * 1000000; }
        return copy_out(a1, ts, 16) ? 0 : -L_EFAULT;
    }
    case 229: { uint64_t ts[2] = { 0, 1000000 }; return a1 && copy_out(a1, ts, 16) ? 0 : 0; }   /* clock_getres */
    case 158: {  /* arch_prctl */
        if (a0 == 0x1002) { thread_t *t = scheduler_get_current_thread(); if (!t) return -L_EINVAL; scheduler_set_thread_tls(t, a1); c->fs_base = a1; return 0; }   /* ARCH_SET_FS */
        if (a0 == 0x1003) return copy_out(a1, &c->fs_base, 8) ? 0 : -L_EFAULT;                                                                                 /* ARCH_GET_FS */
        return -L_EINVAL;
    }
    case 218: th_add(c, cur_tid(), a0); return (int64_t)cur_tid();   /* set_tid_address */
    case 273: return 0;     /* set_robust_list */
    case 334: return -L_ENOSYS;   /* rseq: libc copes */
    case 302: {  /* prlimit64 */
        if (a3) { uint64_t cur = a1 == 3 ? 8ull << 20 : a1 == 7 ? LX_MAX_FD : a1 == 6 ? 4096 : ~0ull; uint64_t lim[2] = { cur, a1 == 3 || a1 == 7 || a1 == 6 ? cur : ~0ull }; if (!copy_out(a3, lim, 16)) return -L_EFAULT; } return 0;
    }
    case 97: { uint64_t cur = a0 == 3 ? 8ull << 20 : a0 == 7 ? LX_MAX_FD : a0 == 6 ? 4096 : ~0ull; uint64_t lim[2] = { cur, a0 == 3 || a0 == 7 || a0 == 6 ? cur : ~0ull }; return copy_out(a1, lim, 16) ? 0 : -L_EFAULT; }   /* getrlimit */
    case 98: { uint64_t z[18]; memset(z, 0, sizeof z); if (a1 && !copy_out(a1, z, sizeof z)) return -L_EFAULT; return 0; }   /* getrlimit, getrusage */
    case 318: {  /* getrandom */
        uint64_t n = a1 > 256 ? 256 : a1; uint8_t buf[256];
        for (uint64_t i = 0; i < n; i += 8) { uint64_t v = cos_random_u64(); for (int k = 0; k < 8 && i + k < n; ++k) buf[i + k] = (uint8_t)(v >> (8 * k)); }
        return copy_out(a0, buf, n) ? (int64_t)n : -L_EFAULT;
    }
    case 202: return do_futex(a0, (uint32_t)a1, (uint32_t)a2, a3, a4, (uint32_t)a5);
    case 56: {   /* clone: a thread (VM + THREAD), or a copy of the process (fork, vfork) */
        if ((a0 & (L_CLONE_VM | L_CLONE_THREAD)) == (L_CLONE_VM | L_CLONE_THREAD)) return do_clone_thread(p, c, r, a0, a1, a2, a3, a4);
        if (a0 & L_CLONE_THREAD) return -L_ENOSYS;
        return do_fork(p, c, r, a0, a1, a2);
    }
    case 57: case 58: return do_fork(p, c, r, 17, 0, 0);          /* fork, vfork (a vfork child gets its own copy) */
    case 59: return do_execve(p, c, r, a0, a1, a2);
    case 61: {  /* wait4 */
        int64_t want = (int32_t)a0; int st = 0;
        for (;;) {
            int64_t got = process_try_reap_child(p->pid, want > 0 ? want : -1, &st);
            if (got > 0) { if (a1) { int32_t ws = wait_status_of(st); if (!copy_out(a1, &ws, 4)) return -L_EFAULT; } return got; }
            if (got < 0) return -L_ECHILD;
            if (a2 & 1) return 0;                                  /* WNOHANG */
            if (sig_interrupt(c)) return -L_EINTR;
            wait_tick();
        }
    }
    case 247: {  /* waitid(idtype, id, infop, options): P_ALL / P_PID, WNOHANG, WNOWAIT */
        extern int64_t process_try_reap_child_ex(uint64_t parent_pid, int64_t want_pid, int *out_status, bool consume);
        int64_t want = a0 == 1 ? (int64_t)a1 : -1; int st = 0; bool consume = !(a3 & 0x01000000);
        for (;;) {
            int64_t got = process_try_reap_child_ex(p->pid, want, &st, consume);
            if (got > 0) {
                if (a2) { uint32_t si[32]; memset(si, 0, sizeof si); int ws = wait_status_of(st); si[0] = L_SIGCHLD; si[2] = (ws & 0x7F) ? ((ws & 0x80) ? 3 : 2) : 1; si[4] = (uint32_t)got; si[6] = (ws & 0x7F) ? (uint32_t)(ws & 0x7F) : (uint32_t)((ws >> 8) & 0xFF); if (!copy_out(a2, si, 128)) return -L_EFAULT; }
                return 0;
            }
            if (got < 0) return -L_ECHILD;
            if (a3 & 1) { if (a2) { uint32_t si[32]; memset(si, 0, sizeof si); copy_out(a2, si, 128); } return 0; }
            if (sig_interrupt(c)) return -L_EINTR;
            wait_tick();
        }
    }
    case 13: {  /* rt_sigaction */
        int sg = (int)a0; if (sg < 1 || sg > 64 || sg == 9 || sg == 19) return -L_EINVAL;
        if (a2) { uint64_t old[4] = { c->sig[sg].handler, c->sig[sg].flags, c->sig[sg].restorer, c->sig[sg].mask }; if (!copy_out(a2, old, 32)) return -L_EFAULT; }
        if (a1) {
            uint64_t nw[4]; if (!copy_in(nw, a1, 32)) return -L_EFAULT;
            c->sig[sg].handler = nw[0]; c->sig[sg].flags = nw[1]; c->sig[sg].restorer = nw[2]; c->sig[sg].mask = nw[3];
            if (nw[0] == LX_SIG_IGN || (nw[0] == LX_SIG_DFL && !sig_default_ends(sg))) c->sig_pending &= ~sig_bit(sg);
        }
        return 0;
    }
    case 14: {  /* rt_sigprocmask */
        if (a2 && !copy_out(a2, &c->sig_blocked, 8)) return -L_EFAULT;
        if (a1) {
            uint64_t set; if (!copy_in(&set, a1, 8)) return -L_EFAULT;
            if (a0 == 0) c->sig_blocked |= set; else if (a0 == 1) c->sig_blocked &= ~set; else if (a0 == 2) c->sig_blocked = set; else return -L_EINVAL;
            c->sig_blocked &= ~(sig_bit(L_SIGKILL) | sig_bit(19));
        }
        return 0;
    }
    case 15: return do_sigreturn(c, r);
    case 127: return copy_out(a0, &c->sig_pending, 8) ? 0 : -L_EFAULT;       /* rt_sigpending */
    case 34: while (!sig_interrupt(c)) wait_tick(); return -L_EINTR;          /* pause */
    case 37: {  /* alarm */
        uint64_t now = now_ms(), left = c->alarm_ms > now ? (c->alarm_ms - now + 999) / 1000 : 0;
        c->alarm_ms = a0 ? now + a0 * 1000 : 0; return (int64_t)left;
    }
    case 38: {  /* setitimer (ITIMER_REAL, one shot) */
        if (a0 != 0) return 0;
        if (a2) { uint64_t z[4] = { 0, 0, 0, 0 }; copy_out(a2, z, 32); }
        if (a1) { uint64_t it[4]; if (!copy_in(it, a1, 32)) return -L_EFAULT; uint64_t ms = it[2] * 1000 + it[3] / 1000; c->alarm_ms = ms ? now_ms() + ms : 0; }
        return 0;
    }
    case 62: return do_kill(p, c, (int32_t)a0, (int)a1);
    case 200: { thread_t *t = thread_get_by_tid(a0); return t ? do_kill(p, c, (int64_t)t->pid, (int)a1) : -L_ESRCH; }     /* tkill */
    case 234: return do_kill(p, c, (int32_t)a0, (int)a2);                      /* tgkill */
    case 109: case 112: return 0;                                              /* setpgid, setsid */
    case 111: return (int64_t)p->pid;                                          /* getpgrp */
    case 121: return (int64_t)p->pid;                                          /* getpgid */
    case 95: return 022;                                                       /* umask */
    case 157: return 0;                                                        /* prctl */
    case 90: case 91: case 92: case 93: case 94: case 268: case 260: case 74: case 75: return 0;   /* chmod, chown, fsync: nothing to do here */
    case 82: { char ra[200], rb[200], pa[256], pb[256]; if (!fs_ok(p, COS_CAP_FS_WRITE)) return -L_EACCES; if (!copy_str(ra, a0, sizeof ra) || !copy_str(rb, a1, sizeof rb)) return -L_EFAULT; resolve(c, L_AT_FDCWD, ra, pa, sizeof pa); resolve(c, L_AT_FDCWD, rb, pb, sizeof pb); return cos_fs_rename(pa, pb) ? 0 : -L_ENOENT; }
    case 1000000: break;
    case 435: {  /* clone3: struct clone_args mapped onto clone's arguments */
        if (a1 < 64) return -L_EINVAL;
        uint64_t ca[8]; if (!copy_in(ca, a0, 64)) return -L_EFAULT;
        uint64_t flags = ca[0] | (ca[4] & 0xFF);
        if (ca[0] >> 32) return -L_EINVAL;                       /* CLONE_CLEAR_SIGHAND, CLONE_INTO_CGROUP, ... */
        uint64_t stack = ca[5] ? ca[5] + ca[6] : 0;
        if ((flags & (L_CLONE_VM | L_CLONE_THREAD)) == (L_CLONE_VM | L_CLONE_THREAD)) return do_clone_thread(p, c, r, flags, stack, ca[3], ca[2], ca[7]);
        if (flags & L_CLONE_THREAD) return -L_ENOSYS;
        return do_fork(p, c, r, flags, stack, ca[3]);
    }
    case 204: {  /* sched_getaffinity */
        uint64_t mask[16]; memset(mask, 0, sizeof mask);
        uint32_t n = smp_online_cpu_count(); if (n < 1) n = 1; if (n > 64) n = 64;
        mask[0] = n >= 64 ? ~0ull : ((1ull << n) - 1);
        uint64_t len = a1 > sizeof mask ? sizeof mask : a1; if (len < 8) return -L_EINVAL;
        return copy_out(a2, mask, len) ? (int64_t)len : -L_EFAULT;
    }
    case 203: return 0;                  /* sched_setaffinity */
    case 99: { uint8_t si[112]; memset(si, 0, sizeof si); *(uint64_t *)(si + 32) = 128ull << 20; *(uint64_t *)(si + 40) = 64ull << 20; si[104] = 1; return copy_out(a0, si, sizeof si) ? 0 : -L_EFAULT; }   /* sysinfo */
    case 60: case 231: {  /* exit (this thread), exit_group (all of them) */
        if (nr == 60 && p->thread_count > 1) thread_end(p, c, (int)a0);
        serial_puts("[LINUX] pid "); serial_putdec(p->pid); serial_puts(" exit "); serial_putdec((uint64_t)(int64_t)(int)a0); serial_puts("\n");
        notify_parent(p);
        process_exit(p, (int)a0);
        thread_exit(scheduler_get_current_thread(), (int)a0);
        for (;;) __asm__ volatile("hlt");
    }
    case 41: {  /* socket */
        uint32_t af = (uint32_t)a0;
        if (af == 1) return unix_socket(c, (uint32_t)a1);
        if (af != L_AF_INET && af != L_AF_INET6) return -L_EAFNOSUPPORT;
        uint32_t t = (uint32_t)a1 & 0xF; if (t != COS_SOCK_STREAM && t != COS_SOCK_DGRAM) return -L_EPROTONOSUPPORT;
        int sd = af == L_AF_INET6 ? cos_sock_create6((uint32_t)p->pid, (int)t) : cos_sock_create((uint32_t)p->pid, (int)t); if (sd < 0) return sd;
        return sock_new_fd(c, p->pid, sd, (int)t, (int)af, (uint32_t)a1);
    }
    case 53: {  /* socketpair (AF_UNIX stream/dgram as two pipes) */
        if ((uint32_t)a0 != 1) return -L_EAFNOSUPPORT;
        uint32_t ty = (uint32_t)a1 & 0xF; if (ty != 1 && ty != 2 && ty != 5) return -L_EPROTONOSUPPORT;
        int fa = fd_alloc_from(c, 0); if (fa < 0) return -L_EMFILE;
        c->fd[fa] = (ofd_t *)1; int fb = fd_alloc_from(c, 0); c->fd[fa] = NULL; if (fb < 0) return -L_EMFILE;
        pipe_t *p1 = pipe_new(), *p2 = pipe_new(); uint32_t fl = (a1 & 0x800) ? L_O_NONBLOCK : 0;
        ofd_t *oa = ofd_new(FD_UPAIR, fl | L_O_RDWR), *ob = ofd_new(FD_UPAIR, fl | L_O_RDWR);
        if (!p1 || !p2 || !oa || !ob) { if (oa) kfree(oa); if (ob) kfree(ob); return -L_ENOMEM; }
        p1->readers = p1->writers = p2->readers = p2->writers = 1; p1->packet = p2->packet = ty != 1; oa->counter = ob->counter = ty;
        oa->pipe = p2; oa->pipe2 = p1; ob->pipe = p1; ob->pipe2 = p2;
        c->fd[fa] = oa; c->fd[fb] = ob; c->cloexec[fa] = c->cloexec[fb] = (a1 & 0x80000) != 0;
        int sv[2] = { fa, fb }; return copy_out(a3, sv, 8) ? 0 : -L_EFAULT;
    }
    case 42: {  /* connect */
        { ofd_t *u = F(c, (int)a0); if (u && u->kind == FD_UPAIR) return unix_connect(c, u, a1, a2); }
        int64_t e; ofd_t *o = sock_of(c, (int)a0, &e); if (!o) return e;
        cos_sockaddr6_t a; int pe = sa_get(a1, a2, &a); if (pe) return pe;
        bool started = false;
        for (;;) {
            int r = a.v6 ? cos_sock_connect6((uint32_t)o->owner, (int)o->cosfd, a.a, a.port)
                         : cos_sock_connect((uint32_t)o->owner, (int)o->cosfd, (uint32_t)a.a[0] << 24 | (uint32_t)a.a[1] << 16 | (uint32_t)a.a[2] << 8 | a.a[3], a.port);
            if (r == -COS_EISCONN && started) return 0;
            if (r != -COS_EINPROGRESS) return r;
            started = true;
            if (o->flags & L_O_NONBLOCK) return -L_EINPROGRESS;
            if (sig_interrupt(c)) return -L_EINTR;
            scheduler_sleep(1);
        }
    }
    case 43: case 288: {  /* accept, accept4 */
        { ofd_t *u = F(c, (int)a0); if (u && u->kind == FD_UPAIR) return unix_accept(c, u, nr == 288 ? (uint32_t)a3 : 0, a1, a2); }
        int64_t e; ofd_t *o = sock_of(c, (int)a0, &e); if (!o) return e;
        cos_sockaddr6_t peer; memset(&peer, 0, sizeof peer); int sd;
        for (;;) {
            sd = cos_sock_accept6((uint32_t)o->owner, (int)o->cosfd, &peer);
            if (sd != -COS_EAGAIN) break;
            if (o->flags & L_O_NONBLOCK) return -L_EAGAIN;
            if (sig_interrupt(c)) return -L_EINTR;
            scheduler_sleep(1);
        }
        if (sd < 0) return sd;
        int64_t n = sock_new_fd(c, o->owner, sd, COS_SOCK_STREAM, (int)o->efd_flags, nr == 288 ? (uint32_t)a3 : 0); if (n < 0) return n;
        sa_put(o, a1, a2, &peer); return n;
    }
    case 44: {  /* sendto */
        { ofd_t *u = F(c, (int)a0); if (u && u->kind == FD_UPAIR) {
            uint64_t n = a2 > 65536 ? 65536 : a2; uint8_t *b = (uint8_t *)kmalloc(n ? n : 1); if (!b) return -L_ENOMEM;
            int64_t r2 = copy_in(b, a1, n) ? upair_send(c, u, b, n, (u->flags & L_O_NONBLOCK) || (a3 & 0x40), NULL, 0) : -L_EFAULT; kfree(b); return r2; } }
        int64_t e; ofd_t *o = sock_of(c, (int)a0, &e); if (!o) return e;
        cos_sockaddr6_t to; bool has = false;
        if (a4 && a5) { int pe = sa_get(a4, a5, &to); if (pe) return pe; has = true; }
        uint64_t n = a2 > 65536 ? 65536 : a2; uint8_t *b = (uint8_t *)kmalloc(n ? n : 1); if (!b) return -L_ENOMEM;
        int64_t r = copy_in(b, a1, n) ? sock_send(c, o, b, n, (uint32_t)a3, has ? &to : NULL) : -L_EFAULT;
        kfree(b); return r;
    }
    case 45: {  /* recvfrom */
        { ofd_t *u = F(c, (int)a0); if (u && u->kind == FD_UPAIR) {
            uint64_t n = a2 > 65536 ? 65536 : a2; uint8_t *b = (uint8_t *)kmalloc(n ? n : 1); if (!b) return -L_ENOMEM;
            int nf = 0; bool tr; int64_t r2 = upair_recv(c, u, b, n, (u->flags & L_O_NONBLOCK) || (a3 & 0x40), (a3 & 2) != 0, NULL, &nf, &tr);
            if (r2 > 0 && !copy_out(a1, b, (uint64_t)r2)) r2 = -L_EFAULT;
            if (r2 >= 0 && a4 && a5) un_put(NULL, a4, a5);
            kfree(b); return r2; } }
        int64_t e; ofd_t *o = sock_of(c, (int)a0, &e); if (!o) return e;
        uint64_t n = a2 > 65536 ? 65536 : a2; uint8_t *b = (uint8_t *)kmalloc(n ? n : 1); if (!b) return -L_ENOMEM;
        cos_sockaddr6_t from; memset(&from, 0, sizeof from);
        int64_t r = sock_recv(c, o, b, n, (uint32_t)a3, &from);
        if (r > 0) { if (!copy_out(a1, b, (uint64_t)r)) r = -L_EFAULT; else if (a4 && o->counter == COS_SOCK_DGRAM) sa_put(o, a4, a5, &from); }
        kfree(b); return r;
    }
    case 46: case 47: {  /* sendmsg, recvmsg */
        { ofd_t *u = F(c, (int)a0); if (u && u->kind == FD_UPAIR) return upair_msg(p, c, u, a1, (uint32_t)a2, nr == 46); }
        int64_t e; ofd_t *o = sock_of(c, (int)a0, &e); if (!o) return e;
        return sock_msg(p, c, o, a1, (uint32_t)a2, nr == 46);
    }
    case 48: {  /* shutdown: SHUT_WR sends the FIN, the read side stays open */
        ofd_t *o = F(c, (int)a0); if (!o) return -L_EBADF;
        if (o->kind == FD_UPAIR) {
            if (!o->pipe) return -107;
            if ((a1 == 1 || a1 == 2) && !(o->ushut & 1)) { o->ushut |= 1; o->pipe2->writers--; }
            if ((a1 == 0 || a1 == 2) && !(o->ushut & 2)) { o->ushut |= 2; o->pipe->readers--; }
            return 0;
        }
        int64_t e; o = sock_of(c, (int)a0, &e); if (!o) return e;
        return cos_sock_shutdown((uint32_t)o->owner, (int)o->cosfd, (int)a1);
    }
    case 49: {  /* bind */
        { ofd_t *u = F(c, (int)a0); if (u && u->kind == FD_UPAIR) return unix_bind(u, a1, a2); }
        int64_t e; ofd_t *o = sock_of(c, (int)a0, &e); if (!o) return e;
        cos_sockaddr6_t a; int pe = sa_get(a1, a2, &a); if (pe) return pe;
        return cos_sock_bind((uint32_t)o->owner, (int)o->cosfd, 0, a.port);
    }
    case 50: { { ofd_t *u = F(c, (int)a0); if (u && u->kind == FD_UPAIR) { if (!u->lis) return -L_EINVAL; u->lis->listening = 1; return 0; } } int64_t e; ofd_t *o = sock_of(c, (int)a0, &e); if (!o) return e; return cos_sock_listen((uint32_t)o->owner, (int)o->cosfd, (int)a1); }
    case 51: case 52: {  /* getsockname, getpeername */
        { ofd_t *u = F(c, (int)a0); if (u && u->kind == FD_UPAIR) return un_put(nr == 51 ? u->path : NULL, a1, a2); }
        int64_t e; ofd_t *o = sock_of(c, (int)a0, &e); if (!o) return e;
        cos_sockaddr6_t a; int r = cos_sock_name6((uint32_t)o->owner, (int)o->cosfd, nr == 52, &a); if (r < 0) return r;
        return sa_put(o, a1, a2, &a);
    }
    case 54: { int64_t e; return sock_of(c, (int)a0, &e) || F(c, (int)a0) ? 0 : -L_EBADF; }   /* setsockopt: accepted and ignored */
    case 55: {  /* getsockopt */
        ofd_t *o = F(c, (int)a0); if (!o) return -L_EBADF;
        int v = 0;
        if (o->kind == FD_UPAIR && a1 == 1 && a2 == 17 /* SO_PEERCRED */) { uint32_t cr[3] = { (uint32_t)p->pid, 0, 0 }, l0 = 0; if (a4 && !copy_in(&l0, a4, 4)) return -L_EFAULT; if (l0 > 12) l0 = 12; if (!copy_out(a3, cr, l0)) return -L_EFAULT; l0 = 12; return a4 && !copy_out(a4, &l0, 4) ? -L_EFAULT : 0; }
        if (a1 == 1 && (a2 == 7 || a2 == 8)) v = 212992;
        if (a1 == 1 && a2 == 3 /* SO_TYPE */) v = o->kind == FD_SOCK || o->kind == FD_UPAIR ? (int)o->counter : 1;
        if (a1 == 1 && a2 == 4 /* SO_ERROR */ && o->kind == FD_SOCK) { cos_pollfd_t pf = { (int32_t)o->cosfd, COS_POLLIN, 0 }; cos_sock_poll((uint32_t)o->owner, &pf, 1); if (pf.revents & COS_POLLERR) v = 111; }
        if (a1 == 1 && a2 == 39 /* SO_DOMAIN */ && o->kind == FD_SOCK) v = (int)o->efd_flags;
        uint32_t l = 4; if (a4 && !copy_in(&l, a4, 4)) return -L_EFAULT;
        if (!copy_out(a3, &v, l < 4 ? l : 4)) return -L_EFAULT;
        l = 4; return a4 && !copy_out(a4, &l, 4) ? -L_EFAULT : 0;
    }
    case 40: {  /* sendfile */
        ofd_t *out = F(c, (int)a0), *in = F(c, (int)a1); if (!out || !in) return -L_EBADF;
        if (in->kind != FD_FILE) return -L_EINVAL;
        uint64_t off = 0; bool usepos = a2 != 0; if (usepos && !copy_in(&off, a2, 8)) return -L_EFAULT;
        uint8_t *b = (uint8_t *)kmalloc(65536); if (!b) return -L_ENOMEM;
        int64_t total = 0, err = 0; uint64_t left = a3;
        while (left) {
            uint64_t k = left > 65536 ? 65536 : left;
            int64_t got = ofd_read(p, c, in, b, k, usepos, off);
            if (got < 0) { err = got; break; }
            if (got == 0) break;
            int64_t w = ofd_write(p, c, out, b, (uint64_t)got, false, 0);
            if (w < 0) { if (!usepos) in->pos -= (uint64_t)got; err = w; break; }
            if (w < got && !usepos) in->pos -= (uint64_t)(got - w);
            total += w; left -= (uint64_t)w; off += (uint64_t)w;
            if (w < got) break;
        }
        kfree(b); if (usepos) copy_out(a2, &off, 8);
        return total ? total : err;
    }
    default: break;
    }
    { static uint8_t seen[512 / 8]; uint64_t k = nr < 512 ? nr : 511;
      if (!(seen[k >> 3] & (1 << (k & 7)))) { seen[k >> 3] |= (uint8_t)(1 << (k & 7)); serial_puts("[LINUX] unimplemented syscall "); serial_putdec(nr); serial_puts(" (first call, a0=0x"); serial_puthex(a0); serial_puts(")\n"); } }
    (void)a5;
    return -L_ENOSYS;
}

/* the last system calls, kept so a crash can say what the program had just been doing (printed by cos_linux_page_fault when a fault is not resolved) */
typedef struct { uint64_t nr, a0, a1, a2, ret; uint32_t pid, tid; char s[40]; } lx_trace_t;
static lx_trace_t s_trace[64]; static volatile uint32_t s_trace_n;
/* the system call each thread is in right now (printed by the scheduler's stall report) */
typedef struct { uint32_t tid; uint64_t nr, a0, a1, a2; } lx_cur_t;
static lx_cur_t s_cur[512];
static void crash_threads(process_t *proc) {
    lctx_t *cx = ctx_of(proc);
    serial_puts("[THREADS] crashing tid "); serial_putdec(cur_tid()); serial_puts(" mmap_busy "); serial_putdec((uint64_t)cx->mmap_busy); serial_puts(" threads:");
    for (int i = 0; i < cx->nth && i < 64; ++i) { uint32_t t = (uint32_t)cx->th[i].tid; const lx_cur_t *cu = &s_cur[t & 511]; serial_puts(" "); serial_putdec(t); serial_puts("(nr "); serial_putdec(cu->tid == t ? cu->nr : 9999); serial_puts(")"); }
    serial_puts("\n");
}
void cos_linux_dbg_thread(uint32_t tid) {
    const lx_cur_t *cu = &s_cur[tid & 511]; if (cu->tid != tid) return;
    serial_puts("      syscall "); serial_putdec(cu->nr); serial_puts(" a0=0x"); serial_puthex(cu->a0); serial_puts(" a1=0x"); serial_puthex(cu->a1); serial_puts(" a2=0x"); serial_puthex(cu->a2); serial_puts("\n");
}
static int s_trace_all = -1;      /* /linux/etc/cos.trace present: print process, socket and wait calls as they finish */
static void trace_note(process_t *p, struct regs *r, int64_t res) {
    if (res < 0 && res >= -133 && r->rax < 400 && s_fail[r->rax][-res] < 65535) s_fail[r->rax][-res]++;
    lx_trace_t *t = &s_trace[s_trace_n++ & 63];
    t->nr = r->rax; t->a0 = r->rdi; t->a1 = r->rsi; t->a2 = r->rdx; t->ret = (uint64_t)res; t->pid = (uint32_t)p->pid; t->tid = (uint32_t)cur_tid(); t->s[0] = 0;
    uint64_t sp = 0; switch (t->nr) { case 2: case 4: case 6: case 21: case 59: case 80: case 89: case 83: case 84: case 87: sp = r->rdi; break; case 257: case 262: case 267: case 269: case 258: case 263: sp = r->rsi; break; default: break; }
    if (sp) copy_str(t->s, sp, sizeof t->s);
    { static int memfail_n; uint64_t n = r->rax; if (res < 0 && res > -4096 && (n == 9 || n == 10 || n == 11 || n == 12 || n == 25 || n == 56 || n == 57 || n == 319) && memfail_n < 60) { memfail_n++; serial_puts("[LXE] memory call "); serial_putdec(n); serial_puts(" failed with "); serial_putdec((uint64_t)-res); serial_puts(" a0=0x"); serial_puthex(r->rdi); serial_puts(" a1=0x"); serial_puthex(r->rsi); serial_puts(" a2=0x"); serial_puthex(r->rdx); serial_puts(" a3=0x"); serial_puthex(r->r10); serial_puts("\n"); } }
    { static int efault_n; if (res == -14 && efault_n < 40) { efault_n++; serial_puts("[LXE] EFAULT from system call "); serial_putdec(r->rax); serial_puts(" a0=0x"); serial_puthex(r->rdi); serial_puts(" a1=0x"); serial_puthex(r->rsi); serial_puts(" a2=0x"); serial_puthex(r->rdx); serial_puts(" a3=0x"); serial_puthex(r->r10); serial_puts("\n"); } }
    if (s_trace_all < 0) { uint64_t sz; uint8_t d; s_trace_all = cos_fs_stat("/linux/etc/cos.trace", &sz, &d) ? 1 : 0; }
    if (s_trace_all) {
        uint64_t n = r->rax;
        static int all = -1; if (all < 0) { uint64_t sz2; uint8_t d2; all = cos_fs_stat("/linux/etc/cos.traceall", &sz2, &d2) ? 1 : 0; }
        if (all || (n >= 41 && n <= 55) || n == 288 || n == 53 || n == 7 || n == 232 || n == 281 || n == 56 || n == 57 || n == 58 || n == 59 || n == 435 || n == 61 || n == 60 || n == 231 || n == 247 || n == 271 || n == 17 || n == 0 || n == 1 || n == 3 || n == 22 || n == 293 || n == 290 || n == 284) {
            serial_puts("[LXT] "); serial_putdec(p->pid); serial_puts("/"); serial_putdec(cur_tid()); serial_puts(" nr "); serial_putdec(n);
            serial_puts(" "); serial_puthex(r->rdi); serial_puts(" "); serial_puthex(r->rsi); serial_puts(" "); serial_puthex(r->rdx); serial_puts(" = "); serial_puthex((uint64_t)res);
            if (n == 7 && r->rsi <= 4) {                                                   /* poll: which descriptors, what kind, what came back */
                struct { int32_t fd; int16_t ev, rev; } pf[4];
                if (copy_in(pf, r->rdi, r->rsi * 8)) for (uint64_t i = 0; i < r->rsi; ++i) {
                    ofd_t *o = F(ctx_of(p), pf[i].fd);
                    serial_puts(" [fd"); serial_putdec((uint64_t)pf[i].fd); serial_puts(" k"); serial_putdec(o ? o->kind : 99); serial_puts(" ev"); serial_puthex((uint16_t)pf[i].ev); serial_puts(" rev"); serial_puthex((uint16_t)pf[i].rev);
                    if (o && o->pipe) { serial_puts(" len"); serial_putdec(o->pipe->len); } serial_puts("]");
                }
            }
            if (t->s[0]) { serial_puts(" \""); serial_puts(t->s); serial_puts("\""); }
            serial_puts("\n");
        }
    }

}
static void trace_dump(void) {
    serial_puts("[LINUX] last system calls (oldest first):\n");
    for (uint32_t i = 0; i < 64; ++i) {
        const lx_trace_t *t = &s_trace[(s_trace_n + i) & 63]; if (!t->nr && !t->pid) continue;
        serial_puts("  pid "); serial_putdec(t->pid); serial_puts(" tid "); serial_putdec(t->tid); serial_puts(" nr "); serial_putdec(t->nr);
        serial_puts(" a0=0x"); serial_puthex(t->a0); serial_puts(" a1=0x"); serial_puthex(t->a1); serial_puts(" a2=0x"); serial_puthex(t->a2); serial_puts(" -> "); serial_puthex(t->ret);
        if (t->s[0]) { serial_puts(" \""); serial_puts(t->s); serial_puts("\""); } serial_puts("\n");
    }
}

/* ---- debugging: /linux/etc/cos.codewatch ---------------------------------------------------------------------------------------------------------------------
 * Pages of read-only file mappings (code, rodata) must never change. While system calls run, a few of them are hashed each time (16 hashes per page, one per
 * 256 bytes, kept the first time a page is seen); a page whose hash changes is reported with the library, the file offset and the changed words. */
typedef struct { uint64_t addr; uint64_t frame; uint32_t fp; uint32_t h[16]; } cw_ent_t;
#define CW_N 65536u
static cw_ent_t *s_cw; static uint32_t s_cw_reports, s_cw_cursor; static int s_cw_on = -1;
static void cw_hash(const uint8_t *pg, uint32_t *h) {
    for (int b = 0; b < 16; ++b) { uint32_t x = 2166136261u; const uint32_t *w = (const uint32_t *)(pg + b * 256); for (int i = 0; i < 64; ++i) { x ^= w[i]; x *= 16777619u; x ^= x >> 15; } h[b] = x; }
}
static void code_watch_step(process_t *p, lctx_t *c) {
    if (s_cw_on < 0) { uint64_t sz; uint8_t d; s_cw_on = cos_fs_stat("/linux/etc/cos.codewatch", &sz, &d) ? 1 : 0; if (s_cw_on) { s_cw = (cw_ent_t *)kmalloc(sizeof(cw_ent_t) * CW_N); if (s_cw) memset(s_cw, 0, sizeof(cw_ent_t) * CW_N); else s_cw_on = 0; } }
    if (s_cw_on <= 0 || s_cw_reports >= 40) return;
    uint32_t fp = c->fpid ? c->fpid : (uint32_t)p->pid, me = (uint32_t)p->pid;      /* fp tells which file a mapping is; the watched page belongs to this very process */
    uint64_t total = 0;
    for (int i = 0; i < c->nvma; ++i) { const lvma_t *v = &c->vma[i]; if (!v->shm && (v->prot == 5 || v->prot == 1) && fmap_has(fp, v->start)) total += (v->end - v->start) / PAGE; }
    if (!total) return;
    for (int k = 0; k < 4; ++k) {
        uint64_t idx = (s_cw_cursor++ * 2654435761u) % total, addr = 0;
        for (int i = 0; i < c->nvma && !addr; ++i) { const lvma_t *v = &c->vma[i]; if (!v->shm && (v->prot == 5 || v->prot == 1) && fmap_has(fp, v->start)) { uint64_t n = (v->end - v->start) / PAGE; if (idx < n) addr = v->start + idx * PAGE; else idx -= n; } }
        if (!addr) continue;
        uint64_t ph = paging_virt_to_phys(addr); if (!ph) continue;
        const uint8_t *pg = (const uint8_t *)(uintptr_t)PHYS_TO_VIRT(ph & ~(uint64_t)(PAGE - 1));
        uint32_t h[16]; cw_hash(pg, h);
        uint32_t slot = (uint32_t)(((addr >> 12) * 40503u + me * 2654435761u) % CW_N), tries = 0;
        while (s_cw[slot].addr && !(s_cw[slot].addr == addr && s_cw[slot].fp == me) && tries < 64) { slot = (slot + 1) % CW_N; tries++; }
        cw_ent_t *e = &s_cw[slot];
        if (tries >= 64) continue;
        if (!e->addr || e->frame != (ph & ~(uint64_t)(PAGE - 1))) { e->addr = addr; e->fp = me; e->frame = ph & ~(uint64_t)(PAGE - 1); memcpy(e->h, h, sizeof h); continue; }      /* a new frame at that address is a new mapping, not a change */
        for (int b = 0; b < 16; ++b) if (e->h[b] != h[b]) {
            s_cw_reports++;
            serial_puts("[CODEWATCH] pid "); serial_putdec(p->pid); serial_puts(" tid "); serial_putdec(cur_tid()); serial_puts(" page 0x"); serial_puthex(addr); serial_puts(" block "); serial_putdec((uint64_t)b);
            serial_puts(" changed; frame 0x"); serial_puthex(ph); serial_puts(" cow-shared "); serial_putdec(paging_cow_is_shared(ph & ~(uint64_t)(PAGE - 1)) ? 1 : 0); serial_puts("\n");
            fmap_where("[CODEWATCH] page", fp, addr + (uint64_t)b * 256);
            serial_puts("[CODEWATCH] words:");
            for (int i = 0; i < 32; ++i) { serial_puts(" "); serial_puthex(*(const uint32_t *)(pg + b * 256 + i * 4)); }
            serial_puts("\n");
            memcpy(e->h, h, sizeof h); break;
        }
    }
}

/* ---- profiling aid: every 60 s of uptime, the busiest threads and system calls since the last report (/linux/etc/cos.prof) ---- */
uint32_t s_epdbg_n; uint32_t s_epdbg[8][5]; uint64_t s_fill_ms, s_fill_pages;
static uint32_t s_pf_tid[512], s_pf_nr[400]; static uint64_t s_pf_last, s_pf_total; static int s_pf_on = -1;

/* ---- profiling aid (/linux/etc/cos.prof): where the user code of a Linux program spends its time, sampled by the timer interrupts ---- */
static struct { char name[56]; uint32_t n; } s_ps[48]; static uint32_t s_ps_total;
void cos_linux_prof_sample(struct regs *r) {
    if (s_pf_on <= 0 || (r->cs & 3) != 3) return;
    process_t *p = process_get_current(); if (!p || !p->linux_ctx) return;
    lctx_t *c = ctx_of(p); uint32_t fp = c->fpid ? c->fpid : (uint32_t)p->pid; const char *nm = "(anonymous: JIT, heap, stack)";
    for (uint32_t k = 0; k < LX_FMAP_N && k < s_fmap_n; ++k) { const lx_fmap_t *m = &s_fmap[(s_fmap_n - 1 - k) & (LX_FMAP_N - 1)]; if (m->pid == fp && r->rip >= m->start && r->rip < m->end) { nm = m->name; break; } }
    const char *base = nm; for (const char *q = nm; *q; ++q) if (*q == '/') base = q + 1;
    s_ps_total++;
    for (int i = 0; i < 48; ++i) { if (!s_ps[i].name[0]) { snprintf(s_ps[i].name, sizeof s_ps[i].name, "%s", base); s_ps[i].n = 1; return; } if (!strcmp(s_ps[i].name, base)) { s_ps[i].n++; return; } }
}
static void prof_sample_report(void) {
    serial_puts("[PROF]  user code by library (timer samples):");
    for (int k = 0; k < 8; ++k) { int b = -1; uint32_t bv = 0; for (int i = 0; i < 48; ++i) if (s_ps[i].n > bv) { bv = s_ps[i].n; b = i; } if (b < 0) break; serial_puts(" "); serial_puts(s_ps[b].name); serial_puts("="); serial_putdec(bv); s_ps[b].n = 0; }
    serial_puts(" of "); serial_putdec(s_ps_total); serial_puts("\n"); s_ps_total = 0; memset(s_ps, 0, sizeof s_ps);
}
static void prof_note(uint32_t tid, uint64_t nr) {
    if (s_pf_on < 0) { uint64_t sz; uint8_t d; s_pf_on = cos_fs_stat("/linux/etc/cos.prof", &sz, &d) ? 1 : 0; s_pf_last = hal_timer_get_ms(); }
    if (!s_pf_on) return;
    s_pf_tid[tid & 511]++; if (nr < 400) s_pf_nr[nr]++; s_pf_total++;
    if ((s_pf_total & 255) || hal_timer_get_ms() - s_pf_last < 60000) return;
    s_pf_last = hal_timer_get_ms();
    serial_puts("[PROF] syscalls in the last minute: "); serial_putdec(s_pf_total); serial_puts("\n[PROF]  by thread:");
    for (int k = 0; k < 6; ++k) { int b = -1; uint32_t bv = 0; for (int i = 0; i < 512; ++i) if (s_pf_tid[i] > bv) { bv = s_pf_tid[i]; b = i; } if (b < 0) break; serial_puts(" "); serial_putdec((uint64_t)b); serial_puts("="); serial_putdec(bv); s_pf_tid[b] = 0; }
    serial_puts("\n"); prof_sample_report();
    serial_puts("[PROF]  file fill so far: "); serial_putdec(s_fill_ms); serial_puts(" ms for "); serial_putdec(s_fill_pages); serial_puts(" pages");
    serial_puts("\n[PROF]  by call:");
    for (int k = 0; k < 8; ++k) { int b = -1; uint32_t bv = 0; for (int i = 0; i < 400; ++i) if (s_pf_nr[i] > bv) { bv = s_pf_nr[i]; b = i; } if (b < 0) break; serial_puts(" "); serial_putdec((uint64_t)b); serial_puts("="); serial_putdec(bv); s_pf_nr[b] = 0; }
    serial_puts("\n[PROF]  last epoll hits (tid fd kind watch ready):");
    for (int k = 0; k < 8; ++k) { serial_puts(" ("); for (int q = 0; q < 5; ++q) { if (q) serial_puts(" "); serial_puthex(s_epdbg[k][q]); } serial_puts(")"); }
    serial_puts("\n"); memset(s_pf_tid, 0, sizeof s_pf_tid); memset(s_pf_nr, 0, sizeof s_pf_nr); s_pf_total = 0;
}
bool cos_linux_try_syscall(struct regs *r) {
    if ((r->cs & 3) != 3) return false;
    process_t *p = process_get_current();
    if (!p || !p->linux_ctx) return false;
    uint8_t op[2];
    if (!copy_in(op, r->rip, 2) || op[0] != 0x0F || op[1] != 0x05) return false;
    lctx_t *c = ctx_of(p);
    uint64_t nr = r->rax;
    { lx_cur_t *cu = &s_cur[cur_tid() & 511]; cu->nr = r->rax; cu->a0 = r->rdi; cu->a1 = r->rsi; cu->a2 = r->rdx; cu->tid = (uint32_t)cur_tid(); }
    int64_t res = linux_syscall(p, c, r);
    prof_note((uint32_t)cur_tid(), nr);
    { static uint32_t cwn; if ((++cwn & 31) == 0) code_watch_step(p, c); }
    if (res != LX_FRAME_SET) trace_note(p, r, res);
    if (res == LX_FRAME_SET) return true;          /* execve and rt_sigreturn set every register themselves */
    r->rax = (uint64_t)res;
    r->rcx = r->rip + 2; r->r11 = r->rflags;      /* what the real instruction leaves behind */
    r->rip += 2;
    sig_poll_timers(c);
    deliver_signals(p, c, r, nr, res);             /* a handler runs when the call returns */
    return true;
}

/* debugging (/linux/etc/cos.audit): after this process gave its frames back, does its parent still map a frame the allocator thinks is free? */
static void cos_linux_audit_parent(process_t *proc) {
    static int on = -1; uint64_t sz; uint8_t d; if (on < 0) on = cos_fs_stat("/linux/etc/cos.audit", &sz, &d) ? 1 : 0; if (!on) return;
    process_t *pp = process_get_by_pid(proc->parent_pid); if (!pp || !pp->linux_ctx || !pp->page_dir || pp == proc) return;
    lctx_t *pc = ctx_of(pp); int bad = 0; uint64_t irq = sync_irq_save(); page_directory_t *saved = paging_get_current_directory();
    paging_switch_directory((page_directory_t *)pp->page_dir);
    for (int i = 0; i < pc->nvma && bad < 5; ++i) { if (pc->vma[i].shm) continue;
        for (uint64_t a = pc->vma[i].start; a < pc->vma[i].end && bad < 5; a += PAGE) { uint64_t ph = paging_virt_to_phys(a); if (ph && phys_page_is_free(ph & ~(PAGE - 1))) { bad++; serial_puts("[AUDIT] after pid "); serial_putdec(proc->pid); serial_puts(" released its pages, parent "); serial_putdec(pp->pid); serial_puts(" maps free frame 0x"); serial_puthex(ph); serial_puts(" at 0x"); serial_puthex(a); serial_puts(" vma prot "); serial_putdec(pc->vma[i].prot); serial_puts("\n"); } } }
    paging_switch_directory(saved); sync_irq_restore(irq);
}

/* Gives the frames of every range back (the address space of the process is active). */
void cos_linux_release_pages(process_t *proc) {
    if (!proc || !proc->linux_ctx) return;
    lctx_t *c = ctx_of(proc);
    for (int i = 0; i < c->nvma; ++i) {
        if (c->vma[i].shm) { for (uint64_t a = c->vma[i].start; a < c->vma[i].end; a += PAGE) if (paging_virt_to_phys(a)) paging_unmap_page(a); shm_unref(c->vma[i].shm); c->vma[i].shm = NULL; }
        else paging_release_user_range(c->vma[i].start, (c->vma[i].end - c->vma[i].start) / PAGE);
    }
    if (c->brk_start && c->brk_cur > c->brk_start) paging_release_user_range(c->brk_start, (pg_up(c->brk_cur) - c->brk_start) / PAGE);
    c->nvma = 0; c->brk_cur = c->brk_start;
    cos_linux_audit_parent(proc);
}

void cos_linux_release(process_t *proc) {
    if (!proc || !proc->linux_ctx) return;
    { lctx_t *cc = ctx_of(proc); for (int i = 0; i < LX_MAX_FD; ++i) fd_close_slot(cc, i); if (cc->vma) kfree(cc->vma); }
    kfree(proc->linux_ctx);
    proc->linux_ctx = NULL;
}

/* (the loader's RELRO pass is switched off above: a Linux libc makes its own RELRO read-only with mprotect once it has relocated itself) */

/* ---- starting a program ---------------------------------------------------- */

static const char *env_get(const char *const *envp, int envc, const char *name) {
    size_t n = strlen(name);
    for (int i = 0; i < envc; ++i) if (envp && envp[i] && !strncmp(envp[i], name, n) && envp[i][n] == '=') return envp[i] + n + 1;
    return NULL;
}

/* Maps `image` and its interpreter into the address space of `proc` (this switches to it) and builds the initial stack. The ranges of `c` must be empty. */
static bool lx_load(process_t *proc, lctx_t *c, const char *path, const uint8_t *image, uint64_t len, const char *const *argv, int argc, const char *const *envp, int envc, uint64_t *out_entry, uint64_t *out_rsp) {
    cos_elf_info_t info;
    if (!cos_elf_inspect(image, len, &info)) { serial_puts("[LINUX] not a loadable ELF\n"); return false; }
    if (info.e_type != 3 /* ET_DYN */) { serial_puts("[LINUX] only position-independent programs can run (build with -pie or -static-pie): this one is fixed-address\n"); return false; }

    uint8_t *interp_img = NULL; uint64_t interp_len = 0; char interp_path[256] = "";
    if (info.flags & COS_ELF_F_INTERP) {
        snprintf(interp_path, sizeof interp_path, "%s", info.interp);
        if (!interp_path[0]) return false;
        char host[300]; { char nm[320], fl[320]; path_normalize(interp_path, nm, sizeof nm); lxm_load(); lx_follow(nm, fl, sizeof fl, 1, NULL); snprintf(host, sizeof host, "/linux%s", fl); }
        uint64_t sz; uint8_t isdir; if (!cos_fs_stat(host, &sz, &isdir) || isdir || sz > (16ull << 20)) { serial_puts("[LINUX] interpreter not found: /linux"); serial_puts(interp_path); serial_puts("\n"); return false; }
        interp_img = (uint8_t *)kmalloc(sz); if (!interp_img) return false;
        int got = cos_fs_read_file(host, interp_img, sz); if (got <= 0) { kfree(interp_img); return false; }
        interp_len = (uint64_t)got;
    }

    cos_elf_object_t *obj = (cos_elf_object_t *)kmalloc(sizeof *obj), *iobj = NULL;
    uint64_t rsp = 0, entry = 0;
    bool ok = obj != NULL;
    uint64_t irq = sync_irq_save();
    page_directory_t *saved = paging_get_current_directory();
    cos_user_access_begin();
    paging_switch_directory((page_directory_t *)proc->page_dir);
    if (ok) ok = cos_elf_map((page_directory_t *)proc->page_dir, image, len, cos_elf_pie_base(), obj) && (obj->flags &= ~COS_ELF_F_RELRO, true) && cos_elf_protect((page_directory_t *)proc->page_dir, obj);
    if (ok && interp_img) {
        iobj = (cos_elf_object_t *)kmalloc(sizeof *iobj); ok = iobj != NULL;
        if (ok) ok = cos_elf_map((page_directory_t *)proc->page_dir, interp_img, interp_len, LX_INTERP_BIAS, iobj) && (iobj->flags &= ~COS_ELF_F_RELRO, true) && cos_elf_protect((page_directory_t *)proc->page_dir, iobj);
    }
    if (ok) {
        for (uint32_t i = 0; i < obj->seg_count; ++i) { uint32_t pr = (obj->segs[i].pflags & 4 ? L_PROT_READ : 0) | (obj->segs[i].pflags & 2 ? L_PROT_WRITE : 0) | (obj->segs[i].pflags & 1 ? L_PROT_EXEC : 0); vma_insert(c, obj->segs[i].start, obj->segs[i].end, pr ? pr : L_PROT_READ); }
        if (iobj) for (uint32_t i = 0; i < iobj->seg_count; ++i) { uint32_t pr = (iobj->segs[i].pflags & 4 ? L_PROT_READ : 0) | (iobj->segs[i].pflags & 2 ? L_PROT_WRITE : 0) | (iobj->segs[i].pflags & 1 ? L_PROT_EXEC : 0); vma_insert(c, iobj->segs[i].start, iobj->segs[i].end, pr ? pr : L_PROT_READ); }
        uint64_t top = 0; for (uint32_t i = 0; i < obj->seg_count; ++i) if (obj->segs[i].end > top) top = obj->segs[i].end;
        c->brk_start = c->brk_cur = pg_up(top) + ((cos_random_u64() & 0xFF) << 12) + 0x100000; c->brk_max = c->brk_start + (256ull << 20);
        c->mmap_next = LX_MMAP_BASE;

        cos_elf_stack_params_t sp; memset(&sp, 0, sizeof sp);
        sp.argv = argv; sp.argc = argc; sp.envp = envp; sp.envc = envc;
        sp.at_phdr = obj->phdr; sp.at_phent = obj->phentsize; sp.at_phnum = obj->phnum;
        sp.at_entry = obj->entry; sp.at_base = iobj ? iobj->base : 0;
        sp.execfn = path;
        sp.at_random_seed[0] = cos_random_u64(); sp.at_random_seed[1] = cos_random_u64();
        ok = cos_elf_setup_stack_ex((page_directory_t *)proc->page_dir, proc->stack_end, &sp, &rsp);
        entry = iobj ? iobj->entry : obj->entry;
    }
    paging_switch_directory(saved);
    cos_user_access_end();
    sync_irq_restore(irq);
    kfree(interp_img); kfree(obj); kfree(iobj);
    if (!ok) { serial_puts("[LINUX] could not load the program\n"); return false; }
    snprintf(c->exe, sizeof c->exe, "%s", path ? path : "");
    *out_entry = entry; *out_rsp = rsp;
    return true;
}

static int64_t spawn_image_inner(const char *name, const char *path, const uint8_t *image, uint64_t len, const char *const *argv, int argc, const char *const *envp, int envc) {
    process_t *proc = process_create(name, TASK_TYPE_USER);
    if (!proc) return -1;
    lctx_t *c = (lctx_t *)kmalloc(sizeof(lctx_t));
    if (!c) { process_destroy(proc); return -1; }
    memset(c, 0, sizeof *c);
    c->mmap_next = LX_MMAP_BASE; c->out_fd = -1;
    snprintf(c->cwd, sizeof c->cwd, "/");
    for (int i = 0; i < 3; ++i) { c->fd[i] = ofd_new(FD_CON, i == 0 ? 0 : L_O_WRONLY); if (!c->fd[i]) break; }
    proc->linux_ctx = c;
    const char *so = env_get(envp, envc, "COS_STDOUT");
    if (so && so[0]) { snprintf(c->out_path, sizeof c->out_path, "%s", so); c->out_fd = cos_fd_open(proc->pid, so, COS_O_WRONLY_APPEND); c->out_pid = proc->pid; }

    uint64_t rsp = 0, entry = 0;
    if (!lx_load(proc, c, path, image, len, argv, argc, envp, envc, &entry, &rsp)) { process_destroy(proc); return -1; }

    uint64_t fl = sync_irq_save();
    thread_t *th = thread_create(proc, (void *)(uintptr_t)entry, NULL);
    if (!th) { sync_irq_restore(fl); process_destroy(proc); return -1; }
    th->context.rsp = rsp;
    th_add(c, th->tid, 0);
    sync_irq_restore(fl);
    serial_puts("[LINUX] started "); serial_puts(path ? path : name); serial_puts(" pid="); serial_putdec(proc->pid);
    serial_puts(" entry=0x"); serial_puthex(entry); serial_puts("\n");
    return (int64_t)proc->pid;
}

int64_t cos_linux_spawn_image(const char *name, const char *path, const uint8_t *image, uint64_t len, const char *const *argv, int argc, const char *const *envp, int envc) {
    /* the environment a login shell would have given, for what the caller did not set; /linux/etc/cos.env (KEY=VALUE lines) adds more */
    static const char *const defenv[] = { "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root", "USER=root", "LOGNAME=root", "LANG=C", "TERM=xterm", "TMPDIR=/tmp", "SHELL=/bin/sh", NULL };
    const char **menv = NULL; char *envfile = NULL; int menvc = 0;
    {
        uint64_t esz = 0; uint8_t isd = 0; int extra = 0;
        if (cos_fs_stat("/linux/etc/cos.env", &esz, &isd) && !isd && esz > 0 && esz < 65536 && (envfile = (char *)kmalloc(esz + 1))) {
            if (cos_fs_read_file("/linux/etc/cos.env", envfile, esz) == (int)esz) { envfile[esz] = 0; for (char *q = envfile; *q; ++q) if (*q == '\n') extra++; extra++; } else { kfree(envfile); envfile = NULL; }
        }
        menv = (const char **)kmalloc((size_t)(envc + 16 + extra) * sizeof(char *));
        if (menv) {
            for (int i = 0; i < envc; ++i) menv[menvc++] = envp[i];
            for (int i = 0; defenv[i]; ++i) { size_t n = strchr(defenv[i], '=') - defenv[i]; int have = 0; for (int k = 0; k < menvc; ++k) if (!strncmp(menv[k], defenv[i], n + 1)) have = 1; if (!have) menv[menvc++] = defenv[i]; }
            if (envfile) for (char *q = envfile; *q; ) { char *nl = q; while (*nl && *nl != '\n') nl++; if (*nl) *nl++ = 0; if (*q && *q != '#' && strchr(q, '=')) { size_t n = (size_t)(strchr(q, '=') - q); int have = 0; for (int k = 0; k < menvc; ++k) if (!strncmp(menv[k], q, n + 1)) have = 1; if (!have) menv[menvc++] = q; } q = nl; }
            envp = (const char *const *)menv; envc = menvc;
        }
    }

    int64_t res = spawn_image_inner(name, path, image, len, argv, argc, envp, envc);
    if (menv) kfree(menv);
    if (envfile) kfree(envfile);
    return res;
}


int64_t cos_linux_spawn(const char *path, const char *const *argv, int argc, const char *const *envp, int envc) {
    uint64_t sz = 0; uint8_t isdir = 0;
    if (!path || !cos_fs_stat(path, &sz, &isdir) || isdir || sz < 64 || sz > (64ull << 20)) return -1;
    uint8_t *img = (uint8_t *)kmalloc(sz); if (!img) return -1;
    int got = cos_fs_read_file(path, img, sz);
    if (got <= 0) { kfree(img); return -1; }
    const char *base = path; for (const char *q = path; *q; ++q) if (*q == '/') base = q + 1;
    const char *dargv[1] = { path };
    if (!argv || argc <= 0) { argv = dargv; argc = 1; }
    int64_t pid = cos_linux_spawn_image(base, path, img, (uint64_t)got, argv, argc, envp, envc);
    kfree(img);
    return pid;
}
