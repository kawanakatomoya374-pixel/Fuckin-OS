/**
 * cos_rtld.c - the ring3 half of the C-OS dynamic loader.
 *
 * WHY ANY OF THIS RUNS IN USERLAND
 * --------------------------------
 * The kernel maps and relocates every object, but two jobs are left
 * deliberately undone, because doing them in the kernel would mean
 * executing code chosen by the program's own file at ring0:
 *
 *   - R_X86_64_IRELATIVE, and pointer relocations against an
 *     STT_GNU_IFUNC symbol. The "address" in those is a RESOLVER
 *     FUNCTION; the real address is whatever calling it returns.
 *   - DT_PREINIT_ARRAY / DT_INIT / DT_INIT_ARRAY constructors, and the
 *     matching destructors.
 *
 * Running any of them from ring0 is exactly the privilege-escalation
 * shape this tree already found and fixed once in its signal delivery
 * path, so the kernel publishes them instead - a read-only link map in
 * the process's own address space, advertised through a private
 * auxiliary-vector tag - and this file applies them at ring3, where they
 * belong.
 *
 * A program that does not link this file still runs. It just gets no
 * constructors and no ifuncs, which is precisely the behaviour of the
 * loader before any of this existed.
 */
#include "cos.h"

/* ---- the contract with the kernel --------------------------------------
 * These must match src/kernel/cos_elf.h exactly. Duplicated rather than
 * shared because userland is freestanding and does not see kernel
 * headers; the magic number is what catches a mismatch at runtime
 * instead of letting a stale layout be read as if it were current. */

#define COS_AT_NULL          0
#define COS_AT_PHDR          3
#define COS_AT_PHENT         4
#define COS_AT_PHNUM         5
#define COS_AT_PAGESZ        6
#define COS_AT_BASE          7
#define COS_AT_ENTRY         9
#define COS_AT_PLATFORM     15
#define COS_AT_RANDOM       25
#define COS_AT_EXECFN       31
#define COS_AT_COS_LINKMAP  0x434F5300u
#define COS_AT_COS_TP       0x434F5301u

#define COS_ELF_LINKMAP_MAGIC   0x434F534C4D415031ULL   /* "COSLMAP1" */
#define COS_ELF_LINKMAP_VERSION 1u
#define COS_ELF_MAX_IFUNC       192u
#define COS_ELF_LINKMAP_MAX_OBJ 32u

typedef struct {
    uint64_t base;
    uint64_t map_start, map_end;
    uint64_t dynamic;
    uint64_t init, init_array, init_array_sz;
    uint64_t fini, fini_array, fini_array_sz;
    uint64_t preinit_array, preinit_array_sz;
    int32_t  tls_modid;
    int64_t  tls_offset;
    uint64_t tls_image;
    uint64_t tls_filesz;
    uint64_t tls_memsz;
    uint64_t tls_align;
    uint32_t flags;
    uint32_t serial;
    char     name[64];
} cos_linkmap_obj_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t obj_count;
    uint64_t tls_tp;
    uint64_t tls_block_size;
    uint32_t ifunc_count;
    uint32_t _pad;
    uint64_t ifunc_target[COS_ELF_MAX_IFUNC];
    uint64_t ifunc_resolver[COS_ELF_MAX_IFUNC];
    cos_linkmap_obj_t obj[COS_ELF_LINKMAP_MAX_OBJ];
} cos_linkmap_t;

/* ---- process-wide state the runtime exposes ---------------------------- */
int          cos_argc;
char       **cos_argv;
char       **cos_environ;
uint64_t    *cos_auxv;                  /* tag/value pairs, AT_NULL terminated */
const cos_linkmap_t *cos_linkmap;

static uint64_t auxv_get(uint64_t tag)
{
    if (!cos_auxv) return 0;
    for (uint64_t *p = cos_auxv; p[0] != COS_AT_NULL; p += 2) {
        if (p[0] == tag) return p[1];
    }
    return 0;
}

uint64_t cos_getauxval(uint64_t tag) { return auxv_get(tag); }

