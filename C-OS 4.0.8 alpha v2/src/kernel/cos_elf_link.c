/**
 * cos_elf_link.c - the in-kernel dynamic linker for C-OS.
 *
 * See cos_elf_link.h for what this is and what it replaces.
 *
 * THE DIRECTORY CONTRACT, AND A BUG IT EXPOSED
 * --------------------------------------------
 * Everything the loader does resolves addresses against the ACTIVE page
 * tables: paging_root() returns `current_directory`, so mapping "into"
 * a directory that is not active silently maps into whichever address
 * space is running instead. cos_elf.h has always documented that the
 * caller must switch first.
 *
 * The tree's one caller did not. cos_launch_elf_image() in
 * userspace_demo.c called cos_elf_load(proc->page_dir, ...) immediately
 * after process_create(), with no paging_switch_directory() anywhere in
 * the path - while paging_create_directory() explicitly ZEROES PML4
 * entries 1..255 on the new directory, so the freshly created process
 * cannot possibly already be active and cannot inherit the mapping.
 * Every other place in the tree that touches another address space
 * (paging_setup_user_stack, task.c's fork path, task.c:1044) does the
 * save/switch/restore dance; this path was the exception.
 *
 * So every mapping here goes through with_process_dir(), which does that
 * dance once and correctly, and the launcher is routed through it too.
 */
#include "cos_elf_link.h"
#include "cos_elf.h"
#include "serial.h"
#include "string.h"
#include "memory.h"
#include "sync.h"
#include "mm/paging.h"

extern int cos_fs_read_file(const char *path, void *buffer, uint64_t size);

#define PHYS_TO_VIRT_L(phys) ((phys) + 0xFFFF800000000000ULL)

/* Largest single object this linker will read off disk. Bounds the
 * kernel allocation made BEFORE anything has been parsed, which is the
 * one allocation an attacker controls purely by file size. */
#define COS_LINK_MAX_IMAGE (16ULL * 1024ULL * 1024ULL)

/* Depth cap on the dependency graph. Cycles are also broken by the
 * already-loaded check, but the cap is kept as a backstop that does not
 * depend on that check being right. */
#define COS_LINK_MAX_DEPTH 8

static void lg(const char *m)
{
    serial_puts("[LINK] "); serial_puts(m); serial_puts("\n");
}
static void lg2(const char *m, const char *d)
{
    serial_puts("[LINK] "); serial_puts(m); serial_puts(d ? d : "(null)");
    serial_puts("\n");
}
static void lg_hex(const char *m, uint64_t v)
{
    serial_puts("[LINK] "); serial_puts(m); serial_puts("0x");
    serial_puthex(v); serial_puts("\n");
}

/* ================================================================== */
/* Namespaces                                                          */
/* ================================================================== */

static cos_link_ns_t g_ns[COS_LINK_MAX_PROCS];
static char g_search_path[512] = "/lib:/usr/lib:/";

void cos_link_set_search_path(const char *paths)
{
    if (!paths) return;
    strncpy(g_search_path, paths, sizeof(g_search_path) - 1);
    g_search_path[sizeof(g_search_path) - 1] = '\0';
}

static cos_link_ns_t *ns_find(uint64_t pid)
{
    for (uint32_t i = 0; i < COS_LINK_MAX_PROCS; ++i) {
        if (g_ns[i].used && g_ns[i].pid == pid) return &g_ns[i];
    }
    return NULL;
}

static cos_link_ns_t *ns_get_or_create(uint64_t pid)
{
    cos_link_ns_t *ns = ns_find(pid);
    if (ns) return ns;
    for (uint32_t i = 0; i < COS_LINK_MAX_PROCS; ++i) {
        if (g_ns[i].used) continue;
        memset(&g_ns[i], 0, sizeof(g_ns[i]));
        g_ns[i].used = true;
        g_ns[i].pid = pid;
        g_ns[i].next_base = COS_LINK_LIB_BASE;
        return &g_ns[i];
    }
    lg("no free link namespace - too many processes with loaded objects");
    return NULL;
}

const cos_link_ns_t *cos_link_ns_peek(uint64_t pid) { return ns_find(pid); }

void cos_link_release_pid(uint64_t pid)
{
    cos_link_ns_t *ns = ns_find(pid);
    if (!ns) return;
    /* Only the bookkeeping is released here. The MAPPINGS belong to the
     * process's page directory and are torn down with it by
     * process_destroy() - freeing them here as well would double-free
     * every frame. */
    for (uint32_t i = 0; i < COS_LINK_MAX_OBJECTS; ++i) {
        if (ns->objs[i].obj) { kfree(ns->objs[i].obj); ns->objs[i].obj = NULL; }
    }
    memset(ns, 0, sizeof(*ns));
}

