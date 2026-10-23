/* cos_kmod.c - loads .c-osys kernel modules (see src/include/cosys.h and docs/c-osys.md).
 *
 * A .c-osys is a shared object like a .c-osll, and it goes through the same ELF engine (cos_elf.c, here in "kernel mode": the module window of the kernel half,
 * supervisor-only pages): header and segment validation, W^X, eager relocation, RELRO, GNU hash lookup, DT_NEEDED. In this order:
 *  1. The signature. The module runs in ring 0, so nothing of an unsigned or modified file is even parsed: the SHA-256 of the object bytes must verify
 *     (ECDSA P-256) against the module key compiled into the kernel (build/kmod_pubkey.h; tools/gen_kmod_keys.py makes it).
 *  2. cos_elf_inspect(): an ET_DYN with no interpreter, no TLS, no executable stack and no text relocations, small enough for its slot (4 MiB).
 *  3. DT_NEEDED: every dependency must be a module that is already loaded (boot retries a few times so the order of files does not matter).
 *  4. Mapping into the module's slot, then the module's own header (cosys_module_info) is read to learn its permission LEVEL before anything is relocated.
 *  5. Relocation. Undefined symbols resolve ONLY against the kernel's export table below (every entry has a level; the module's level must be at least that)
 *     and against the exports of the modules it needs. Anything else fails the load, naming the symbol.
 *  6. Final permissions (code r-x, constants r--, data rw- and never both w and x), metadata checks, then init(). A module that fails init is unloaded.
 * Loading at run time (SYS_KMOD_LOAD) takes a fully privileged caller (never a sandboxed one) and the administrator password when an account exists;
 * the signature is what makes the file trustworthy, the password what makes the person at the keyboard the one who asked. Unloading refuses while another
 * module needs this one or while one of its system calls is running. */
#include "types.h"
#include "string.h"
#include "serial.h"
#include "memory.h"
#include "paging.h"
#include "memory_physical.h"
#include "timer.h"
#include "sync.h"
#include "io.h"
#include "task.h"
#include "../fs/fs.h"
#include "cos_fm_support.h"
#include "cos_elf.h"
extern page_directory_t *paging_get_current_directory(void);
extern uint64_t paging_virt_to_phys(uint64_t v);
extern void paging_free_physical(uint64_t phys);
#include "cosys.h"
#include "idt.h"
#include "smp.h"
#include "irq.h"
#include "cos_caps.h"
#include "bearssl.h"
#include "kmod_pubkey.h"                                   /* cos_kmod_keys[]: { level, uncompressed P-256 point } for each trusted signing key */

#define KMOD_BASE      0xFFFFFFFFC0000000ULL
#define KMOD_SLOT      (8ULL << 20)                        /* the address window of one module; the module itself is limited to KMOD_MAX_SPAN of it */
#define KMOD_MAX_SPAN  (4ULL << 20)
#define KMOD_END_PAGE  0xFFFFFFFFFFFFF000ULL               /* kept mapped, so the page tables above the window always exist */
#define KMOD_MAX_FILE  (1024u * 1024u)
#define KMOD_MAX_MODS  16
#define KMOD_MAX_DEPS  4
#define KMOD_MAX_XSEG  4
#define KMOD_ALLOC_MAX   (256u * 1024u)
#define KMOD_MEM_QUOTA   (1024u * 1024u)

#define ET_DYN_ 3
#define PF_X_ 1
#define PF_W_ 2

typedef struct alloc_hdr { struct alloc_hdr *next, *prev; uint32_t owner, magic; uint64_t size; } alloc_hdr_t;     /* 32 bytes: the payload stays 16-aligned */
#define ALLOC_MAGIC 0xA110C8EDu

typedef struct {
    bool used, unloading;
    char name[32], version[16];
    uint32_t level;
    uint64_t base, lo, hi;                                 /* slot base; the mapped range [lo, hi) */
    uint64_t xlo[KMOD_MAX_XSEG], xhi[KMOD_MAX_XSEG]; int nx;      /* the executable ranges */
    cos_elf_object_t obj;
    const struct cosys_module *info;
    int deps[KMOD_MAX_DEPS]; int ndeps; int refs;
    volatile int inflight;
    alloc_hdr_t *allocs; uint64_t mem_used;
    struct { uint64_t va, size; bool used; } mmio[4];                  /* device memory mapped for this module */
    struct { void *p; size_t size; } dma[8]; uint64_t dma_total;       /* DMA blocks */
} kmod_t;

static kmod_t mods[KMOD_MAX_MODS];
static bool kmod_ready;
static volatile int busy;                                  /* one load or unload at a time */
static spinlock_t tab_lock = SPINLOCK_INIT;                /* guards the tables below and the allocation lists */
static struct { bool used; cosys_syscall_fn fn; uint32_t caps; int owner; } sc_tab[COSYS_SYSCALL_COUNT];
#define KMOD_MAX_SUBS 16
static struct { bool used; cosys_event_fn fn; uint32_t mask; int owner; } subs[KMOD_MAX_SUBS];
static volatile uint32_t g_event_mask;                    /* union of every subscription's mask: the kernel's fast path */
static char fail_why[96];
static bool fail(const char *why) { size_t n = strlen(why); if (n >= sizeof fail_why) n = sizeof fail_why - 1; memcpy(fail_why, why, n); fail_why[n] = 0; return false; }

/* ---------------------------------------------------------------- who is calling (by return address) */
static int owner_of(uint64_t ra) {
    for (int i = 0; i < KMOD_MAX_MODS; i++) if (mods[i].used && ra >= mods[i].lo && ra < mods[i].hi) return i;
    return -1;
}
#define CALLER() owner_of((uint64_t)(uintptr_t)__builtin_return_address(0))

/* ---------------------------------------------------------------- the export table: BASIC */
__attribute__((noinline)) static void k_log(const char *text) {
    int o = CALLER();
    serial_puts("[KMOD:"); serial_puts(o >= 0 ? mods[o].name : "?"); serial_puts("] "); serial_puts(text ? text : ""); serial_puts("\n");
}
__attribute__((noinline)) static void k_log_hex(const char *text, uint64_t v) {
    int o = CALLER();
    serial_puts("[KMOD:"); serial_puts(o >= 0 ? mods[o].name : "?"); serial_puts("] "); serial_puts(text ? text : ""); serial_puthex(v); serial_puts("\n");
}
static uint64_t k_time_ms(void) { return (get_timer_ticks() * 1000ULL) / TIMER_TICKS_PER_SEC; }
__attribute__((noinline)) static void *k_alloc(size_t n) {
    int o = CALLER();
    if (o < 0 || n == 0 || n > KMOD_ALLOC_MAX) return NULL;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    bool ok = mods[o].mem_used + n <= KMOD_MEM_QUOTA; if (ok) mods[o].mem_used += n;
    spin_unlock_irqrestore(&tab_lock, fl);
    if (!ok) return NULL;
    alloc_hdr_t *h = (alloc_hdr_t *)kmalloc(sizeof *h + n);
    if (!h) { fl = spin_lock_irqsave(&tab_lock); mods[o].mem_used -= n; spin_unlock_irqrestore(&tab_lock, fl); return NULL; }
    memset(h, 0, sizeof *h + n); h->owner = (uint32_t)o; h->magic = ALLOC_MAGIC; h->size = n;
    fl = spin_lock_irqsave(&tab_lock);
    h->next = mods[o].allocs; if (h->next) h->next->prev = h; mods[o].allocs = h;
    spin_unlock_irqrestore(&tab_lock, fl);
    return h + 1;
}
static void alloc_unlink(alloc_hdr_t *h) {                 /* tab_lock held */
    if (h->prev) h->prev->next = h->next; else mods[h->owner].allocs = h->next;
    if (h->next) h->next->prev = h->prev;
    mods[h->owner].mem_used -= h->size; h->magic = 0;
}
__attribute__((noinline)) static void k_free(void *p) {
    int o = CALLER();
    if (!p || o < 0) return;
    alloc_hdr_t *h = (alloc_hdr_t *)p - 1; bool ok = false;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (alloc_hdr_t *a = mods[o].allocs; a; a = a->next) if (a == h && a->magic == ALLOC_MAGIC) { alloc_unlink(h); ok = true; break; }      /* only its own live blocks */
    spin_unlock_irqrestore(&tab_lock, fl);
    if (ok) kfree(h);
}
static void *k_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *k_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static void *k_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static int k_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static size_t k_strlen(const char *s) { return strlen(s); }