char *cos_getenv(const char *name)
{
    if (!cos_environ || !name) return NULL;
    size_t n = 0;
    while (name[n]) n++;
    for (char **e = cos_environ; *e; ++e) {
        const char *s = *e;
        size_t i = 0;
        while (i < n && s[i] && s[i] == name[i]) i++;
        if (i == n && s[i] == '=') return (char *)(s + n + 1);
    }
    return NULL;
}

/* ---- destructors ------------------------------------------------------- */
/* Registered as the link map is walked and run in reverse at exit, so an
 * object is torn down before anything it depends on. */
#define COS_MAX_ATEXIT 64
typedef void (*cos_void_fn)(void);
static cos_void_fn g_atexit[COS_MAX_ATEXIT];
static int         g_atexit_n;

int cos_atexit(cos_void_fn fn)
{
    if (!fn || g_atexit_n >= COS_MAX_ATEXIT) return -1;
    g_atexit[g_atexit_n++] = fn;
    return 0;
}

void cos_run_atexit(void)
{
    /* Reverse order, and the counter is decremented BEFORE the call so a
     * destructor that itself exits cannot re-enter the same entry and
     * loop forever. */
    while (g_atexit_n > 0) {
        cos_void_fn fn = g_atexit[--g_atexit_n];
        if (fn) fn();
    }
}

/* ---- the startup sequence ---------------------------------------------- */

static void apply_ifuncs(const cos_linkmap_t *lm)
{
    /* Every ifunc in the whole process, before ANY constructor runs: a
     * constructor may legitimately call through an ifunc slot, and a
     * slot that has not been resolved yet holds the trap value the
     * kernel left there. */
    for (uint32_t i = 0; i < lm->ifunc_count && i < COS_ELF_MAX_IFUNC; ++i) {
        uint64_t target = lm->ifunc_target[i];
        uint64_t resolver = lm->ifunc_resolver[i];
        if (!target || !resolver) continue;
        uint64_t (*fn)(void) = (uint64_t (*)(void))(uintptr_t)resolver;
        *(uint64_t *)(uintptr_t)target = fn();
    }
}

static void run_array(uint64_t array, uint64_t size_bytes, int argc,
                      char **argv, char **envp)
{
    if (!array || size_bytes < sizeof(void *)) return;
    uint64_t n = size_bytes / sizeof(void *);
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t slot = ((uint64_t *)(uintptr_t)array)[i];
        /* A linker may leave 0 or -1 in an array slot; calling either is
         * an immediate fault for no reason. */
        if (slot == 0 || slot == (uint64_t)-1) continue;
        /* Init functions are called with (argc, argv, envp) - the GNU
         * convention every __attribute__((constructor)) is compiled
         * against. Passing nothing works for most, and silently breaks
         * any that read their arguments. */
        void (*fn)(int, char **, char **) =
            (void (*)(int, char **, char **))(uintptr_t)slot;
        fn(argc, argv, envp);
    }
}

static void register_destructors(const cos_linkmap_t *lm)
{
    /* Registered in link-map order so cos_run_atexit()'s reverse walk
     * tears objects down in the opposite order to construction. */
    for (uint32_t i = 0; i < lm->obj_count && i < COS_ELF_LINKMAP_MAX_OBJ; ++i) {
        const cos_linkmap_obj_t *o = &lm->obj[i];
        uint64_t n = o->fini_array_sz / sizeof(void *);
        for (uint64_t k = 0; k < n; ++k) {
            uint64_t slot = ((uint64_t *)(uintptr_t)o->fini_array)[k];
            if (slot && slot != (uint64_t)-1) cos_atexit((cos_void_fn)(uintptr_t)slot);
        }
        if (o->fini) cos_atexit((cos_void_fn)(uintptr_t)o->fini);
    }
}