/* ================================================================== */
/* Running with a process's address space active                       */
/* ================================================================== */

typedef struct {
    page_directory_t *saved;
    uint64_t irq;
    bool switched;
} dir_guard_t;

/* Makes `dir` the active address space, saving what was there. Interrupts
 * are disabled for the whole window because current_directory is a single
 * global that IS the CPU's current address space - a timer tick in the
 * middle would run the next thread under this process's mappings. */
static void dir_enter(dir_guard_t *g, page_directory_t *dir)
{
    g->irq = sync_irq_save();
    g->saved = paging_get_current_directory();
    g->switched = false;
    if (dir && dir != g->saved) {
        paging_switch_directory(dir);
        g->switched = true;
    }
}

static void dir_leave(dir_guard_t *g)
{
    if (g->switched && g->saved) paging_switch_directory(g->saved);
    sync_irq_restore(g->irq);
}

/* Maps `bytes` of fresh, zeroed, writable user pages at `at` in the
 * currently active directory. Used for the TLS block and the link map,
 * neither of which comes from a file. */
static bool map_zeroed(uint64_t at, uint64_t bytes, bool writable)
{
    uint64_t start = at & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = (at + bytes + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        if (paging_virt_to_phys(v)) continue;
        uint64_t phys = paging_alloc_physical();
        if (!phys) return false;
        uint64_t flags = PAGE_PRESENT | PAGE_USER | (writable ? PAGE_RW : 0);
        /* Mapped writable regardless, then narrowed by the caller: the
         * kernel has to write the contents in first, and paging_map_page()
         * refuses to re-map an already-present leaf. */
        if (!paging_map_page(v, phys, flags | PAGE_RW)) {
            paging_free_physical(phys);
            return false;
        }
        memset((void *)(uintptr_t)PHYS_TO_VIRT_L(phys), 0, PAGE_SIZE);
    }
    return true;
}

static void protect_range_ro(uint64_t at, uint64_t bytes)
{
    uint64_t start = at & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = (at + bytes + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        (void)paging_protect_page(v, PAGE_PRESENT | PAGE_USER);
    }
}

static bool write_user_bytes(uint64_t at, const void *src, uint64_t len)
{
    const uint8_t *in = (const uint8_t *)src;
    while (len) {
        uint64_t page = at & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t off = at - page;
        uint64_t n = PAGE_SIZE - off;
        if (n > len) n = len;
        uint64_t phys = paging_virt_to_phys(page);
        if (!phys) return false;
        memcpy((uint8_t *)(uintptr_t)PHYS_TO_VIRT_L(phys) + off, in, (size_t)n);
        at += n; in += n; len -= n;
    }
    return true;
}

/* ================================================================== */
/* Finding a dependency on disk                                        */
/* ================================================================== */

static void path_dirname(const char *path, char *out, uint64_t out_size)
{
    uint64_t last = 0, i = 0;
    for (; path[i] && i + 1 < out_size; ++i) {
        out[i] = path[i];
        if (path[i] == '/') last = i;
    }
    out[last ? last : 0] = '\0';
    if (!last) { out[0] = '/'; out[1] = '\0'; }
}

static void path_join(char *out, uint64_t out_size, const char *dir, const char *name)
{
    uint64_t i = 0;
    for (; dir[i] && i + 1 < out_size; ++i) out[i] = dir[i];
    if (i && out[i - 1] != '/' && i + 1 < out_size) out[i++] = '/';
    for (uint64_t k = 0; name[k] && i + 1 < out_size; ++k) out[i++] = name[k];
    out[i] = '\0';
}

/* Tries `dir/name`; returns true and fills `out` if the file reads. */
static bool try_path(const char *dir, const char *name, char *out, uint64_t out_size)
{
    char candidate[COS_ELF_MAX_PATH];
    path_join(candidate, sizeof(candidate), dir, name);

    /* A 4-byte read is enough to answer "does this exist and is it an
     * ELF?" without allocating a buffer for a file that is neither. */
    uint8_t magic[4];
    int got = cos_fs_read_file(candidate, magic, sizeof(magic));
    if (got < 4) return false;
    if (magic[0] != 0x7f || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
        return false;
    }
    strncpy(out, candidate, out_size - 1);
    out[out_size - 1] = '\0';
    return true;
}