/* ---------------------------------------------------------------- SERVICE */
__attribute__((noinline)) static int k_register_syscall(uint32_t nr, cosys_syscall_fn fn, uint32_t caps) {
    int o = CALLER();
    if (o < 0 || nr < COSYS_SYSCALL_FIRST || nr >= COSYS_SYSCALL_FIRST + COSYS_SYSCALL_COUNT || !fn) return -1;
    kmod_t *m = &mods[o]; uint64_t a = (uint64_t)(uintptr_t)fn; bool in_code = false;
    for (int i = 0; i < m->nx; i++) if (a >= m->xlo[i] && a < m->xhi[i]) in_code = true;
    if (!in_code) return -1;                                                                    /* the handler is executable code of this module */
    uint32_t i = nr - COSYS_SYSCALL_FIRST; int rc = -1;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    if (!sc_tab[i].used) { sc_tab[i].used = true; sc_tab[i].fn = fn; sc_tab[i].caps = caps; sc_tab[i].owner = o; rc = 0; }
    spin_unlock_irqrestore(&tab_lock, fl);
    return rc;
}
__attribute__((noinline)) static int k_unregister_syscall(uint32_t nr) {
    int o = CALLER();
    if (o < 0 || nr < COSYS_SYSCALL_FIRST || nr >= COSYS_SYSCALL_FIRST + COSYS_SYSCALL_COUNT) return -1;
    uint32_t i = nr - COSYS_SYSCALL_FIRST; int rc = -1;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    if (sc_tab[i].used && sc_tab[i].owner == o) { sc_tab[i].used = false; rc = 0; }
    spin_unlock_irqrestore(&tab_lock, fl);
    return rc;
}
static int k_copy_from_user(void *dst, uint64_t src, size_t n) {
    if (!dst) return -1;
    if (n == 0) return 0;
    if (!paging_user_range_ok(src, n, false)) return -1;
    memcpy(dst, (const void *)(uintptr_t)src, n); return 0;
}
static int k_copy_to_user(uint64_t dst, const void *src, size_t n) {
    if (!src) return -1;
    if (n == 0) return 0;
    if (!paging_user_range_ok(dst, n, true)) return -1;
    memcpy((void *)(uintptr_t)dst, src, n); return 0;
}
static uint64_t k_current_pid(void) { process_t *p = process_get_current(); return p ? p->pid : 0; }
static uint32_t k_current_caps(void) { process_t *p = process_get_current(); return p ? p->caps : 0; }

__attribute__((noinline)) static void k_log_dec(const char *text, uint64_t v) {
    int o = CALLER();
    serial_puts("[KMOD:"); serial_puts(o >= 0 ? mods[o].name : "?"); serial_puts("] "); serial_puts(text ? text : ""); serial_putdec(v); serial_puts("\n");
}
static uint64_t k_lock(cosys_lock_t *l) { return spin_lock_irqsave((spinlock_t *)l); }
static void k_unlock(cosys_lock_t *l, uint64_t flags) { spin_unlock_irqrestore((spinlock_t *)l, flags); }

/* Event subscriptions. cos_kmod_event() (called by the kernel at the points of cos_kevent.h) runs the callbacks outside the table lock, counted in the
 * module's `inflight` like a system call, so a module cannot unload while one of its callbacks runs. */
static void recompute_event_mask(void) {                  /* tab_lock held */
    uint32_t m = 0;
    for (int i = 0; i < KMOD_MAX_SUBS; i++) if (subs[i].used) m |= subs[i].mask;
    __atomic_store_n(&g_event_mask, m, __ATOMIC_RELEASE);
}
__attribute__((noinline)) static int k_subscribe(uint32_t mask, cosys_event_fn fn) {
    int o = CALLER();
    if (o < 0 || !fn || !(mask & ((1u << COSYS_EV_COUNT) - 1u))) return -1;
    kmod_t *m = &mods[o]; uint64_t a = (uint64_t)(uintptr_t)fn; bool in_code = false;
    for (int i = 0; i < m->nx; i++) if (a >= m->xlo[i] && a < m->xhi[i]) in_code = true;
    if (!in_code) return -1;
    int rc = -1, freei = -1; bool dup = false;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < KMOD_MAX_SUBS; i++) {
        if (subs[i].used && subs[i].fn == fn) dup = true;
        if (!subs[i].used && freei < 0) freei = i;
    }
    if (!dup && freei >= 0) { subs[freei].used = true; subs[freei].fn = fn; subs[freei].mask = mask & ((1u << COSYS_EV_COUNT) - 1u); subs[freei].owner = o; rc = 0; }
    recompute_event_mask();
    spin_unlock_irqrestore(&tab_lock, fl);
    return rc;
}
__attribute__((noinline)) static int k_unsubscribe(cosys_event_fn fn) {
    int o = CALLER(); int rc = -1;
    if (o < 0) return -1;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < KMOD_MAX_SUBS; i++) if (subs[i].used && subs[i].fn == fn && subs[i].owner == o) { subs[i].used = false; rc = 0; }
    recompute_event_mask();
    spin_unlock_irqrestore(&tab_lock, fl);
    return rc;
}
void cos_kmod_event(uint32_t type, uint64_t a, uint64_t b, uint64_t c) {
    if (type >= COSYS_EV_COUNT || !(__atomic_load_n(&g_event_mask, __ATOMIC_ACQUIRE) & (1u << type))) return;
    cosys_event_fn fns[KMOD_MAX_SUBS]; int owners[KMOD_MAX_SUBS]; int n = 0;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < KMOD_MAX_SUBS; i++) {
        if (!subs[i].used || !(subs[i].mask & (1u << type))) continue;
        int o = subs[i].owner;
        if (!mods[o].used || mods[o].unloading) continue;
        mods[o].inflight++; fns[n] = subs[i].fn; owners[n] = o; n++;
    }
    spin_unlock_irqrestore(&tab_lock, fl);
    for (int i = 0; i < n; i++) { fns[i](type, a, b, c); __sync_fetch_and_sub(&mods[owners[i]].inflight, 1); }
}

/* ---------------------------------------------------------------- DEVICE */
static bool port_ok(uint16_t p, unsigned width) {
    uint32_t last = (uint32_t)p + width - 1;
    static const struct { uint16_t lo, hi; } deny[] = { {0x20,0x21},{0x40,0x43},{0x60,0x64},{0x70,0x71},{0x92,0x92},{0xA0,0xA1},{0xCF8,0xCFF} };
    for (unsigned i = 0; i < sizeof deny / sizeof deny[0]; i++) if ((uint32_t)p <= deny[i].hi && last >= deny[i].lo) return false;
    return true;
}
static uint8_t  k_inb(uint16_t p) { return port_ok(p, 1) ? inb(p) : 0xFF; }
static uint16_t k_inw(uint16_t p) { return port_ok(p, 2) ? inw(p) : 0xFFFF; }
static uint32_t k_inl(uint16_t p) { return port_ok(p, 4) ? inl(p) : 0xFFFFFFFFu; }
static int k_outb(uint16_t p, uint8_t v)  { if (!port_ok(p, 1)) return -1; outb(p, v); return 0; }
static int k_outw(uint16_t p, uint16_t v) { if (!port_ok(p, 2)) return -1; outw(p, v); return 0; }
static int k_outl(uint16_t p, uint32_t v) { if (!port_ok(p, 4)) return -1; outl(p, v); return 0; }
static uint32_t k_pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    if (dev > 31 || fn > 7) return 0xFFFFFFFFu;
    uint64_t fl = spin_lock_irqsave(&tab_lock);                                                 /* the two-step access must not interleave */
    outl(0xCF8, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (off & 0xFC));
    uint32_t v = inl(0xCFC);
    spin_unlock_irqrestore(&tab_lock, fl);
    return v;
}

/* ---- drivers: interrupts, device memory, DMA ---- */
#define KMOD_MMIO_BASE   0xFFFFFFFFC8000000ULL                 /* right above the module slots: 64 slots of 1 MiB */
#define KMOD_MMIO_SLOT   (1ULL << 20)
#define KMOD_MMIO_SLOTS  64
static bool mmio_slot_used[KMOD_MMIO_SLOTS];
static struct { bool used; cosys_irq_fn fn; void *ctx; int owner; int irq; } irq_tab[16];
extern bool irq_line_is_free(int irq);
extern void irq_release_line(int irq);
extern void irq_set_mask(uint8_t irq);
extern void irq_clear_mask(uint8_t irq);

static bool irq_has_subs(int irq) { for (int i = 0; i < 16; i++) if (irq_tab[i].used && irq_tab[i].irq == irq) return true; return false; }   /* tab_lock held */

