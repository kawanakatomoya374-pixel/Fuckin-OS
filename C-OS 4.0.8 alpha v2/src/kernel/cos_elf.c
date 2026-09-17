/**
 * cos_elf.c - ELF64 program/shared-object loader for C-OS.
 *
 * See cos_elf.h for the full rationale. In short: this replaces a loader
 * that could only run statically linked non-PIE ET_EXEC files built by
 * hand for this OS, with one that reads the ELF format an ordinary
 * x86-64 toolchain actually emits - PIE, TLS, GNU_HASH, RELR, RELRO,
 * versioned symbols, ifuncs and all.
 *
 * ORDER OF OPERATIONS
 * -------------------
 * Loading is split into phases because a dynamic linker cannot do them
 * in one pass:
 *
 *   inspect  - read dependencies/SONAME/search paths/size from the FILE,
 *              before anything is mapped, so the caller can decide where
 *              to put this object and what to load first
 *   map      - map every PT_LOAD at the chosen bias, parse PT_DYNAMIC.
 *              Pages are mapped WRITABLE at this stage regardless of the
 *              segment's declared permissions, because relocations must
 *              still be written into them (including into .text, for an
 *              object with DT_TEXTREL). Nothing is running in this
 *              address space yet, so there is no window in which that
 *              writability is reachable by the program.
 *   relocate - apply every relocation table, resolving external symbols
 *              through the caller's resolver
 *   protect  - apply the FINAL page permissions: W^X per segment, NX
 *              where the CPU allows it, and PT_GNU_RELRO made read-only.
 *              This is the step that makes the writability above safe:
 *              by the time any ring3 instruction executes, the image has
 *              exactly the permissions the ELF asked for.
 *
 * WHY WRITES GO THROUGH THE PHYSICAL WINDOW
 * -----------------------------------------
 * Every read/write of the target address space here resolves the virtual
 * address to a physical one and touches it through PHYS_TO_VIRT. That
 * works no matter which directory is active for *writes*, but
 * paging_virt_to_phys() resolves against the ACTIVE tables - so the
 * caller must have made the target directory active. Same contract as
 * the original loader, restated because considerably more code now
 * depends on it.
 */
#include "sync.h"

#define PHYS_TO_VIRT(phys) ((phys) + 0xFFFF800000000000ULL)
/* paging_virt_to_phys / paging_alloc_physical / paging_free_physical /
 * paging_protect_page all come from mm/paging.h, included via cos_elf.h
 * below. They used to be re-declared here by hand with `unsigned long`
 * and `int` in place of the real uint64_t/bool - compatible on this
 * target by luck rather than by contract, and silently wrong on any
 * other. */

#include "cos_elf.h"
#include "serial.h"
#include "string.h"
#include "task.h"

/* ================================================================== */
/* ELF64 format definitions                                            */
/* ================================================================== */

#define EI_NIDENT   16
#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EV_CURRENT  1

#define ET_NONE 0
#define ET_REL  1
#define ET_EXEC 2
#define ET_DYN  3
#define ET_CORE 4

#define EM_X86_64 62

#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6
#define PT_TLS          7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552
#define PT_GNU_PROPERTY 0x6474e553

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/* Dynamic tags. Anything not listed is skipped rather than rejected: a
 * linker is free to emit tags this loader has no opinion on, and
 * refusing them would reject perfectly valid objects. */
#define DT_NULL             0
#define DT_NEEDED           1
#define DT_PLTRELSZ         2
#define DT_PLTGOT           3
#define DT_HASH             4
#define DT_STRTAB           5
#define DT_SYMTAB           6
#define DT_RELA             7
#define DT_RELASZ           8
#define DT_RELAENT          9
#define DT_STRSZ            10
#define DT_SYMENT           11
#define DT_INIT             12
#define DT_FINI             13
#define DT_SONAME           14
#define DT_RPATH            15
#define DT_SYMBOLIC         16
#define DT_REL              17
#define DT_RELSZ            18
#define DT_RELENT           19
#define DT_PLTREL           20
#define DT_DEBUG            21
#define DT_TEXTREL          22
#define DT_JMPREL           23
#define DT_BIND_NOW         24
#define DT_INIT_ARRAY       25
#define DT_FINI_ARRAY       26
#define DT_INIT_ARRAYSZ     27
#define DT_FINI_ARRAYSZ     28
#define DT_RUNPATH          29
#define DT_FLAGS            30
#define DT_PREINIT_ARRAY    32
#define DT_PREINIT_ARRAYSZ  33
#define DT_RELRSZ           35
#define DT_RELR             36
#define DT_RELRENT          37
#define DT_GNU_HASH         0x6ffffef5
#define DT_VERSYM           0x6ffffff0
#define DT_RELACOUNT        0x6ffffff9
#define DT_RELCOUNT         0x6ffffffa
#define DT_FLAGS_1          0x6ffffffb
#define DT_VERDEF           0x6ffffffc
#define DT_VERDEFNUM        0x6ffffffd
#define DT_VERNEED          0x6ffffffe
#define DT_VERNEEDNUM       0x6fffffff

#define DF_SYMBOLIC   0x02
#define DF_TEXTREL    0x04
#define DF_BIND_NOW   0x08
#define DF_STATIC_TLS 0x10
#define DF_1_NOW      0x00000001
#define DF_1_NODELETE 0x00000008
#define DF_1_PIE      0x08000000

/* x86-64 relocation types. */
#define R_X86_64_NONE            0
#define R_X86_64_64              1   /* S + A, 64-bit                     */
#define R_X86_64_PC32            2   /* S + A - P, 32-bit                 */
#define R_X86_64_GOT32           3
#define R_X86_64_PLT32           4   /* L + A - P, 32-bit                 */
#define R_X86_64_COPY            5   /* copy st_size bytes from S         */
#define R_X86_64_GLOB_DAT        6   /* S                                 */
#define R_X86_64_JUMP_SLOT       7   /* S                                 */
#define R_X86_64_RELATIVE        8   /* B + A                             */
#define R_X86_64_GOTPCREL        9
#define R_X86_64_32             10   /* S + A, zero-extended 32-bit       */
#define R_X86_64_32S            11   /* S + A, sign-extended 32-bit       */
#define R_X86_64_16             12
#define R_X86_64_PC16           13
#define R_X86_64_8              14
#define R_X86_64_PC8            15
#define R_X86_64_DTPMOD64       16   /* TLS module id                     */
#define R_X86_64_DTPOFF64       17   /* offset within a TLS module        */
#define R_X86_64_TPOFF64        18   /* offset from the thread pointer    */
#define R_X86_64_TLSGD          19
#define R_X86_64_TLSLD          20
#define R_X86_64_DTPOFF32       21
#define R_X86_64_GOTTPOFF       22
#define R_X86_64_TPOFF32        23
#define R_X86_64_PC64           24
#define R_X86_64_GOTOFF64       25
#define R_X86_64_GOTPC32        26
#define R_X86_64_SIZE32         32
#define R_X86_64_SIZE64         33
#define R_X86_64_TLSDESC_CALL   35
#define R_X86_64_TLSDESC        36
#define R_X86_64_IRELATIVE      37   /* resolver-determined, ring3 only   */
#define R_X86_64_RELATIVE64     38

#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)((i) & 0xffffffffULL))

#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STB_WEAK    2
#define STB_GNU_UNIQUE 10

#define STT_NOTYPE     0
#define STT_OBJECT     1
#define STT_FUNC       2
#define STT_SECTION    3
#define STT_FILE       4
#define STT_COMMON     5
#define STT_TLS        6
#define STT_GNU_IFUNC  10

#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)

#define SHN_UNDEF  0
#define SHN_ABS    0xfff1
#define SHN_COMMON 0xfff2

#define VER_FLG_BASE 0x1
#define VERSYM_HIDDEN 0x8000
#define VER_NDX_LOCAL  0
#define VER_NDX_GLOBAL 1

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

typedef struct {
    uint64_t d_tag;
    uint64_t d_val;   /* union of d_val / d_ptr - same width, same slot */
} __attribute__((packed)) elf64_dyn_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed)) elf64_rela_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
} __attribute__((packed)) elf64_rel_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed)) elf64_sym_t;

typedef struct {
    uint16_t vd_version;
    uint16_t vd_flags;
    uint16_t vd_ndx;
    uint16_t vd_cnt;
    uint32_t vd_hash;
    uint32_t vd_aux;
    uint32_t vd_next;
} __attribute__((packed)) elf64_verdef_t;

typedef struct {
    uint32_t vda_name;
    uint32_t vda_next;
} __attribute__((packed)) elf64_verdaux_t;

typedef struct {
    uint16_t vn_version;
    uint16_t vn_cnt;
    uint32_t vn_file;
    uint32_t vn_aux;
    uint32_t vn_next;
} __attribute__((packed)) elf64_verneed_t;

typedef struct {
    uint32_t vna_hash;
    uint16_t vna_flags;
    uint16_t vna_other;
    uint32_t vna_name;
    uint32_t vna_next;
} __attribute__((packed)) elf64_vernaux_t;

/* Hardware NX bit. Only ever emitted once cos_elf_nx_enable() has
 * confirmed EFER.NXE is set - before that it is a RESERVED bit, and a
 * PTE carrying it faults on first access. */
#define COS_PAGE_NX (1ULL << 63)

/* ================================================================== */
/* Logging                                                             */
/* ================================================================== */

static void elf_log(const char *msg)
{
    serial_puts("[ELF] ");
    serial_puts(msg);
    serial_puts("\n");
}

static void elf_log2(const char *msg, const char *detail)
{
    serial_puts("[ELF] ");
    serial_puts(msg);
    serial_puts(detail ? detail : "(null)");
    serial_puts("\n");
}

static void elf_log_hex(const char *msg, uint64_t v)
{
    serial_puts("[ELF] ");
    serial_puts(msg);
    serial_puts("0x");
    serial_puthex(v);
    serial_puts("\n");
}

/* ================================================================== */
/* NX support                                                          */
/* ================================================================== */

static bool g_nx_available = false;

bool cos_elf_nx_available(void) { return g_nx_available; }

#ifdef COS_ELF_HOSTTEST
/* The host harness runs this code in user mode, where RDMSR/WRMSR are
 * privileged and CPUID leaf availability is irrelevant. NX stays off,
 * which is also the kernel's default, so the tested behaviour matches. */
bool cos_elf_nx_enable(void) { return false; }
#else
bool cos_elf_nx_enable(void)
{
    uint32_t eax, ebx, ecx, edx;

    /* Does the CPU advertise extended leaf 0x80000001 at all? */
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                             : "a"(0x80000000u));
    if (eax < 0x80000001u) {
        elf_log("NX unavailable: CPU has no extended CPUID leaf 0x80000001");
        return false;
    }

    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                             : "a"(0x80000001u));
    if (!(edx & (1u << 20))) {
        elf_log("NX unavailable: CPUID.80000001h:EDX[20] clear");
        return false;
    }

    /* EFER is per-CPU, so this must run on every CPU that will ever
     * touch a page table carrying an NX bit. The caller is responsible
     * for that; this function reports only what it did here. */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    if (!(lo & (1u << 11))) {
        lo |= (1u << 11);   /* EFER.NXE */
        __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0xC0000080u));
    }
    g_nx_available = true;
    elf_log("NX enabled: non-executable pages will be enforced by hardware");
    return true;
}
#endif

/* ================================================================== */
/* Bounds arithmetic                                                   */
/* ================================================================== */

/* Adding `add` to `base` must neither overflow nor exceed `limit`. */
static bool range_ok(uint64_t base, uint64_t add, uint64_t limit)
{
    if (base > limit) return false;
    if (base + add < base) return false;   /* overflow */
    return (base + add) <= limit;
}

static uint64_t align_up(uint64_t v, uint64_t a)
{
    if (a <= 1) return v;
    /* Only power-of-two alignments are meaningful here, and p_align is
     * required by the spec to be one; a crafted non-power-of-two would
     * otherwise turn the mask below into nonsense. */
    if (a & (a - 1)) return v;
    return (v + a - 1) & ~(a - 1);
}