static void run_constructors(const cos_linkmap_t *lm, int argc,
                             char **argv, char **envp)
{
    /* Objects are listed executable-first, dependencies after. A
     * dependency's constructors must run BEFORE those of whatever
     * depends on it, so the array is walked backwards. Getting this
     * order wrong is the classic static-initialisation-order bug: the
     * executable's constructor runs against a library that has not
     * initialised itself yet. */
    for (int i = (int)lm->obj_count - 1; i >= 0; --i) {
        const cos_linkmap_obj_t *o = &lm->obj[i];
        /* DT_PREINIT_ARRAY is defined to run before every other
         * constructor, and only for the executable. */
        if (i == 0) run_array(o->preinit_array, o->preinit_array_sz, argc, argv, envp);
    }
    for (int i = (int)lm->obj_count - 1; i >= 0; --i) {
        const cos_linkmap_obj_t *o = &lm->obj[i];
        if (o->init) {
            void (*fn)(int, char **, char **) =
                (void (*)(int, char **, char **))(uintptr_t)o->init;
            fn(argc, argv, envp);
        }
        run_array(o->init_array, o->init_array_sz, argc, argv, envp);
    }
}

/* ---- dlopen / dlclose after startup ---------------------------------------------
 * The kernel maps, relocates and protects a library; the code that has to run in it - constructors and destructors - is run here, at ring3, from the same link map
 * the startup sequence uses. Objects are told apart by their serial number, which the kernel never reuses: a library that was closed and opened again is a new
 * object with new constructors, never mistaken for the old one. */
#define COS_DL_MAX 24
static struct { uint32_t serial, seq; } g_dl[COS_DL_MAX];    /* objects loaded after startup whose constructors have run (serial 0 = free) */
static uint32_t g_dl_startup_serial, g_dl_seq;
static volatile int g_dl_lock;
static void dl_lock(void) { while (__sync_lock_test_and_set(&g_dl_lock, 1)) cos_yield(); }
static void dl_unlock(void) { __sync_lock_release(&g_dl_lock); }

static const cos_linkmap_t *dl_map(void)
{
    uint64_t a = cos_dl_info();                          /* -1 when the system call is refused (a sandboxed process may not use the dl interface) */
    if (a == 0 || a >= 0x0000800000000000ULL) return 0;
    const cos_linkmap_t *lm = (const cos_linkmap_t *)(uintptr_t)a;
    return (lm && lm->magic == COS_ELF_LINKMAP_MAGIC && lm->version == COS_ELF_LINKMAP_VERSION) ? lm : 0;
}
static const cos_linkmap_obj_t *dl_find(const cos_linkmap_t *lm, uint32_t serial)
{
    for (uint32_t i = 0; i < lm->obj_count && i < COS_ELF_LINKMAP_MAX_OBJ; ++i) if (lm->obj[i].serial == serial) return &lm->obj[i];
    return 0;
}
static int dl_track(uint32_t serial, bool add)         /* is it tracked? with add: track it now (returns 0 when it was new) */
{
    int hit = 0;
    dl_lock();
    for (int i = 0; i < COS_DL_MAX; ++i) if (g_dl[i].serial == serial) { hit = 1; break; }
    if (!hit && add) for (int i = 0; i < COS_DL_MAX; ++i) if (!g_dl[i].serial) { g_dl[i].serial = serial; g_dl[i].seq = ++g_dl_seq; break; }
    dl_unlock();
    return hit;
}
static void dl_untrack(uint32_t serial)
{
    dl_lock();
    for (int i = 0; i < COS_DL_MAX; ++i) if (g_dl[i].serial == serial) g_dl[i].serial = 0;
    dl_unlock();
}
static void dl_run_fini(const cos_linkmap_obj_t *o)
{
    uint64_t n = o->fini_array_sz / sizeof(void *);
    for (uint64_t k = n; k > 0; --k) {
        uint64_t slot = ((uint64_t *)(uintptr_t)o->fini_array)[k - 1];
        if (slot && slot != (uint64_t)-1) ((cos_void_fn)(uintptr_t)slot)();
    }
    if (o->fini) ((cos_void_fn)(uintptr_t)o->fini)();
}