/* The kernel's handler for a line taken by modules: runs each subscriber like a system call (counted in `inflight`, so a module cannot unload under it). */
static void kmod_irq_dispatch(struct regs *r) {
    int irq = (int)r->int_no - 32;
    cosys_irq_fn fns[16]; void *ctxs[16]; int owners[16]; int n = 0;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < 16; i++) {
        if (!irq_tab[i].used || irq_tab[i].irq != irq) continue;
        int o = irq_tab[i].owner;
        if (!mods[o].used || mods[o].unloading) continue;
        mods[o].inflight++; fns[n] = irq_tab[i].fn; ctxs[n] = irq_tab[i].ctx; owners[n] = o; n++;
    }
    spin_unlock_irqrestore(&tab_lock, fl);
    for (int i = 0; i < n; i++) { fns[i](ctxs[i]); __sync_fetch_and_sub(&mods[owners[i]].inflight, 1); }
}
static bool in_module_code(int o, const void *fn) {
    uint64_t a = (uint64_t)(uintptr_t)fn;
    for (int i = 0; i < mods[o].nx; i++) if (a >= mods[o].xlo[i] && a < mods[o].xhi[i]) return true;
    return false;
}
__attribute__((noinline)) static int k_irq_register(int irq, cosys_irq_fn fn, void *ctx) {
    int o = CALLER();
    static const uint16_t allowed = (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 15);
    if (o < 0 || irq < 0 || irq > 15 || !((allowed >> irq) & 1u) || !fn || !in_module_code(o, fn)) return -1;
    int rc = -1, mine = 0, freei = -1; bool first = false;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < 16; i++) {
        if (irq_tab[i].used && irq_tab[i].owner == o) mine++;
        if (irq_tab[i].used && irq_tab[i].irq == irq && irq_tab[i].fn == fn) { freei = -2; break; }
        if (!irq_tab[i].used && freei == -1) freei = i;
    }
    bool taken_by_modules = irq_has_subs(irq);
    if (freei >= 0 && mine < 4 && (taken_by_modules || irq_line_is_free(irq))) {
        irq_tab[freei].used = true; irq_tab[freei].fn = fn; irq_tab[freei].ctx = ctx; irq_tab[freei].owner = o; irq_tab[freei].irq = irq;
        first = !taken_by_modules; rc = 0;
    }
    spin_unlock_irqrestore(&tab_lock, fl);
    if (rc == 0 && first) irq_install_handler(irq, kmod_irq_dispatch);     /* installs the vector and unmasks the line */
    return rc;
}
static void irq_drop(int irq) {                             /* after the last subscriber of a line left: back to the placeholder */
    bool any;
    uint64_t fl = spin_lock_irqsave(&tab_lock); any = irq_has_subs(irq); spin_unlock_irqrestore(&tab_lock, fl);
    if (!any) irq_release_line(irq);
}
__attribute__((noinline)) static int k_irq_unregister(int irq, cosys_irq_fn fn) {
    int o = CALLER(); int rc = -1;
    if (o < 0) return -1;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < 16; i++) if (irq_tab[i].used && irq_tab[i].irq == irq && irq_tab[i].fn == fn && irq_tab[i].owner == o) { irq_tab[i].used = false; rc = 0; }
    spin_unlock_irqrestore(&tab_lock, fl);
    if (rc == 0) irq_drop(irq);
    return rc;
}

__attribute__((noinline)) static void *k_mmio_map(uint64_t phys, size_t size) {
    int o = CALLER();
    if (o < 0 || size == 0 || size > KMOD_MMIO_SLOT || phys + size < phys) return NULL;
    uint64_t pa0 = phys & ~(uint64_t)(PAGE_SIZE - 1), end = (phys + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t pa = pa0; pa < end; pa += PAGE_SIZE) if (phys_is_valid_addr((phys_addr_t)pa)) return NULL;       /* RAM is not device memory */
    int mi = -1, si = -1;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < 4; i++) if (!mods[o].mmio[i].used) { mi = i; break; }
    for (int i = 0; i < KMOD_MMIO_SLOTS; i++) if (!mmio_slot_used[i]) { si = i; break; }
    if (mi >= 0 && si >= 0) { mmio_slot_used[si] = true; mods[o].mmio[mi].used = true; mods[o].mmio[mi].va = KMOD_MMIO_BASE + (uint64_t)si * KMOD_MMIO_SLOT; mods[o].mmio[mi].size = end - pa0; }
    spin_unlock_irqrestore(&tab_lock, fl);
    if (mi < 0 || si < 0) return NULL;
    uint64_t va = mods[o].mmio[mi].va;
    for (uint64_t off = 0; off < end - pa0; off += PAGE_SIZE) {
        if (!paging_map_page(va + off, pa0 + off, PAGE_PRESENT | PAGE_RW | PAGE_NOCACHE | PAGE_WRITETHROUGH | PAGE_NX)) {
            for (uint64_t u = 0; u < off; u += PAGE_SIZE) paging_unmap_page(va + u);
            fl = spin_lock_irqsave(&tab_lock); mods[o].mmio[mi].used = false; mmio_slot_used[si] = false; spin_unlock_irqrestore(&tab_lock, fl);
            return NULL;
        }
    }
    return (void *)(uintptr_t)(va + (phys - pa0));
}
static void mmio_unmap_entry(int o, int mi) {                /* the entry is marked used */
    uint64_t va = mods[o].mmio[mi].va, n = mods[o].mmio[mi].size;
    for (uint64_t off = 0; off < n; off += PAGE_SIZE) paging_unmap_page(va + off);
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    mmio_slot_used[(va - KMOD_MMIO_BASE) / KMOD_MMIO_SLOT] = false; mods[o].mmio[mi].used = false;
    spin_unlock_irqrestore(&tab_lock, fl);
}
__attribute__((noinline)) static void k_mmio_unmap(void *p) {
    int o = CALLER();
    if (o < 0) return;
    uint64_t a = (uint64_t)(uintptr_t)p;
    for (int i = 0; i < 4; i++) if (mods[o].mmio[i].used && a >= mods[o].mmio[i].va && a < mods[o].mmio[i].va + mods[o].mmio[i].size) { mmio_unmap_entry(o, i); return; }
}

__attribute__((noinline)) static void *k_dma_alloc(size_t size, uint64_t *phys) {
    int o = CALLER();
    if (o < 0 || !phys || size == 0 || size > 65536) return NULL;
    size_t n = (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    int di = -1; bool ok = false;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < 8; i++) if (!mods[o].dma[i].p && mods[o].dma[i].size == 0) { di = i; break; }
    if (di >= 0 && mods[o].dma_total + n <= (1u << 20)) { mods[o].dma_total += n; mods[o].dma[di].size = n; ok = true; }      /* reserved: size set, pointer filled below */
    spin_unlock_irqrestore(&tab_lock, fl);
    if (!ok) return NULL;
    void *p = kmalloc_aligned(n, PAGE_SIZE);
    uint64_t p0 = p ? paging_virt_to_phys((uint64_t)(uintptr_t)p) : 0;
    bool contiguous = p0 && p0 + n <= (1ull << 32);
    for (size_t off = PAGE_SIZE; contiguous && off < n; off += PAGE_SIZE) if (paging_virt_to_phys((uint64_t)(uintptr_t)p + off) != p0 + off) contiguous = false;
    if (!contiguous) {
        if (p) kfree(p);
        fl = spin_lock_irqsave(&tab_lock); mods[o].dma_total -= n; mods[o].dma[di].size = 0; spin_unlock_irqrestore(&tab_lock, fl);
        return NULL;
    }
    memset(p, 0, n);
    fl = spin_lock_irqsave(&tab_lock); mods[o].dma[di].p = p; spin_unlock_irqrestore(&tab_lock, fl);
    *phys = p0;
    return p;
}
__attribute__((noinline)) static void k_dma_free(void *p) {
    int o = CALLER(); void *f = NULL;
    if (o < 0 || !p) return;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < 8; i++) if (mods[o].dma[i].p == p) { f = p; mods[o].dma_total -= mods[o].dma[i].size; mods[o].dma[i].p = NULL; mods[o].dma[i].size = 0; break; }
    spin_unlock_irqrestore(&tab_lock, fl);
    if (f) kfree(f);
}
static int k_pci_enable(uint8_t bus, uint8_t dev, uint8_t fn, uint32_t flags) {
    if (dev > 31 || fn > 7 || !(flags & 7u)) return -1;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    uint32_t addr = 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8;
    outl(0xCF8, addr); uint32_t id = inl(0xCFC);
    int rc = -1;
    if ((id & 0xFFFFu) != 0xFFFFu) {
        outl(0xCF8, addr | 0x04); uint32_t cmd = inl(0xCFC) & 0xFFFFu;           /* the status half is written as 0: its bits are write-one-to-clear */
        outl(0xCF8, addr | 0x04); outl(0xCFC, cmd | (flags & 7u));
        rc = 0;
    }
    spin_unlock_irqrestore(&tab_lock, fl);
    return rc;
}

/* ---- interrupts through the Local APIC: MSI of a PCI function, and routes of the I/O APIC (vectors 0x40-0x5F, see boot/irq.asm) ---- */
#define APIC_VEC_FIRST 0x40
#define APIC_VEC_COUNT 32
#define APIC_PER_MOD   8
static struct { bool used; uint8_t kind; /* 1 MSI, 2 I/O APIC */ cosys_irq_fn fn; void *ctx; int owner; uint32_t where; /* bus<<16|dev<<8|fn, or the GSI */ } apic_tab[APIC_VEC_COUNT];
static bool apic_handlers_set;