static uint64_t page_down(uint64_t v) { return v & ~(uint64_t)(PAGE_SIZE - 1); }
static uint64_t page_up(uint64_t v)
{
    if (v + PAGE_SIZE - 1 < v) return v;   /* saturate rather than wrap */
    return (v + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

/* A destination range must lie wholly in the user half without wrapping. */
static bool user_range_ok(uint64_t start, uint64_t len)
{
    if (len == 0) return true;
    if (start + len < start) return false;
    return (start + len) <= COS_ELF_USER_LIMIT;
}

/* ================================================================== */
/* Target address space access                                         */
/* ================================================================== */
/*
 * The original loader copied byte by byte, resolving the virtual address
 * to a physical one with a full four-level page walk FOR EVERY BYTE. For
 * a 4 MiB image that is four million page walks, and the same cost again
 * for every relocation read. These helpers walk once per page and then
 * memcpy within it, which is the same correctness with a constant factor
 * three orders of magnitude smaller. It matters: a real program's
 * .rela.dyn can hold tens of thousands of entries.
 */

static bool user_write(uint64_t uaddr, const void *src, uint64_t len)
{
    const uint8_t *in = (const uint8_t *)src;
    while (len) {
        uint64_t page = page_down(uaddr);
        uint64_t off  = uaddr - page;
        uint64_t n    = PAGE_SIZE - off;
        if (n > len) n = len;

        uint64_t phys = paging_virt_to_phys(page);
        if (!phys) return false;
        uint8_t *dst = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
        memcpy(dst + off, in, (size_t)n);

        uaddr += n; in += n; len -= n;
    }
    return true;
}

static bool user_read(uint64_t uaddr, void *dst, uint64_t len)
{
    uint8_t *out = (uint8_t *)dst;
    while (len) {
        uint64_t page = page_down(uaddr);
        uint64_t off  = uaddr - page;
        uint64_t n    = PAGE_SIZE - off;
        if (n > len) n = len;

        uint64_t phys = paging_virt_to_phys(page);
        if (!phys) return false;
        const uint8_t *src = (const uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
        memcpy(out, src + off, (size_t)n);

        uaddr += n; out += n; len -= n;
    }
    return true;
}

static bool user_zero(uint64_t uaddr, uint64_t len)
{
    while (len) {
        uint64_t page = page_down(uaddr);
        uint64_t off  = uaddr - page;
        uint64_t n    = PAGE_SIZE - off;
        if (n > len) n = len;

        uint64_t phys = paging_virt_to_phys(page);
        if (!phys) return false;
        uint8_t *dst = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
        memset(dst + off, 0, (size_t)n);

        uaddr += n; len -= n;
    }
    return true;
}

/* Copies a NUL-terminated string OUT of the target address space.
 * Returns false if it is unterminated within `out_size` or crosses into
 * unmapped memory - a crafted string table must not be able to walk the
 * kernel off the end of a mapping. */
static bool user_strncpy_from(uint64_t uaddr, char *out, uint64_t out_size)
{
    if (out_size == 0) return false;
    for (uint64_t i = 0; i + 1 < out_size; ++i) {
        char c = 0;
        if (!user_read(uaddr + i, &c, 1)) return false;
        out[i] = c;
        if (c == '\0') return true;
    }
    out[out_size - 1] = '\0';
    return false;
}

/* ================================================================== */
/* Mapping                                                             */
/* ================================================================== */

/* Ensures [start, end) is mapped writable+user in the active directory,
 * zeroing any page it has to allocate. Pages that already exist (because
 * an adjacent segment shares the boundary page) are left alone: zeroing
 * them again would erase the neighbour's bytes. */
static bool map_range_rw(uint64_t start, uint64_t end)
{
    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        if (paging_virt_to_phys(v)) continue;   /* already mapped */

        uint64_t phys = paging_alloc_physical();
        if (!phys) {
            elf_log("failed: out of physical memory mapping segment");
            return false;
        }
        if (!paging_map_page(v, phys, PAGE_PRESENT | PAGE_USER | PAGE_RW)) {
            paging_free_physical(phys);
            elf_log_hex("failed: could not map segment page at ", v);
            return false;
        }
        /* Zero before anything is copied in: guarantees .bss is zero AND
         * that no residue of a previously-used frame reaches ring3. */
        memset((void *)(uintptr_t)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
    }
    return true;
}

/* Translates PF_* into page-table flags. */
static uint64_t pf_to_page_flags(uint32_t pf)
{
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (pf & PF_W) flags |= PAGE_RW;
    /* W^X is only genuinely enforced once NX exists: without it, "not
     * writable" stops a program rewriting its code but does nothing to
     * stop it executing its data. */
    if (g_nx_available && !(pf & PF_X)) flags |= COS_PAGE_NX;
    return flags;
}

/* ================================================================== */
/* PT_DYNAMIC parsing                                                  */
/* ================================================================== */

/* Reads the dynamic section out of the MAPPED image. Every d_ptr-style
 * tag is a link-time address, which for ET_DYN means "offset from the
 * load base" - so the bias is applied here, once, and every later user
 * of these fields gets a real runtime address. */
static bool scan_dynamic(cos_elf_object_t *obj, uint64_t dyn_addr, uint64_t dyn_size)
{
    uint64_t bias = obj->base;
    uint64_t addr = dyn_addr;
    uint64_t remaining = dyn_size;
    uint64_t pltrel = DT_RELA;        /* DT_PLTREL default, corrected below */
    uint64_t pltrelsz = 0, jmprel = 0;
    uint64_t rela = 0, relasz = 0, relaent = sizeof(elf64_rela_t);
    uint64_t rel = 0, relsz = 0, relent = sizeof(elf64_rel_t);
    uint64_t relr = 0, relrsz = 0, relrent = sizeof(uint64_t);
    uint64_t flags = 0, flags1 = 0;

    obj->syment = sizeof(elf64_sym_t);

    for (unsigned n = 0; n < COS_ELF_MAX_DYN_ENTRIES && remaining >= sizeof(elf64_dyn_t);
         ++n, addr += sizeof(elf64_dyn_t), remaining -= sizeof(elf64_dyn_t)) {
        elf64_dyn_t d;
        if (!user_read(addr, &d, sizeof(d))) {
            elf_log("rejected: PT_DYNAMIC runs outside mapped memory");
            return false;
        }
        if (d.d_tag == DT_NULL) break;

        switch (d.d_tag) {
        case DT_STRTAB:   obj->strtab = bias + d.d_val; break;
        case DT_STRSZ:    obj->strsz  = d.d_val; break;
        case DT_SYMTAB:   obj->symtab = bias + d.d_val; break;
        case DT_SYMENT:   obj->syment = d.d_val; break;
        case DT_HASH:     obj->hash     = bias + d.d_val; break;
        case DT_GNU_HASH: obj->gnu_hash = bias + d.d_val; break;
        case DT_PLTGOT:   obj->pltgot   = bias + d.d_val; break;

        case DT_RELA:     rela    = bias + d.d_val; break;
        case DT_RELASZ:   relasz  = d.d_val; break;
        case DT_RELAENT:  relaent = d.d_val; break;
        case DT_REL:      rel     = bias + d.d_val; break;
        case DT_RELSZ:    relsz   = d.d_val; break;
        case DT_RELENT:   relent  = d.d_val; break;
        case DT_RELR:     relr    = bias + d.d_val; break;
        case DT_RELRSZ:   relrsz  = d.d_val; break;
        case DT_RELRENT:  relrent = d.d_val; break;
        case DT_JMPREL:   jmprel  = bias + d.d_val; break;
        case DT_PLTRELSZ: pltrelsz = d.d_val; break;
        case DT_PLTREL:   pltrel  = d.d_val; break;

        case DT_INIT:          obj->init = bias + d.d_val; break;
        case DT_FINI:          obj->fini = bias + d.d_val; break;
        case DT_INIT_ARRAY:    obj->init_array = bias + d.d_val; break;
        case DT_INIT_ARRAYSZ:  obj->init_array_sz = d.d_val; break;
        case DT_FINI_ARRAY:    obj->fini_array = bias + d.d_val; break;
        case DT_FINI_ARRAYSZ:  obj->fini_array_sz = d.d_val; break;
        case DT_PREINIT_ARRAY:   obj->preinit_array = bias + d.d_val; break;
        case DT_PREINIT_ARRAYSZ: obj->preinit_array_sz = d.d_val; break;

        case DT_VERSYM:     obj->versym  = bias + d.d_val; break;
        case DT_VERDEF:     obj->verdef  = bias + d.d_val; break;
        case DT_VERDEFNUM:  obj->verdefnum = (uint32_t)d.d_val; break;
        case DT_VERNEED:    obj->verneed = bias + d.d_val; break;
        case DT_VERNEEDNUM: obj->verneednum = (uint32_t)d.d_val; break;

        case DT_SONAME:  obj->soname_off  = (uint32_t)d.d_val; break;
        case DT_RPATH:   obj->rpath_off   = (uint32_t)d.d_val; break;
        case DT_RUNPATH: obj->runpath_off = (uint32_t)d.d_val; break;

        case DT_NEEDED:
            /* Kept as the raw strtab OFFSET: DT_STRTAB may appear after
             * DT_NEEDED, so the string table address is not necessarily
             * known yet at this point in the walk. */
            if (obj->needed_count < COS_ELF_MAX_NEEDED) {
                obj->needed[obj->needed_count++] = (uint32_t)d.d_val;
            } else {
                elf_log("warning: DT_NEEDED list truncated at the dependency cap");
            }
            break;

        case DT_FLAGS:    flags  = d.d_val; break;
        case DT_FLAGS_1:  flags1 = d.d_val; break;
        case DT_TEXTREL:  obj->flags |= COS_ELF_F_TEXTREL; break;
        case DT_BIND_NOW: obj->flags |= COS_ELF_F_BIND_NOW; break;
        default: break;   /* tags this loader has no opinion on */
        }
    }

    if (flags & DF_TEXTREL)  obj->flags |= COS_ELF_F_TEXTREL;
    if (flags & DF_BIND_NOW) obj->flags |= COS_ELF_F_BIND_NOW;
    if (flags1 & DF_1_NOW)   obj->flags |= COS_ELF_F_BIND_NOW;
    if (flags1 & DF_1_NODELETE) obj->flags |= COS_ELF_F_NODELETE;

    /* Stash the relocation tables where cos_elf_relocate() finds them.
     * DT_JMPREL's entry format is dictated by DT_PLTREL, not assumed:
     * `ld` on x86-64 emits RELA, but the tag exists precisely because
     * that is not universal, and reading a REL table as RELA would
     * misinterpret the next entry's r_offset as an addend. */
    obj->_rela = rela;   obj->_relasz = relasz;   obj->_relaent = relaent;
    obj->_rel  = rel;    obj->_relsz  = relsz;    obj->_relent  = relent;
    obj->_relr = relr;   obj->_relrsz = relrsz;   obj->_relrent = relrent;
    obj->_jmprel = jmprel; obj->_pltrelsz = pltrelsz;
    obj->_pltrel = pltrel;

    return true;
}

/* ================================================================== */
/* Header validation                                                   */
/* ================================================================== */

/* Validates the parts of the ELF header every path here depends on, and
 * reports the program header table. Shared by inspect and map so the two
 * cannot drift apart - a mismatch between "we said we could load it" and
 * "we actually can" is exactly the class of bug that leaves a
 * half-mapped address space behind. */
static bool validate_header(const uint8_t *image, uint64_t image_len,
                            const elf64_ehdr_t **out_eh,
                            const elf64_phdr_t **out_phs)
{
    if (image_len < sizeof(elf64_ehdr_t)) {
        elf_log("rejected: file smaller than an ELF header");
        return false;
    }
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;

    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        elf_log("rejected: not an ELF file (bad magic)");
        return false;
    }
    if (eh->e_ident[4] != ELFCLASS64) {
        elf_log("rejected: not ELF64");
        return false;
    }
    if (eh->e_ident[5] != ELFDATA2LSB) {
        elf_log("rejected: not little-endian");
        return false;
    }
    if (eh->e_machine != EM_X86_64) {
        elf_log("rejected: not x86-64");
        return false;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        /* ET_REL is an object file, not a program: it has no program
         * headers at all and needs a full static link, not a load.
         * ET_CORE is a crash dump. Saying so is more useful than a
         * generic "bad file". */
        elf_log("rejected: not an executable or shared object "
                "(ET_EXEC / ET_DYN required)");
        return false;
    }
    if (eh->e_phnum == 0 || eh->e_phnum > COS_ELF_MAX_PHNUM) {
        elf_log("rejected: bad program header count");
        return false;
    }
    if (eh->e_phentsize != sizeof(elf64_phdr_t)) {
        elf_log("rejected: unexpected program header size");
        return false;
    }
    if (!range_ok(eh->e_phoff, (uint64_t)eh->e_phnum * sizeof(elf64_phdr_t),
                  image_len)) {
        elf_log("rejected: program header table outside file");
        return false;
    }

    *out_eh = eh;
    *out_phs = (const elf64_phdr_t *)(image + eh->e_phoff);
    return true;
}

/* Validates every PT_LOAD and computes the image's virtual span.
 *
 * The byte-range overlap check is the security-relevant one. Two PT_LOAD
 * segments claiming the same BYTES could declare different p_flags, and
 * whichever was applied last would silently win - defeating W^X. Two
 * segments sharing only a boundary PAGE is a different thing entirely
 * and is completely normal linker output, so that is permitted and
 * resolved by taking the union of permissions (see cos_elf_protect). */
static bool validate_loads(const elf64_ehdr_t *eh, const elf64_phdr_t *phs,
                           uint64_t image_len,
                           uint64_t *out_min, uint64_t *out_max,
                           uint64_t *out_align, uint32_t *out_count)
{
    uint64_t total = 0, min_v = ~0ULL, max_v = 0, max_align = PAGE_SIZE;
    uint32_t count = 0;

    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        if (phs[i].p_type != PT_LOAD) continue;

        if (phs[i].p_filesz > phs[i].p_memsz) {
            elf_log("rejected: p_filesz exceeds p_memsz");
            return false;
        }
        if (phs[i].p_memsz > COS_ELF_MAX_SEGMENT) {
            elf_log("rejected: segment exceeds the per-segment size cap");
            return false;
        }
        if (!range_ok(phs[i].p_offset, phs[i].p_filesz, image_len)) {
            elf_log("rejected: segment file range outside image");
            return false;
        }
        if (phs[i].p_vaddr + phs[i].p_memsz < phs[i].p_vaddr) {
            elf_log("rejected: segment address range wraps");
            return false;
        }
        /* p_offset and p_vaddr must be congruent modulo the page size,
         * or the segment cannot be page-mapped from the file at all.
         * Real linkers always satisfy this; a crafted file need not. */
        if (phs[i].p_align > 1 &&
            ((phs[i].p_vaddr - phs[i].p_offset) & (PAGE_SIZE - 1)) != 0) {
            elf_log("rejected: segment p_vaddr/p_offset not page congruent");
            return false;
        }

        total += phs[i].p_memsz;
        if (total > COS_ELF_MAX_TOTAL) {
            elf_log("rejected: image exceeds the total size cap");
            return false;
        }
        if (phs[i].p_memsz == 0) continue;

        if (++count > COS_ELF_MAX_LOAD) {
            elf_log("rejected: too many PT_LOAD segments");
            return false;
        }
        if (phs[i].p_vaddr < min_v) min_v = phs[i].p_vaddr;
        if (phs[i].p_vaddr + phs[i].p_memsz > max_v) {
            max_v = phs[i].p_vaddr + phs[i].p_memsz;
        }
        if (phs[i].p_align > max_align && phs[i].p_align <= (1ULL << 30)) {
            max_align = phs[i].p_align;
        }

        for (uint16_t j = (uint16_t)(i + 1); j < eh->e_phnum; ++j) {
            if (phs[j].p_type != PT_LOAD || phs[j].p_memsz == 0) continue;
            uint64_t a0 = phs[i].p_vaddr, a1 = a0 + phs[i].p_memsz;
            uint64_t b0 = phs[j].p_vaddr, b1 = b0 + phs[j].p_memsz;
            if (a0 < b1 && b0 < a1) {
                elf_log("rejected: PT_LOAD segments overlap in address range");
                return false;
            }
        }
    }

    if (count == 0) {
        elf_log("rejected: no loadable segments");
        return false;
    }
    *out_min = min_v; *out_max = max_v;
    *out_align = max_align; *out_count = count;
    return true;
}

/* ================================================================== */
/* Phase 1: inspect                                                    */
/* ================================================================== */

/* Translates a link-time virtual address into a FILE offset, by finding
 * the PT_LOAD that contains it. A vaddr cannot be used as a file offset
 * directly - they differ whenever p_offset and p_vaddr are not equal,
 * which is the normal case for everything past the first segment. */
static bool vaddr_to_file_off(const elf64_ehdr_t *eh, const elf64_phdr_t *phs,
                              uint64_t vaddr, uint64_t *out_off)
{
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        if (phs[i].p_type != PT_LOAD) continue;
        if (vaddr >= phs[i].p_vaddr && vaddr < phs[i].p_vaddr + phs[i].p_filesz) {
            *out_off = phs[i].p_offset + (vaddr - phs[i].p_vaddr);
            return true;
        }
    }
    return false;
}

