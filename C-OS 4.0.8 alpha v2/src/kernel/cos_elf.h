/**
 * cos_elf.h - ELF64 program/shared-object loader for C-OS.
 *
 * WHAT CHANGED AND WHY
 * --------------------
 * The previous loader accepted exactly one shape of file: a statically
 * linked, non-PIE ET_EXEC built by hand with `ld -T cos.ld`. That is
 * enough to run programs written FOR this OS and nothing else. Every
 * mainstream x86-64 toolchain today emits ET_DYN position-independent
 * executables by default, uses PT_TLS for `__thread`/errno, emits
 * DT_GNU_HASH rather than DT_HASH, increasingly emits DT_RELR instead of
 * thousands of R_X86_64_RELATIVE entries, marks part of the image
 * PT_GNU_RELRO, and expects an auxiliary vector on the initial stack.
 * A loader that handles none of those cannot run an externally-built
 * program at all - not because the program is unreasonable, but because
 * the loader only implements a small, dated subset of the format.
 *
 * This loader handles the modern format:
 *
 *   - ET_EXEC (fixed address) and ET_DYN (PIE executables and shared
 *     objects), with a load bias chosen by the caller
 *   - PT_LOAD, PT_DYNAMIC, PT_TLS, PT_INTERP, PT_GNU_STACK,
 *     PT_GNU_RELRO, PT_PHDR, PT_NOTE
 *   - .rela.dyn (DT_RELA), .rela.plt (DT_JMPREL), Elf64_Rel tables
 *     (DT_REL/DT_JMPREL with DT_PLTREL=DT_REL) and DT_RELR
 *   - relocation types: NONE, 64, PC32, PLT32, COPY, GLOB_DAT,
 *     JUMP_SLOT, RELATIVE, GOTPCREL, 32, 32S, 16, PC16, 8, PC8,
 *     DTPMOD64, DTPOFF64, TPOFF64, IRELATIVE, PC64, SIZE32, SIZE64
 *   - DT_GNU_HASH and DT_HASH symbol lookup (both give an exact symbol
 *     count; the old loader had neither and guessed by walking until a
 *     name fell outside DT_STRSZ)
 *   - weak undefined symbols resolving to 0 instead of failing the load
 *   - symbol versioning (DT_VERSYM/DT_VERDEF/DT_VERNEED) carried through
 *     to the resolver so a versioned lookup is not silently wrong
 *   - DT_INIT/DT_INIT_ARRAY/DT_PREINIT_ARRAY and
 *     DT_FINI/DT_FINI_ARRAY, reported to ring3 rather than called from
 *     ring0 (see the note on constructor safety below)
 *   - DT_SONAME/DT_RPATH/DT_RUNPATH, so a library search path is
 *     possible at all
 *   - a full System V initial stack: argv, envp AND the auxiliary vector
 *
 * SECURITY POSTURE (unchanged in intent, widened in coverage)
 * ----------------------------------------------------------
 * Every byte parsed here is untrusted. The rules from the original
 * loader still hold and now apply to considerably more fields:
 *
 *   - all offsets are bounds-checked against the real file length, with
 *     the arithmetic itself checked for overflow
 *   - p_filesz > p_memsz is rejected
 *   - every destination address must lie wholly in the user half
 *   - counts and sizes are capped so a crafted file cannot make the
 *     kernel spin or exhaust memory
 *   - fresh pages are zeroed before use, so .bss and partial pages can
 *     never leak a previous owner's data to ring3
 *   - W^X is honoured per segment, and (where the CPU and kernel support
 *     it) enforced with the hardware NX bit rather than only by omitting
 *     PAGE_RW
 *
 * CONSTRUCTOR SAFETY
 * ------------------
 * DT_INIT / DT_INIT_ARRAY / DT_PREINIT_ARRAY entries and the resolver
 * functions named by R_X86_64_IRELATIVE relocations are all RING3 code.
 * The kernel never calls them. Doing so would execute user-chosen code
 * at ring0 - which is exactly the privilege-escalation bug this tree
 * already found and fixed once in its signal delivery path. They are
 * published to the process instead, through the link map (see
 * cos_elf_linkmap_t), and the program's own startup code runs them.
 */