static void kmod_apic_dispatch(struct regs *r) {
    int v = (int)r->int_no - APIC_VEC_FIRST;
    if (v < 0 || v >= APIC_VEC_COUNT) return;
    cosys_irq_fn fn = NULL; void *ctx = NULL; int o = -1;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    if (apic_tab[v].used && mods[apic_tab[v].owner].used && !mods[apic_tab[v].owner].unloading) { fn = apic_tab[v].fn; ctx = apic_tab[v].ctx; o = apic_tab[v].owner; mods[o].inflight++; }
    spin_unlock_irqrestore(&tab_lock, fl);
    if (!fn) return;
    fn(ctx);
    __sync_fetch_and_sub(&mods[o].inflight, 1);
}
/* PCI configuration dword access; the caller holds tab_lock (the two-step access must not interleave) */
static uint32_t pci_rd(uint32_t bdf, uint8_t off) { outl(0xCF8, 0x80000000u | bdf << 8 | (off & 0xFC)); return inl(0xCFC); }
static void pci_wr(uint32_t bdf, uint8_t off, uint32_t v) { outl(0xCF8, 0x80000000u | bdf << 8 | (off & 0xFC)); outl(0xCFC, v); }
static int pci_find_cap(uint32_t bdf, uint8_t id) {         /* offset of the capability, or -1 */
    if (!(pci_rd(bdf, 0x04) & (1u << 20))) return -1;
    unsigned p = pci_rd(bdf, 0x34) & 0xFCu;
    for (int n = 0; p >= 0x40 && n < 48; n++) {
        uint32_t h = pci_rd(bdf, (uint8_t)p);
        if ((h & 0xFF) == id) return (int)p;
        p = (h >> 8) & 0xFCu;
    }
    return -1;
}
static int apic_alloc(int o, uint8_t kind, uint32_t where, cosys_irq_fn fn, void *ctx) {   /* tab_lock held: a vector index, or -1 */
    int mine = 0, freei = -1;
    for (int i = 0; i < APIC_VEC_COUNT; i++) {
        if (apic_tab[i].used && apic_tab[i].owner == o) mine++;
        if (apic_tab[i].used && apic_tab[i].kind == kind && apic_tab[i].where == where) return -1;
        if (!apic_tab[i].used && freei < 0) freei = i;
    }
    if (freei < 0 || mine >= APIC_PER_MOD) return -1;
    apic_tab[freei].used = true; apic_tab[freei].kind = kind; apic_tab[freei].where = where; apic_tab[freei].fn = fn; apic_tab[freei].ctx = ctx; apic_tab[freei].owner = o;
    return freei;
}
static void apic_hw_off(uint8_t kind, uint32_t where) {      /* stop the source of an interrupt that is going away; tab_lock not held */
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    if (kind == 1) {
        int c = pci_find_cap(where, 0x05);
        if (c >= 0) {
            uint32_t h = pci_rd(where, (uint8_t)c);
            pci_wr(where, (uint8_t)c, (h & ~(1u << 16)));                         /* MSI enable off (the control word is the high half) */
            uint32_t cmd = pci_rd(where, 0x04) & 0xFFFFu;
            pci_wr(where, 0x04, cmd & ~(1u << 10));                               /* INTx allowed again */
        }
    } else if (kind == 2) {
        uint32_t base; volatile uint32_t *io = smp_ioapic(&base);
        if (io) { uint32_t e = where - base; io[0] = 0x10 + 2 * e; io[4] = 1u << 16; }          /* masked */
    }
    spin_unlock_irqrestore(&tab_lock, fl);
}
static void apic_set_handlers(void) {
    if (apic_handlers_set) return;
    apic_handlers_set = true;
    for (int v = 0; v < APIC_VEC_COUNT; v++) register_interrupt_handler((uint8_t)(APIC_VEC_FIRST + v), kmod_apic_dispatch);
}
/* Message-signalled interrupt: the vector (0x40..0x5F) is allocated here and written into the function's MSI capability, which is then enabled (and legacy INTx disabled). 1 message, edge. */
__attribute__((noinline)) static int k_msi_register(uint8_t bus, uint8_t dev, uint8_t fn, cosys_irq_fn handler, void *ctx) {
    int o = CALLER();
    if (o < 0 || dev > 31 || fn > 7 || !handler || !in_module_code(o, handler) || !smp_lapic_ensure()) return -1;
    uint32_t where = (uint32_t)bus << 16 | (uint32_t)dev << 8 | fn;
    uint32_t bdf = (uint32_t)bus << 8 | (uint32_t)dev << 3 | fn;                         /* pci_rd shifts it left by 8 into the CF8 address */
    int rc = -1;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    if ((pci_rd(bdf, 0) & 0xFFFFu) != 0xFFFFu) {
        int c = pci_find_cap(bdf, 0x05);
        int vi = c >= 0 ? apic_alloc(o, 1, where, handler, ctx) : -1;
        if (vi >= 0) {
            uint32_t h = pci_rd(bdf, (uint8_t)c);
            bool is64 = (h >> 16) & 0x80;
            pci_wr(bdf, (uint8_t)c + 4, 0xFEE00000u | smp_bsp_apic_id() << 12);
            if (is64) { pci_wr(bdf, (uint8_t)c + 8, 0); pci_wr(bdf, (uint8_t)c + 12, (uint32_t)(APIC_VEC_FIRST + vi)); }
            else pci_wr(bdf, (uint8_t)c + 8, (uint32_t)(APIC_VEC_FIRST + vi));
            h &= ~(7u << 20);                                                           /* multiple message enable = 0: one message */
            pci_wr(bdf, (uint8_t)c, h | (1u << 16));                                    /* MSI enable */
            uint32_t cmd = pci_rd(bdf, 0x04) & 0xFFFFu;
            pci_wr(bdf, 0x04, cmd | (1u << 10));                                        /* INTx disable */
            rc = APIC_VEC_FIRST + vi;
        }
    }
    spin_unlock_irqrestore(&tab_lock, fl);
    if (rc >= 0) { apic_set_handlers(); }
    return rc;
}
static void apic_drop(int o, uint8_t kind, uint32_t where) {
    bool found = false;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < APIC_VEC_COUNT; i++) if (apic_tab[i].used && apic_tab[i].owner == o && apic_tab[i].kind == kind && apic_tab[i].where == where) { found = true; break; }
    spin_unlock_irqrestore(&tab_lock, fl);
    if (!found) return;
    uint32_t hw = kind == 1 ? ((where >> 16) << 8 | ((where >> 8) & 0xFF) << 3 | (where & 7)) : where;
    apic_hw_off(kind, hw);                                                       /* silence the source first, then free the vector */
    fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < APIC_VEC_COUNT; i++) if (apic_tab[i].used && apic_tab[i].owner == o && apic_tab[i].kind == kind && apic_tab[i].where == where) apic_tab[i].used = false;
    spin_unlock_irqrestore(&tab_lock, fl);
}
__attribute__((noinline)) static int k_msi_unregister(uint8_t bus, uint8_t dev, uint8_t fn) {
    int o = CALLER();
    if (o < 0) return -1;
    uint32_t where = (uint32_t)bus << 16 | (uint32_t)dev << 8 | fn;
    bool had = false;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < APIC_VEC_COUNT; i++) if (apic_tab[i].used && apic_tab[i].owner == o && apic_tab[i].kind == 1 && apic_tab[i].where == where) had = true;
    spin_unlock_irqrestore(&tab_lock, fl);
    if (!had) return -1;
    apic_drop(o, 1, where);
    return 0;
}
/* A route of the I/O APIC: global system interrupt `gsi` (16 and up: the PCI interrupt inputs; lower ones belong to the legacy lines and cosys_irq_register) is delivered to the
 * boot CPU as a free vector. flags: COSYS_IOAPIC_LOW (active low) | COSYS_IOAPIC_LEVEL (level-triggered, as PCI INTx is). One module per GSI. Returns the vector or -1. */
__attribute__((noinline)) static int k_ioapic_route(uint32_t gsi, uint32_t flags, cosys_irq_fn handler, void *ctx) {
    int o = CALLER();
    if (o < 0 || gsi < 16 || gsi > 255 || (flags & ~3u) || !handler || !in_module_code(o, handler) || !smp_lapic_ensure()) return -1;
    uint32_t base = 0; volatile uint32_t *io = smp_ioapic(&base);
    if (!io) return -1;
    int rc = -1;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    io[0] = 1; uint32_t maxe = (io[4] >> 16) & 0xFF;
    if (gsi >= base && gsi - base <= maxe) {
        int vi = apic_alloc(o, 2, gsi, handler, ctx);
        if (vi >= 0) {
            uint32_t e = gsi - base;
            io[0] = 0x10 + 2 * e; io[4] = 1u << 16;                                       /* masked while the destination is set */
            io[0] = 0x11 + 2 * e; io[4] = smp_bsp_apic_id() << 24;
            io[0] = 0x10 + 2 * e; io[4] = (uint32_t)(APIC_VEC_FIRST + vi) | ((flags & 1u) ? 1u << 13 : 0) | ((flags & 2u) ? 1u << 15 : 0);
            rc = APIC_VEC_FIRST + vi;
        }
    }
    spin_unlock_irqrestore(&tab_lock, fl);
    if (rc >= 0) apic_set_handlers();
    return rc;
}
__attribute__((noinline)) static int k_ioapic_unroute(uint32_t gsi) {
    int o = CALLER();
    if (o < 0) return -1;
    bool had = false;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < APIC_VEC_COUNT; i++) if (apic_tab[i].used && apic_tab[i].owner == o && apic_tab[i].kind == 2 && apic_tab[i].where == gsi) had = true;
    spin_unlock_irqrestore(&tab_lock, fl);
    if (!had) return -1;
    apic_drop(o, 2, gsi);
    return 0;
}
/* A system call runs with interrupts off; yielding opens a window (so timer ticks and a device's interrupts arrive while a module waits) and restores the state after. */
static void k_yield(void) {
    uint64_t fl; __asm__ volatile("pushfq; popq %0; sti" : "=r"(fl) :: "memory");
    thread_yield();
    __asm__ volatile("pause" ::: "memory");
    if (!(fl & 0x200)) __asm__ volatile("cli" ::: "memory");
}