static void copy_file_string(const uint8_t *image, uint64_t image_len,
                             uint64_t off, char *out, uint64_t out_size)
{
    uint64_t k = 0;
    out[0] = '\0';
    for (; k + 1 < out_size && (off + k) < image_len; ++k) {
        char c = (char)image[off + k];
        out[k] = c;
        if (c == '\0') return;
    }
    out[(k < out_size) ? k : (out_size - 1)] = '\0';
}

bool cos_elf_inspect(const uint8_t *image, uint64_t image_len, cos_elf_info_t *out)
{
    if (!image || !out) return false;
    memset(out, 0, sizeof(*out));

    const elf64_ehdr_t *eh; const elf64_phdr_t *phs;
    if (!validate_header(image, image_len, &eh, &phs)) return false;

    uint64_t min_v, max_v, align; uint32_t count;
    if (!validate_loads(eh, phs, image_len, &min_v, &max_v, &align, &count)) {
        return false;
    }

    out->e_type    = eh->e_type;
    out->entry     = eh->e_entry;
    out->min_vaddr = page_down(min_v);
    out->span      = page_up(max_v) - out->min_vaddr;
    out->align     = align;
    out->flags     = (eh->e_type == ET_EXEC) ? COS_ELF_F_EXEC : 0u;

    const elf64_phdr_t *dynph = NULL;
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        switch (phs[i].p_type) {
        case PT_DYNAMIC:
            dynph = &phs[i];
            out->flags |= COS_ELF_F_DYNAMIC;
            break;
        case PT_INTERP:
            out->flags |= COS_ELF_F_INTERP;
            if (range_ok(phs[i].p_offset, phs[i].p_filesz, image_len)) {
                copy_file_string(image, image_len, phs[i].p_offset,
                                 out->interp, sizeof(out->interp));
            }
            break;
        case PT_TLS:
            out->flags |= COS_ELF_F_HAS_TLS;
            out->tls_memsz = phs[i].p_memsz;
            out->tls_align = phs[i].p_align ? phs[i].p_align : 1;
            break;
        case PT_GNU_STACK:
            if (phs[i].p_flags & PF_X) out->flags |= COS_ELF_F_EXEC_STACK;
            break;
        case PT_GNU_RELRO:
            out->flags |= COS_ELF_F_RELRO;
            break;
        default: break;
        }
    }

    if (!dynph) {
        out->flags |= COS_ELF_F_STATIC;
        return true;
    }
    if (!range_ok(dynph->p_offset, dynph->p_filesz, image_len)) {
        elf_log("rejected: PT_DYNAMIC outside file");
        return false;
    }

    /* Two passes over the dynamic section: the ELF spec does not require
     * DT_STRTAB to precede the tags that index into it, so the string
     * table's address is not necessarily known when DT_NEEDED is seen. */
    uint32_t needed_off[COS_ELF_MAX_NEEDED];
    uint32_t needed_n = 0;
    uint32_t soname_off = 0, rpath_off = 0, runpath_off = 0;
    uint64_t strtab_v = 0;

    const elf64_dyn_t *dyns = (const elf64_dyn_t *)(image + dynph->p_offset);
    uint64_t dyn_count = dynph->p_filesz / sizeof(elf64_dyn_t);
    if (dyn_count > COS_ELF_MAX_DYN_ENTRIES) dyn_count = COS_ELF_MAX_DYN_ENTRIES;

    for (uint64_t i = 0; i < dyn_count; ++i) {
        if (dyns[i].d_tag == DT_NULL) break;
        switch (dyns[i].d_tag) {
        case DT_STRTAB:  strtab_v    = dyns[i].d_val; break;
        case DT_SONAME:  soname_off  = (uint32_t)dyns[i].d_val; break;
        case DT_RPATH:   rpath_off   = (uint32_t)dyns[i].d_val; break;
        case DT_RUNPATH: runpath_off = (uint32_t)dyns[i].d_val; break;
        case DT_NEEDED:
            if (needed_n < COS_ELF_MAX_NEEDED) {
                needed_off[needed_n++] = (uint32_t)dyns[i].d_val;
            }
            break;
        default: break;
        }
    }

    if (strtab_v == 0) return true;   /* nothing to resolve names against */

    uint64_t strtab_off = 0;
    if (!vaddr_to_file_off(eh, phs, strtab_v, &strtab_off)) {
        elf_log("rejected: DT_STRTAB is not inside any PT_LOAD segment");
        return false;
    }

    if (soname_off)  copy_file_string(image, image_len, strtab_off + soname_off,
                                      out->soname, sizeof(out->soname));
    if (rpath_off)   copy_file_string(image, image_len, strtab_off + rpath_off,
                                      out->rpath, sizeof(out->rpath));
    if (runpath_off) copy_file_string(image, image_len, strtab_off + runpath_off,
                                      out->runpath, sizeof(out->runpath));

    for (uint32_t i = 0; i < needed_n; ++i) {
        copy_file_string(image, image_len, strtab_off + needed_off[i],
                         out->needed[out->needed_count],
                         sizeof(out->needed[0]));
        if (out->needed[out->needed_count][0] != '\0') out->needed_count++;
    }
    return true;
}

bool cos_elf_is_runnable(const uint8_t *image, uint64_t image_len)
{
    cos_elf_info_t info;
    if (!cos_elf_inspect(image, image_len, &info)) return false;
    /* A shared object with no entry point is a library, not a program.
     * A PIE has ET_DYN too, which is exactly why the type alone is not
     * the answer and the entry point has to be consulted. */
    if (info.e_type == ET_DYN && info.entry == 0) return false;
    return true;
}

/* ================================================================== */
/* Phase 2: map                                                        */
/* ================================================================== */