int64_t cos_dlopen_flags(const char *path, uint32_t flags)
{
    int64_t h = cos_dl_open_raw(path, flags);
    if (h < 0) return h;
    const cos_linkmap_t *lm = dl_map();
    if (!lm) return h;
    apply_ifuncs(lm);                                    /* idempotent: a resolver returns the same answer for an object that was already resolved */
    uint32_t todo[COS_DL_MAX]; int n = 0;
    for (uint32_t i = 0; i < lm->obj_count && i < COS_ELF_LINKMAP_MAX_OBJ && n < COS_DL_MAX; ++i) {
        uint32_t s = lm->obj[i].serial;
        if (s > g_dl_startup_serial && !dl_track(s, false)) todo[n++] = s;
    }
    for (int a = 1; a < n; ++a) { uint32_t v = todo[a]; int b = a - 1; while (b >= 0 && todo[b] < v) { todo[b + 1] = todo[b]; b--; } todo[b + 1] = v; }   /* highest serial first: dependencies before dependents */
    for (int k = 0; k < n; ++k) {
        const cos_linkmap_obj_t *o = dl_find(lm, todo[k]);
        if (!o || dl_track(todo[k], true)) continue;     /* gone, or another thread got there first */
        if (o->init) ((void (*)(int, char **, char **))(uintptr_t)o->init)(cos_argc, cos_argv, cos_environ);
        run_array(o->init_array, o->init_array_sz, cos_argc, cos_argv, cos_environ);
    }
    return h;
}

int cos_dlclose(int64_t handle)
{
    uint32_t plan[COS_DL_MAX];
    int64_t n = cos_dl_ctl(1 /* COS_DLCTL_PLAN_CLOSE */, (uint64_t)handle, (uint64_t)(uintptr_t)plan, 0);
    const cos_linkmap_t *lm = dl_map();
    if (n > 0 && lm) {
        for (int64_t k = 0; k < n && k < COS_DL_MAX; ++k) {          /* dependents first, while everything is still mapped */
            const cos_linkmap_obj_t *o = dl_find(lm, plan[k]);
            if (o && dl_track(plan[k], false)) dl_run_fini(o);
            dl_untrack(plan[k]);
        }
    }
    return cos_dl_close_raw(handle);
}

/* At exit: the destructors of the libraries opened after startup that are still loaded, last constructed first (the startup objects' were registered with atexit). */
static void dl_fini_all(void)
{
    if (!g_dl_seq) return;                               /* nothing was opened after startup: no system call at exit */
    const cos_linkmap_t *lm = dl_map();
    if (!lm) return;
    for (;;) {
        uint32_t best = 0, best_seq = 0;
        dl_lock();
        for (int i = 0; i < COS_DL_MAX; ++i) if (g_dl[i].serial && g_dl[i].seq > best_seq) { best = g_dl[i].serial; best_seq = g_dl[i].seq; }
        for (int i = 0; i < COS_DL_MAX; ++i) if (g_dl[i].serial == best) g_dl[i].serial = 0;
        dl_unlock();
        if (!best) break;
        const cos_linkmap_obj_t *o = dl_find(lm, best);
        if (o) dl_run_fini(o);
    }
}

const char *cos_dlerror(void)
{
    static char msg[160];
    int64_t n = cos_dl_ctl(4 /* COS_DLCTL_ERROR */, (uint64_t)(uintptr_t)msg, sizeof msg, 0);
    return n > 0 ? msg : 0;
}
int cos_dladdr(const void *addr, cos_dlinfo_t *out) { return out ? (int)cos_dl_ctl(2 /* COS_DLCTL_ADDR */, (uint64_t)(uintptr_t)addr, (uint64_t)(uintptr_t)out, 0) : -1; }
void *cos_dlsym_next(const void *caller, const char *name) { return (void *)(uintptr_t)cos_dl_ctl(3 /* COS_DLCTL_NEXT */, (uint64_t)(uintptr_t)caller, (uint64_t)(uintptr_t)name, 0); }
int cos_dl_set_path(const char *dirs) { return (int)cos_dl_ctl(5 /* COS_DLCTL_SET_PATH */, (uint64_t)(uintptr_t)dirs, 0, 0); }
int cos_dl_iterate(int (*cb)(const cos_dlobj_t *obj, void *ctx), void *ctx)
{
    const cos_linkmap_t *lm = dl_map();
    if (!lm || !cb) return 0;
    for (uint32_t i = 0; i < lm->obj_count && i < COS_ELF_LINKMAP_MAX_OBJ; ++i) {
        const cos_linkmap_obj_t *o = &lm->obj[i];
        cos_dlobj_t d = { o->name, o->base, o->map_start, o->map_end, o->serial, o->flags };
        int r = cb(&d, ctx);
        if (r) return r;
    }
    return 0;
}