/* ---- SYSTEM: file system and network ---- */
extern int cos_fs_write_file(const char *path, const void *data, uint64_t size);
__attribute__((noinline)) static int64_t k_fs_read(const char *path, void *buf, size_t cap) {
    int o = CALLER();
    if (o < 0 || !path || !buf || cap == 0) return -1;
    uint64_t size = 0; bool is_dir = false;
    if (!fs_stat_path(path, &size, &is_dir) || is_dir || size > KMOD_MAX_FILE) return -1;
    uint8_t *tmp = (uint8_t *)kmalloc((size_t)(size ? size : 1)); if (!tmp) return -1;
    uint64_t got = 0; int64_t rc = -1;
    if (fs_read_path_into(path, tmp, size, &got)) { size_t n = got < cap ? (size_t)got : cap; memcpy(buf, tmp, n); rc = (int64_t)n; }
    kfree(tmp);
    return rc;
}
__attribute__((noinline)) static int64_t k_fs_write(const char *path, const void *buf, size_t n) {
    int o = CALLER();
    if (o < 0 || !path || (!buf && n) || n > KMOD_MAX_FILE) return -1;
    int w = cos_fs_write_file(path, buf, n);
    return w < 0 ? -1 : (int64_t)w;
}
static int k_fs_stat(const char *path, uint64_t *size, int *is_dir) {
    uint64_t sz = 0; bool d = false;
    if (!path || !fs_stat_path(path, &sz, &d)) return -1;
    if (size) *size = sz;
    if (is_dir) *is_dir = d ? 1 : 0;
    return 0;
}
#define KMOD_MAX_FILTERS 4
static struct { bool used; void *fn; int owner; } fs_filters[KMOD_MAX_FILTERS], net_filters[KMOD_MAX_FILTERS], net_rewriters[KMOD_MAX_FILTERS];
static volatile uint32_t g_filter_flags;                  /* bit 0: some FS filter exists, bit 1: some network filter exists, bit 2: some frame rewriter exists */
static void recompute_filter_flags(void) {                /* tab_lock held */
    uint32_t f = 0;
    for (int i = 0; i < KMOD_MAX_FILTERS; i++) { if (fs_filters[i].used) f |= 1u; if (net_filters[i].used) f |= 2u; if (net_rewriters[i].used) f |= 4u; }
    __atomic_store_n(&g_filter_flags, f, __ATOMIC_RELEASE);
}
static int filter_register(typeof(fs_filters) tab, void *fn, int o) {
    if (o < 0 || !fn || !in_module_code(o, fn)) return -1;
    int rc = -1, freei = -1; bool dup = false;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < KMOD_MAX_FILTERS; i++) { if (tab[i].used && tab[i].fn == fn) dup = true; if (!tab[i].used && freei < 0) freei = i; }
    if (!dup && freei >= 0) { tab[freei].used = true; tab[freei].fn = fn; tab[freei].owner = o; rc = 0; }
    recompute_filter_flags();
    spin_unlock_irqrestore(&tab_lock, fl);
    return rc;
}
static int filter_unregister(typeof(fs_filters) tab, void *fn, int o) {
    int rc = -1;
    if (o < 0) return -1;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < KMOD_MAX_FILTERS; i++) if (tab[i].used && tab[i].fn == fn && tab[i].owner == o) { tab[i].used = false; rc = 0; }
    recompute_filter_flags();
    spin_unlock_irqrestore(&tab_lock, fl);
    return rc;
}
__attribute__((noinline)) static int k_fs_filter_register(cosys_fs_filter_fn fn) { return filter_register(fs_filters, (void *)fn, CALLER()); }
__attribute__((noinline)) static int k_fs_filter_unregister(cosys_fs_filter_fn fn) { return filter_unregister(fs_filters, (void *)fn, CALLER()); }
__attribute__((noinline)) static int k_net_filter_register(cosys_net_filter_fn fn) { return filter_register(net_filters, (void *)fn, CALLER()); }
__attribute__((noinline)) static int k_net_filter_unregister(cosys_net_filter_fn fn) { return filter_unregister(net_filters, (void *)fn, CALLER()); }
__attribute__((noinline)) static int k_net_rewrite_register(cosys_net_rewrite_fn fn) { return filter_register(net_rewriters, (void *)fn, CALLER()); }
__attribute__((noinline)) static int k_net_rewrite_unregister(cosys_net_rewrite_fn fn) { return filter_unregister(net_rewriters, (void *)fn, CALLER()); }

/* Recomputes the IPv4 header checksum and the TCP / UDP checksum of an Ethernet frame after a rewriter changed it. Anything that is not a complete, unfragmented
 * IPv4 TCP or UDP frame is left alone (a UDP checksum of 0 stays 0: the sender did not use one). Returns 0, or -1 for a frame it could not parse. */
static uint32_t csum_add(uint32_t sum, const uint8_t *p, size_t n) {
    for (; n > 1; p += 2, n -= 2) sum += (uint32_t)p[0] << 8 | p[1];
    if (n) sum += (uint32_t)p[0] << 8;
    return sum;
}
static uint16_t csum_fold(uint32_t sum) { while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16); return (uint16_t)~sum; }
static int k_net_fix_checksums(uint8_t *f, size_t len) {
    if (!f || len < 14u + 20u || f[12] != 0x08 || f[13] != 0x00) return -1;
    uint8_t *ip = f + 14; size_t ihl = (size_t)(ip[0] & 0x0Fu) * 4u;
    if ((ip[0] >> 4) != 4 || ihl < 20 || 14 + ihl > len) return -1;
    size_t total = (size_t)ip[2] << 8 | ip[3];
    if (total < ihl || 14 + total > len) return -1;
    ip[10] = ip[11] = 0;
    uint16_t c = csum_fold(csum_add(0, ip, ihl)); ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    if ((((size_t)ip[6] << 8 | ip[7]) & 0x3FFFu) != 0) return 0;                  /* fragment: no L4 header to fix */
    uint8_t proto = ip[9]; size_t l4 = total - ihl; uint8_t *h = ip + ihl;
    size_t off = proto == 6 ? 16 : proto == 17 ? 6 : 0;
    if (!off || l4 < off + 2) return 0;
    if (proto == 17 && h[6] == 0 && h[7] == 0) return 0;
    h[off] = h[off + 1] = 0;
    uint32_t sum = csum_add(0, ip + 12, 8) + proto + (uint32_t)l4;
    c = csum_fold(csum_add(sum, h, l4));
    if (proto == 17 && c == 0) c = 0xFFFFu;
    h[off] = (uint8_t)(c >> 8); h[off + 1] = (uint8_t)c;
    return 0;
}

/* The kernel side: ask every registered filter (outside the table lock, counted in the module's inflight like a system call). */
int cos_kmod_fs_allow(uint32_t op, uint64_t pid, const char *path, const char *path2) {
    if (!(__atomic_load_n(&g_filter_flags, __ATOMIC_ACQUIRE) & 1u)) return 1;
    cosys_fs_filter_fn fns[KMOD_MAX_FILTERS]; int owners[KMOD_MAX_FILTERS]; int n = 0;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < KMOD_MAX_FILTERS; i++) {
        if (!fs_filters[i].used) continue;
        int o = fs_filters[i].owner;
        if (!mods[o].used || mods[o].unloading) continue;
        mods[o].inflight++; fns[n] = (cosys_fs_filter_fn)fs_filters[i].fn; owners[n] = o; n++;
    }
    spin_unlock_irqrestore(&tab_lock, fl);
    int allow = 1;
    for (int i = 0; i < n; i++) { if (allow && fns[i](op, pid, path, path2) != 0) allow = 0; __sync_fetch_and_sub(&mods[owners[i]].inflight, 1); }
    return allow;
}
/* Frame rewriters run first, in registration order, on a buffer of `cap` bytes: each returns the frame's new length, or -1 to drop it. Returns 1 to go on (*len updated), 0 to drop. */
int cos_kmod_net_rewrite_active(void) { return (__atomic_load_n(&g_filter_flags, __ATOMIC_ACQUIRE) & 4u) != 0; }
int cos_kmod_net_rewrite(uint32_t dir, void *frame, size_t *len, size_t cap) {
    if (!cos_kmod_net_rewrite_active()) return 1;
    cosys_net_rewrite_fn fns[KMOD_MAX_FILTERS]; int owners[KMOD_MAX_FILTERS]; int n = 0;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < KMOD_MAX_FILTERS; i++) {
        if (!net_rewriters[i].used) continue;
        int o = net_rewriters[i].owner;
        if (!mods[o].used || mods[o].unloading) continue;
        mods[o].inflight++; fns[n] = (cosys_net_rewrite_fn)net_rewriters[i].fn; owners[n] = o; n++;
    }
    spin_unlock_irqrestore(&tab_lock, fl);
    int pass = 1;
    for (int i = 0; i < n; i++) {
        if (pass) {
            int64_t r = fns[i](dir, (uint8_t *)frame, *len, cap);
            if (r < 14 || (uint64_t)r > cap) pass = 0; else *len = (size_t)r;      /* too short to be a frame, or claims more than the buffer: drop */
        }
        __sync_fetch_and_sub(&mods[owners[i]].inflight, 1);
    }
    return pass;
}
int cos_kmod_net_pass(uint32_t dir, const void *frame, size_t len) {
    if (!(__atomic_load_n(&g_filter_flags, __ATOMIC_ACQUIRE) & 2u)) return 1;
    cosys_net_filter_fn fns[KMOD_MAX_FILTERS]; int owners[KMOD_MAX_FILTERS]; int n = 0;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (int i = 0; i < KMOD_MAX_FILTERS; i++) {
        if (!net_filters[i].used) continue;
        int o = net_filters[i].owner;
        if (!mods[o].used || mods[o].unloading) continue;
        mods[o].inflight++; fns[n] = (cosys_net_filter_fn)net_filters[i].fn; owners[n] = o; n++;
    }
    spin_unlock_irqrestore(&tab_lock, fl);
    int pass = 1;
    for (int i = 0; i < n; i++) { if (pass && fns[i](dir, (const uint8_t *)frame, len) != 0) pass = 0; __sync_fetch_and_sub(&mods[owners[i]].inflight, 1); }
    return pass;
}