bool cos_elf_map(page_directory_t *dir, const uint8_t *image,
                 uint64_t image_len, uint64_t bias, cos_elf_object_t *out)
{
    if (!dir || !image || !out) return false;

    const elf64_ehdr_t *eh; const elf64_phdr_t *phs;
    if (!validate_header(image, image_len, &eh, &phs)) return false;

    uint64_t min_v, max_v, align; uint32_t nload;
    if (!validate_loads(eh, phs, image_len, &min_v, &max_v, &align, &nload)) {
        return false;
    }

    if (eh->e_type == ET_EXEC) {
        if (bias != 0) {
            /* An ET_EXEC's addresses are absolute. Biasing one would put
             * its code somewhere its own internal pointers do not
             * reference - it would load and then jump into nothing. */
            elf_log("rejected: ET_EXEC cannot be loaded at a bias");
            return false;
        }
    } else {
        if (bias == 0) bias = COS_ELF_PIE_BASE;
        if (bias & (PAGE_SIZE - 1)) {
            elf_log("rejected: ET_DYN load bias must be page aligned");
            return false;
        }
    }

    if (!user_range_ok(bias + page_down(min_v), page_up(max_v) - page_down(min_v))) {
        elf_log("rejected: image would not fit in the user address space");
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->base      = bias;
    out->map_start = bias + page_down(min_v);
    out->map_end   = bias + page_up(max_v);
    out->entry     = eh->e_entry ? (bias + eh->e_entry) : 0;
    out->phentsize = eh->e_phentsize;
    out->phnum     = eh->e_phnum;
    out->tls_modid = -1;
    out->flags     = (eh->e_type == ET_EXEC) ? COS_ELF_F_EXEC : COS_ELF_F_PIE;

    uint64_t irq_flags = sync_irq_save();
    bool ok = true;

    /* --- map every PT_LOAD, writable for now (see the file header) --- */
    for (uint16_t i = 0; i < eh->e_phnum && ok; ++i) {
        if (phs[i].p_type != PT_LOAD || phs[i].p_memsz == 0) continue;

        uint64_t v0 = bias + phs[i].p_vaddr;
        uint64_t v1 = v0 + phs[i].p_memsz;
        uint64_t s  = page_down(v0), e = page_up(v1);

        if (!user_range_ok(s, e - s)) {
            elf_log("rejected: segment lands outside the user address space");
            ok = false; break;
        }
        if (!map_range_rw(s, e)) { ok = false; break; }

        if (phs[i].p_filesz &&
            !user_write(v0, image + phs[i].p_offset, phs[i].p_filesz)) {
            elf_log("failed: could not copy segment contents");
            ok = false; break;
        }
        /* Bytes between p_filesz and p_memsz are .bss. Freshly allocated
         * pages were zeroed in map_range_rw(), but a page SHARED with a
         * neighbouring segment was not - it already held that
         * neighbour's data - so this range is zeroed explicitly rather
         * than assumed clean. Getting this wrong gives a program
         * non-zero .bss, which is a genuinely baffling bug to chase. */
        if (phs[i].p_memsz > phs[i].p_filesz &&
            !user_zero(v0 + phs[i].p_filesz, phs[i].p_memsz - phs[i].p_filesz)) {
            elf_log("failed: could not zero the .bss tail of a segment");
            ok = false; break;
        }

        out->segs[out->seg_count].start  = s;
        out->segs[out->seg_count].end    = e;
        out->segs[out->seg_count].pflags = phs[i].p_flags;
        out->seg_count++;
    }

    /* --- the non-loadable headers that still describe the image --- */
    const elf64_phdr_t *dynph = NULL;
    uint64_t phdr_vaddr = 0;

    for (uint16_t i = 0; i < eh->e_phnum && ok; ++i) {
        switch (phs[i].p_type) {
        case PT_PHDR:
            phdr_vaddr = phs[i].p_vaddr;
            break;
        case PT_DYNAMIC:
            dynph = &phs[i];
            out->flags |= COS_ELF_F_DYNAMIC;
            break;
        case PT_INTERP:
            out->flags |= COS_ELF_F_INTERP;
            out->interp_off = phs[i].p_offset;
            out->interp_len = phs[i].p_filesz;
            break;
        case PT_TLS:
            out->flags |= COS_ELF_F_HAS_TLS;
            out->tls_vaddr  = bias + phs[i].p_vaddr;
            out->tls_filesz = phs[i].p_filesz;
            out->tls_memsz  = phs[i].p_memsz;
            out->tls_align  = phs[i].p_align ? phs[i].p_align : 1;
            if (out->tls_filesz > out->tls_memsz) {
                elf_log("rejected: PT_TLS p_filesz exceeds p_memsz");
                ok = false;
            }
            break;
        case PT_GNU_STACK:
            if (phs[i].p_flags & PF_X) out->flags |= COS_ELF_F_EXEC_STACK;
            break;
        case PT_GNU_RELRO:
            out->flags |= COS_ELF_F_RELRO;
            out->relro_start = bias + phs[i].p_vaddr;
            out->relro_size  = phs[i].p_memsz;
            break;
        default: break;
        }
    }

    /* AT_PHDR. PT_PHDR is the authoritative answer when present; when it
     * is absent (common for shared objects and for -static-pie), the
     * table still lives at e_phoff inside the first PT_LOAD, so it can be
     * derived by finding the segment whose file range covers e_phoff. A
     * libc that cannot find its own program headers cannot find its own
     * PT_TLS, so "absent" is not an acceptable answer here. */
    if (ok) {
        if (phdr_vaddr) {
            out->phdr = bias + phdr_vaddr;
        } else {
            for (uint16_t i = 0; i < eh->e_phnum; ++i) {
                if (phs[i].p_type != PT_LOAD) continue;
                if (eh->e_phoff >= phs[i].p_offset &&
                    eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(elf64_phdr_t)
                        <= phs[i].p_offset + phs[i].p_filesz) {
                    out->phdr = bias + phs[i].p_vaddr + (eh->e_phoff - phs[i].p_offset);
                    break;
                }
            }
        }
    }

    if (ok && dynph) {
        out->dynamic = bias + dynph->p_vaddr;
        ok = scan_dynamic(out, out->dynamic, dynph->p_memsz);
    } else if (ok) {
        out->flags |= COS_ELF_F_STATIC;
    }

    sync_irq_restore(irq_flags);

    if (!ok) {
        elf_log("map failed; the process address space must be discarded");
        return false;
    }
    return true;
}

/* ================================================================== */
/* Symbol tables, hash tables and versioning                           */
/* ================================================================== */

bool cos_elf_string(page_directory_t *dir, const cos_elf_object_t *obj,
                    uint32_t offset, char *out, uint64_t out_size)
{
    (void)dir;
    if (!obj || !out || out_size == 0) return false;
    if (obj->strtab == 0) return false;
    /* DT_STRSZ bounds the table. A crafted st_name past the end must not
     * be allowed to read whatever follows the string table in memory. */
    if (obj->strsz && offset >= obj->strsz) return false;
    return user_strncpy_from(obj->strtab + offset, out, out_size);
}

static bool read_sym(const cos_elf_object_t *obj, uint32_t index, elf64_sym_t *out)
{
    if (obj->symtab == 0 || obj->syment < sizeof(elf64_sym_t)) return false;
    if (index >= COS_ELF_MAX_SYMBOLS) return false;
    if (obj->nsyms && index >= obj->nsyms) return false;
    return user_read(obj->symtab + (uint64_t)index * obj->syment, out, sizeof(*out));
}

/* Compares a C string against one in the object's string table, without
 * copying it out first. Bounded by DT_STRSZ and by the name cap, so an
 * unterminated table cannot run away. */
static bool strtab_equals(const cos_elf_object_t *obj, uint32_t st_name,
                          const char *want)
{
    if (obj->strsz && st_name >= obj->strsz) return false;
    uint64_t addr = obj->strtab + st_name;
    for (uint64_t k = 0; k <= COS_ELF_MAX_SYMNAME; ++k) {
        char c = 0;
        if (!user_read(addr + k, &c, 1)) return false;
        if (want[k] != c) return false;
        if (c == '\0') return true;
    }
    return false;
}

/* The SysV ELF hash, used by DT_HASH. */
static uint32_t elf_hash(const char *name)
{
    uint32_t h = 0, g;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        h = (h << 4) + *p;
        g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* The GNU hash (djb2 with multiplier 33), used by DT_GNU_HASH. */
static uint32_t gnu_hash_name(const char *name)
{
    uint32_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        h = h * 33 + *p;
    }
    return h;
}

/* DT_GNU_HASH header, as laid out in the file. */
typedef struct {
    uint32_t nbuckets;
    uint32_t symoffset;   /* index of the first symbol in the hash chain */
    uint32_t bloom_size;  /* number of 64-bit Bloom filter words         */
    uint32_t bloom_shift;
} __attribute__((packed)) gnu_hash_hdr_t;

/* Determines the exact number of dynamic symbols.
 *
 * The old loader had no way to know this: it walked the symbol table
 * until an st_name fell outside DT_STRSZ and treated that as the end,
 * which is a heuristic, not a bound - a table whose last entries happen
 * to have small st_name values is silently truncated, and one whose
 * entries are all in range is walked to the 4096 cap every lookup.
 *
 * DT_HASH states the count outright (nchain). DT_GNU_HASH does not, but
 * it can be derived: the highest bucket value indexes the last chain,
 * and the chain's terminator bit marks its end. */
static uint32_t compute_nsyms(const cos_elf_object_t *obj)
{
    if (obj->hash) {
        uint32_t hdr[2];
        if (user_read(obj->hash, hdr, sizeof(hdr))) {
            if (hdr[1] && hdr[1] <= COS_ELF_MAX_SYMBOLS) return hdr[1];
        }
    }
    if (obj->gnu_hash) {
        gnu_hash_hdr_t gh;
        if (!user_read(obj->gnu_hash, &gh, sizeof(gh))) return 0;
        if (gh.nbuckets == 0 || gh.nbuckets > COS_ELF_MAX_SYMBOLS) return 0;
        if (gh.bloom_size == 0 || gh.bloom_size > COS_ELF_MAX_SYMBOLS) return 0;

        uint64_t buckets = obj->gnu_hash + sizeof(gh) + (uint64_t)gh.bloom_size * 8;
        uint64_t chains  = buckets + (uint64_t)gh.nbuckets * 4;

        uint32_t last = 0;
        for (uint32_t i = 0; i < gh.nbuckets; ++i) {
            uint32_t b = 0;
            if (!user_read(buckets + (uint64_t)i * 4, &b, 4)) return 0;
            if (b > last) last = b;
        }
        if (last < gh.symoffset) return gh.symoffset;

        /* Walk to the end of the chain the highest bucket starts. */
        for (uint32_t guard = 0; guard < COS_ELF_MAX_SYMBOLS; ++guard) {
            uint32_t c = 0;
            if (!user_read(chains + (uint64_t)(last - gh.symoffset) * 4, &c, 4)) return 0;
            last++;
            if (c & 1u) break;
        }
        return last;
    }
    return 0;
}

/* Resolves the version index recorded in DT_VERSYM for symbol `idx` in
 * `obj` to a version NAME, looking in DT_VERDEF (for a version this
 * object defines) and then DT_VERNEED (for one it imports).
 *
 * Without this, a versioned symbol lookup is a guess: two definitions of
 * the same name at different versions are indistinguishable, and picking
 * the wrong one binds a caller to the wrong implementation. */
static bool version_name_for_index(const cos_elf_object_t *obj, uint16_t vidx,
                                   char *out, uint64_t out_size)
{
    vidx &= (uint16_t)~VERSYM_HIDDEN;
    if (vidx <= VER_NDX_GLOBAL) return false;   /* local/global: unversioned */

    /* DT_VERDEF: versions this object defines. */
    if (obj->verdef && obj->verdefnum) {
        uint64_t p = obj->verdef;
        for (uint32_t i = 0; i < obj->verdefnum && i < 1024u; ++i) {
            elf64_verdef_t vd;
            if (!user_read(p, &vd, sizeof(vd))) break;
            if ((vd.vd_ndx & ~VERSYM_HIDDEN) == vidx && vd.vd_cnt >= 1) {
                elf64_verdaux_t aux;
                if (user_read(p + vd.vd_aux, &aux, sizeof(aux))) {
                    return cos_elf_string(NULL, obj, aux.vda_name, out, out_size);
                }
            }
            if (vd.vd_next == 0) break;
            p += vd.vd_next;
        }
    }

    /* DT_VERNEED: versions this object requires from its dependencies. */
    if (obj->verneed && obj->verneednum) {
        uint64_t p = obj->verneed;
        for (uint32_t i = 0; i < obj->verneednum && i < 1024u; ++i) {
            elf64_verneed_t vn;
            if (!user_read(p, &vn, sizeof(vn))) break;
            uint64_t a = p + vn.vn_aux;
            for (uint32_t j = 0; j < vn.vn_cnt && j < 1024u; ++j) {
                elf64_vernaux_t va;
                if (!user_read(a, &va, sizeof(va))) break;
                if ((va.vna_other & ~VERSYM_HIDDEN) == vidx) {
                    return cos_elf_string(NULL, obj, va.vna_name, out, out_size);
                }
                if (va.vna_next == 0) break;
                a += va.vna_next;
            }
            if (vn.vn_next == 0) break;
            p += vn.vn_next;
        }
    }
    return false;
}

/* True if symbol `idx` in `obj` satisfies a request for `want_version`.
 *
 * A NULL request accepts any version but prefers a default (non-hidden)
 * one - which is what a caller that does not care about versions means,
 * and what `dlsym()` does. A named request must match exactly. */
static bool version_matches(const cos_elf_object_t *obj, uint32_t idx,
                            const char *want_version, bool *out_is_default)
{
    *out_is_default = true;
    if (!obj->versym) return want_version == NULL;   /* unversioned object */

    uint16_t vs = 0;
    if (!user_read(obj->versym + (uint64_t)idx * 2, &vs, 2)) return false;
    *out_is_default = (vs & VERSYM_HIDDEN) == 0;

    if (!want_version) {
        /* An unversioned reference binds to the DEFAULT version only.
         * A hidden version (the `name@OLD` form, as opposed to
         * `name@@CURRENT`) exists precisely so that old binaries keep
         * reaching the old implementation while new callers get the new
         * one; matching it here would hand a caller that asked for
         * nothing in particular whichever definition the hash chain
         * happened to reach first. */
        return *out_is_default;
    }

    char have[COS_ELF_MAX_PATH];
    if (!version_name_for_index(obj, vs, have, sizeof(have))) {
        /* Requested a specific version from an object whose symbol has
         * none. Accepting would bind to something that does not claim to
         * be compatible. */
        return false;
    }
    return strcmp(have, want_version) == 0;
}

/* Fills out a cos_elf_symval_t from a symbol table entry. */
static void fill_symval(const cos_elf_object_t *obj, const elf64_sym_t *sym,
                        cos_elf_symval_t *out)
{
    memset(out, 0, sizeof(*out));
    out->size = sym->st_size;
    if (ELF64_ST_TYPE(sym->st_info) == STT_TLS) {
        /* A TLS symbol's st_value is an offset into its module's TLS
         * block, NOT an address. Treating it as one - which is what any
         * loader without TLS awareness does - produces a pointer into
         * whatever happens to live at that low address. */
        out->is_tls       = true;
        out->tls_modid    = obj->tls_modid;
        out->tls_offset   = (int64_t)sym->st_value;
        out->tls_tp_offset = (int64_t)sym->st_value - obj->tls_offset;
        out->address      = 0;
    } else if (sym->st_shndx == SHN_ABS) {
        /* An absolute symbol is not relocated by the load bias. */
        out->address = sym->st_value;
    } else {
        out->address = obj->base + sym->st_value;
    }
}

/* DT_GNU_HASH lookup: Bloom-filter reject, then one short bucket chain. */
static bool lookup_gnu_hash(const cos_elf_object_t *obj, const char *name,
                            const char *version, cos_elf_symval_t *out)
{
    gnu_hash_hdr_t gh;
    if (!user_read(obj->gnu_hash, &gh, sizeof(gh))) return false;
    if (gh.nbuckets == 0 || gh.bloom_size == 0) return false;
    if (gh.nbuckets > COS_ELF_MAX_SYMBOLS || gh.bloom_size > COS_ELF_MAX_SYMBOLS) {
        return false;
    }
    if (gh.bloom_shift >= 64) return false;

    uint64_t bloom   = obj->gnu_hash + sizeof(gh);
    uint64_t buckets = bloom + (uint64_t)gh.bloom_size * 8;
    uint64_t chains  = buckets + (uint64_t)gh.nbuckets * 4;

    uint32_t h1 = gnu_hash_name(name);

    /* The Bloom filter exists so that the common case - the symbol is
     * NOT in this object - costs one 64-bit load instead of a chain
     * walk. With a dozen libraries in the search scope, that is the
     * difference between a linear and a near-constant-time link. */
    uint64_t word = 0;
    uint64_t widx = (h1 / 64) % gh.bloom_size;
    if (!user_read(bloom + widx * 8, &word, 8)) return false;
    uint64_t mask = (1ULL << (h1 % 64)) |
                    (1ULL << ((h1 >> gh.bloom_shift) % 64));
    if ((word & mask) != mask) return false;

    uint32_t n = 0;
    if (!user_read(buckets + (uint64_t)(h1 % gh.nbuckets) * 4, &n, 4)) return false;
    if (n < gh.symoffset) return false;

    for (uint32_t guard = 0; guard < COS_ELF_MAX_SYMBOLS; ++guard, ++n) {
        uint32_t h2 = 0;
        if (!user_read(chains + (uint64_t)(n - gh.symoffset) * 4, &h2, 4)) return false;

        if (((h1 ^ h2) >> 1) == 0) {
            elf64_sym_t sym;
            if (read_sym(obj, n, &sym) && sym.st_shndx != SHN_UNDEF &&
                strtab_equals(obj, sym.st_name, name)) {
                bool is_default;
                if (version_matches(obj, n, version, &is_default)) {
                    fill_symval(obj, &sym, out);
                    return true;
                }
            }
        }
        if (h2 & 1u) break;   /* end of chain */
    }
    return false;
}

/* DT_HASH lookup. */
static bool lookup_sysv_hash(const cos_elf_object_t *obj, const char *name,
                             const char *version, cos_elf_symval_t *out)
{
    uint32_t hdr[2];
    if (!user_read(obj->hash, hdr, sizeof(hdr))) return false;
    uint32_t nbucket = hdr[0], nchain = hdr[1];
    if (nbucket == 0 || nbucket > COS_ELF_MAX_SYMBOLS) return false;
    if (nchain == 0 || nchain > COS_ELF_MAX_SYMBOLS) return false;

    uint64_t buckets = obj->hash + 8;
    uint64_t chain   = buckets + (uint64_t)nbucket * 4;

    uint32_t idx = 0;
    if (!user_read(buckets + (uint64_t)(elf_hash(name) % nbucket) * 4, &idx, 4)) {
        return false;
    }
    for (uint32_t guard = 0; guard < nchain && idx != 0; ++guard) {
        elf64_sym_t sym;
        if (read_sym(obj, idx, &sym) && sym.st_shndx != SHN_UNDEF &&
            strtab_equals(obj, sym.st_name, name)) {
            bool is_default;
            if (version_matches(obj, idx, version, &is_default)) {
                fill_symval(obj, &sym, out);
                return true;
            }
        }
        if (!user_read(chain + (uint64_t)idx * 4, &idx, 4)) break;
        if (idx >= nchain) break;
    }
    return false;
}

/* Last resort: a bounded linear walk, for an object with no hash table
 * at all. Kept because an object CAN legitimately lack both (a hand-
 * linked one, or anything built with --hash-style=none), and refusing it
 * would be a regression against the old loader, which only ever did
 * this. */
static bool lookup_linear(const cos_elf_object_t *obj, const char *name,
                          const char *version, cos_elf_symval_t *out)
{
    uint32_t limit = obj->nsyms ? obj->nsyms : COS_ELF_MAX_SYMBOLS;
    for (uint32_t i = 0; i < limit; ++i) {
        elf64_sym_t sym;
        if (!user_read(obj->symtab + (uint64_t)i * obj->syment, &sym, sizeof(sym))) {
            break;
        }
        if (sym.st_name == 0 || sym.st_shndx == SHN_UNDEF) continue;
        if (obj->strsz && sym.st_name >= obj->strsz) {
            if (!obj->nsyms) break;   /* heuristic end, only when unbounded */
            continue;
        }
        if (!strtab_equals(obj, sym.st_name, name)) continue;
        bool is_default;
        if (!version_matches(obj, i, version, &is_default)) continue;
        fill_symval(obj, &sym, out);
        return true;
    }
    return false;
}

bool cos_elf_lookup(page_directory_t *dir, const cos_elf_object_t *obj,
                    const char *name, const char *version,
                    cos_elf_symval_t *out)
{
    (void)dir;
    if (!obj || !name || !out) return false;
    if (obj->symtab == 0 || obj->strtab == 0 || obj->syment == 0) return false;

    uint64_t irq_flags = sync_irq_save();
    bool found = false;

    if (obj->gnu_hash)  found = lookup_gnu_hash(obj, name, version, out);
    if (!found && obj->hash) found = lookup_sysv_hash(obj, name, version, out);
    if (!found && !obj->gnu_hash && !obj->hash) {
        found = lookup_linear(obj, name, version, out);
    }

    sync_irq_restore(irq_flags);
    return found;
}

bool cos_elf_library_symbol(page_directory_t *dir, const cos_elf_object_t *lib,
                            const char *name, uint64_t *out_addr)
{
    cos_elf_symval_t v;
    if (!out_addr) return false;
    if (!cos_elf_lookup(dir, lib, name, NULL, &v)) return false;
    *out_addr = v.address;
    return true;
}

/* ================================================================== */
/* Phase 3: relocation                                                 */
/* ================================================================== */

/* Everything one relocation entry needs, normalised so that RELA, REL
 * and RELR entries all flow through the same application code. REL has
 * no explicit addend - the addend lives in the target memory and must be
 * read from there, which is the entire difference between the two
 * formats and the one thing a loader must not get backwards. */
typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} reloc_t;