#ifndef COS_ELF_H
#define COS_ELF_H

#include <stdint.h>
#include <stdbool.h>
#include "mm/paging.h"

/* ------------------------------------------------------------------ */
/* Policy caps                                                         */
/* ------------------------------------------------------------------ */
/* These are policy, not format limits. They bound how much work a
 * hostile file can make the kernel do. They are far larger than the old
 * loader's because the old values (32 phdrs, 16 MiB/segment, 8
 * dependencies) are below what ordinary toolchain output needs. */
#define COS_ELF_MAX_PHNUM        128u
#define COS_ELF_MAX_SEGMENT      (256u * 1024u * 1024u)   /* 256 MiB / segment */
#define COS_ELF_MAX_TOTAL        (512u * 1024u * 1024u)   /* 512 MiB / image   */
#define COS_ELF_MAX_DYN_ENTRIES  1024u
#define COS_ELF_MAX_RELOCS       (1u << 21)               /* 2 M entries       */
#define COS_ELF_MAX_SYMBOLS      (1u << 18)               /* 256 K symbols     */
#define COS_ELF_MAX_SYMNAME      255u
#define COS_ELF_MAX_NEEDED       32u
#define COS_ELF_MAX_PATH         256u

/* Deferred ring3 work published per object. IRELATIVE resolvers are user
 * code, so the count is capped at what the link map page can hold. */
#define COS_ELF_MAX_IFUNC        192u

/* PT_LOAD segments recorded per object so the final page permissions can
 * be applied AFTER relocation without re-parsing the file. Real objects
 * have 2-5; the cap is generous and rejects rather than truncating. */
#define COS_ELF_MAX_LOAD         16u

/* User half only: everything at or above this is kernel territory. */
#define COS_ELF_USER_LIMIT       0x0000800000000000ULL

/* Default load bias for a PIE that the caller does not place itself.
 * Inside PML4[1], the per-process private region already used for
 * program images - NOT PML4[0], which is the shared kernel identity map
 * (see paging_create_directory()). */
#define COS_ELF_PIE_BASE         0x0000008000000000ULL

/* ------------------------------------------------------------------ */
/* Object flags                                                        */
/* ------------------------------------------------------------------ */
#define COS_ELF_F_EXEC        0x00000001u  /* ET_EXEC, fixed load address     */
#define COS_ELF_F_PIE         0x00000002u  /* ET_DYN that is an executable    */
#define COS_ELF_F_LIB         0x00000004u  /* ET_DYN loaded as a library      */
#define COS_ELF_F_DYNAMIC     0x00000008u  /* has PT_DYNAMIC                  */
#define COS_ELF_F_INTERP      0x00000010u  /* has PT_INTERP (wants an ld.so)  */
#define COS_ELF_F_EXEC_STACK  0x00000020u  /* PT_GNU_STACK asked for PF_X     */
#define COS_ELF_F_HAS_TLS     0x00000040u  /* has a PT_TLS segment            */
#define COS_ELF_F_BIND_NOW    0x00000080u  /* DF_BIND_NOW / DF_1_NOW          */
#define COS_ELF_F_TEXTREL     0x00000100u  /* DT_TEXTREL: relocations in text */
#define COS_ELF_F_RELRO       0x00000200u  /* has PT_GNU_RELRO                */
#define COS_ELF_F_STATIC      0x00000400u  /* no PT_DYNAMIC at all            */
#define COS_ELF_F_NODELETE    0x00000800u  /* DF_1_NODELETE                   */
#define COS_ELF_F_HAS_IFUNC   0x00001000u  /* produced IRELATIVE work         */