static const struct { const char *name; void *addr; uint8_t level; } ksyms[] = {
    { "cosys_log", (void *)k_log, COSYS_LEVEL_BASIC }, { "cosys_log_hex", (void *)k_log_hex, COSYS_LEVEL_BASIC }, { "cosys_log_dec", (void *)k_log_dec, COSYS_LEVEL_BASIC },
    { "cosys_lock", (void *)k_lock, COSYS_LEVEL_BASIC }, { "cosys_unlock", (void *)k_unlock, COSYS_LEVEL_BASIC },
    { "cosys_subscribe", (void *)k_subscribe, COSYS_LEVEL_SERVICE }, { "cosys_unsubscribe", (void *)k_unsubscribe, COSYS_LEVEL_SERVICE },
    { "cosys_time_ms", (void *)k_time_ms, COSYS_LEVEL_BASIC }, { "cosys_alloc", (void *)k_alloc, COSYS_LEVEL_BASIC }, { "cosys_free", (void *)k_free, COSYS_LEVEL_BASIC },
    { "memcpy", (void *)k_memcpy, COSYS_LEVEL_BASIC }, { "memmove", (void *)k_memmove, COSYS_LEVEL_BASIC }, { "memset", (void *)k_memset, COSYS_LEVEL_BASIC },
    { "memcmp", (void *)k_memcmp, COSYS_LEVEL_BASIC }, { "strlen", (void *)k_strlen, COSYS_LEVEL_BASIC },
    { "cosys_register_syscall", (void *)k_register_syscall, COSYS_LEVEL_SERVICE }, { "cosys_unregister_syscall", (void *)k_unregister_syscall, COSYS_LEVEL_SERVICE },
    { "cosys_copy_from_user", (void *)k_copy_from_user, COSYS_LEVEL_SERVICE }, { "cosys_copy_to_user", (void *)k_copy_to_user, COSYS_LEVEL_SERVICE },
    { "cosys_current_pid", (void *)k_current_pid, COSYS_LEVEL_SERVICE }, { "cosys_current_caps", (void *)k_current_caps, COSYS_LEVEL_SERVICE },
    { "cosys_inb", (void *)k_inb, COSYS_LEVEL_DEVICE }, { "cosys_inw", (void *)k_inw, COSYS_LEVEL_DEVICE }, { "cosys_inl", (void *)k_inl, COSYS_LEVEL_DEVICE },
    { "cosys_outb", (void *)k_outb, COSYS_LEVEL_DEVICE }, { "cosys_outw", (void *)k_outw, COSYS_LEVEL_DEVICE }, { "cosys_outl", (void *)k_outl, COSYS_LEVEL_DEVICE },
    { "cosys_pci_read32", (void *)k_pci_read32, COSYS_LEVEL_DEVICE },
    { "cosys_irq_register", (void *)k_irq_register, COSYS_LEVEL_DEVICE }, { "cosys_irq_unregister", (void *)k_irq_unregister, COSYS_LEVEL_DEVICE },
    { "cosys_msi_register", (void *)k_msi_register, COSYS_LEVEL_DEVICE }, { "cosys_msi_unregister", (void *)k_msi_unregister, COSYS_LEVEL_DEVICE },
    { "cosys_ioapic_route", (void *)k_ioapic_route, COSYS_LEVEL_DEVICE }, { "cosys_ioapic_unroute", (void *)k_ioapic_unroute, COSYS_LEVEL_DEVICE },
    { "cosys_mmio_map", (void *)k_mmio_map, COSYS_LEVEL_DEVICE }, { "cosys_mmio_unmap", (void *)k_mmio_unmap, COSYS_LEVEL_DEVICE },
    { "cosys_dma_alloc", (void *)k_dma_alloc, COSYS_LEVEL_DEVICE }, { "cosys_dma_free", (void *)k_dma_free, COSYS_LEVEL_DEVICE },
    { "cosys_fs_read", (void *)k_fs_read, COSYS_LEVEL_SYSTEM }, { "cosys_fs_write", (void *)k_fs_write, COSYS_LEVEL_SYSTEM }, { "cosys_fs_stat", (void *)k_fs_stat, COSYS_LEVEL_SYSTEM },
    { "cosys_fs_filter_register", (void *)k_fs_filter_register, COSYS_LEVEL_SYSTEM }, { "cosys_fs_filter_unregister", (void *)k_fs_filter_unregister, COSYS_LEVEL_SYSTEM },
    { "cosys_net_rewrite_register", (void *)k_net_rewrite_register, COSYS_LEVEL_SYSTEM }, { "cosys_net_rewrite_unregister", (void *)k_net_rewrite_unregister, COSYS_LEVEL_SYSTEM },
    { "cosys_net_fix_checksums", (void *)k_net_fix_checksums, COSYS_LEVEL_SYSTEM },
    { "cosys_net_filter_register", (void *)k_net_filter_register, COSYS_LEVEL_SYSTEM }, { "cosys_net_filter_unregister", (void *)k_net_filter_unregister, COSYS_LEVEL_SYSTEM },
    { "cosys_pci_enable", (void *)k_pci_enable, COSYS_LEVEL_DEVICE }, { "cosys_yield", (void *)k_yield, COSYS_LEVEL_SERVICE },
};

/* ---------------------------------------------------------------- system calls provided by modules */
bool cos_kmod_syscall_known(uint64_t nr) { return nr >= COSYS_SYSCALL_FIRST && nr < COSYS_SYSCALL_FIRST + COSYS_SYSCALL_COUNT && sc_tab[nr - COSYS_SYSCALL_FIRST].used; }
uint32_t cos_kmod_syscall_caps(uint64_t nr) { return cos_kmod_syscall_known(nr) ? sc_tab[nr - COSYS_SYSCALL_FIRST].caps : COS_CAP_UNKNOWN; }
int64_t cos_kmod_syscall(uint64_t nr, uint64_t a, uint64_t b, uint64_t c) {
    if (nr < COSYS_SYSCALL_FIRST || nr >= COSYS_SYSCALL_FIRST + COSYS_SYSCALL_COUNT) return -1;
    uint32_t i = (uint32_t)(nr - COSYS_SYSCALL_FIRST); cosys_syscall_fn fn = NULL; int o = -1;
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    if (sc_tab[i].used && mods[sc_tab[i].owner].used && !mods[sc_tab[i].owner].unloading) { fn = sc_tab[i].fn; o = sc_tab[i].owner; mods[o].inflight++; }
    spin_unlock_irqrestore(&tab_lock, fl);
    if (!fn) return -1;
    int64_t r = fn(a, b, c);
    __sync_fetch_and_sub(&mods[o].inflight, 1);
    return r;
}

/* ---------------------------------------------------------------- signature */
/* The trailer names the key by its level (the highest module level that key may sign); only the keys compiled into this kernel (build/kmod_pubkey.h) are known. On
 * success *key_level is that level: a module may not declare a higher one. */
