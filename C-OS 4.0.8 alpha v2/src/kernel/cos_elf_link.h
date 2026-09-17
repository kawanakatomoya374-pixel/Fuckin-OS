/**
 * cos_elf_link.h - the in-kernel dynamic linker for C-OS.
 *
 * WHAT THIS IS FOR
 * ----------------
 * cos_elf.c can map and relocate ONE object. Running an externally-built
 * program needs rather more than that: its dependencies have to be found
 * on disk, loaded in an order where every symbol can resolve, given
 * non-overlapping load addresses, share one process-wide TLS layout, and
 * be described back to the program so its own startup code can run
 * constructors. That orchestration is this file.
 *
 * WHAT IT REPLACES
 * ----------------
 * The previous implementation lived inside userspace_demo.c as a
 * four-entry array:
 *
 *     static cos_lib_slot_t g_lib_slots[COS_LIB_MAX_PER_PROC];
 *
 * The name says "per proc" and the storage is a single GLOBAL array.
 * Every process in the system shared those four slots, so two programs
 * each dlopen()ing two libraries exhausted the table, and a library
 * loaded by one process occupied a slot that another process's lookup
 * then walked past (the ownership check made the result safe, not
 * correct - the capacity was still being consumed by an unrelated
 * process). Library bases were also assigned by SLOT INDEX times a fixed
 * 256 MiB stride, so slot reuse across processes was fine but a library
 * larger than the stride would silently overlap the next one.
 *
 * Here each process gets its own namespace, allocated on demand, and
 * load addresses come from a bump allocator sized by each object's
 * actual span.
 *
 * THE SEARCH SCOPE
 * ----------------
 * Symbol resolution uses the standard global scope: the executable
 * first, then every library in breadth-first load order. That ordering
 * is what gives interposition its meaning - a symbol defined by the
 * executable wins over the same name in a library, which is how a
 * program overrides a library function. The old resolver searched only
 * "other libraries this process dlopen'd", in open order, and
 * deliberately excluded the executable because an ET_EXEC loaded by the
 * old loader had no parsed dynamic section to export from. A dynamic
 * executable does, so it is in scope now.
 */
#ifndef COS_ELF_LINK_H
#define COS_ELF_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include "cos_elf.h"
#include "task.h"

/* Per-process capacity. Generous compared to the old four, and bounded
 * because each object costs a kmalloc of a few KiB plus its mappings. */
#define COS_LINK_MAX_OBJECTS 24u
#define COS_LINK_MAX_PROCS   32u

/* Address-space layout, inside PML4[4] - the slot already reserved for
 * libraries (see COS_LIB_BASE in the old code and the PML4 slot map in
 * syscall.c). Carved rather than claiming a new PML4 slot, because the
 * remaining slots are spoken for by the heap, stack, image and mmap
 * regions and silently taking one would collide with them later. */
#define COS_LINK_LIB_BASE   0x0000020000000000ULL   /* bump allocator start  */
#define COS_LINK_LIB_LIMIT  0x0000027000000000ULL   /* 448 GiB of library VA */
#define COS_LINK_TLS_BASE   0x0000027800000000ULL   /* static TLS block      */
#define COS_LINK_MAP_BASE   0x0000027C00000000ULL   /* read-only link map    */

/* Libraries are spaced apart by at least this much, so a later ASLR
 * implementation has somewhere to jitter and so two adjacent objects can
 * never share a page even when one is tiny. */
#define COS_LINK_LIB_ALIGN  0x0000000000200000ULL   /* 2 MiB */

/* dlopen flags. RTLD_GLOBAL/LOCAL control whether the object joins the
 * process-wide search scope; RTLD_NOW is accepted and ignored because
 * this linker is always eager (see cos_elf_relocate). */
#define COS_RTLD_LAZY    0x0001
#define COS_RTLD_NOW     0x0002
#define COS_RTLD_GLOBAL  0x0100
#define COS_RTLD_LOCAL   0x0000
#define COS_RTLD_NODELETE 0x1000

/* Pseudo-handles for cos_link_dlsym(). */
#define COS_RTLD_DEFAULT ((int64_t)0)   /* search the whole global scope */
#define COS_RTLD_NEXT    ((int64_t)-1)  /* not implemented; see the note */

typedef struct {
    bool              used;
    bool              is_main;      /* the executable itself   */
    bool              global;       /* in the process-wide scope */
    uint32_t          refcount;     /* dlopen/dlclose balance  */
    char              path[COS_ELF_MAX_PATH];
    char              soname[COS_ELF_MAX_PATH];
    cos_elf_object_t *obj;          /* kmalloc'd: ~4 KiB each  */
} cos_link_obj_t;

typedef struct {
    bool     used;
    uint64_t pid;
    /* The process's page directory. Held here because every loader call
     * takes one and rejects NULL: the loader operates on the ACTIVE
     * tables, but it still requires the caller to name the directory it
     * believes is active, which is the check that catches a caller that
     * forgot to switch. */
    void    *dir;
    cos_link_obj_t objs[COS_LINK_MAX_OBJECTS];
    uint32_t count;
    uint64_t next_base;             /* library bump allocator cursor */
    /* Process-wide TLS, established once every object is loaded. */
    uint64_t tls_block_size;
    uint64_t tls_region;
    uint64_t tls_region_size;
    uint64_t tls_tp;
    /* The read-only link map published to ring3. */
    uint64_t linkmap_addr;
    uint64_t linkmap_size;
} cos_link_ns_t;

/* What the caller needs in order to actually start the thread. */
typedef struct {
    uint64_t entry;     /* runtime entry point                       */
    uint64_t rsp;       /* initial stack pointer                     */
    uint64_t tp;        /* thread pointer for %fs base, or 0         */
    uint64_t linkmap;   /* address of the cos_elf_linkmap_t, or 0    */
} cos_link_result_t;

/* Loads `image` as the main executable of `proc`, resolving and loading
 * every DT_NEEDED dependency, laying out TLS, publishing the link map
 * and building the initial stack. `proc` must already have its page
 * directory and user stack; this switches to that directory internally.
 *
 * On failure the caller must destroy the process: the address space may
 * be partially built. */
bool cos_link_load_executable(process_t *proc, const char *path,
                              const uint8_t *image, uint64_t image_len,
                              const char *const *argv, int argc,
                              const char *const *envp, int envc,
                              cos_link_result_t *out);

/* dlopen()/dlsym()/dlclose() for a running process. Handles are indices
 * plus one, so 0 is never a valid library handle and an uninitialised
 * variable in a user program cannot name one. */
int64_t  cos_link_dlopen(process_t *proc, const char *path, uint32_t flags);
uint64_t cos_link_dlsym(process_t *proc, int64_t handle, const char *name);
int      cos_link_dlclose(process_t *proc, int64_t handle);

/* Releases a process's whole namespace. Called from process teardown. */
void cos_link_release_pid(uint64_t pid);

/* Read-only accessor, for the task manager / debugging. */
const cos_link_ns_t *cos_link_ns_peek(uint64_t pid);

/* Sets the search path used for a bare DT_NEEDED name, as a
 * colon-separated list. Defaults to "/lib:/usr/lib:/". DT_RUNPATH and
 * DT_RPATH from the requesting object are consulted first, exactly as a
 * real linker does. */
void cos_link_set_search_path(const char *paths);

#endif /* COS_ELF_LINK_H */