/* ------------------------------------------------------------------ */
/* Symbol request passed to a resolver                                 */
/* ------------------------------------------------------------------ */
#define COS_ELF_SYM_WEAK   0x0001u   /* STB_WEAK: may resolve to 0          */
#define COS_ELF_SYM_FUNC   0x0002u   /* STT_FUNC                            */
#define COS_ELF_SYM_OBJECT 0x0004u   /* STT_OBJECT                          */
#define COS_ELF_SYM_TLS    0x0008u   /* STT_TLS: wants module id + offset   */
#define COS_ELF_SYM_IFUNC  0x0010u   /* STT_GNU_IFUNC                       */
#define COS_ELF_SYM_COPY   0x0020u   /* R_X86_64_COPY: skip the requester   */

struct cos_elf_object;

/* What a resolver is asked for, and what it may answer with. Passing a
 * struct rather than a bare name is what makes correct TLS and correct
 * weak/versioned handling possible at all: a plain `name -> address`
 * callback cannot express "this is a TLS symbol, I need its module id
 * and its offset within that module", nor "this one is weak, so
 * returning nothing is a valid answer". */
typedef struct {
    const char *name;
    const char *version;      /* from DT_VERNEED, or NULL if unversioned */
    uint32_t    flags;        /* COS_ELF_SYM_* */
    const struct cos_elf_object *requester;
} cos_elf_symreq_t;

typedef struct {
    uint64_t address;         /* resolved runtime address (symbol value)   */
    uint64_t size;            /* st_size, needed by R_X86_64_COPY/SIZE*    */
    int32_t  tls_modid;       /* TLS module id, for DTPMOD64               */
    int64_t  tls_offset;      /* offset within that module, for DTPOFF64   */
    int64_t  tls_tp_offset;   /* offset from the thread pointer, TPOFF64   */
    bool     is_tls;
} cos_elf_symval_t;

/* Returns true if the symbol was found. Returning false for a NON-weak
 * symbol fails the whole load: an undefined symbol is a load-time error,
 * reported up front rather than left as a stale pointer that faults
 * later, far from its cause. Weak symbols are handled by the loader
 * itself - a resolver that returns false for a COS_ELF_SYM_WEAK request
 * results in address 0, which is what a weak undefined symbol means. */
typedef bool (*cos_elf_symbol_resolver_t)(void *ctx, const cos_elf_symreq_t *req,
                                          cos_elf_symval_t *out);

/* ------------------------------------------------------------------ */
/* A loaded object                                                     */
/* ------------------------------------------------------------------ */
/* One structure for executables and libraries alike. The old loader had
 * two half-overlapping ones (an entry point for executables, a symbol
 * table for libraries), which is precisely why a dynamic executable -
 * an object that needs both - could not be represented, let alone run. */