/* Walks a colon-separated list. */
static bool try_path_list(const char *list, const char *name,
                          char *out, uint64_t out_size)
{
    if (!list || !list[0]) return false;
    char dir[COS_ELF_MAX_PATH];
    uint64_t di = 0;
    for (uint64_t i = 0; ; ++i) {
        char c = list[i];
        if (c != ':' && c != '\0') {
            if (di + 1 < sizeof(dir)) dir[di++] = c;
            continue;
        }
        dir[di] = '\0';
        if (di && try_path(dir, name, out, out_size)) return true;
        di = 0;
        if (c == '\0') break;
    }
    return false;
}

/* Resolves a DT_NEEDED name to a real path.
 *
 * Order matches a real dynamic linker: an absolute name is taken
 * literally, then DT_RUNPATH of the object that asked, then its
 * DT_RPATH, then the directory the requester itself came from, then the
 * system search path. The old code did none of this - it prepended "/"
 * and hoped, which is why every dependency had to live in the
 * filesystem root. */
static bool resolve_needed(cos_link_ns_t *ns, const cos_link_obj_t *requester,
                           const char *name, char *out, uint64_t out_size)
{
    if (!name || !name[0]) return false;

    if (name[0] == '/') {
        uint8_t magic[4];
        if (cos_fs_read_file(name, magic, sizeof(magic)) >= 4 && magic[0] == 0x7f) {
            strncpy(out, name, out_size - 1);
            out[out_size - 1] = '\0';
            return true;
        }
        return false;
    }

    char buf[COS_ELF_MAX_PATH];
    if (requester && requester->obj) {
        if (requester->obj->runpath_off &&
            cos_elf_string((page_directory_t *)ns->dir, requester->obj, requester->obj->runpath_off,
                           buf, sizeof(buf)) &&
            try_path_list(buf, name, out, out_size)) {
            return true;
        }
        if (requester->obj->rpath_off &&
            cos_elf_string((page_directory_t *)ns->dir, requester->obj, requester->obj->rpath_off,
                           buf, sizeof(buf)) &&
            try_path_list(buf, name, out, out_size)) {
            return true;
        }
        path_dirname(requester->path, buf, sizeof(buf));
        if (try_path(buf, name, out, out_size)) return true;
    }
    return try_path_list(g_search_path, name, out, out_size);
}

/* ================================================================== */
/* The global symbol scope                                             */
/* ================================================================== */

typedef struct {
    cos_link_ns_t *ns;
    const cos_elf_object_t *skip;   /* for R_X86_64_COPY */
    char last_missing[COS_ELF_MAX_SYMNAME + 1];
} scope_ctx_t;

/* Resolution order is the object table's order, which is breadth-first
 * load order with the executable at index 0. That ordering is the whole
 * mechanism behind interposition: whoever comes first wins, so a symbol
 * the executable defines shadows a library's definition of the same
 * name. */
static bool scope_resolve(void *ctx_v, const cos_elf_symreq_t *req,
                          cos_elf_symval_t *out)
{
    scope_ctx_t *ctx = (scope_ctx_t *)ctx_v;
    cos_link_ns_t *ns = ctx->ns;

    for (uint32_t i = 0; i < COS_LINK_MAX_OBJECTS; ++i) {
        cos_link_obj_t *slot = &ns->objs[i];
        if (!slot->used || !slot->obj || !slot->global) continue;
        /* A COPY relocation wants the OTHER object's definition by
         * definition: the requester reserved space precisely because it
         * does not have one. */
        if ((req->flags & COS_ELF_SYM_COPY) && slot->obj == req->requester) continue;
        if (cos_elf_lookup((page_directory_t *)ns->dir, slot->obj, req->name, req->version, out)) return true;
    }

    /* A versioned request that nothing satisfies is retried unversioned.
     * Real objects frequently import `name@GLIBC_2.2.5`-style versions
     * from libraries that were built without a version script at all;
     * refusing outright would reject them even though exactly one
     * definition exists and there is no ambiguity about which is meant. */
    if (req->version) {
        for (uint32_t i = 0; i < COS_LINK_MAX_OBJECTS; ++i) {
            cos_link_obj_t *slot = &ns->objs[i];
            if (!slot->used || !slot->obj || !slot->global) continue;
            if ((req->flags & COS_ELF_SYM_COPY) && slot->obj == req->requester) continue;
            if (cos_elf_lookup((page_directory_t *)ns->dir, slot->obj, req->name, NULL, out)) {
                lg2("note: satisfied a versioned import unversioned: ", req->name);
                return true;
            }
        }
    }

    strncpy(ctx->last_missing, req->name, sizeof(ctx->last_missing) - 1);
    ctx->last_missing[sizeof(ctx->last_missing) - 1] = '\0';
    return false;
}

/* ================================================================== */
/* Loading one object into a namespace                                 */
/* ================================================================== */