/* Writes `value` into the target, truncated to `width` bytes, rejecting
 * a value that does not fit. The narrow relocation types exist precisely
 * so a linker can catch this at link time; a loader that silently
 * truncates produces a program that jumps somewhere plausible-looking
 * and wrong. */
static bool write_sized(uint64_t target, uint64_t value, unsigned width, bool is_signed)
{
    switch (width) {
    case 8: return user_write(target, &value, 8);
    case 4: {
        if (is_signed) {
            int64_t sv = (int64_t)value;
            if (sv < -2147483648LL || sv > 2147483647LL) {
                elf_log("rejected: 32-bit signed relocation overflows");
                return false;
            }
        } else if (value > 0xFFFFFFFFULL) {
            elf_log("rejected: 32-bit relocation overflows");
            return false;
        }
        uint32_t v32 = (uint32_t)value;
        return user_write(target, &v32, 4);
    }
    case 2: {
        if (value > 0xFFFFULL && (int64_t)value < -32768LL) {
            elf_log("rejected: 16-bit relocation overflows");
            return false;
        }
        uint16_t v16 = (uint16_t)value;
        return user_write(target, &v16, 2);
    }
    case 1: {
        uint8_t v8 = (uint8_t)value;
        return user_write(target, &v8, 1);
    }
    default:
        return false;
    }
}

/* Resolves the symbol a relocation names, via the object's own symbol
 * table first and the caller's resolver second.
 *
 * Own-object-first matters: a symbol an object defines itself must bind
 * to its own copy unless something earlier in the global scope
 * interposed on it. Asking the resolver first would let an unrelated
 * library's same-named symbol capture an internal reference. */
static bool resolve_for_reloc(const cos_elf_object_t *obj, uint32_t sym_index,
                              uint32_t reloc_type,
                              cos_elf_symbol_resolver_t resolver, void *resolver_ctx,
                              cos_elf_symval_t *out, bool *out_is_weak,
                              bool *out_is_ifunc)
{
    memset(out, 0, sizeof(*out));
    *out_is_weak = false;
    *out_is_ifunc = false;

    if (sym_index == 0) {
        /* Symbol index 0 with a TLS relocation means "this module" -
         * used by local-dynamic TLS, where the module is known but no
         * particular symbol is. */
        out->is_tls    = true;
        out->tls_modid = obj->tls_modid;
        return true;
    }

    elf64_sym_t sym;
    if (!read_sym(obj, sym_index, &sym)) {
        elf_log("rejected: relocation names a symbol outside the symbol table");
        return false;
    }

    unsigned bind = ELF64_ST_BIND(sym.st_info);
    unsigned type = ELF64_ST_TYPE(sym.st_info);
    *out_is_weak = (bind == STB_WEAK);
    *out_is_ifunc = (type == STT_GNU_IFUNC);

    /* A defined symbol in this very object, and not a COPY relocation
     * (which by definition wants the OTHER object's copy). */
    if (sym.st_shndx != SHN_UNDEF && reloc_type != R_X86_64_COPY) {
        fill_symval(obj, &sym, out);
        return true;
    }

    char name[COS_ELF_MAX_SYMNAME + 1];
    if (!cos_elf_string(NULL, obj, sym.st_name, name, sizeof(name))) {
        elf_log("rejected: could not read the name of a relocated symbol");
        return false;
    }

    char version_buf[COS_ELF_MAX_PATH];
    const char *version = NULL;
    if (obj->versym) {
        uint16_t vs = 0;
        if (user_read(obj->versym + (uint64_t)sym_index * 2, &vs, 2) &&
            version_name_for_index(obj, vs, version_buf, sizeof(version_buf))) {
            version = version_buf;
        }
    }

    cos_elf_symreq_t req;
    req.name      = name;
    req.version   = version;
    req.requester = obj;
    req.flags     = 0;
    if (bind == STB_WEAK)        req.flags |= COS_ELF_SYM_WEAK;
    if (type == STT_FUNC)        req.flags |= COS_ELF_SYM_FUNC;
    if (type == STT_OBJECT)      req.flags |= COS_ELF_SYM_OBJECT;
    if (type == STT_TLS)         req.flags |= COS_ELF_SYM_TLS;
    if (type == STT_GNU_IFUNC)   req.flags |= COS_ELF_SYM_IFUNC;
    if (reloc_type == R_X86_64_COPY) req.flags |= COS_ELF_SYM_COPY;

    if (resolver && resolver(resolver_ctx, &req, out)) return true;

    if (bind == STB_WEAK) {
        /* A weak undefined symbol resolving to 0 is not an error - it is
         * the entire point of weakness, and the idiom
         * `if (&maybe_present) maybe_present();` depends on it. The old
         * loader failed the whole load here, which rejects a large share
         * of real shared objects outright: glibc-linked code carries
         * weak references to pthread and malloc hooks as a matter of
         * course. */
        memset(out, 0, sizeof(*out));
        return true;
    }

    elf_log2("rejected: undefined symbol: ", name);
    return false;
}