typedef struct cos_elf_object {
    uint64_t base;            /* load bias; 0 for ET_EXEC                   */
    uint64_t entry;           /* runtime entry point; 0 for a pure library  */
    uint64_t map_start;       /* lowest mapped page                         */
    uint64_t map_end;         /* one past the highest mapped byte           */

    /* Runtime address of the program header table, for AT_PHDR. A program
     * that wants to find its own PT_DYNAMIC, PT_TLS or build-id at
     * runtime (every real libc does) reads it from here. */
    uint64_t phdr;
    uint64_t phentsize;
    uint64_t phnum;

    uint64_t dynamic;         /* runtime address of PT_DYNAMIC              */

    /* Dynamic tables, as runtime addresses (bias already applied). */
    uint64_t strtab, strsz;
    uint64_t symtab, syment;
    uint64_t hash;            /* DT_HASH                                    */
    uint64_t gnu_hash;        /* DT_GNU_HASH                                */
    uint64_t versym;          /* DT_VERSYM                                  */
    uint64_t verdef;  uint32_t verdefnum;
    uint64_t verneed; uint32_t verneednum;
    uint64_t pltgot;          /* DT_PLTGOT                                  */

    /* Constructors / destructors. Ring3 addresses - see the header note. */
    uint64_t init, fini;
    uint64_t init_array,    init_array_sz;
    uint64_t fini_array,    fini_array_sz;
    uint64_t preinit_array, preinit_array_sz;

    /* String-table OFFSETS, not pointers: DT_STRTAB is not guaranteed to
     * appear before the tags that reference it. */
    uint32_t soname_off, rpath_off, runpath_off;
    uint32_t needed[COS_ELF_MAX_NEEDED];
    uint32_t needed_count;

    /* PT_GNU_RELRO: made read-only by cos_elf_protect_relro() once every
     * relocation that writes into it has been applied. */
    uint64_t relro_start, relro_size;

    /* PT_TLS. tls_modid/tls_offset are assigned by whoever owns the
     * process-wide TLS layout, not by the loader, which has no view of
     * the other objects sharing the address space. */
    uint64_t tls_vaddr;       /* runtime address of the initialisation image */
    uint64_t tls_filesz, tls_memsz, tls_align;
    int32_t  tls_modid;
    int64_t  tls_offset;      /* offset of this module within the static TLS
                               * block, as a POSITIVE distance BELOW the
                               * thread pointer (x86-64 variant II)          */

    /* Exact symbol count, from DT_GNU_HASH or DT_HASH. 0 means neither
     * was present and lookups must fall back to a bounded walk. */
    uint32_t nsyms;

    uint32_t flags;           /* COS_ELF_F_*                                */

    /* File offset/length of PT_INTERP, so a caller can read the
     * interpreter's path out of the image without re-parsing it. */
    uint64_t interp_off, interp_len;

    /* Deferred ring3 work: R_X86_64_IRELATIVE targets and their resolver
     * functions. Applied by userland startup, never by the kernel. */
    uint64_t ifunc_target[COS_ELF_MAX_IFUNC];
    uint64_t ifunc_resolver[COS_ELF_MAX_IFUNC];
    uint32_t ifunc_count;

    /* PT_LOAD segments, in file order, with the bias already applied.
     * Recorded at map time so the final W^X/NX permissions can be applied
     * after relocation without the caller having to hold on to the file
     * image. */
    struct {
        uint64_t start, end;   /* page-aligned outer bounds */
        uint32_t pflags;       /* PF_R / PF_W / PF_X        */
    } segs[COS_ELF_MAX_LOAD];
    uint32_t seg_count;

    /* ---- internal: relocation tables, filled in by cos_elf_map() ----
     * Kept in the object rather than returned separately so relocation is
     * a single call that needs nothing but the object. Underscore-
     * prefixed because no caller outside this loader should read them. */
    uint64_t _rela,   _relasz,   _relaent;
    uint64_t _rel,    _relsz,    _relent;
    uint64_t _relr,   _relrsz,   _relrent;
    uint64_t _jmprel, _pltrelsz, _pltrel;
} cos_elf_object_t;

/* Backwards-compatible alias, so callers written against the old
 * two-type API keep compiling. The fields they used are all still
 * present, except `end` and `sym_ent`, which are spelled `map_end` and
 * `syment` now. */
typedef cos_elf_object_t cos_elf_library_t;

/* ------------------------------------------------------------------ */
/* Phase 1: inspect a file WITHOUT mapping anything                    */
/* ------------------------------------------------------------------ */
/* A dynamic linker must know an object's dependencies, its SONAME, its
 * search paths, how much address space it needs and whether it has TLS
 * BEFORE it decides where to put it - and its dependencies have to be
 * loaded before its relocations can resolve. All of that has to come
 * from the file image, because at this point nothing is mapped. */
typedef struct {
    uint16_t e_type;                  /* ET_EXEC / ET_DYN / ...              */
    uint64_t entry;                   /* unbiased entry point                */
    uint64_t span;                    /* total virtual span of all PT_LOADs  */
    uint64_t min_vaddr;               /* lowest PT_LOAD p_vaddr (page-down)  */
    uint64_t align;                   /* strictest PT_LOAD alignment         */
    uint32_t flags;                   /* COS_ELF_F_*                         */
    uint64_t tls_memsz, tls_align;
    char     soname[COS_ELF_MAX_PATH];
    char     rpath[COS_ELF_MAX_PATH];
    char     runpath[COS_ELF_MAX_PATH];
    char     interp[COS_ELF_MAX_PATH];
    char     needed[COS_ELF_MAX_NEEDED][COS_ELF_MAX_PATH];
    uint32_t needed_count;
} cos_elf_info_t;