static bool verify_signature(const uint8_t *file, size_t len, size_t *elf_len, unsigned *key_level) {
    const size_t trailer = 8 + 4 + 1 + COSYS_SIG_LEN;
    if (len < trailer + 64) return false;
    const uint8_t *t = file + len - trailer;
    if (memcmp(t, COSYS_SIG_MAGIC, 8) != 0) return false;
    uint32_t sl = (uint32_t)t[8] | (uint32_t)t[9] << 8 | (uint32_t)t[10] << 16 | (uint32_t)t[11] << 24;
    if (sl != 1 + COSYS_SIG_LEN) return false;
    unsigned keyid = t[12];
    const unsigned char *pub = NULL;
    for (unsigned i = 0; i < sizeof cos_kmod_keys / sizeof cos_kmod_keys[0]; i++) if (cos_kmod_keys[i].level == keyid) pub = cos_kmod_keys[i].pub;
    if (!pub) {
        char w[96]; snprintf(w, sizeof w, "bad or missing signature (the key of level %u is not trusted by this kernel)", keyid); fail(w);
        return false;
    }
    br_sha256_context sc; uint8_t hash[32]; br_sha256_init(&sc); br_sha256_update(&sc, file, len - trailer); br_sha256_out(&sc, hash);
    br_ec_public_key pk = { BR_EC_secp256r1, (void *)pub, 65 };
    if (br_ecdsa_i31_vrfy_raw(&br_ec_prime_i31, hash, sizeof hash, &pk, t + 13, COSYS_SIG_LEN) != 1) return false;
    *elf_len = len - trailer; *key_level = keyid; return true;
}

/* ---------------------------------------------------------------- loading */
typedef struct { int slot; uint32_t level; } resolve_ctx_t;

static bool resolver(void *ctx_v, const cos_elf_symreq_t *req, cos_elf_symval_t *out) {
    resolve_ctx_t *ctx = (resolve_ctx_t *)ctx_v;
    if (!req || !req->name || (req->flags & (COS_ELF_SYM_TLS | COS_ELF_SYM_IFUNC | COS_ELF_SYM_COPY))) return false;
    for (unsigned i = 0; i < sizeof ksyms / sizeof ksyms[0]; i++) if (strcmp(ksyms[i].name, req->name) == 0) {
        if (ctx->level < ksyms[i].level) {
            char w[96]; snprintf(w, sizeof w, "level: %s needs level %u, the module has %u", req->name, (unsigned)ksyms[i].level, ctx->level); fail(w);
            return false;
        }
        memset(out, 0, sizeof *out); out->address = (uint64_t)(uintptr_t)ksyms[i].addr; return true;
    }
    kmod_t *m = &mods[ctx->slot];
    for (int d = 0; d < m->ndeps; d++) {                                                        /* the exports of the modules this one needs */
        if (mods[m->deps[d]].obj.symtab && cos_elf_lookup(paging_get_current_directory(), &mods[m->deps[d]].obj, req->name, NULL, out)) return true;
    }
    if (!(req->flags & COS_ELF_SYM_WEAK)) { char w[96]; snprintf(w, sizeof w, "undefined symbol: %s", req->name); fail(w); }
    return false;
}

static void free_pages(uint64_t lo, uint64_t hi) {
    for (uint64_t va = lo; va < hi; va += 4096) {
        uint64_t pa = paging_virt_to_phys(va);
        if (pa) { paging_unmap_page(va); paging_free_physical(pa); }
    }
}
static void release_slot_memory(int slot) {                 /* tab_lock not held */
    kmod_t *m = &mods[slot];
    uint64_t fl = spin_lock_irqsave(&tab_lock);
    for (uint32_t i = 0; i < COSYS_SYSCALL_COUNT; i++) if (sc_tab[i].used && sc_tab[i].owner == slot) sc_tab[i].used = false;
    for (int i = 0; i < KMOD_MAX_SUBS; i++) if (subs[i].used && subs[i].owner == slot) subs[i].used = false;
    for (int i = 0; i < KMOD_MAX_FILTERS; i++) { if (fs_filters[i].used && fs_filters[i].owner == slot) fs_filters[i].used = false; if (net_filters[i].used && net_filters[i].owner == slot) net_filters[i].used = false; if (net_rewriters[i].used && net_rewriters[i].owner == slot) net_rewriters[i].used = false; }
    recompute_filter_flags();
    recompute_event_mask();
    int dropped_irqs[16], ndi = 0;
    for (int i = 0; i < 16; i++) if (irq_tab[i].used && irq_tab[i].owner == slot) { irq_tab[i].used = false; dropped_irqs[ndi++] = irq_tab[i].irq; }
    struct { uint8_t kind; uint32_t where; } apic_off[APIC_VEC_COUNT]; int nao = 0;
    for (int i = 0; i < APIC_VEC_COUNT; i++) if (apic_tab[i].used && apic_tab[i].owner == slot) { apic_off[nao].kind = apic_tab[i].kind; apic_off[nao].where = apic_tab[i].where; nao++; }
    void *dma_free_list[8]; int ndf = 0;
    for (int i = 0; i < 8; i++) if (m->dma[i].p) { dma_free_list[ndf++] = m->dma[i].p; m->dma[i].p = NULL; m->dma[i].size = 0; }
    m->dma_total = 0;
    alloc_hdr_t *a = m->allocs; m->allocs = NULL; m->mem_used = 0;
    spin_unlock_irqrestore(&tab_lock, fl);
    while (a) { alloc_hdr_t *n = a->next; a->magic = 0; kfree(a); a = n; }
    for (int i = 0; i < ndi; i++) irq_drop(dropped_irqs[i]);
    for (int i = 0; i < nao; i++) apic_drop(slot, apic_off[i].kind, apic_off[i].where);
    for (int i = 0; i < ndf; i++) kfree(dma_free_list[i]);
    for (int i = 0; i < 4; i++) if (m->mmio[i].used) mmio_unmap_entry(slot, i);
    free_pages(m->base, m->base + KMOD_SLOT);
}