/* Applies a single normalised relocation. */
static bool apply_reloc(cos_elf_object_t *obj, const reloc_t *rel,
                        cos_elf_symbol_resolver_t resolver, void *resolver_ctx)
{
    uint32_t type = ELF64_R_TYPE(rel->r_info);
    uint32_t sidx = ELF64_R_SYM(rel->r_info);
    if (type == R_X86_64_NONE) return true;

    uint64_t target = obj->base + rel->r_offset;
    if (!user_range_ok(target, 8)) {
        elf_log_hex("rejected: relocation target outside user memory at ", target);
        return false;
    }
    /* The target must be inside this object's own mapping. Without this,
     * a crafted r_offset relocates over ANY writable user page - the
     * stack, the heap, another library's code - which is a write-what-
     * where primitive handed to whoever wrote the file. */
    if (target < obj->map_start || target >= obj->map_end) {
        elf_log_hex("rejected: relocation target outside the object at ", target);
        return false;
    }

    uint64_t A = (uint64_t)rel->r_addend;
    uint64_t P = target;
    uint64_t B = obj->base;

    switch (type) {
    /* --- no symbol involved --------------------------------------- */
    case R_X86_64_RELATIVE:
    case R_X86_64_RELATIVE64:
        return user_write(target, (uint64_t[]){ B + A }, 8);

    case R_X86_64_IRELATIVE: {
        /* B + A names a RESOLVER FUNCTION, whose return value is the
         * real address. That function is ring3 code - running it here
         * would execute user-chosen code at ring0. It is recorded for
         * the process's own startup to apply instead. */
        if (obj->ifunc_count >= COS_ELF_MAX_IFUNC) {
            elf_log("rejected: too many IRELATIVE relocations");
            return false;
        }
        obj->ifunc_target[obj->ifunc_count]   = target;
        obj->ifunc_resolver[obj->ifunc_count] = B + A;
        obj->ifunc_count++;
        obj->flags |= COS_ELF_F_HAS_IFUNC;
        /* Leave a trap value rather than the resolver address: if ring3
         * startup never applies these, a call through the slot faults at
         * a recognisable address instead of calling the resolver as if
         * it were the function. */
        return user_write(target, (uint64_t[]){ 0 }, 8);
    }

    default: break;
    }

    /* --- everything below needs a symbol -------------------------- */
    cos_elf_symval_t sv;
    bool is_weak = false, is_ifunc = false;
    if (!resolve_for_reloc(obj, sidx, type, resolver, resolver_ctx,
                           &sv, &is_weak, &is_ifunc)) {
        return false;
    }
    uint64_t S = sv.address;

    /* An STT_GNU_IFUNC symbol's "address" is a RESOLVER, not the
     * function - calling it is what produces the real address. A
     * pointer-sized relocation against one is therefore the same
     * deferred ring3 work as R_X86_64_IRELATIVE, just reached by name
     * instead of by address. `ld` emits exactly this shape whenever a
     * shared object exports an ifunc and something takes its address,
     * so skipping it writes the resolver stub into the slot and the
     * program calls the resolver every time instead of the function. */
    if (is_ifunc && S != 0 &&
        (type == R_X86_64_64 || type == R_X86_64_GLOB_DAT ||
         type == R_X86_64_JUMP_SLOT)) {
        if (obj->ifunc_count >= COS_ELF_MAX_IFUNC) {
            elf_log("rejected: too many ifunc relocations");
            return false;
        }
        obj->ifunc_target[obj->ifunc_count]   = target;
        obj->ifunc_resolver[obj->ifunc_count] = S;
        obj->ifunc_count++;
        obj->flags |= COS_ELF_F_HAS_IFUNC;
        return user_write(target, (uint64_t[]){ 0 }, 8);
    }

    switch (type) {
    case R_X86_64_64:
        return write_sized(target, S + A, 8, false);

    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT:
        /* Per the ABI the addend is 0 for these; adding it is harmless
         * and correct either way. */
        return write_sized(target, S + A, 8, false);

    case R_X86_64_PC32:
    case R_X86_64_PLT32:
        return write_sized(target, S + A - P, 4, true);

    case R_X86_64_PC64:
        return write_sized(target, S + A - P, 8, false);

    case R_X86_64_32:
        return write_sized(target, S + A, 4, false);

    case R_X86_64_32S:
        return write_sized(target, S + A, 4, true);

    case R_X86_64_16:  return write_sized(target, S + A, 2, false);
    case R_X86_64_PC16: return write_sized(target, S + A - P, 2, true);
    case R_X86_64_8:   return write_sized(target, S + A, 1, false);
    case R_X86_64_PC8: return write_sized(target, S + A - P, 1, true);

    case R_X86_64_SIZE32: return write_sized(target, sv.size + A, 4, false);
    case R_X86_64_SIZE64: return write_sized(target, sv.size + A, 8, false);

    case R_X86_64_COPY: {
        /* The classic "an executable references a data object defined in
         * a shared library" case: the executable reserves space in its
         * own .bss and the loader copies the library's initial value
         * into it, so both sides then refer to the same storage. */
        if (sv.size == 0) return true;
        if (sv.size > COS_ELF_MAX_SEGMENT) {
            elf_log("rejected: COPY relocation size is implausible");
            return false;
        }
        if (!user_range_ok(target, sv.size) || !user_range_ok(S, sv.size)) {
            elf_log("rejected: COPY relocation range outside user memory");
            return false;
        }
        if (target + sv.size > obj->map_end) {
            elf_log("rejected: COPY relocation would write past the object");
            return false;
        }
        /* Copied in bounded chunks so an arbitrarily large st_size does
         * not need a kernel buffer of the same size. */
        uint8_t buf[256];
        for (uint64_t off = 0; off < sv.size; ) {
            uint64_t n = sv.size - off;
            if (n > sizeof(buf)) n = sizeof(buf);
            if (!user_read(S + off, buf, n)) return false;
            if (!user_write(target + off, buf, n)) return false;
            off += n;
        }
        return true;
    }

    /* --- thread-local storage ------------------------------------- */
    case R_X86_64_DTPMOD64:
        /* Which module the variable lives in. An unresolved weak TLS
         * symbol has no module, and 0 is the correct "no module" value. */
        return write_sized(target,
                           (sv.is_tls && sv.tls_modid > 0) ? (uint64_t)sv.tls_modid : 0ULL,
                           8, false);

    case R_X86_64_DTPOFF64:
        if (!sv.is_tls && !is_weak) {
            elf_log("rejected: DTPOFF64 against a non-TLS symbol");
            return false;
        }
        return write_sized(target, (uint64_t)(sv.tls_offset + (int64_t)A), 8, false);

    case R_X86_64_TPOFF64:
        if (!sv.is_tls && !is_weak) {
            elf_log("rejected: TPOFF64 against a non-TLS symbol");
            return false;
        }
        /* x86-64 variant II: the variable sits BELOW the thread pointer,
         * so the offset stored here is negative and the access is
         * `%fs:offset`. tls_tp_offset already accounts for where this
         * module was placed in the static TLS block. */
        return write_sized(target, (uint64_t)(sv.tls_tp_offset + (int64_t)A), 8, false);

    case R_X86_64_TLSDESC:
    case R_X86_64_TLSDESC_CALL:
        /* A TLS descriptor is a two-word structure whose first word is a
         * RESOLVER FUNCTION called at ring3 on every access. Supporting
         * it properly needs that resolver to live in userland, the same
         * way IRELATIVE does. Refused explicitly rather than written as
         * a plain offset, which would be silently wrong: build shared
         * objects with -mtls-dialect=gnu2 disabled, or link with
         * -Wl,-z,notlsdesc. */
        elf_log("rejected: TLSDESC relocations are not supported "
                "(rebuild without -mtls-dialect=gnu2)");
        return false;

    case R_X86_64_GOTPCREL:
    case R_X86_64_GOT32:
    case R_X86_64_GOTOFF64:
    case R_X86_64_GOTPC32:
    case R_X86_64_TLSGD:
    case R_X86_64_TLSLD:
    case R_X86_64_GOTTPOFF:
    case R_X86_64_DTPOFF32:
    case R_X86_64_TPOFF32:
        /* These are link-time-only types: the static linker resolves them
         * while building the GOT and they must never reach a dynamic
         * loader. Seeing one means the file was produced by a partial
         * link (ld -r) rather than a final one. */
        elf_log("rejected: link-time-only relocation type in a loadable image "
                "(was this linked with -r?)");
        return false;

    default:
        elf_log_hex("rejected: unsupported relocation type ", type);
        return false;
    }
}

/* Processes one RELA or REL table. */
static bool apply_reloc_table(cos_elf_object_t *obj, uint64_t addr, uint64_t size,
                              uint64_t entsize, bool is_rela,
                              cos_elf_symbol_resolver_t resolver, void *resolver_ctx)
{
    if (addr == 0 || size == 0) return true;

    uint64_t min_ent = is_rela ? sizeof(elf64_rela_t) : sizeof(elf64_rel_t);
    if (entsize < min_ent) {
        elf_log("rejected: relocation entry size smaller than a real entry");
        return false;
    }
    uint64_t count = size / entsize;
    if (count > COS_ELF_MAX_RELOCS) {
        elf_log("rejected: too many relocations");
        return false;
    }

    for (uint64_t i = 0; i < count; ++i) {
        reloc_t r;
        if (is_rela) {
            elf64_rela_t e;
            if (!user_read(addr + i * entsize, &e, sizeof(e))) {
                elf_log("failed: could not read a relocation entry");
                return false;
            }
            r.r_offset = e.r_offset; r.r_info = e.r_info; r.r_addend = e.r_addend;
        } else {
            elf64_rel_t e;
            if (!user_read(addr + i * entsize, &e, sizeof(e))) {
                elf_log("failed: could not read a relocation entry");
                return false;
            }
            r.r_offset = e.r_offset; r.r_info = e.r_info;
            /* REL keeps the addend in the target location itself. Reading
             * it is not optional: treating it as zero silently drops
             * every offset the linker encoded there. */
            uint64_t implicit = 0;
            uint64_t t = obj->base + e.r_offset;
            if (!user_range_ok(t, 8) || t < obj->map_start || t >= obj->map_end) {
                elf_log("rejected: REL target outside the object");
                return false;
            }
            if (!user_read(t, &implicit, 8)) return false;
            r.r_addend = (int64_t)implicit;
        }
        if (!apply_reloc(obj, &r, resolver, resolver_ctx)) return false;
    }
    return true;
}

/* Processes a DT_RELR table.
 *
 * RELR is a compression of R_X86_64_RELATIVE entries and nothing else.
 * A modern PIE has thousands of them, each costing 24 bytes as RELA;
 * RELR encodes runs of them as bitmaps and typically shrinks that by
 * more than 90%. Toolchains emit it by default in more and more places
 * (and always with -z pack-relative-relocs), and an object using it has
 * NO DT_RELA at all - so a loader that does not read RELR applies zero
 * relocations, loads happily, and then crashes on the first pointer.
 *
 * Format: a stream of 64-bit words. An EVEN word is an address; relocate
 * there and set the cursor just past it. An ODD word is a bitmap of the
 * next 63 words after the cursor - bit i (counting from 1) means
 * "relocate at cursor + (i-1)*8". */
static bool apply_relr_table(cos_elf_object_t *obj, uint64_t addr,
                             uint64_t size, uint64_t entsize)
{
    if (addr == 0 || size == 0) return true;
    if (entsize < sizeof(uint64_t)) {
        elf_log("rejected: DT_RELRENT smaller than a word");
        return false;
    }
    uint64_t count = size / entsize;
    if (count > COS_ELF_MAX_RELOCS) {
        elf_log("rejected: too many RELR entries");
        return false;
    }

    uint64_t where = 0;
    uint64_t applied = 0;

    for (uint64_t i = 0; i < count; ++i) {
        uint64_t entry = 0;
        if (!user_read(addr + i * entsize, &entry, 8)) {
            elf_log("failed: could not read a RELR entry");
            return false;
        }

        if ((entry & 1ULL) == 0) {
            where = obj->base + entry;
            if (where < obj->map_start || where + 8 > obj->map_end) {
                elf_log("rejected: RELR address outside the object");
                return false;
            }
            uint64_t v = 0;
            if (!user_read(where, &v, 8)) return false;
            if (!user_write(where, (uint64_t[]){ v + obj->base }, 8)) return false;
            where += 8;
            if (++applied > COS_ELF_MAX_RELOCS) {
                elf_log("rejected: RELR expands to too many relocations");
                return false;
            }
            continue;
        }

        uint64_t bits = entry >> 1;
        uint64_t p = where;
        for (unsigned b = 0; b < 63; ++b, p += 8) {
            if (!(bits & (1ULL << b))) continue;
            if (p < obj->map_start || p + 8 > obj->map_end) {
                elf_log("rejected: RELR bitmap address outside the object");
                return false;
            }
            uint64_t v = 0;
            if (!user_read(p, &v, 8)) return false;
            if (!user_write(p, (uint64_t[]){ v + obj->base }, 8)) return false;
            if (++applied > COS_ELF_MAX_RELOCS) {
                elf_log("rejected: RELR expands to too many relocations");
                return false;
            }
        }
        where += 63 * 8;
    }
    return true;
}