/* Returns false if the file is not something this loader could ever
 * load. Never maps, never allocates, never touches `dir`. */
bool cos_elf_inspect(const uint8_t *image, uint64_t image_len,
                     cos_elf_info_t *out);

/* Convenience predicate for the shell/file manager: "is this a program
 * this OS can run?" - answered from the header alone, which is the whole
 * reason the .c-os extension existed in the first place. */
bool cos_elf_is_runnable(const uint8_t *image, uint64_t image_len);

/* ------------------------------------------------------------------ */
/* Phase 2: map the image                                              */
/* ------------------------------------------------------------------ */
/* Maps every PT_LOAD into `dir` at `bias` and fills in `out` with
 * everything parsed from PT_DYNAMIC. No relocations are applied yet -
 * dependencies may not be loaded, so symbols may not be resolvable.
 *
 * `bias` must be 0 for ET_EXEC. For ET_DYN it must be page-aligned and
 * non-zero (address 0 must stay unmapped so a NULL dereference faults).
 *
 * `dir` must be the ACTIVE page directory: writes go through the
 * kernel's physical window, but paging_virt_to_phys() resolves against
 * the active tables.
 *
 * On failure the caller must discard the address space: segments may
 * have been partially mapped. */
bool cos_elf_map(page_directory_t *dir, const uint8_t *image,
                 uint64_t image_len, uint64_t bias, cos_elf_object_t *out);

/* ------------------------------------------------------------------ */
/* Phase 3: relocate                                                   */
/* ------------------------------------------------------------------ */
/* Applies every relocation table the object has: DT_RELA, DT_REL,
 * DT_JMPREL (either flavour, per DT_PLTREL) and DT_RELR.
 *
 * `resolver` may be NULL only for an object that is genuinely
 * self-contained; any relocation that names a symbol then fails the load
 * rather than silently writing nothing.
 *
 * Relocations are applied EAGERLY (the equivalent of -z now). Lazy PLT
 * binding would need a ring3 resolver trampoline and a writable GOT for
 * the life of the process; binding now is both simpler and strictly
 * safer, and it is what lets PT_GNU_RELRO actually be made read-only
 * afterwards. Stated rather than implied: a program built expecting lazy
 * binding still works, it just pays its resolution cost up front. */
bool cos_elf_relocate(page_directory_t *dir, cos_elf_object_t *obj,
                      cos_elf_symbol_resolver_t resolver, void *resolver_ctx);

/* Phase 4: apply the object's FINAL page permissions.
 *
 * cos_elf_map() deliberately maps everything writable, because
 * relocations have to be written into pages the ELF marks read-only -
 * including into .text for an object with DT_TEXTREL. This is the call
 * that undoes that: every page gets the union of the PF_* flags of the
 * PT_LOAD segments covering it, NX where the CPU allows it, and the
 * PT_GNU_RELRO region is dropped to read-only on top.
 *
 * Must be called after cos_elf_relocate() and before any ring3
 * instruction runs in this address space. Both halves matter: relocating
 * after this would fail on a read-only GOT, and running before it would
 * hand the program a writable, executable image.
 *
 * RELRO specifically is what turns a stray pointer overwrite into a
 * fault instead of a hijacked call, and it is only possible at all
 * because relocations are applied eagerly - a lazily-bound GOT has to
 * stay writable for the life of the process. */
bool cos_elf_protect(page_directory_t *dir, const cos_elf_object_t *obj);

/* Original name, kept so existing callers still compile. */
bool cos_elf_protect_relro(page_directory_t *dir, const cos_elf_object_t *obj);