extern int main(int argc, char **argv, char **envp);

/* Stack protector. A function with a local buffer keeps a copy of this value below its return address and checks it before returning: a stack
 * overflow that rewrites the return address has to overwrite the copy as well, and does not know the value. It is chosen from the kernel's
 * random bytes (AT_RANDOM) when the program starts; its low byte is zero, so a string overflow cannot copy it (the string ends there). The
 * initial value below only matters for code that runs before __cos_rt_start sets the real one, and for a JIT image (tcc -run) that never runs it. */
uintptr_t __stack_chk_guard = (uintptr_t)0x5CA1AB1E0BADF000ULL;
__attribute__((noreturn)) void __stack_chk_fail(void)
{
    cos_puts("[STACK] stack smashing detected - the program is stopped\n");
    cos_exit(134);
}

/**
 * __cos_rt_start - the C half of program startup.
 *
 * `sp` points at the initial stack the kernel built:
 *     [ argc ][ argv... ][ NULL ][ envp... ][ NULL ][ auxv... ][ AT_NULL ]
 *
 * Done in C rather than in the assembly stub because walking the
 * auxiliary vector, applying ifuncs and running constructor arrays in
 * hand-written assembly would be several hundred lines of exactly the
 * kind of code that is hard to get right and harder to review.
 */
__attribute__((no_stack_protector))                /* it changes the guard: its own check would compare the old copy with the new value */
void __cos_rt_start(uint64_t *sp)
{
    cos_argc = (int)sp[0];
    cos_argv = (char **)&sp[1];
    cos_environ = (char **)&sp[1 + cos_argc + 1];

    char **e = cos_environ;
    while (*e) ++e;
    cos_auxv = (uint64_t *)(e + 1);

    {   const uint8_t *rnd = (const uint8_t *)(uintptr_t)auxv_get(25 /* AT_RANDOM */);
        if (rnd) { uintptr_t g = 0; for (int i = 0; i < 8; i++) g = (g << 8) | rnd[i]; __stack_chk_guard = g & ~(uintptr_t)0xFF; }
    }

    uint64_t lm_addr = auxv_get(COS_AT_COS_LINKMAP);
    const cos_linkmap_t *lm = (const cos_linkmap_t *)(uintptr_t)lm_addr;

    /* The magic check is what makes a version skew survivable: a kernel
     * and a program built at different times will disagree about the
     * layout, and reading a stale one as if it were current would call
     * whatever happens to sit where `init_array` used to be. Ignoring an
     * unrecognised link map costs constructors; trusting one costs
     * control of the program. */
    if (lm && lm->magic == COS_ELF_LINKMAP_MAGIC &&
        lm->version == COS_ELF_LINKMAP_VERSION) {
        cos_linkmap = lm;
        for (uint32_t i = 0; i < lm->obj_count && i < COS_ELF_LINKMAP_MAX_OBJ; ++i) if (lm->obj[i].serial > g_dl_startup_serial) g_dl_startup_serial = lm->obj[i].serial;
        apply_ifuncs(lm);
        register_destructors(lm);
        run_constructors(lm, cos_argc, cos_argv, cos_environ);
    }

    int status = main(cos_argc, cos_argv, cos_environ);

    dl_fini_all();
    cos_run_atexit();
    cos_exit(status);
}

/* ---- per-thread TLS ------------------------------------------------------
 *
 * The kernel builds the MAIN thread's TLS block, because it has to write
 * the %fs base MSR anyway. A thread the program creates itself needs its
 * own block - that is what thread-LOCAL means - and only ring3 knows
 * where to get the memory from.
 *
 * Variant II layout, matching what the kernel builds:
 *
 *      [ module data ... ][ TCB ][ DTV ]
 *                         ^ tp     *(void**)tp == tp
 *
 * Returns the thread pointer to hand to cos_set_tls(), or 0 if the
 * process has no TLS at all (in which case the thread simply runs
 * without one, which is correct rather than an error).
 */
