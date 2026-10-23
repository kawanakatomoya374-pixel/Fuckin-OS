/* cos_pcache.c - see cos_pcache.h */
#include "cos_pcache.h"
#include "mm/paging.h"
#include "memory.h"
#include "string.h"
#include "sync.h"
#include "serial.h"

#ifndef PHYS_TO_VIRT
#define PHYS_TO_VIRT(phys) ((phys) + 0xFFFF800000000000ULL)      /* the kernel's window onto all physical memory (the host simulator provides none of paging.h) */
#endif

#define PC_ENTRIES   16384u                     /* shared frames at most (64 MiB of library and program text) */
#define PC_BUCKETS   4096u                      /* power of two */

typedef struct { uint64_t phys, hash; uint32_t refs; int32_t next_h, next_p; } pc_entry_t;

static pc_entry_t *g_ent;                       /* allocated at first use */
static int32_t     *g_bh, *g_bp;                /* bucket heads: by content hash, by frame */
static int32_t      g_free = -1;
static uint32_t     g_frames, g_maps;
static bool         g_failed;

static uint64_t page_hash(const uint64_t *p)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (unsigned i = 0; i < PAGE_SIZE / 8; ++i) {
        h ^= p[i];
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    return h;
}
static bool init_tables(void)
{
    if (g_ent) return true;
    if (g_failed) return false;
    g_ent = (pc_entry_t *)kmalloc(sizeof(pc_entry_t) * PC_ENTRIES);
    g_bh = (int32_t *)kmalloc(sizeof(int32_t) * PC_BUCKETS);
    g_bp = (int32_t *)kmalloc(sizeof(int32_t) * PC_BUCKETS);
    if (!g_ent || !g_bh || !g_bp) {
        if (g_ent) kfree(g_ent);
        if (g_bh) kfree(g_bh);
        if (g_bp) kfree(g_bp);
        g_ent = NULL; g_bh = g_bp = NULL; g_failed = true;
        return false;
    }
    for (uint32_t i = 0; i < PC_BUCKETS; ++i) g_bh[i] = g_bp[i] = -1;
    for (uint32_t i = 0; i < PC_ENTRIES; ++i) { g_ent[i].next_h = g_free; g_free = (int32_t)i; }
    return true;
}
static uint32_t bh_of(uint64_t h) { return (uint32_t)(h >> 20) & (PC_BUCKETS - 1); }
static uint32_t bp_of(uint64_t phys) { return (uint32_t)(phys >> 12) & (PC_BUCKETS - 1); }

static void unlink_entry(int32_t idx)
{
    pc_entry_t *e = &g_ent[idx];
    for (int32_t *pp = &g_bh[bh_of(e->hash)]; *pp >= 0; pp = &g_ent[*pp].next_h)
        if (*pp == idx) { *pp = e->next_h; break; }
    for (int32_t *pp = &g_bp[bp_of(e->phys)]; *pp >= 0; pp = &g_ent[*pp].next_p)
        if (*pp == idx) { *pp = e->next_p; break; }
    e->next_h = g_free; g_free = idx;
}

bool cos_pcache_share(uint64_t virt, uint64_t flags)
{
    virt &= ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t irq = sync_irq_save();
    uint64_t phys = paging_virt_to_phys(virt) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!phys || (flags & PAGE_RW) || !init_tables()) { sync_irq_restore(irq); return false; }
    const uint64_t *mine = (const uint64_t *)(uintptr_t)PHYS_TO_VIRT(phys);
    uint64_t h = page_hash(mine);

    for (int32_t i = g_bh[bh_of(h)]; i >= 0; i = g_ent[i].next_h) {
        pc_entry_t *e = &g_ent[i];
        if (e->hash != h) continue;
        if (e->phys == phys) { sync_irq_restore(irq); return true; }              /* already the shared frame */
        if (memcmp((const void *)(uintptr_t)PHYS_TO_VIRT(e->phys), mine, PAGE_SIZE) != 0) continue;
        if (!paging_remap_page(virt, e->phys, flags)) { sync_irq_restore(irq); return false; }
        e->refs++; g_maps++;
        paging_free_physical(phys);                                              /* the private copy is no longer needed */
        sync_irq_restore(irq);
        return true;
    }
    if (g_free < 0) { sync_irq_restore(irq); return false; }
    int32_t idx = g_free; g_free = g_ent[idx].next_h;
    pc_entry_t *e = &g_ent[idx];
    e->phys = phys; e->hash = h; e->refs = 1;
    e->next_h = g_bh[bh_of(h)]; g_bh[bh_of(h)] = idx;
    e->next_p = g_bp[bp_of(phys)]; g_bp[bp_of(phys)] = idx;
    g_frames++; g_maps++;
    sync_irq_restore(irq);
    return true;
}

bool cos_pcache_release(uint64_t phys)
{
    phys &= ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t irq = sync_irq_save();
    if (!g_ent) { sync_irq_restore(irq); return false; }
    for (int32_t i = g_bp[bp_of(phys)]; i >= 0; i = g_ent[i].next_p) {
        if (g_ent[i].phys != phys) continue;
        if (g_maps) g_maps--;
        if (--g_ent[i].refs == 0) {
            unlink_entry(i);
            if (g_frames) g_frames--;
            paging_free_physical(phys);
        }
        sync_irq_restore(irq);
        return true;
    }
    sync_irq_restore(irq);
    return false;
}

void cos_pcache_stats(uint32_t *frames, uint32_t *mappings)
{
    if (frames) *frames = g_frames;
    if (mappings) *mappings = g_maps;
}

/* Forgets everything without freeing frames: for the host simulator, which resets its whole memory between test cases. */
void cos_pcache_reset(void)
{
    uint64_t irq = sync_irq_save();
    if (g_ent) {
        for (uint32_t i = 0; i < PC_BUCKETS; ++i) g_bh[i] = g_bp[i] = -1;
        g_free = -1;
        for (uint32_t i = 0; i < PC_ENTRIES; ++i) { g_ent[i].next_h = g_free; g_free = (int32_t)i; }
    }
    g_frames = g_maps = 0;
    sync_irq_restore(irq);
}

/* debugging: is this frame one the cache shares between processes? */
bool cos_pcache_is_shared(uint64_t phys)
{
    phys &= ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t irq = sync_irq_save(); bool r = false;
    if (g_ent) for (int32_t i = g_bp[bp_of(phys)]; i >= 0; i = g_ent[i].next_p) if (g_ent[i].phys == phys) { r = true; break; }
    sync_irq_restore(irq); return r;
}