/* ------------------------------------------------------------------ */
/* Symbol lookup                                                       */
/* ------------------------------------------------------------------ */
/* Resolves an exported symbol. Uses DT_GNU_HASH when present (a
 * Bloom-filter reject then a short bucket chain), then DT_HASH, then a
 * bounded linear walk, in that order - so a stripped-down object with no
 * hash table still works, and a normal one is not scanned linearly
 * through hundreds of thousands of entries.
 *
 * `version` may be NULL to accept any version. Undefined symbols
 * (st_shndx == SHN_UNDEF) are never returned: an object that merely
 * imports a name does not export it. */
bool cos_elf_lookup(page_directory_t *dir, const cos_elf_object_t *obj,
                    const char *name, const char *version,
                    cos_elf_symval_t *out);

/* Name-only convenience wrapper, matching the old API shape. */
bool cos_elf_library_symbol(page_directory_t *dir, const cos_elf_object_t *lib,
                            const char *name, uint64_t *out_addr);

/* Reads a NUL-terminated string out of the object's DT_STRTAB. */
bool cos_elf_string(page_directory_t *dir, const cos_elf_object_t *obj,
                    uint32_t offset, char *out, uint64_t out_size);

/* ------------------------------------------------------------------ */
/* Thread-local storage                                                */
/* ------------------------------------------------------------------ */
/* x86-64 uses TLS variant II: the thread pointer (%fs base) points at
 * the END of the static TLS block, and each module's data sits at a
 * NEGATIVE offset from it. The TCB starts at the thread pointer, and
 * *(void**)tp must be tp itself - a self-pointer every real libc relies
 * on to load the thread pointer with a single `mov %fs:0, %rax`. */
typedef struct {
    uint64_t block_addr;   /* lowest address of the static TLS block        */
    uint64_t block_size;   /* size of the block, excluding the TCB          */
    uint64_t tp;           /* thread pointer: block_addr + block_size       */
    uint64_t tcb_size;     /* bytes reserved at/after tp for the TCB + DTV  */
    uint64_t dtv_addr;     /* dynamic thread vector, published at tp + 8    */
    uint32_t modules;      /* number of modules in the DTV                  */
} cos_elf_tls_t;

/* Computes the static TLS layout for `objs` and assigns each object's
 * tls_modid / tls_offset. Purely arithmetic - maps nothing. Module ids
 * start at 1, matching the ABI (id 0 is never a module). */
bool cos_elf_tls_layout(cos_elf_object_t **objs, uint32_t count,
                        uint64_t *out_block_size, uint64_t *out_align);

/* Builds a thread's TLS block at `region` (which must already be mapped
 * writable and at least block_size + TCB bytes long), copying each
 * module's initialisation image in and zeroing the rest, then writing
 * the TCB self-pointer and DTV. Reports the thread pointer to load into
 * the %fs base. */
bool cos_elf_tls_init(page_directory_t *dir, uint64_t region, uint64_t region_size,
                      cos_elf_object_t **objs, uint32_t count,
                      cos_elf_tls_t *out);

/* Bytes cos_elf_tls_init() needs at `region` for a given block size:
 * the block itself plus the TCB and DTV that sit above the thread
 * pointer. Callers size their allocation with this rather than
 * guessing. */
uint64_t cos_elf_tls_region_size(uint64_t block_size, uint32_t modules);

/* ------------------------------------------------------------------ */
/* The link map published to ring3                                     */
/* ------------------------------------------------------------------ */
/* Everything the kernel resolved that the process's own startup code
 * needs in order to finish the job: where each object landed, its
 * constructors and destructors, and the IRELATIVE relocations only ring3
 * may apply.
 *
 * Written into read-only pages in the process's address space and
 * advertised through a private auxv tag (COS_AT_COS_LINKMAP). A process
 * that ignores it still runs - it just does not get constructors or
 * ifuncs, which is exactly the behaviour of the old loader. */