bool cos_elf_relocate(page_directory_t *dir, cos_elf_object_t *obj,
                      cos_elf_symbol_resolver_t resolver, void *resolver_ctx)
{
    (void)dir;
    if (!obj) return false;
    if (!(obj->flags & COS_ELF_F_DYNAMIC)) return true;   /* nothing to do */

    /* The symbol count is needed before any lookup, because it is what
     * bounds them. Computed here rather than in cos_elf_map() because it
     * reads the hash table, which is only meaningful once the object is
     * mapped and its DT_* addresses are real. */
    if (obj->nsyms == 0) obj->nsyms = compute_nsyms(obj);

    uint64_t irq_flags = sync_irq_save();
    bool ok = true;

    /* .rela.dyn / .rel.dyn */
    if (ok) ok = apply_reloc_table(obj, obj->_rela, obj->_relasz, obj->_relaent,
                                   true, resolver, resolver_ctx);
    if (ok) ok = apply_reloc_table(obj, obj->_rel, obj->_relsz, obj->_relent,
                                   false, resolver, resolver_ctx);
    /* .relr.dyn */
    if (ok) ok = apply_relr_table(obj, obj->_relr, obj->_relrsz, obj->_relrent);
    /* .rela.plt / .rel.plt - a SEPARATE table from the above. A linker
     * that emits only PLT-bound relocations produces no DT_RELA at all,
     * so reading just one table silently processes nothing. */
    if (ok) ok = apply_reloc_table(obj, obj->_jmprel, obj->_pltrelsz,
                                   (obj->_pltrel == DT_REL) ? obj->_relent
                                                            : obj->_relaent,
                                   obj->_pltrel != DT_REL, resolver, resolver_ctx);

    sync_irq_restore(irq_flags);
    return ok;
}

/* ================================================================== */
/* Phase 4: final permissions                                          */
/* ================================================================== */

bool cos_elf_protect(page_directory_t *dir, const cos_elf_object_t *obj)
{
    (void)dir;
    if (!obj) return false;

    uint64_t irq_flags = sync_irq_save();
    bool ok = true;

    for (uint32_t i = 0; i < obj->seg_count; ++i) {
        for (uint64_t v = obj->segs[i].start; v < obj->segs[i].end; v += PAGE_SIZE) {
            /* Union across segments, not this segment's flags alone: a
             * page shared with a neighbouring segment must satisfy both,
             * and applying them one segment at a time would let whichever
             * ran last silently narrow the other's access. */
            uint32_t pf = 0;
            for (uint32_t j = 0; j < obj->seg_count; ++j) {
                if (v >= obj->segs[j].start && v < obj->segs[j].end) {
                    pf |= obj->segs[j].pflags;
                }
            }
            if (!paging_protect_page(v, pf_to_page_flags(pf))) {
                /* Not fatal, but it means this page kept the writable
                 * mapping the loader used - worth saying out loud rather
                 * than leaving a silently weaker W^X than advertised. */
                elf_log_hex("warning: could not set final permissions on page ", v);
                ok = false;
            }
        }
    }

    /* PT_GNU_RELRO last, so it overrides the segment permissions for the
     * range it covers - that range is inside a writable PT_LOAD by
     * construction, which is exactly why it needs a second pass.
     *
     * Both ends are aligned DOWN. Rounding the END up instead is a
     * tempting and completely wrong-looking-correct mistake: a real
     * linker ends RELRO mid-page, with .data and .bss sharing the rest
     * of that page, so rounding up write-protects the program's mutable
     * data and it faults on the first store. Confirmed against actual
     * `ld` output while building the fixtures for this - every PIE
     * tested has PT_GNU_RELRO ending exactly at a page boundary only
     * because the linker pads it there, and nothing in the format
     * guarantees that. */
    if (obj->flags & COS_ELF_F_RELRO) {
        uint64_t s = page_down(obj->relro_start);
        uint64_t e = page_down(obj->relro_start + obj->relro_size);
        for (uint64_t v = s; v < e; v += PAGE_SIZE) {
            uint64_t flags = PAGE_PRESENT | PAGE_USER;
            if (g_nx_available) flags |= COS_PAGE_NX;
            if (!paging_protect_page(v, flags)) {
                elf_log_hex("warning: could not apply RELRO to page ", v);
                ok = false;
            }
        }
    }

    sync_irq_restore(irq_flags);
    return ok;
}

bool cos_elf_protect_relro(page_directory_t *dir, const cos_elf_object_t *obj)
{
    return cos_elf_protect(dir, obj);
}

/* ================================================================== */
/* Thread-local storage                                                */
/* ================================================================== */
/*
 * x86-64 uses TLS variant II. The thread pointer (the %fs base) sits at
 * the TOP of the static TLS block, and every module's data is at a
 * NEGATIVE offset from it:
 *
 *      lower addresses
 *      +------------------+  <- block_addr
 *      | module N data    |
 *      | ...              |
 *      | module 1 data    |
 *      +------------------+  <- tp        *(void**)tp == tp
 *      | TCB (self ptr,   |
 *      |      DTV ptr)    |
 *      +------------------+
 *      | DTV[0..modules]  |
 *      +------------------+
 *      higher addresses
 *
 * The self-pointer is not decorative: `mov %fs:0, %rax` is how every
 * real libc materialises the thread pointer as an ordinary address,
 * because there is no instruction to read the segment base directly from
 * ring3 on older CPUs. Getting it wrong produces code that reads TLS
 * variables from address 0.
 */

#define COS_TLS_TCB_SIZE 64u   /* self pointer, DTV pointer, room to grow */

uint64_t cos_elf_tls_region_size(uint64_t block_size, uint32_t modules)
{
    return align_up(block_size + COS_TLS_TCB_SIZE +
                    ((uint64_t)modules + 1ULL) * 8ULL, 64);
}

bool cos_elf_tls_layout(cos_elf_object_t **objs, uint32_t count,
                        uint64_t *out_block_size, uint64_t *out_align)
{
    if (!objs || !out_block_size || !out_align) return false;

    uint64_t offset = 0, max_align = 8;
    int32_t modid = 0;

    for (uint32_t i = 0; i < count; ++i) {
        cos_elf_object_t *o = objs[i];
        if (!o) continue;
        if (!(o->flags & COS_ELF_F_HAS_TLS) || o->tls_memsz == 0) {
            o->tls_modid = -1;
            o->tls_offset = 0;
            continue;
        }
        uint64_t a = o->tls_align ? o->tls_align : 8;
        if (a & (a - 1)) a = 8;             /* not a power of two: distrust it */
        if (a > (1u << 16)) {
            elf_log("rejected: PT_TLS alignment is implausible");
            return false;
        }
        if (a > max_align) max_align = a;

        /* Variant II accumulation: the module's block ends `offset` bytes
         * below the thread pointer, and that distance must satisfy the
         * module's own alignment. */
        offset = align_up(offset + o->tls_memsz, a);
        if (offset > COS_ELF_MAX_SEGMENT) {
            elf_log("rejected: static TLS block is too large");
            return false;
        }
        o->tls_modid  = ++modid;
        o->tls_offset = (int64_t)offset;
    }

    *out_block_size = align_up(offset, max_align);
    *out_align = max_align;
    return true;
}

bool cos_elf_tls_init(page_directory_t *dir, uint64_t region, uint64_t region_size,
                      cos_elf_object_t **objs, uint32_t count,
                      cos_elf_tls_t *out)
{
    (void)dir;
    if (!objs || !out) return false;

    uint64_t block_size = 0, align = 8;
    if (!cos_elf_tls_layout(objs, count, &block_size, &align)) return false;

    uint32_t modules = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (objs[i] && objs[i]->tls_modid > 0) modules++;
    }

    uint64_t need = cos_elf_tls_region_size(block_size, modules);
    if (region_size < need) {
        elf_log("rejected: TLS region too small for the static TLS block");
        return false;
    }
    if (!user_range_ok(region, need)) {
        elf_log("rejected: TLS region outside the user address space");
        return false;
    }

    uint64_t irq_flags = sync_irq_save();
    bool ok = true;

    /* Zero first: a TLS variable with no initialiser must read as zero,
     * and the block must not expose whatever the frame held before. */
    ok = user_zero(region, need);

    /* The thread pointer must be aligned to the strictest module
     * alignment, since every module's data is at a fixed offset below
     * it. Align upward within the region rather than assuming `region`
     * itself was aligned. */
    uint64_t tp = align_up(region + block_size, align);
    if (ok && tp + COS_TLS_TCB_SIZE + ((uint64_t)modules + 1) * 8 > region + region_size) {
        elf_log("rejected: TLS alignment pushes the TCB past the region");
        ok = false;
    }

    uint64_t dtv = tp + COS_TLS_TCB_SIZE;

    for (uint32_t i = 0; i < count && ok; ++i) {
        cos_elf_object_t *o = objs[i];
        if (!o || o->tls_modid <= 0) continue;

        uint64_t dest = tp - (uint64_t)o->tls_offset;
        if (dest < region || dest + o->tls_memsz > tp) {
            elf_log("rejected: a TLS module does not fit in the computed block");
            ok = false; break;
        }
        if (o->tls_filesz) {
            /* Copy the module's initialisation image. Done through a
             * bounded bounce buffer because source and destination are
             * both in the target address space and may be far apart. */
            uint8_t buf[256];
            for (uint64_t off = 0; off < o->tls_filesz; ) {
                uint64_t n = o->tls_filesz - off;
                if (n > sizeof(buf)) n = sizeof(buf);
                if (!user_read(o->tls_vaddr + off, buf, n) ||
                    !user_write(dest + off, buf, n)) {
                    elf_log("failed: could not copy a TLS initialisation image");
                    ok = false; break;
                }
                off += n;
            }
        }
        if (ok) {
            uint64_t slot = dtv + (uint64_t)o->tls_modid * 8;
            ok = user_write(slot, (uint64_t[]){ dest }, 8);
        }
    }

    if (ok) ok = user_write(tp, (uint64_t[]){ tp }, 8);          /* self pointer */
    if (ok) ok = user_write(tp + 8, (uint64_t[]){ dtv }, 8);     /* DTV pointer  */
    if (ok) ok = user_write(dtv, (uint64_t[]){ modules }, 8);    /* DTV[0]=count */

    sync_irq_restore(irq_flags);
    if (!ok) return false;

    out->block_addr = region;
    out->block_size = block_size;
    out->tp         = tp;
    out->tcb_size   = COS_TLS_TCB_SIZE;
    out->dtv_addr   = dtv;
    out->modules    = modules;
    return true;
}

/* ================================================================== */
/* Initial process stack                                               */
/* ================================================================== */

/* A small pushdown cursor over the top of the stack region, so the
 * string-copying and array-writing below cannot silently run past the
 * budget. Every push checks the floor; one missed check is a kernel
 * write into whatever is mapped below the stack. */
typedef struct {
    uint64_t p;       /* current position, grows DOWN */
    uint64_t floor;   /* lowest address the builder may use */
    bool     ok;
} stack_cursor_t;

static uint64_t cursor_push(stack_cursor_t *c, const void *data, uint64_t len,
                            uint64_t alignment)
{
    if (!c->ok) return 0;
    c->p -= len;
    if (alignment > 1) c->p &= ~(alignment - 1);
    if (c->p < c->floor) { c->ok = false; return 0; }
    if (data && !user_write(c->p, data, len)) { c->ok = false; return 0; }
    if (!data && !user_zero(c->p, len)) { c->ok = false; return 0; }
    return c->p;
}

