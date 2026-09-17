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
    uint32_t _pad;
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

extern int main(int argc, char **argv, char **envp);

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
void __cos_rt_start(uint64_t *sp)
{
    cos_argc = (int)sp[0];
    cos_argv = (char **)&sp[1];
    cos_environ = (char **)&sp[1 + cos_argc + 1];

    char **e = cos_environ;
    while (*e) ++e;
    cos_auxv = (uint64_t *)(e + 1);

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
        apply_ifuncs(lm);
        register_destructors(lm);
        run_constructors(lm, cos_argc, cos_argv, cos_environ);
    }

    int status = main(cos_argc, cos_argv, cos_environ);

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