#define COS_TLS_TCB_SIZE 64u

static uint64_t align_up_u(uint64_t v, uint64_t a)
{
    if (a <= 1 || (a & (a - 1))) return v;
    return (v + a - 1) & ~(a - 1);
}

uint64_t cos_tls_create_block(void)
{
    const cos_linkmap_t *lm = cos_linkmap;
    if (!lm || lm->tls_block_size == 0) return 0;

    uint32_t modules = 0;
    uint64_t align = 8;
    for (uint32_t i = 0; i < lm->obj_count && i < COS_ELF_LINKMAP_MAX_OBJ; ++i) {
        if (lm->obj[i].tls_modid > 0) {
            modules++;
            if (lm->obj[i].tls_align > align) align = lm->obj[i].tls_align;
        }
    }

    uint64_t need = lm->tls_block_size + COS_TLS_TCB_SIZE
                  + ((uint64_t)modules + 1) * 8 + align;
    /* mmap rather than malloc: this block lives for the thread's whole
     * life and must not move, and the allocator is free to relocate on
     * realloc. */
    void *region = cos_mmap((size_t)need, COS_PROT_READ | COS_PROT_WRITE);
    if (!region) return 0;

    uint8_t *base = (uint8_t *)region;
    for (uint64_t i = 0; i < need; ++i) base[i] = 0;   /* .tbss must read as 0 */

    uint64_t tp = align_up_u((uint64_t)(uintptr_t)base + lm->tls_block_size, align);
    uint64_t dtv = tp + COS_TLS_TCB_SIZE;

    for (uint32_t i = 0; i < lm->obj_count && i < COS_ELF_LINKMAP_MAX_OBJ; ++i) {
        const cos_linkmap_obj_t *o = &lm->obj[i];
        if (o->tls_modid <= 0) continue;
        uint8_t *dest = (uint8_t *)(uintptr_t)(tp - (uint64_t)o->tls_offset);
        const uint8_t *src = (const uint8_t *)(uintptr_t)o->tls_image;
        for (uint64_t k = 0; k < o->tls_filesz; ++k) dest[k] = src[k];
        ((uint64_t *)(uintptr_t)dtv)[o->tls_modid] = (uint64_t)(uintptr_t)dest;
    }

    *(uint64_t *)(uintptr_t)tp = tp;          /* self pointer: `mov %fs:0,%rax` */
    *(uint64_t *)(uintptr_t)(tp + 8) = dtv;
    *(uint64_t *)(uintptr_t)dtv = modules;
    return tp;
}

/* __tls_get_addr - the general-dynamic TLS entry point.
 *
 * Any -fPIC object compiled without -ftls-model=initial-exec imports
 * this, so a shared library using `__thread` fails to link without it.
 * The argument is a two-word (module id, offset) pair the linker built
 * with DTPMOD64/DTPOFF64 relocations; the job is to turn that into an
 * address using this thread's DTV.
 *
 * No lazy allocation: every module in the static TLS block already has a
 * DTV entry by the time any thread runs, because this system has no
 * dlopen-after-startup TLS (the linker refuses it explicitly rather than
 * leaving a module id pointing at storage that does not exist). */
typedef struct { uint64_t modid; uint64_t offset; } cos_tls_index_t;

void *__tls_get_addr(cos_tls_index_t *ti)
{
    uint64_t tp;
    __asm__ volatile ("mov %%fs:0, %0" : "=r"(tp));
    if (!tp || !ti) return NULL;
    uint64_t dtv = *(uint64_t *)(uintptr_t)(tp + 8);
    if (!dtv) return NULL;
    uint64_t count = *(uint64_t *)(uintptr_t)dtv;
    if (ti->modid == 0 || ti->modid > count) return NULL;
    uint64_t block = ((uint64_t *)(uintptr_t)dtv)[ti->modid];
    if (!block) return NULL;
    return (void *)(uintptr_t)(block + ti->offset);
}