/* returns 0 loaded, 1 rejected, 2 a dependency is not loaded yet */
static int load_buffer(const char *label, const uint8_t *file, size_t len, bool quiet_missing_dep) {
    if (!kmod_ready) return 1;
    if (__sync_lock_test_and_set(&busy, 1)) { serial_puts("[KMOD] busy, try again\n"); return 1; }
    int rc = 1, slot = -1; size_t elf_len = 0; fail_why[0] = 0;
    cos_elf_info_t *info = NULL;
    #define REJECT(why) do { fail(why); goto reject; } while (0)

    unsigned key_level = 0;
    if (!verify_signature(file, len, &elf_len, &key_level)) { if (!fail_why[0]) fail("bad or missing signature"); goto reject; }
    if (!cos_elf_nx_available()) REJECT("no NX: W^X cannot be enforced");
    info = (cos_elf_info_t *)kmalloc(sizeof *info); if (!info) REJECT("out of memory");
    if (!cos_elf_inspect(file, elf_len, info)) REJECT("not a loadable ELF object");
    if (info->e_type != ET_DYN_) REJECT("not a shared object (ET_DYN)");
    if (info->flags & (COS_ELF_F_INTERP | COS_ELF_F_HAS_TLS | COS_ELF_F_EXEC_STACK | COS_ELF_F_TEXTREL)) REJECT("interpreter, TLS, executable stack or text relocations");
    if (info->span == 0 || info->span > KMOD_MAX_SPAN) REJECT("empty or too large");
    for (int i = 0; i < KMOD_MAX_MODS; i++) if (!mods[i].used) { slot = i; break; }
    if (slot < 0) REJECT("too many modules");
    kmod_t *m = &mods[slot];
    {   /* dependencies, before anything is mapped */
        int deps[KMOD_MAX_DEPS], nd = 0;
        if (info->needed_count > KMOD_MAX_DEPS) REJECT("too many dependencies");
        for (uint32_t d = 0; d < info->needed_count; d++) {
            int f = -1; for (int i = 0; i < KMOD_MAX_MODS; i++) if (mods[i].used && !mods[i].unloading && strcmp(mods[i].name, info->needed[d]) == 0) f = i;
            if (f < 0) { char w[96]; snprintf(w, sizeof w, "missing dependency: %s", info->needed[d]); fail(w); rc = 2; goto reject; }
            deps[nd++] = f;
        }
        memset(m, 0, sizeof *m); m->used = true;                                                // reserve the slot (names/levels are filled in below)
        for (int i = 0; i < nd; i++) m->deps[i] = deps[i];
        m->ndeps = nd;
    }
    m->base = KMOD_BASE + (uint64_t)slot * KMOD_SLOT;
    m->lo = m->base; m->hi = m->base + KMOD_MAX_SPAN;                                           // calls made while loading are attributed to this slot

    cos_elf_set_kernel_mode(true, m->base, m->base + KMOD_MAX_SPAN);
    page_directory_t *dir = paging_get_current_directory();
    bool ok = cos_elf_map(dir, file, elf_len, m->base, &m->obj);
    if (!ok) { cos_elf_set_kernel_mode(false, 0, 0); REJECT("cannot map the object"); }
    m->lo = m->obj.map_start; m->hi = m->obj.map_end;
    for (uint32_t i = 0; i < m->obj.seg_count; i++) {
        uint32_t pf = m->obj.segs[i].pflags;
        if ((pf & PF_X_) && (pf & PF_W_)) { cos_elf_set_kernel_mode(false, 0, 0); REJECT("a segment that is writable and executable"); }
    }
    cos_elf_symval_t sv;
    if (!cos_elf_lookup(dir, &m->obj, "cosys_module_info", NULL, &sv) || sv.address < m->lo || sv.address + sizeof(struct cosys_module) > m->hi) {
        cos_elf_set_kernel_mode(false, 0, 0); REJECT("no cosys_module_info");
    }
    const struct cosys_module *mi = (const struct cosys_module *)(uintptr_t)sv.address;
    if (mi->magic != COSYS_MAGIC || mi->abi != COSYS_ABI) { cos_elf_set_kernel_mode(false, 0, 0); REJECT("wrong module ABI"); }
    if (mi->level > COSYS_LEVEL_MAX) { cos_elf_set_kernel_mode(false, 0, 0); REJECT("level: unknown level"); }
    if (mi->level > key_level) {                                   /* the signing key may not vouch for a module this powerful */
        char w[96]; snprintf(w, sizeof w, "level: signed by a key limited to level %u, the module declares level %u", key_level, (unsigned)mi->level);
        cos_elf_set_kernel_mode(false, 0, 0); REJECT(w);
    }
    m->level = mi->level;
    for (int d = 0; d < m->ndeps; d++) if (mods[m->deps[d]].level > m->level) { cos_elf_set_kernel_mode(false, 0, 0); REJECT("level: a dependency has a higher level than the module"); }
    resolve_ctx_t rctx = { slot, m->level };
    ok = cos_elf_relocate(dir, &m->obj, resolver, &rctx);
    if (!ok) { cos_elf_set_kernel_mode(false, 0, 0); if (!fail_why[0]) fail("relocation failed"); goto reject; }
    if (m->obj.ifunc_count) { cos_elf_set_kernel_mode(false, 0, 0); REJECT("indirect functions (IFUNC)"); }
    ok = cos_elf_protect(dir, &m->obj);
    cos_elf_set_kernel_mode(false, 0, 0);
    if (!ok) REJECT("cannot set page permissions");
    for (uint32_t i = 0; i < m->obj.seg_count && m->nx < KMOD_MAX_XSEG; i++) if (m->obj.segs[i].pflags & PF_X_) { m->xlo[m->nx] = m->obj.segs[i].start; m->xhi[m->nx] = m->obj.segs[i].end; m->nx++; }

    #define IN_MOD(p) ((uint64_t)(uintptr_t)(p) >= m->lo && (uint64_t)(uintptr_t)(p) < m->hi)
    #define IN_CODE(p) ({ bool r_ = false; for (int k_ = 0; k_ < m->nx; k_++) if ((uint64_t)(uintptr_t)(p) >= m->xlo[k_] && (uint64_t)(uintptr_t)(p) < m->xhi[k_]) r_ = true; r_; })
    if (!IN_MOD(mi->name) || !IN_MOD(mi->version) || !IN_CODE(mi->init) || !IN_CODE(mi->exit)) REJECT("module metadata points outside the module");
    {   size_t nl = 0; while (nl < sizeof m->name - 1 && mi->name[nl]) { m->name[nl] = mi->name[nl]; nl++; } m->name[nl] = 0;
        nl = 0; while (nl < sizeof m->version - 1 && mi->version[nl]) { m->version[nl] = mi->version[nl]; nl++; }
        m->version[nl] = 0; }
    if (!m->name[0]) REJECT("the module has no name");
    for (int i = 0; i < KMOD_MAX_MODS; i++) if (i != slot && mods[i].used && strcmp(mods[i].name, m->name) == 0) REJECT("a module with this name is already loaded");
    m->info = mi;
    for (int d = 0; d < m->ndeps; d++) mods[m->deps[d]].refs++;
    if (mi->init() != 0) {
        serial_puts("[KMOD] "); serial_puts(label); serial_puts(": init failed, unloaded\n");
        for (int d = 0; d < m->ndeps; d++) mods[m->deps[d]].refs--;
        release_slot_memory(slot); memset(m, 0, sizeof *m); rc = 1; goto done;
    }
    serial_puts("[KMOD] loaded "); serial_puts(m->name); serial_puts(" "); serial_puts(m->version); serial_puts(" level "); serial_putdec(m->level); serial_puts("\n");
    rc = 0; goto done;

reject:
    if (slot >= 0 && mods[slot].used) { release_slot_memory(slot); memset(&mods[slot], 0, sizeof mods[slot]); }
    if (!(rc == 2 && quiet_missing_dep)) { serial_puts("[KMOD] rejected "); serial_puts(label); serial_puts(": "); serial_puts(fail_why); serial_puts("\n"); }
done:
    if (info) kfree(info);
    __sync_lock_release(&busy);
    return rc;
}

/* run time: path -> 0 loaded, -1 refused (the reason is in the kernel log) */
int cos_kmod_load_path(const char *path) {
    uint64_t size = 0; bool is_dir = false;
    if (!fs_stat_path(path, &size, &is_dir) || is_dir || size == 0 || size > KMOD_MAX_FILE) { serial_puts("[KMOD] cannot read "); serial_puts(path); serial_puts("\n"); return -1; }
    uint8_t *buf = (uint8_t *)kmalloc((size_t)size); if (!buf) return -1;
    uint64_t got = 0; int rc = -1;
    if (fs_read_path_into(path, buf, size, &got) && got == size) rc = load_buffer(path, buf, (size_t)got, false) == 0 ? 0 : -1;
    else { serial_puts("[KMOD] cannot read "); serial_puts(path); serial_puts("\n"); }
    kfree(buf); return rc;
}

/* 0 unloaded, -1 no such module, -2 in use (another module needs it, or one of its system calls is running) */
int cos_kmod_unload(const char *name) {
    if (!kmod_ready || __sync_lock_test_and_set(&busy, 1)) return -2;
    int rc = -1;
    for (int i = 0; i < KMOD_MAX_MODS; i++) if (mods[i].used && strcmp(mods[i].name, name) == 0) {
        uint64_t fl = spin_lock_irqsave(&tab_lock);
        bool can = mods[i].refs == 0 && !mods[i].unloading;
        if (can) mods[i].unloading = true;
        if (can && mods[i].inflight > 0) { mods[i].unloading = false; can = false; }
        spin_unlock_irqrestore(&tab_lock, fl);
        if (!can) { serial_puts("[KMOD] unload refused (in use): "); serial_puts(name); serial_puts("\n"); rc = -2; break; }
        mods[i].info->exit();
        for (int d = 0; d < mods[i].ndeps; d++) mods[mods[i].deps[d]].refs--;
        serial_puts("[KMOD] unloaded "); serial_puts(mods[i].name); serial_puts("\n");
        release_slot_memory(i); memset(&mods[i], 0, sizeof mods[i]); rc = 0; break;
    }
    __sync_lock_release(&busy);
    return rc;
}

int cos_kmod_list(char *out, size_t cap) {                    /* "name version level\n" per module; returns the count */
    size_t o = 0; int n = 0;
    for (int i = 0; i < KMOD_MAX_MODS; i++) if (mods[i].used && !mods[i].unloading) {
        char line[80]; int l = snprintf(line, sizeof line, "%s %s %u\n", mods[i].name, mods[i].version, mods[i].level); n++;
        for (int k = 0; k < l && o + 1 < cap; k++) out[o++] = line[k];
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return n;
}

/* boot: every *.c-osys in /system/drivers. Called once the file system is up and before any program runs, so that the page-table slot of the module
 * area exists in the kernel page tables before any process copies them. Modules may need each other, so files that wait for a dependency are retried. */
void cos_kmod_init(void) {
    uint64_t pa = phys_alloc_page();
    if (!pa || !paging_map_page(KMOD_END_PAGE, pa, PAGE_PRESENT | PAGE_RW | PAGE_NX)) { serial_puts("[KMOD] cannot prepare the module area\n"); if (pa) phys_free_page(pa); return; }
    /* (the page stays mapped on purpose: it keeps the page-table levels of the module area alive) */
    kmod_ready = true;
    static cos_dirent_ex_t ent[32];
    int n = cos_fm_listdir_ex("/system/drivers", ent, 32);
    bool done[32] = { false };
    for (int pass = 0; pass < 4; pass++) {
        bool last = pass == 3;
        for (int i = 0; i < n; i++) {
            size_t l = strlen(ent[i].name);
            if (done[i] || ent[i].is_dir || l < 8 || strcmp(ent[i].name + l - 7, ".c-osys") != 0 || ent[i].size == 0 || ent[i].size > KMOD_MAX_FILE) { done[i] = true; continue; }
            char path[160]; snprintf(path, sizeof path, "/system/drivers/%s", ent[i].name);
            uint8_t *buf = (uint8_t *)kmalloc((size_t)ent[i].size); if (!buf) continue;
            uint64_t got = 0;
            if (fs_read_path_into(path, buf, ent[i].size, &got) && got == ent[i].size) { if (load_buffer(ent[i].name, buf, (size_t)got, !last) != 2) done[i] = true; }
            else { serial_puts("[KMOD] cannot read "); serial_puts(path); serial_puts("\n"); done[i] = true; }
            kfree(buf);
        }
    }
}