static int find_loaded(cos_link_ns_t *ns, const char *path, const char *soname)
{
    for (uint32_t i = 0; i < COS_LINK_MAX_OBJECTS; ++i) {
        if (!ns->objs[i].used) continue;
        if (strcmp(ns->objs[i].path, path) == 0) return (int)i;
        /* Matching on SONAME as well, not just path: a diamond
         * dependency reached through two different paths (/lib/x.c-osll
         * and ./x.c-osll) is still ONE library, and loading it twice
         * gives the two copies separate, silently divergent global
         * state. */
        if (soname && soname[0] && strcmp(ns->objs[i].soname, soname) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int alloc_slot(cos_link_ns_t *ns)
{
    for (uint32_t i = 0; i < COS_LINK_MAX_OBJECTS; ++i) {
        if (!ns->objs[i].used) return (int)i;
    }
    return -1;
}

static uint64_t alloc_base(cos_link_ns_t *ns, uint64_t span)
{
    uint64_t base = (ns->next_base + COS_LINK_LIB_ALIGN - 1) & ~(COS_LINK_LIB_ALIGN - 1);
    uint64_t end  = base + ((span + COS_LINK_LIB_ALIGN - 1) & ~(COS_LINK_LIB_ALIGN - 1));
    if (end <= base || end > COS_LINK_LIB_LIMIT) return 0;
    ns->next_base = end;
    return base;
}

/* Forward declaration: dependency loading is mutually recursive with
 * object loading, because an object's dependencies must be mapped and
 * relocated before it can be relocated itself. */
static int load_object(cos_link_ns_t *ns, const char *path, bool global,
                       int depth, int requester_slot);

/* Maps and registers one object WITHOUT relocating it, then recursively
 * does the same for its dependencies, and finally relocates. Mapping the
 * whole graph before relocating any of it is what makes a cycle work:
 * A -> B -> A resolves because A is already mapped and registered by the
 * time B is relocated, even though A has not been relocated yet. */
static int load_object(cos_link_ns_t *ns, const char *path, bool global,
                       int depth, int requester_slot)
{
    if (depth >= COS_LINK_MAX_DEPTH) {
        lg("dependency chain too deep - refusing");
        return -1;
    }

    uint8_t *buf = (uint8_t *)kmalloc((size_t)COS_LINK_MAX_IMAGE);
    if (!buf) return -1;

    int got = cos_fs_read_file(path, buf, COS_LINK_MAX_IMAGE);
    if (got <= 0) {
        kfree(buf);
        lg2("cannot read: ", path);
        return -1;
    }

    cos_elf_info_t info;
    if (!cos_elf_inspect(buf, (uint64_t)got, &info)) {
        kfree(buf);
        lg2("not a loadable object: ", path);
        return -1;
    }

    int existing = find_loaded(ns, path, info.soname);
    if (existing >= 0) {
        kfree(buf);
        ns->objs[existing].refcount++;
        if (global) ns->objs[existing].global = true;
        return existing;
    }

    int slot = alloc_slot(ns);
    if (slot < 0) {
        kfree(buf);
        lg("no free object slot in this process's namespace");
        return -1;
    }

    uint64_t base = alloc_base(ns, info.span);
    if (!base) {
        kfree(buf);
        lg("library address space exhausted");
        return -1;
    }

    cos_elf_object_t *obj = (cos_elf_object_t *)kmalloc(sizeof(cos_elf_object_t));
    if (!obj) { kfree(buf); return -1; }

    if (!cos_elf_map((page_directory_t *)ns->dir, buf, (uint64_t)got, base, obj)) {
        kfree(obj); kfree(buf);
        lg2("map failed: ", path);
        return -1;
    }
    obj->flags = (obj->flags & ~COS_ELF_F_PIE) | COS_ELF_F_LIB;

    ns->objs[slot].used     = true;
    ns->objs[slot].is_main  = false;
    ns->objs[slot].global   = global;
    ns->objs[slot].refcount = 1;
    ns->objs[slot].obj      = obj;
    strncpy(ns->objs[slot].path, path, COS_ELF_MAX_PATH - 1);
    ns->objs[slot].path[COS_ELF_MAX_PATH - 1] = '\0';
    strncpy(ns->objs[slot].soname, info.soname, COS_ELF_MAX_PATH - 1);
    ns->objs[slot].soname[COS_ELF_MAX_PATH - 1] = '\0';
    if (ns->count <= (uint32_t)slot) ns->count = (uint32_t)slot + 1;

    /* Dependencies next, now that this object is registered - which is
     * what breaks a dependency cycle. */
    for (uint32_t i = 0; i < info.needed_count; ++i) {
        char dep_path[COS_ELF_MAX_PATH];
        if (!resolve_needed(ns, &ns->objs[slot], info.needed[i],
                            dep_path, sizeof(dep_path))) {
            lg2("cannot find dependency: ", info.needed[i]);
            kfree(buf);
            return -1;
        }
        lg2("loading dependency ", dep_path);
        if (load_object(ns, dep_path, global, depth + 1, slot) < 0) {
            kfree(buf);
            return -1;
        }
    }

    kfree(buf);
    (void)requester_slot;
    return slot;
}

/* Relocates every object that has not been relocated yet, in reverse
 * registration order so that dependencies (registered later) are done
 * before their dependents. */
static bool relocate_all(cos_link_ns_t *ns, scope_ctx_t *ctx)
{
    for (int i = (int)ns->count - 1; i >= 0; --i) {
        cos_link_obj_t *slot = &ns->objs[i];
        if (!slot->used || !slot->obj) continue;
        if (!cos_elf_relocate((page_directory_t *)ns->dir, slot->obj, scope_resolve, ctx)) {
            lg2("relocation failed for ", slot->path);
            if (ctx->last_missing[0]) lg2("  undefined symbol: ", ctx->last_missing);
            return false;
        }
    }
    for (uint32_t i = 0; i < ns->count; ++i) {
        if (ns->objs[i].used && ns->objs[i].obj) {
            (void)cos_elf_protect((page_directory_t *)ns->dir, ns->objs[i].obj);
        }
    }
    return true;
}

/* ================================================================== */
/* TLS and the link map                                                */
/* ================================================================== */

static bool setup_tls(cos_link_ns_t *ns)
{
    cos_elf_object_t *set[COS_LINK_MAX_OBJECTS];
    uint32_t n = 0;
    bool any = false;
    for (uint32_t i = 0; i < ns->count; ++i) {
        if (!ns->objs[i].used || !ns->objs[i].obj) continue;
        set[n++] = ns->objs[i].obj;
        if (ns->objs[i].obj->flags & COS_ELF_F_HAS_TLS) any = true;
    }
    if (!any) { ns->tls_tp = 0; return true; }

    uint64_t block = 0, align = 0;
    if (!cos_elf_tls_layout(set, n, &block, &align)) return false;

    uint32_t modules = 0;
    for (uint32_t i = 0; i < n; ++i) if (set[i]->tls_modid > 0) modules++;

    /* Extra slack so that alignment inside the region cannot push the
     * TCB past its end - cos_elf_tls_init() aligns the thread pointer
     * upward and would otherwise fail on a region sized exactly. */
    uint64_t need = cos_elf_tls_region_size(block, modules) + align + PAGE_SIZE;
    if (!map_zeroed(COS_LINK_TLS_BASE, need, true)) {
        lg("could not map the static TLS region");
        return false;
    }

    cos_elf_tls_t tls;
    if (!cos_elf_tls_init((page_directory_t *)ns->dir, COS_LINK_TLS_BASE, need, set, n, &tls)) {
        lg("could not build the static TLS block");
        return false;
    }
    ns->tls_block_size  = tls.block_size;
    ns->tls_region      = COS_LINK_TLS_BASE;
    ns->tls_region_size = need;
    ns->tls_tp          = tls.tp;
    lg_hex("static TLS thread pointer = ", tls.tp);
    return true;
}

static bool publish_linkmap(cos_link_ns_t *ns)
{
    /* The link map is several KiB and must be built somewhere before it
     * is copied in - the process's page is not addressable as a struct.
     * kmalloc'd rather than put on the kernel stack: sizeof() is around
     * 6 KiB, which is a lot to spend on a stack shared with the whole
     * filesystem call chain. */
    cos_elf_linkmap_t *lm = (cos_elf_linkmap_t *)kmalloc(sizeof(cos_elf_linkmap_t));
    if (!lm) return false;
    memset(lm, 0, sizeof(*lm));

    lm->magic   = COS_ELF_LINKMAP_MAGIC;
    lm->version = COS_ELF_LINKMAP_VERSION;
    lm->tls_tp  = ns->tls_tp;
    lm->tls_block_size = ns->tls_block_size;

    for (uint32_t i = 0; i < ns->count && lm->obj_count < COS_ELF_LINKMAP_MAX_OBJ; ++i) {
        cos_link_obj_t *slot = &ns->objs[i];
        if (!slot->used || !slot->obj) continue;
        cos_elf_object_t *o = slot->obj;
        cos_elf_linkmap_obj_t *e = &lm->obj[lm->obj_count++];

        e->base = o->base;
        e->map_start = o->map_start; e->map_end = o->map_end;
        e->dynamic = o->dynamic;
        e->init = o->init;
        e->init_array = o->init_array; e->init_array_sz = o->init_array_sz;
        e->fini = o->fini;
        e->fini_array = o->fini_array; e->fini_array_sz = o->fini_array_sz;
        e->preinit_array = o->preinit_array;
        e->preinit_array_sz = o->preinit_array_sz;
        e->tls_modid = o->tls_modid;
        e->tls_offset = o->tls_offset;
        e->tls_image = o->tls_vaddr;
        e->tls_filesz = o->tls_filesz;
        e->tls_memsz = o->tls_memsz;
        e->tls_align = o->tls_align;
        e->flags = o->flags;
        strncpy(e->name, slot->soname[0] ? slot->soname : slot->path,
                sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';

        /* IFUNC work is aggregated across objects: ring3 applies the
         * whole list before any constructor runs, because a constructor
         * may well call through an ifunc slot. */
        for (uint32_t k = 0; k < o->ifunc_count; ++k) {
            if (lm->ifunc_count >= COS_ELF_MAX_IFUNC) {
                lg("warning: ifunc list truncated in the link map");
                break;
            }
            lm->ifunc_target[lm->ifunc_count]   = o->ifunc_target[k];
            lm->ifunc_resolver[lm->ifunc_count] = o->ifunc_resolver[k];
            lm->ifunc_count++;
        }
    }

    bool ok = map_zeroed(COS_LINK_MAP_BASE, sizeof(*lm), true) &&
              write_user_bytes(COS_LINK_MAP_BASE, lm, sizeof(*lm));
    if (ok) {
        /* Read-only: the process reads it, and nothing good comes of the
         * process being able to rewrite the list of resolver functions
         * its own startup code is about to call. */
        protect_range_ro(COS_LINK_MAP_BASE, sizeof(*lm));
        ns->linkmap_addr = COS_LINK_MAP_BASE;
        ns->linkmap_size = sizeof(*lm);
    } else {
        lg("could not publish the link map");
    }
    kfree(lm);
    return ok;
}

/* ================================================================== */
/* Loading an executable                                               */
/* ================================================================== */

bool cos_link_load_executable(process_t *proc, const char *path,
                              const uint8_t *image, uint64_t image_len,
                              const char *const *argv, int argc,
                              const char *const *envp, int envc,
                              cos_link_result_t *out)
{
    if (!proc || !proc->page_dir || !image || !out) return false;
    memset(out, 0, sizeof(*out));

    cos_elf_info_t info;
    if (!cos_elf_inspect(image, image_len, &info)) return false;
    if (info.e_type == 3 /* ET_DYN */ && info.entry == 0) {
        lg("refusing to run a shared object as a program");
        return false;
    }
    if (info.flags & COS_ELF_F_INTERP) {
        /* Mapping an external ld.so and handing it control is a coherent
         * future step; silently ignoring PT_INTERP is not, because the
         * program would start with an unpopulated GOT. */
        lg2("program requests an interpreter this system does not provide: ",
            info.interp);
        return false;
    }

    cos_link_ns_t *ns = ns_get_or_create(proc->pid);
    if (!ns) return false;
    ns->dir = proc->page_dir;

    dir_guard_t g;
    dir_enter(&g, (page_directory_t *)proc->page_dir);

    bool ok = true;
    scope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ns = ns;

    /* --- the executable itself, at slot 0 --- */
    cos_elf_object_t *main_obj = (cos_elf_object_t *)kmalloc(sizeof(cos_elf_object_t));
    if (!main_obj) { dir_leave(&g); return false; }

    uint64_t bias = (info.e_type == 2 /* ET_EXEC */) ? 0 : COS_ELF_PIE_BASE;
    if (!cos_elf_map((page_directory_t *)proc->page_dir, image, image_len, bias, main_obj)) {
        kfree(main_obj);
        dir_leave(&g);
        return false;
    }
    ns->objs[0].used = true;
    ns->objs[0].is_main = true;
    ns->objs[0].global = true;
    ns->objs[0].refcount = 1;
    ns->objs[0].obj = main_obj;
    strncpy(ns->objs[0].path, path ? path : "", COS_ELF_MAX_PATH - 1);
    ns->objs[0].path[COS_ELF_MAX_PATH - 1] = '\0';
    if (ns->count < 1) ns->count = 1;

    /* --- its dependencies --- */
    for (uint32_t i = 0; i < info.needed_count && ok; ++i) {
        char dep_path[COS_ELF_MAX_PATH];
        if (!resolve_needed(ns, &ns->objs[0], info.needed[i],
                            dep_path, sizeof(dep_path))) {
            lg2("cannot find dependency: ", info.needed[i]);
            ok = false; break;
        }
        lg2("loading dependency ", dep_path);
        if (load_object(ns, dep_path, true, 1, 0) < 0) ok = false;
    }

    /* --- TLS ids before relocation: TPOFF64/DTPMOD64 need them --- */
    if (ok) {
        cos_elf_object_t *set[COS_LINK_MAX_OBJECTS];
        uint32_t n = 0;
        for (uint32_t i = 0; i < ns->count; ++i) {
            if (ns->objs[i].used && ns->objs[i].obj) set[n++] = ns->objs[i].obj;
        }
        uint64_t block = 0, align = 0;
        ok = cos_elf_tls_layout(set, n, &block, &align);
        if (!ok) lg("TLS layout failed");
    }

    if (ok) ok = relocate_all(ns, &ctx);
    if (ok) ok = setup_tls(ns);
    if (ok) ok = publish_linkmap(ns);

    /* --- the initial stack, with a full auxiliary vector --- */
    if (ok) {
        cos_elf_stack_params_t p;
        memset(&p, 0, sizeof(p));
        p.argv = argv; p.argc = argc;
        p.envp = envp; p.envc = envc;
        p.at_phdr  = main_obj->phdr;
        p.at_phent = main_obj->phentsize;
        p.at_phnum = main_obj->phnum;
        p.at_entry = main_obj->entry;
        p.at_base  = main_obj->base;
        p.at_linkmap = ns->linkmap_addr;
        p.at_tp    = ns->tls_tp;
        p.execfn   = path;
        /* Weak seed, and labelled as such: the kernel has no entropy
         * source wired up here, and claiming otherwise in AT_RANDOM would
         * give a libc a stack guard it believes is unpredictable. */
        p.at_random_seed[0] = 0x9E3779B97F4A7C15ULL ^ (proc->pid * 0x100000001B3ULL);
        p.at_random_seed[1] = 0xC2B2AE3D27D4EB4FULL ^ (uint64_t)(uintptr_t)main_obj;

        uint64_t rsp = 0;
        ok = cos_elf_setup_stack_ex((page_directory_t *)proc->page_dir, proc->stack_end, &p, &rsp);
        if (ok) out->rsp = rsp;
        else lg("could not build the initial stack");
    }

    if (ok) {
        out->entry   = main_obj->entry;
        out->tp      = ns->tls_tp;
        out->linkmap = ns->linkmap_addr;
    }

    dir_leave(&g);

    if (!ok) {
        cos_link_release_pid(proc->pid);
        return false;
    }

    serial_puts("[LINK] ");
    serial_puts(path ? path : "(program)");
    serial_puts(": ");
    serial_putdec(ns->count);
    serial_puts(" object(s), entry=0x");
    serial_puthex(out->entry);
    if (out->tp) { serial_puts(" tp=0x"); serial_puthex(out->tp); }
    serial_puts("\n");
    return true;
}

/* ================================================================== */
/* dlopen / dlsym / dlclose                                            */
/* ================================================================== */

int64_t cos_link_dlopen(process_t *proc, const char *path, uint32_t flags)
{
    if (!proc || !proc->page_dir || !path || !path[0]) return -1;

    cos_link_ns_t *ns = ns_get_or_create(proc->pid);
    if (!ns) return -1;
    ns->dir = proc->page_dir;

    /* A bare name goes through the same search a DT_NEEDED would, so
     * dlopen("libmath.c-osll") works from any directory - the old code
     * required the caller to know the full path or accept "/". */
    char resolved[COS_ELF_MAX_PATH];
    if (!resolve_needed(ns, ns->objs[0].used ? &ns->objs[0] : NULL,
                        path, resolved, sizeof(resolved))) {
        lg2("dlopen: cannot find ", path);
        return -1;
    }

    dir_guard_t g;
    dir_enter(&g, (page_directory_t *)proc->page_dir);

    uint32_t before = ns->count;
    int slot = load_object(ns, resolved, (flags & COS_RTLD_GLOBAL) != 0, 1, -1);

    bool ok = (slot >= 0);
    if (ok && ns->count != before) {
        /* Newly mapped objects need TLS ids assigned before their
         * relocations run, exactly as at exec time. A library dlopen'd
         * after the static TLS block was built cannot extend it, so one
         * with its own PT_TLS is refused rather than left with a module
         * id pointing at storage that does not exist. */
        for (uint32_t i = before; i < ns->count; ++i) {
            if (ns->objs[i].used && ns->objs[i].obj &&
                (ns->objs[i].obj->flags & COS_ELF_F_HAS_TLS) &&
                ns->tls_tp != 0) {
                lg2("dlopen: refusing a library with its own TLS after startup: ",
                    ns->objs[i].path);
                lg("       (the static TLS block is fixed once the process starts; "
                   "link this library in at build time instead)");
                ok = false;
            }
        }
    }

    scope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ns = ns;
    if (ok) {
        for (uint32_t i = before; i < ns->count; ++i) {
            cos_link_obj_t *s = &ns->objs[i];
            if (!s->used || !s->obj) continue;
            /* A dlopen'd object is temporarily promoted into the global
             * scope during its own relocation even when RTLD_LOCAL was
             * requested, because its internal cross-references have to
             * resolve against its own siblings. It is demoted again
             * below. */
            s->global = true;
        }
        for (int i = (int)ns->count - 1; i >= (int)before && ok; --i) {
            cos_link_obj_t *s = &ns->objs[i];
            if (!s->used || !s->obj) continue;
            ok = cos_elf_relocate((page_directory_t *)ns->dir, s->obj, scope_resolve, &ctx);
            if (!ok) {
                lg2("dlopen: relocation failed for ", s->path);
                if (ctx.last_missing[0]) lg2("  undefined symbol: ", ctx.last_missing);
            }
        }
        for (uint32_t i = before; i < ns->count && ok; ++i) {
            if (ns->objs[i].used && ns->objs[i].obj) {
                (void)cos_elf_protect((page_directory_t *)ns->dir, ns->objs[i].obj);
                if (!(flags & COS_RTLD_GLOBAL)) ns->objs[i].global = false;
            }
        }
        /* The object the caller actually asked for stays reachable by
         * handle regardless of RTLD_LOCAL. */
        if (ok && slot >= 0) ns->objs[slot].global |= (flags & COS_RTLD_GLOBAL) != 0;
    }

    if (ok) ok = publish_linkmap(ns);

    dir_leave(&g);

    if (!ok) return -1;
    return (int64_t)(slot + 1);
}

uint64_t cos_link_dlsym(process_t *proc, int64_t handle, const char *name)
{
    if (!proc || !proc->page_dir || !name) return 0;
    cos_link_ns_t *ns = ns_find(proc->pid);
    if (!ns) return 0;

    dir_guard_t g;
    dir_enter(&g, (page_directory_t *)proc->page_dir);

    cos_elf_symval_t sv;
    uint64_t addr = 0;

    if (handle == COS_RTLD_DEFAULT) {
        /* Search the whole global scope, in load order - the same
         * ordering relocation uses, so dlsym() and a direct call cannot
         * disagree about which definition wins. */
        for (uint32_t i = 0; i < ns->count; ++i) {
            if (!ns->objs[i].used || !ns->objs[i].obj || !ns->objs[i].global) continue;
            if (cos_elf_lookup((page_directory_t *)ns->dir, ns->objs[i].obj, name, NULL, &sv)) {
                addr = sv.address; break;
            }
        }
    } else if (handle >= 1 && handle <= (int64_t)COS_LINK_MAX_OBJECTS) {
        cos_link_obj_t *s = &ns->objs[handle - 1];
        if (s->used && s->obj && cos_elf_lookup((page_directory_t *)ns->dir, s->obj, name, NULL, &sv)) {
            addr = sv.address;
        }
    }

    /* A TLS symbol has no plain address - it has a module and an offset,
     * and returning st_value would hand the caller a pointer into low
     * memory. dlsym() genuinely cannot express one, so report nothing
     * rather than something wrong. */
    if (addr && sv.is_tls) {
        lg2("dlsym: refusing to return an address for a TLS symbol: ", name);
        addr = 0;
    }

    dir_leave(&g);
    return addr;
}

int cos_link_dlclose(process_t *proc, int64_t handle)
{
    if (!proc) return -1;
    cos_link_ns_t *ns = ns_find(proc->pid);
    if (!ns) return -1;
    if (handle < 1 || handle > (int64_t)COS_LINK_MAX_OBJECTS) return -1;

    cos_link_obj_t *s = &ns->objs[handle - 1];
    if (!s->used) return -1;
    if (s->is_main) return -1;

    if (s->refcount > 1) { s->refcount--; return 0; }

    /* The reference is dropped and the handle retired, but the mapping
     * stays. Unmapping would need every relocation that points INTO this
     * object - from any other object, including ones loaded later - to be
     * found and undone, and there is no reverse index for that. An
     * unmap-on-dlclose that skipped it would leave dangling function
     * pointers in other libraries' GOTs, which is far worse than holding
     * the address space until the process exits. Stated plainly rather
     * than implied by a no-op. */
    s->refcount = 0;
    s->global = false;
    lg2("dlclose: handle retired, mapping retained until exit: ", s->path);
    return 0;
}