#define COS_ELF_LINKMAP_MAGIC   0x434F534C4D415031ULL   /* "COSLMAP1" */
#define COS_ELF_LINKMAP_VERSION 1u
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
    /* The module's PT_TLS initialisation image, at its RUNTIME address.
     * Published because a thread the program creates itself needs its
     * own TLS block, and building one means copying each module's
     * initial values - which cannot be recovered from the main thread's
     * block, since that one has been running and mutating them. */
    uint64_t tls_image;
    uint64_t tls_filesz;
    uint64_t tls_memsz;
    uint64_t tls_align;
    uint32_t flags;
    uint32_t _pad;
    char     name[64];
} cos_elf_linkmap_obj_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t obj_count;
    uint64_t tls_tp;          /* thread pointer already installed, or 0 */
    uint64_t tls_block_size;
    uint32_t ifunc_count;
    uint32_t _pad;
    /* IRELATIVE work: for each pair, ring3 must do
     *   *(uint64_t*)target = ((uint64_t(*)(void))resolver)();
     * before any constructor runs. */
    uint64_t ifunc_target[COS_ELF_MAX_IFUNC];
    uint64_t ifunc_resolver[COS_ELF_MAX_IFUNC];
    cos_elf_linkmap_obj_t obj[COS_ELF_LINKMAP_MAX_OBJ];
} cos_elf_linkmap_t;

/* ------------------------------------------------------------------ */
/* Initial process stack                                               */
/* ------------------------------------------------------------------ */
/* Auxiliary vector tags: the standard ones a libc actually reads, plus
 * two private tags for C-OS. Private tags use a high number the ABI
 * leaves free, so a stock libc walking the vector skips them rather than
 * misinterpreting them. */
#define COS_AT_NULL     0
#define COS_AT_IGNORE   1
#define COS_AT_EXECFD   2
#define COS_AT_PHDR     3
#define COS_AT_PHENT    4
#define COS_AT_PHNUM    5
#define COS_AT_PAGESZ   6
#define COS_AT_BASE     7
#define COS_AT_FLAGS    8
#define COS_AT_ENTRY    9
#define COS_AT_NOTELF   10
#define COS_AT_UID      11
#define COS_AT_EUID     12
#define COS_AT_GID      13
#define COS_AT_EGID     14
#define COS_AT_PLATFORM 15
#define COS_AT_HWCAP    16
#define COS_AT_CLKTCK   17
#define COS_AT_SECURE   23
#define COS_AT_RANDOM   25
#define COS_AT_HWCAP2   26
#define COS_AT_EXECFN   31
#define COS_AT_MINSIGSTKSZ  51
/* Private to C-OS. */
#define COS_AT_COS_LINKMAP  0x434F5300u   /* 'C','O','S',0 */
#define COS_AT_COS_TP       0x434F5301u   /* thread pointer for the %fs base */

#define COS_ELF_MAX_ARGC        256
#define COS_ELF_MAX_ENVC        256
#define COS_ELF_MAX_ARG_LEN     1023u
/* Strings, pointer arrays and auxv all live in the top of the stack
 * region. 64 KiB rather than the old 16 KiB, because argv+envp+auxv for
 * a real program does not fit in 16 KiB once environment variables
 * exist. */
#define COS_ELF_STACK_ARG_BUDGET (64u * 1024u)

typedef struct {
    const char *const *argv;
    int argc;
    const char *const *envp;      /* may be NULL */
    int envc;
    /* Values for the auxiliary vector. Zero means "omit this tag",
     * except pagesz/clktck, which get sane defaults. */
    uint64_t at_phdr, at_phent, at_phnum;
    uint64_t at_base;             /* interpreter base, or the PIE bias   */
    uint64_t at_entry;            /* the program's real entry point      */
    uint64_t at_linkmap;          /* address of the cos_elf_linkmap_t    */
    uint64_t at_tp;               /* thread pointer, if TLS was set up   */
    uint64_t at_random_seed[2];   /* 16 bytes of AT_RANDOM               */
    const char *execfn;           /* AT_EXECFN, usually argv[0]'s path   */
    bool     secure;              /* AT_SECURE                           */
} cos_elf_stack_params_t;

