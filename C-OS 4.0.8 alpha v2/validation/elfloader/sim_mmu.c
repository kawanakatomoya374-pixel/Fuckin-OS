/**
 * sim_mmu.c - a simulated MMU for the host-side ELF loader harness.
 *
 * WHY A SIMULATOR RATHER THAN A UNIT TEST OF EXTRACTED LOGIC
 * ----------------------------------------------------------
 * The loader's correctness lives almost entirely in how it interacts
 * with the page tables: which pages it allocates, what it zeroes, where
 * it copies file bytes, and what permissions it leaves behind. A test
 * that re-implemented any of that would prove only that the test agrees
 * with itself. So this provides the four paging primitives cos_elf.c
 * actually calls, backed by a real hash table of simulated frames, and
 * the harness compiles src/kernel/cos_elf.c UNMODIFIED against it.
 *
 * THE PHYS_TO_VIRT TRICK
 * ----------------------
 * cos_elf.c reaches simulated "physical" memory through
 * PHYS_TO_VIRT(phys) == phys + 0xFFFF800000000000. So a frame's
 * "physical address" here is simply its host address MINUS that constant;
 * adding it back in the loader lands exactly on the host allocation.
 * Unsigned wraparound makes the arithmetic exact rather than approximate,
 * and it means the loader's own address translation is exercised rather
 * than stubbed out.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The simulator implements the paging API, so it needs the same
 * declarations the loader compiles against. */
#include "mm/paging.h"

#define KVA_OFFSET 0xFFFF800000000000ULL
#define SIM_PAGE   4096
#define SIM_BUCKETS 8192

typedef struct sim_page {
    uint64_t virt;         /* page-aligned virtual address */
    uint64_t flags;        /* PAGE_* flags as the loader set them */
    void    *frame;        /* host allocation backing this page */
    struct sim_page *next;
} sim_page_t;

/* One page table per simulated address space, not one global table.
 *
 * This is deliberate and load-bearing: the loader resolves every address
 * against the ACTIVE directory, so a caller that forgets to switch maps
 * into the wrong process. A single-table simulator cannot tell the two
 * apart and would pass a test the real kernel fails - which is exactly
 * the bug found in cos_launch_elf_image() while writing this. */
#define SIM_MAX_DIRS 8
typedef struct {
    int         used;
    uint64_t    handle;          /* the page_directory_t* value */
    sim_page_t *table[SIM_BUCKETS];
    uint64_t    mapped;
} sim_dir_t;

static sim_dir_t g_dirs[SIM_MAX_DIRS];
static sim_dir_t *g_active;
static uint64_t g_alloc_count;

static sim_dir_t *dir_for(uint64_t handle)
{
    for (int i = 0; i < SIM_MAX_DIRS; ++i) {
        if (g_dirs[i].used && g_dirs[i].handle == handle) return &g_dirs[i];
    }
    for (int i = 0; i < SIM_MAX_DIRS; ++i) {
        if (!g_dirs[i].used) {
            g_dirs[i].used = 1;
            g_dirs[i].handle = handle;
            return &g_dirs[i];
        }
    }
    fprintf(stderr, "sim_mmu: out of simulated address spaces\n");
    abort();
}

void paging_switch_directory(page_directory_t *dir)
{
    g_active = dir_for((uint64_t)(uintptr_t)dir);
}

page_directory_t *paging_get_current_directory(void)
{
    return g_active ? (page_directory_t *)(uintptr_t)g_active->handle : NULL;
}

/* Harness hook: which address space is live right now. */
uint64_t sim_active_dir(void)
{
    return g_active ? g_active->handle : 0;
}

/* Frames handed out by paging_alloc_physical() but not yet mapped. The
 * loader allocates a frame and then maps it, so the two have to be
 * connected the same way they are in the kernel. */
#define SIM_PENDING 1024
static struct { uint64_t phys; void *frame; } g_pending[SIM_PENDING];
static int g_pending_n;

static unsigned bucket_of(uint64_t v) { return (unsigned)((v >> 12) % SIM_BUCKETS); }

static sim_page_t *find_page_in(sim_dir_t *d, uint64_t v)
{
    if (!d) return NULL;
    v &= ~(uint64_t)(SIM_PAGE - 1);
    for (sim_page_t *p = d->table[bucket_of(v)]; p; p = p->next) {
        if (p->virt == v) return p;
    }
    return NULL;
}

static sim_page_t *find_page(uint64_t v) { return find_page_in(g_active, v); }

void sim_reset(void)
{
    for (int d = 0; d < SIM_MAX_DIRS; ++d) {
        for (unsigned i = 0; i < SIM_BUCKETS; ++i) {
            sim_page_t *p = g_dirs[d].table[i];
            while (p) { sim_page_t *n = p->next; free(p->frame); free(p); p = n; }
            g_dirs[d].table[i] = NULL;
        }
        g_dirs[d].used = 0;
        g_dirs[d].handle = 0;
        g_dirs[d].mapped = 0;
    }
    for (int i = 0; i < g_pending_n; ++i) free(g_pending[i].frame);
    g_pending_n = 0;
    g_alloc_count = 0;
    /* Start in a distinct "kernel" address space, so a test that fails
     * to switch shows up as pages landing somewhere the process cannot
     * see rather than silently working. */
    g_active = dir_for(0xC0DEULL);
}

uint64_t sim_mapped_pages(void) { return g_active ? g_active->mapped : 0; }