static uint64_t cursor_push_str(stack_cursor_t *c, const char *s)
{
    if (!c->ok) return 0;
    if (!s) s = "";
    uint64_t len = 0;
    while (s[len] != '\0' && len < COS_ELF_MAX_ARG_LEN) ++len;

    /* len + 1: the NUL is part of the reservation, not something written
     * past it. Reserving only `len` and then writing the terminator at
     * at+len puts it on the first byte of whatever was pushed
     * previously, silently corrupting the string above this one -
     * caught by the harness checking argv[2] and envp[0] rather than
     * only argv[0], which is the one string nothing overwrites. */
    uint64_t at = cursor_push(c, NULL, len + 1, 1);
    if (!c->ok) return 0;
    if (len && !user_write(at, s, len)) { c->ok = false; return 0; }
    uint8_t nul = 0;
    if (!user_write(at + len, &nul, 1)) { c->ok = false; return 0; }
    return at;
}

bool cos_elf_setup_stack_ex(page_directory_t *dir, uint64_t stack_top,
                            const cos_elf_stack_params_t *params,
                            uint64_t *out_rsp)
{
    if (!dir || !params || !out_rsp) return false;
    if (params->argc < 0 || params->argc > COS_ELF_MAX_ARGC) return false;
    if (params->envc < 0 || params->envc > COS_ELF_MAX_ENVC) return false;
    if (stack_top < COS_ELF_STACK_ARG_BUDGET) return false;

    uint64_t irq_flags = sync_irq_save();

    stack_cursor_t c = {
        .p     = (stack_top & ~0xFULL) - 16ULL,
        .floor = stack_top - COS_ELF_STACK_ARG_BUDGET,
        .ok    = true,
    };

    /* ---- strings and blobs, at the very top ---- */
    uint64_t arg_ptrs[COS_ELF_MAX_ARGC];
    uint64_t env_ptrs[COS_ELF_MAX_ENVC];

    /* AT_RANDOM: sixteen bytes a libc uses to seed its stack guard and
     * pointer mangling. Without it, glibc/musl startup reads uninitialised
     * stack and gets a guard value that is constant across runs - which
     * is worse than no guard, because it is a guard an attacker can
     * predict. */
    uint64_t random_addr = cursor_push(&c, params->at_random_seed, 16, 8);

    /* AT_PLATFORM is a string, not a value, and the pointer must remain
     * valid for the program's lifetime - hence on the stack, not in a
     * kernel buffer. */
    uint64_t platform_addr = cursor_push_str(&c, "x86_64");
    uint64_t execfn_addr = 0;
    if (params->execfn) execfn_addr = cursor_push_str(&c, params->execfn);

    for (int i = params->envc - 1; i >= 0 && c.ok; --i) {
        const char *s = (params->envp && params->envp[i]) ? params->envp[i] : "";
        env_ptrs[i] = cursor_push_str(&c, s);
    }
    for (int i = params->argc - 1; i >= 0 && c.ok; --i) {
        const char *s = (params->argv && params->argv[i]) ? params->argv[i] : "";
        arg_ptrs[i] = cursor_push_str(&c, s);
    }

    if (!c.ok) {
        sync_irq_restore(irq_flags);
        elf_log("failed: arguments and environment do not fit in the stack budget");
        return false;
    }

    /* ---- the auxiliary vector ---- */
    /* Assembled into a local array first so the total size is known
     * before anything is written: RSP has to be 16-byte aligned at the
     * ENTRY point, and that alignment depends on how many slots the whole
     * argc/argv/envp/auxv block occupies. */
    uint64_t aux[2 * 24];
    unsigned n = 0;
#define AUX(tag, val) do { if (n + 2 <= (unsigned)(sizeof(aux)/sizeof(aux[0]))) { \
                               aux[n++] = (uint64_t)(tag); aux[n++] = (uint64_t)(val); } } while (0)

    AUX(COS_AT_PAGESZ, PAGE_SIZE);
    AUX(COS_AT_CLKTCK, 100);
    if (params->at_phdr)  { AUX(COS_AT_PHDR,  params->at_phdr); }
    if (params->at_phent) { AUX(COS_AT_PHENT, params->at_phent); }
    if (params->at_phnum) { AUX(COS_AT_PHNUM, params->at_phnum); }
    if (params->at_entry) { AUX(COS_AT_ENTRY, params->at_entry); }
    AUX(COS_AT_BASE, params->at_base);
    AUX(COS_AT_FLAGS, 0);
    AUX(COS_AT_UID, 0); AUX(COS_AT_EUID, 0);
    AUX(COS_AT_GID, 0); AUX(COS_AT_EGID, 0);
    AUX(COS_AT_SECURE, params->secure ? 1 : 0);
    if (random_addr)   { AUX(COS_AT_RANDOM, random_addr); }
    if (platform_addr) { AUX(COS_AT_PLATFORM, platform_addr); }
    if (execfn_addr)   { AUX(COS_AT_EXECFN, execfn_addr); }
    /* HWCAP is reported as 0 rather than a fabricated feature mask: this
     * kernel does not probe or enable the XSAVE states a non-zero value
     * would promise, and a libc selecting an AVX memcpy on the strength
     * of a lie would fault. */
    AUX(COS_AT_HWCAP, 0);
    if (params->at_linkmap) { AUX(COS_AT_COS_LINKMAP, params->at_linkmap); }
    if (params->at_tp)      { AUX(COS_AT_COS_TP, params->at_tp); }
    AUX(COS_AT_NULL, 0);
#undef AUX

    /* ---- the pointer block ---- */
    uint64_t slots = 1ULL                        /* argc                  */
                   + (uint64_t)params->argc + 1  /* argv[] + NULL         */
                   + (uint64_t)params->envc + 1  /* envp[] + NULL         */
                   + n;                          /* auxv, including AT_NULL */

    uint64_t p = c.p & ~0xFULL;
    /* The ABI requires RSP % 16 == 0 at the entry point. The block is an
     * odd or even number of qwords depending on argc/envc, so one extra
     * qword of padding is inserted when needed. */
    if (slots & 1ULL) p -= 8ULL;
    p -= slots * 8ULL;
    if (p < c.floor || (p & 0xFULL) != 0) {
        sync_irq_restore(irq_flags);
        elf_log("failed: initial stack block does not fit or is misaligned");
        return false;
    }

    bool ok = true;
    uint64_t cur = p;
    uint64_t zero = 0;

    ok = user_write(cur, (uint64_t[]){ (uint64_t)params->argc }, 8); cur += 8;
    for (int i = 0; i < params->argc && ok; ++i) {
        ok = user_write(cur, &arg_ptrs[i], 8); cur += 8;
    }
    if (ok) { ok = user_write(cur, &zero, 8); cur += 8; }
    for (int i = 0; i < params->envc && ok; ++i) {
        ok = user_write(cur, &env_ptrs[i], 8); cur += 8;
    }
    if (ok) { ok = user_write(cur, &zero, 8); cur += 8; }
    for (unsigned i = 0; i < n && ok; ++i) {
        ok = user_write(cur, &aux[i], 8); cur += 8;
    }

    sync_irq_restore(irq_flags);

    if (!ok) {
        elf_log("failed: could not build the initial user stack");
        return false;
    }
    *out_rsp = p;
    return true;
}

bool cos_elf_setup_stack(page_directory_t *dir, uint64_t stack_top,
                         const char *const *argv, int argc, uint64_t *out_rsp)
{
    cos_elf_stack_params_t params;
    memset(&params, 0, sizeof(params));
    params.argv = argv;
    params.argc = argc;
    params.execfn = (argv && argc > 0) ? argv[0] : NULL;
    /* Deliberately weak "randomness" when the caller supplies none: this
     * is a seed value, and pretending it is unpredictable would be worse
     * than being explicit that it is not. Callers that care pass real
     * entropy in at_random_seed. */
    params.at_random_seed[0] = 0x5F3759DF5F3759DFULL ^ stack_top;
    params.at_random_seed[1] = 0x9E3779B97F4A7C15ULL ^ (stack_top >> 13);
    return cos_elf_setup_stack_ex(dir, stack_top, &params, out_rsp);
}

/* ================================================================== */
/* One-call entry points                                               */
/* ================================================================== */

bool cos_elf_load_program(page_directory_t *dir, const uint8_t *image,
                          uint64_t image_len, uint64_t bias,
                          cos_elf_object_t *out)
{
    if (!dir || !image || !out) return false;

    cos_elf_info_t info;
    if (!cos_elf_inspect(image, image_len, &info)) return false;

    if (info.e_type == ET_DYN && info.entry == 0) {
        elf_log("rejected: shared object has no entry point - load it as a "
                "library (.c-osll), not a program");
        return false;
    }
    if (info.flags & COS_ELF_F_INTERP) {
        /* An interpreter-dependent executable expects ld.so to be mapped
         * alongside it and given control first. That is a real thing this
         * loader could do, but pretending to run one without it would
         * jump straight into a program whose GOT was never populated. */
        elf_log2("rejected: this program requests an interpreter: ", info.interp);
        elf_log("          rebuild it static, static-pie, or link it against "
                ".c-osll libraries directly");
        return false;
    }
    if (info.needed_count > 0) {
        elf_log("rejected: program has DT_NEEDED dependencies - load it "
                "through the dynamic linker (cos_elf_link.c), not directly");
        return false;
    }

    if (info.e_type == ET_EXEC) bias = 0;
    else if (bias == 0) bias = COS_ELF_PIE_BASE;

    if (!cos_elf_map(dir, image, image_len, bias, out)) return false;

    /* Self-contained by construction (no DT_NEEDED), so a NULL resolver
     * is correct: any relocation that still names an external symbol
     * genuinely cannot be satisfied and correctly fails the load. */
    if (!cos_elf_relocate(dir, out, NULL, NULL)) return false;
    if (!cos_elf_protect(dir, out)) {
        /* Reported, not fatal: the image is correct and runnable, it just
         * has weaker page permissions than requested. Failing the load
         * here would make a permissions warning fatal to the program. */
        elf_log("warning: image loaded with weaker permissions than requested");
    }

    serial_puts("[ELF] loaded ");
    serial_putdec((uint64_t)out->seg_count);
    serial_puts(" segment(s), base=0x");
    serial_puthex(out->base);
    serial_puts(" entry=0x");
    serial_puthex(out->entry);
    serial_puts((out->flags & COS_ELF_F_PIE) ? " (PIE)\n" : " (fixed)\n");
    return true;
}

bool cos_elf_load(page_directory_t *dir, const uint8_t *image,
                  uint64_t image_len, uint64_t *out_entry)
{
    if (!out_entry) return false;
    cos_elf_object_t obj;
    if (!cos_elf_load_program(dir, image, image_len, 0, &obj)) return false;
    *out_entry = obj.entry;
    return true;
}

bool cos_elf_load_library(page_directory_t *dir, const uint8_t *image,
                          uint64_t image_len, uint64_t base,
                          cos_elf_symbol_resolver_t resolver, void *resolver_ctx,
                          cos_elf_object_t *out)
{
    if (!dir || !image || !out) return false;
    if (base == 0 || (base & (PAGE_SIZE - 1)) != 0) {
        elf_log("rejected: library base must be non-zero and page aligned");
        return false;
    }

    const elf64_ehdr_t *eh; const elf64_phdr_t *phs;
    if (!validate_header(image, image_len, &eh, &phs)) return false;
    if (eh->e_type != ET_DYN) {
        elf_log("rejected: .c-osll must be ET_DYN (a shared object)");
        return false;
    }

    if (!cos_elf_map(dir, image, image_len, base, out)) return false;
    out->flags = (out->flags & ~COS_ELF_F_PIE) | COS_ELF_F_LIB;

    if (!cos_elf_relocate(dir, out, resolver, resolver_ctx)) return false;
    if (!cos_elf_protect(dir, out)) {
        elf_log("warning: library loaded with weaker permissions than requested");
    }

    serial_puts("[ELF] loaded .c-osll at base=0x");
    serial_puthex(base);
    serial_puts(" span=0x");
    serial_puthex(out->map_end - out->map_start);
    serial_puts("\n");
    return true;
}

int cos_elf_read_needed(page_directory_t *dir, const uint8_t *image,
                        uint64_t image_len, char out_names[][256], int max_names)
{
    (void)dir;
    if (!image || !out_names || max_names <= 0) return -1;

    cos_elf_info_t info;
    if (!cos_elf_inspect(image, image_len, &info)) return -1;

    int n = 0;
    for (uint32_t i = 0; i < info.needed_count && n < max_names; ++i) {
        uint64_t k = 0;
        for (; k < 255 && info.needed[i][k]; ++k) out_names[n][k] = info.needed[i][k];
        out_names[n][k] = '\0';
        if (out_names[n][0]) n++;
    }
    return n;
}