/* Builds the System V x86-64 initial stack inside `dir`'s already-mapped
 * user stack region and reports the RSP the new thread must start with:
 *
 *      RSP -> [ argc ][ argv[0..argc-1] ][ NULL ]
 *             [ envp[0..envc-1] ][ NULL ]
 *             [ auxv: (tag, value) pairs ... ][ AT_NULL, 0 ]
 *             ... string data, AT_RANDOM bytes, platform string ...
 *
 * The previous version wrote argc, argv and two NULLs and stopped. That
 * is enough for a hand-written _start that only reads argv, and not
 * enough for any real C runtime: without AT_PHDR a program cannot find
 * its own PT_TLS or PT_DYNAMIC, and without AT_RANDOM a libc's stack
 * guard initialisation reads whatever happened to be there.
 *
 * Returns false if the arguments do not fit or the stack is not mapped;
 * the caller must then discard the process rather than start a thread on
 * a malformed stack. */
bool cos_elf_setup_stack_ex(page_directory_t *dir, uint64_t stack_top,
                            const cos_elf_stack_params_t *params,
                            uint64_t *out_rsp);

/* Compatibility wrapper for the original argv-only signature. */
bool cos_elf_setup_stack(page_directory_t *dir, uint64_t stack_top,
                         const char *const *argv, int argc,
                         uint64_t *out_rsp);

/* ------------------------------------------------------------------ */
/* One-call loading, for callers that do not need the phases            */
/* ------------------------------------------------------------------ */
/* Loads a SELF-CONTAINED executable (ET_EXEC, or an ET_DYN with no
 * unresolved external symbols) and reports its runtime entry point.
 * `bias` is ignored for ET_EXEC and used as the load address for ET_DYN;
 * pass 0 to take COS_ELF_PIE_BASE.
 *
 * This is the direct replacement for the original cos_elf_load(): same
 * shape, but it now also accepts position-independent executables, which
 * is what an externally-built program almost always is. */
bool cos_elf_load_program(page_directory_t *dir, const uint8_t *image,
                          uint64_t image_len, uint64_t bias,
                          cos_elf_object_t *out);

/* Original signature, preserved so existing callers keep working. */
bool cos_elf_load(page_directory_t *dir, const uint8_t *image,
                  uint64_t image_len, uint64_t *out_entry);

/* Original library entry point, preserved. Prefer cos_elf_map() +
 * cos_elf_relocate() when dependencies are involved, because those let
 * the caller load dependencies in between - which is the only order in
 * which cross-object symbols can resolve. */
bool cos_elf_load_library(page_directory_t *dir, const uint8_t *image,
                          uint64_t image_len, uint64_t base,
                          cos_elf_symbol_resolver_t resolver, void *resolver_ctx,
                          cos_elf_object_t *out);

/* Lists an object's DT_NEEDED dependency names, read from the FILE
 * image (nothing is mapped yet - that is the point: dependencies must be
 * loaded before the dependent object can be relocated). */
int cos_elf_read_needed(page_directory_t *dir, const uint8_t *image,
                        uint64_t image_len, char out_names[][256], int max_names);

/* ------------------------------------------------------------------ */
/* Hardware NX                                                         */
/* ------------------------------------------------------------------ */
/* W^X in the old loader meant "a non-writable segment is mapped without
 * PAGE_RW". That stops a program rewriting its own code, but it does NOT
 * stop it EXECUTING its own data - the x86-64 page tables only forbid
 * that through the NX bit, which requires EFER.NXE to be enabled first.
 * Setting bit 63 of a PTE without EFER.NXE is a reserved-bit violation
 * and faults immediately, so this must be explicitly enabled (and on
 * EVERY CPU, since EFER is per-CPU) before the loader will emit it.
 *
 * cos_elf_nx_enable() checks CPUID.80000001h:EDX[20], sets EFER.NXE on
 * the calling CPU, and returns whether NX is now usable. The loader only
 * emits NX bits once cos_elf_nx_available() is true, so a kernel that
 * never calls this behaves exactly as it did before. */
bool cos_elf_nx_enable(void);
bool cos_elf_nx_available(void);

#endif /* COS_ELF_H */