/* Page flags as seen from a SPECIFIC address space, for asserting that
 * the linker mapped into the process and not into the caller. */
uint64_t sim_page_flags_in(uint64_t dir_handle, uint64_t v)
{
    sim_page_t *p = find_page_in(dir_for(dir_handle), v);
    return p ? p->flags : 0;
}

uint64_t paging_alloc_physical(void)
{
    if (g_pending_n >= SIM_PENDING) {
        fprintf(stderr, "sim_mmu: too many un-mapped allocated frames\n");
        return 0;
    }
    void *frame = NULL;
    if (posix_memalign(&frame, SIM_PAGE, SIM_PAGE) != 0) return 0;
    /* Poison rather than zero. The loader is required to zero every
     * fresh page itself so that .bss is genuinely zero and no previous
     * owner's bytes reach ring3; filling with 0xAA is what makes a
     * failure to do that show up as a test failure instead of passing by
     * luck on a zeroed allocator. */
    memset(frame, 0xAA, SIM_PAGE);

    uint64_t phys = (uint64_t)(uintptr_t)frame - KVA_OFFSET;
    g_pending[g_pending_n].phys = phys;
    g_pending[g_pending_n].frame = frame;
    g_pending_n++;
    g_alloc_count++;
    return phys;
}

void paging_free_physical(uint64_t phys)
{
    for (int i = 0; i < g_pending_n; ++i) {
        if (g_pending[i].phys == phys) {
            free(g_pending[i].frame);
            g_pending[i] = g_pending[--g_pending_n];
            return;
        }
    }
}

bool paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    virt &= ~(uint64_t)(SIM_PAGE - 1);
    if (find_page(virt)) return false;   /* the kernel refuses a present leaf */

    void *frame = NULL;
    for (int i = 0; i < g_pending_n; ++i) {
        if (g_pending[i].phys == phys) {
            frame = g_pending[i].frame;
            g_pending[i] = g_pending[--g_pending_n];
            break;
        }
    }
    if (!frame) {
        fprintf(stderr, "sim_mmu: map of a frame that was never allocated\n");
        return false;
    }

    sim_page_t *p = (sim_page_t *)calloc(1, sizeof(*p));
    if (!p) return false;
    p->virt = virt; p->flags = flags; p->frame = frame;
    p->next = g_active->table[bucket_of(virt)];
    g_active->table[bucket_of(virt)] = p;
    g_active->mapped++;
    return true;
}

uint64_t paging_virt_to_phys(uint64_t v)
{
    sim_page_t *p = find_page(v);
    if (!p) return 0;
    uint64_t phys = (uint64_t)(uintptr_t)p->frame - KVA_OFFSET;
    /* The loader treats 0 as "not mapped"; a real frame must never
     * collide with that sentinel. */
    if (phys == 0) { fprintf(stderr, "sim_mmu: frame aliased the 0 sentinel\n"); abort(); }
    return phys | (v & (SIM_PAGE - 1));
}

int paging_protect_page(uint64_t v, uint64_t flags)
{
    sim_page_t *p = find_page(v);
    if (!p) return 0;
    p->flags = flags | 0x001 /* PAGE_PRESENT */;
    return 1;
}

uint64_t sim_page_flags(uint64_t v)
{
    sim_page_t *p = find_page(v);
    return p ? p->flags : 0;
}

void paging_unmap_page(uint64_t v) { (void)v; }

/* ---- kernel allocator ---- */
void *kmalloc(size_t n) { return malloc(n); }
void  kfree(void *p) { free(p); }

/* ---- a tiny simulated filesystem for the dynamic linker ----
 * cos_fs_read_file() is how the linker finds dependencies, so the
 * search-path logic can only be tested against something that says
 * "yes, that path exists" for some paths and "no" for others. */
#define SIM_FS_MAX 32
static struct { char path[256]; char host[256]; } g_fs[SIM_FS_MAX];
static int g_fs_n;

void sim_fs_add(const char *guest_path, const char *host_path)
{
    if (g_fs_n >= SIM_FS_MAX) abort();
    snprintf(g_fs[g_fs_n].path, sizeof(g_fs[0].path), "%s", guest_path);
    snprintf(g_fs[g_fs_n].host, sizeof(g_fs[0].host), "%s", host_path);
    g_fs_n++;
}

void sim_fs_clear(void) { g_fs_n = 0; }

int cos_fs_read_file(const char *path, void *buffer, uint64_t size)
{
    for (int i = 0; i < g_fs_n; ++i) {
        if (strcmp(g_fs[i].path, path) != 0) continue;
        FILE *f = fopen(g_fs[i].host, "rb");
        if (!f) return -1;
        size_t got = fread(buffer, 1, (size_t)size, f);
        fclose(f);
        return (int)got;
    }
    return -1;
}

/* ---- the handful of kernel utilities cos_elf.c calls ---- */

static int g_quiet = 1;
void sim_set_verbose(int on) { g_quiet = !on; }

void serial_putc(char c) { if (!g_quiet) fputc(c, stderr); }
void serial_puts(const char *s) { if (!g_quiet) fputs(s ? s : "(null)", stderr); }
void serial_puthex(uint64_t v) { if (!g_quiet) fprintf(stderr, "%llx", (unsigned long long)v); }
void serial_putdec(uint64_t v) { if (!g_quiet) fprintf(stderr, "%llu", (unsigned long long)v); }
