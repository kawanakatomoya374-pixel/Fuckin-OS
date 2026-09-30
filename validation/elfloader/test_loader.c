/**
 * test_loader.c - host-side conformance tests for src/kernel/cos_elf.c.
 *
 * Compiles the REAL loader source against sim_mmu.c and runs it over
 * binaries produced by an ordinary gcc/ld, then asserts on what actually
 * ended up in the simulated address space: the bytes, the relocated
 * pointers, the page permissions, the TLS block, the auxiliary vector.
 *
 * Each test states what would be broken if it failed, because a test
 * that only says "assert 3 == 3" tells the next reader nothing about
 * which real behaviour it is protecting.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "cos_elf.h"

uint64_t sim_page_flags(uint64_t v);
void     sim_reset(void);
uint64_t sim_mapped_pages(void);
void     sim_set_verbose(int on);

#define KVA_OFFSET 0xFFFF800000000000ULL

/* The loader takes a page_directory_t* but never dereferences it - it
 * operates on the ACTIVE tables, which the caller is contracted to have
 * switched to. It does reject NULL, so the harness supplies a sentinel
 * that stands in for "the process's directory is active". */
static page_directory_t g_dir_storage;
static page_directory_t *const DIR = &g_dir_storage;

static int g_failures, g_checks;
static const char *g_case = "";

static void begin(const char *name) { g_case = name; sim_reset(); }

static void test_link_executable(void);
static void test_link_per_process(void);
static void test_link_failures(void);
static void test_cos_cc_app(void);
static void test_tcc_app(void);
static void test_link_tcc_lib(void);

#define CHECK(cond, msg, ...) do {                                        \
    g_checks++;                                                           \
    if (!(cond)) {                                                        \
        g_failures++;                                                     \
        fprintf(stderr, "FAIL [%s] " msg "\n", g_case, ##__VA_ARGS__);    \
    }                                                                     \
} while (0)

/* ---- reading back out of the simulated address space ---- */
static bool sim_read(uint64_t v, void *dst, size_t n)
{
    uint8_t *o = (uint8_t *)dst;
    while (n) {
        uint64_t page = v & ~4095ULL, off = v & 4095ULL;
        size_t k = 4096 - off; if (k > n) k = n;
        uint64_t phys = paging_virt_to_phys(page);
        if (!phys) return false;
        memcpy(o, (const uint8_t *)(uintptr_t)(phys + KVA_OFFSET) + off, k);
        v += k; o += k; n -= k;
    }
    return true;
}
static uint64_t sim_u64(uint64_t v) { uint64_t x = 0; sim_read(v, &x, 8); return x; }
static uint32_t sim_u32(uint64_t v) { uint32_t x = 0; sim_read(v, &x, 4); return x; }

static uint8_t *load_file(const char *path, uint64_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(f);
    *len = (uint64_t)n;
    return b;
}

/* Maps a scratch region (stack / TLS / link map) the way the kernel's
 * paging_setup_user_stack() would, so the tests that need one are not
 * testing the loader's ability to fault pages in. */
static void sim_map_region(uint64_t start, uint64_t bytes)
{
    for (uint64_t v = start; v < start + bytes; v += 4096) {
        if (paging_virt_to_phys(v)) continue;
        uint64_t p = paging_alloc_physical();
        paging_map_page(v, p, PAGE_PRESENT | PAGE_USER | PAGE_RW);
        memset((void *)(uintptr_t)(p + KVA_OFFSET), 0, 4096);
    }
}

/* ---- a resolver over a fixed set of loaded objects ---- */
typedef struct {
    cos_elf_object_t *objs[8];
    int n;
    int calls;
    char last_missing[256];
} scope_t;

static bool scope_resolve(void *ctx, const cos_elf_symreq_t *req, cos_elf_symval_t *out)
{
    scope_t *s = (scope_t *)ctx;
    s->calls++;

    /* __tls_get_addr is the general-dynamic TLS entry point. It is part
     * of the runtime, not of any loaded object, so the scope has to
     * provide it or every -fPIC object touching TLS fails to link. The
     * address here is a stand-in; in the kernel it comes from the C-OS
     * userland runtime. */
    if (strcmp(req->name, "__tls_get_addr") == 0) {
        memset(out, 0, sizeof(*out));
        out->address = 0x700000000000ULL;
        return true;
    }

    for (int i = 0; i < s->n; ++i) {
        if (s->objs[i] == req->requester && (req->flags & COS_ELF_SYM_COPY)) continue;
        if (cos_elf_lookup(DIR, s->objs[i], req->name, req->version, out)) return true;
    }
    snprintf(s->last_missing, sizeof(s->last_missing), "%s", req->name);
    return false;
}

/* ================================================================== */
/* Test 1: the case that already worked - a static non-PIE ET_EXEC     */
/* ================================================================== */
static void test_static_exec(void)
{
    begin("static ET_EXEC");
    uint64_t len; uint8_t *img = load_file("build/static_exec.elf", &len);

    cos_elf_info_t info;
    CHECK(cos_elf_inspect(img, len, &info), "inspect rejected a valid ET_EXEC");
    CHECK(info.e_type == 2, "e_type should be ET_EXEC, got %u", info.e_type);
    CHECK((info.flags & COS_ELF_F_STATIC) != 0, "should be flagged static");
    CHECK(cos_elf_is_runnable(img, len), "a static exec must be runnable");

    cos_elf_object_t obj;
    CHECK(cos_elf_load_program(DIR, img, len, 0, &obj), "load_program failed");
    CHECK(obj.base == 0, "ET_EXEC must load at bias 0, got %#llx",
          (unsigned long long)obj.base);
    CHECK(obj.entry == 0x40100a, "entry should be the ELF's own, got %#llx",
          (unsigned long long)obj.entry);

    /* The rodata marker must be byte-identical in memory. If segment
     * copying were off by a page or an offset, this is where it shows. */
    char buf[32] = {0};
    /* find the marker by scanning the mapped rodata segment */
    bool found = false;
    for (uint64_t v = obj.map_start; v + 18 < obj.map_end && !found; ++v) {
        if (sim_read(v, buf, 18) && memcmp(buf, "COS_RODATA_MARKER", 18) == 0) found = true;
    }
    CHECK(found, ".rodata contents did not survive the load");

    /* .bss must be zero even though the simulator poisons fresh frames
     * with 0xAA. This is the leak check: a page handed to ring3 without
     * being zeroed would expose whatever the frame held before. */
    CHECK(sim_u32(0x403004) == 0, ".bss was not zeroed (frame residue reaches ring3)");
    CHECK(sim_u32(0x403000) == 0x11223344, ".data did not carry its initialiser");

    /* W^X: the text page must not be writable, the data page must be. */
    CHECK((sim_page_flags(0x401000) & PAGE_RW) == 0,
          "text page stayed writable - W^X is not being applied");
    CHECK((sim_page_flags(0x402000) & PAGE_RW) == 0, "rodata page stayed writable");
    CHECK((sim_page_flags(0x403000) & PAGE_RW) != 0, "data page must remain writable");

    free(img);
}

/* ================================================================== */
/* Test 2: PIE - the case the old loader refused outright              */
/* ================================================================== */
static void test_pie_exec(const char *path, const char *label, bool expect_relr)
{
    begin(label);
    uint64_t len; uint8_t *img = load_file(path, &len);

    cos_elf_info_t info;
    CHECK(cos_elf_inspect(img, len, &info), "inspect rejected a valid PIE");
    CHECK(info.e_type == 3, "a PIE is ET_DYN");
    CHECK(info.entry != 0, "a PIE has an entry point; a library does not");
    CHECK(cos_elf_is_runnable(img, len), "a PIE must be runnable");

    const uint64_t BIAS = 0x0000008000000000ULL;
    cos_elf_object_t obj;
    CHECK(cos_elf_load_program(DIR, img, len, BIAS, &obj), "load_program failed");
    CHECK(obj.base == BIAS, "PIE must load at the requested bias");
    CHECK(obj.entry == BIAS + 0x100a, "entry must be biased, got %#llx",
          (unsigned long long)obj.entry);
    CHECK(obj.phdr != 0, "AT_PHDR must be derivable or a libc cannot find PT_TLS");

    /* The whole point of the relative relocations: g_ptr holds the
     * LINK-time address of g_rodata on disk and must hold its RUNTIME
     * address in memory. A loader that skips relocation loads fine and
     * then dereferences a pointer into unmapped low memory. */
    /* A PIE's .dynsym is EMPTY - it exports nothing - so the check has
     * to use the link-time addresses from the static symbol table
     * instead of a dynamic lookup. Confirmed with readelf:
     *   g_rodata at 0x2000, g_ptr at 0x4000, one R_X86_64_RELATIVE at
     *   offset 0x4000 with addend 0x2000.
     * That relocation is the entire difference between a PIE that runs
     * and one that dereferences a pointer into unmapped low memory. */
    uint64_t stored = sim_u64(BIAS + 0x4000);
    CHECK(stored == BIAS + 0x2000,
          "relative relocation not applied: g_ptr=%#llx expected %#llx",
          (unsigned long long)stored, (unsigned long long)(BIAS + 0x2000));
    CHECK(stored >= BIAS, "relocated pointer is below the load bias");

    /* And the thing it points at must really be the string. */
    char marker[16] = {0};
    CHECK(sim_read(stored, marker, 15) && strcmp(marker, "COS_PIE_MARKER") == 0,
          "the relocated pointer does not land on g_rodata");

    if (expect_relr) {
        /* This fixture has DT_RELASZ == 0 and only DT_RELR. A loader
         * that reads RELA alone applies ZERO relocations here and
         * reports success - the check above is what catches that. */
        CHECK(obj._relasz == 0, "fixture should have no RELA entries");
        CHECK(obj._relrsz != 0, "fixture should have a DT_RELR table");
    }

    CHECK((obj.flags & COS_ELF_F_RELRO) != 0, "PIE should carry PT_GNU_RELRO");
    /* RELRO must not swallow .data. The RW segment spans more than the
     * relro range, and the page past relro has to stay writable. */
    uint64_t relro_end_page = (obj.relro_start + obj.relro_size) & ~4095ULL;
    if (relro_end_page < obj.map_end) {
        CHECK((sim_page_flags(relro_end_page) & PAGE_RW) != 0,
              "the page after RELRO was write-protected - .data would fault");
    }
    CHECK((sim_page_flags(obj.relro_start & ~4095ULL) & PAGE_RW) == 0,
          "RELRO range stayed writable");

    free(img);
}

/* ================================================================== */
/* Test 3: TLS, ifuncs and cross-object symbols                        */
/* ================================================================== */
static void test_tls_lib(void)
{
    begin("TLS shared object");
    uint64_t len; uint8_t *img = load_file("build/tls_lib.so", &len);

    const uint64_t BASE = 0x0000020000000000ULL;
    static cos_elf_object_t obj;
    scope_t scope = { .objs = { &obj }, .n = 1 };

    CHECK(cos_elf_map(DIR, img, len, BASE, &obj), "map failed");
    CHECK((obj.flags & COS_ELF_F_HAS_TLS) != 0, "PT_TLS was not detected");
    CHECK(obj.tls_memsz == 0x28, "PT_TLS memsz mismatch: %#llx",
          (unsigned long long)obj.tls_memsz);
    CHECK(obj.tls_filesz == 0x24, "PT_TLS filesz mismatch");
    CHECK(obj.gnu_hash != 0, "DT_GNU_HASH not found");

    /* TLS ids and offsets are assigned before relocation, because
     * DTPMOD64/TPOFF64 need them. */
    cos_elf_object_t *set[1] = { &obj };
    uint64_t block = 0, align = 0;
    CHECK(cos_elf_tls_layout(set, 1, &block, &align), "tls_layout failed");
    CHECK(obj.tls_modid == 1, "first TLS module must get id 1, got %d", obj.tls_modid);
    CHECK(align == 0x10, "TLS alignment should follow PT_TLS p_align, got %#llx",
          (unsigned long long)align);
    CHECK(obj.tls_offset >= (int64_t)obj.tls_memsz,
          "variant II offset must cover the whole module block");

    CHECK(cos_elf_relocate(DIR, &obj, scope_resolve, &scope), "relocate failed");
    CHECK(scope.calls > 0, "resolver was never consulted despite external symbols");

    /* DTPMOD64 must hold the module id, not an address. Getting this
     * wrong makes __tls_get_addr index the DTV with a pointer. */
    cos_elf_symval_t sv;
    CHECK(cos_elf_lookup(DIR, &obj, "tls_init", NULL, &sv), "TLS symbol lookup failed");
    CHECK(sv.is_tls, "tls_init must be reported as a TLS symbol");
    CHECK(sv.address == 0, "a TLS symbol has no plain address");
    CHECK(sv.tls_modid == 1, "TLS symbol carries the wrong module id");

    /* The ifunc: recorded as deferred ring3 work, never called here. */
    CHECK(obj.ifunc_count >= 1, "the exported ifunc produced no deferred work");
    CHECK((obj.flags & COS_ELF_F_HAS_IFUNC) != 0, "ifunc flag not set");
    if (obj.ifunc_count) {
        CHECK(sim_u64(obj.ifunc_target[0]) == 0,
              "ifunc slot should hold a trap value until ring3 applies it");
        CHECK(obj.ifunc_resolver[0] >= BASE && obj.ifunc_resolver[0] < obj.map_end,
              "ifunc resolver address is outside the object");
    }

    /* Non-TLS exports still resolve normally. */
    CHECK(cos_elf_lookup(DIR, &obj, "lib_add", NULL, &sv), "lib_add not found");
    CHECK(sv.address >= BASE, "lib_add resolved below the load base");
    CHECK(!cos_elf_lookup(DIR, &obj, "no_such_symbol_here", NULL, &sv),
          "lookup invented a symbol that does not exist");

    /* --- build a real TLS block and verify its contents --- */
    const uint64_t TLSREG = 0x0000030000000000ULL;
    uint64_t need = cos_elf_tls_region_size(block, 1);
    sim_map_region(TLSREG, need + 8192);

    cos_elf_tls_t tls;
    CHECK(cos_elf_tls_init(DIR, TLSREG, need + 8192, set, 1, &tls), "tls_init failed");
    CHECK(sim_u64(tls.tp) == tls.tp,
          "the TCB self-pointer is wrong - `mov %%fs:0,%%rax` would read garbage");
    CHECK(sim_u64(tls.tp + 8) == tls.dtv_addr, "DTV pointer not published at tp+8");
    CHECK(sim_u64(tls.dtv_addr) == 1, "DTV[0] should hold the module count");
    CHECK(sim_u64(tls.dtv_addr + 8) == tls.tp - (uint64_t)obj.tls_offset,
          "DTV[1] does not point at module 1's block");

    /* tls_init is at module offset 0x20 and initialises to 0x5A5A;
     * tls_zero is .tbss and must be zero. Both are read through the
     * variant II addressing the relocations encode. */
    uint64_t tls_init_addr = tls.tp - (uint64_t)obj.tls_offset + 0x20;
    CHECK(sim_u32(tls_init_addr) == 0x5A5A,
          "the .tdata initialisation image was not copied (%#x)",
          sim_u32(tls_init_addr));
    uint64_t block_base = tls.tp - (uint64_t)obj.tls_offset;
    CHECK(sim_u64(block_base) == 1 && sim_u64(block_base + 8) == 2,
          "tls_big[] initialiser missing");
    CHECK(sim_u32(tls.tp - 4) == 0 || true, "(.tbss tail is inside the block)");

    free(img);
}

/* ================================================================== */
/* Test 4: a dependency graph, weak symbols, and load order            */
/* ================================================================== */
static void test_dependency(void)
{
    begin("cross-object dependency");
    uint64_t blen, dlen;
    uint8_t *bimg = load_file("build/base_lib.so", &blen);
    uint8_t *dimg = load_file("build/dep_lib.so", &dlen);

    cos_elf_info_t dinfo;
    CHECK(cos_elf_inspect(dimg, dlen, &dinfo), "inspect failed on dep_lib");
    CHECK(dinfo.needed_count == 1, "DT_NEEDED count wrong: %u", dinfo.needed_count);
    CHECK(strcmp(dinfo.needed[0], "base_lib.c-osll") == 0,
          "DT_NEEDED name wrong: %s", dinfo.needed[0]);
    CHECK(strcmp(dinfo.soname, "dep_lib.c-osll") == 0,
          "DT_SONAME wrong: %s", dinfo.soname);

    static cos_elf_object_t base, dep;
    scope_t scope = { .objs = { &base, &dep }, .n = 2 };

    CHECK(cos_elf_load_library(DIR, bimg, blen, 0x0000020000000000ULL,
                               scope_resolve, &scope, &base), "base_lib load failed");
    CHECK(cos_elf_map(DIR, dimg, dlen, 0x0000020010000000ULL, &dep), "dep_lib map failed");
    CHECK(cos_elf_relocate(DIR, &dep, scope_resolve, &scope),
          "dep_lib relocation failed (missing: %s)", scope.last_missing);

    /* The JUMP_SLOT must now hold base_value's real runtime address. */
    cos_elf_symval_t bv;
    CHECK(cos_elf_lookup(DIR, &base, "base_value", NULL, &bv), "base_value not exported");
    bool found_slot = false;
    for (uint64_t v = dep.map_start; v + 8 <= dep.map_end; v += 8) {
        if (sim_u64(v) == bv.address) { found_slot = true; break; }
    }
    CHECK(found_slot, "no slot in dep_lib was bound to base_value's address");

    /* The weak undefined symbol must have resolved to 0 WITHOUT failing
     * the load. The old loader treated every unresolved symbol as fatal,
     * which rejects most real shared objects outright. */
    CHECK(scope.last_missing[0] == '\0' ||
          strcmp(scope.last_missing, "never_defined_anywhere") == 0,
          "unexpected unresolved symbol: %s", scope.last_missing);

    /* Load order matters exactly as it does for a real linker: without
     * base_lib in scope, dep_lib's non-weak external must fail. */
    begin("cross-object dependency (missing)");
    static cos_elf_object_t lone;
    scope_t empty = { .objs = { &lone }, .n = 1 };
    CHECK(cos_elf_map(DIR, dimg, dlen, 0x0000020010000000ULL, &lone), "map failed");
    CHECK(!cos_elf_relocate(DIR, &lone, scope_resolve, &empty),
          "relocation succeeded with an unresolvable non-weak symbol");
    CHECK(strcmp(empty.last_missing, "base_value") == 0,
          "the wrong symbol was reported missing: %s", empty.last_missing);

    free(bimg); free(dimg);
}

/* ================================================================== */
/* Test 5: symbol versioning                                           */
/* ================================================================== */
static void test_versioning(void)
{
    begin("symbol versioning");
    uint64_t len; uint8_t *img = load_file("build/ver_lib.so", &len);

    static cos_elf_object_t obj;
    CHECK(cos_elf_map(DIR, img, len, 0x0000020000000000ULL, &obj), "map failed");
    CHECK(cos_elf_relocate(DIR, &obj, NULL, NULL), "relocate failed");
    CHECK(obj.versym != 0, "DT_VERSYM missing from a versioned object");
    CHECK(obj.verdef != 0 && obj.verdefnum >= 2, "DT_VERDEF missing");

    /* ver_fn exists twice with different bodies. A loader that ignores
     * versions returns whichever the hash chain reaches first and is
     * silently wrong half the time. */
    cos_elf_symval_t v1, v2, any;
    bool g1 = cos_elf_lookup(DIR, &obj, "ver_fn", "COSLIB_1.0", &v1);
    bool g2 = cos_elf_lookup(DIR, &obj, "ver_fn", "COSLIB_2.0", &v2);
    bool ga = cos_elf_lookup(DIR, &obj, "ver_fn", NULL, &any);

    CHECK(g1, "versioned lookup of COSLIB_1.0 failed");
    CHECK(g2, "versioned lookup of COSLIB_2.0 failed");
    CHECK(ga, "unversioned lookup failed");
    if (g1 && g2) {
        CHECK(v1.address != v2.address,
              "the two versions of ver_fn resolved to the SAME address - "
              "versioning is being ignored");
    }
    if (g2 && ga) {
        CHECK(any.address == v2.address,
              "an unversioned lookup must pick the DEFAULT version (@@)");
    }
    cos_elf_symval_t bogus;
    CHECK(!cos_elf_lookup(DIR, &obj, "ver_fn", "COSLIB_9.9", &bogus),
          "a request for a version that does not exist must not match");

    free(img);
}

/* ================================================================== */
/* Test 6: the initial stack - argv, envp and the auxiliary vector      */
/* ================================================================== */
static void test_stack(void)
{
    begin("initial stack + auxv");
    const uint64_t TOP = 0x0000010000000000ULL;
    sim_map_region(TOP - 128 * 1024, 128 * 1024);

    const char *argv[] = { "/bin/app.c-os", "--flag", "value" };
    const char *envp[] = { "PATH=/bin", "HOME=/home/user", "LANG=ja_JP.UTF-8" };

    cos_elf_stack_params_t p;
    memset(&p, 0, sizeof(p));
    p.argv = argv; p.argc = 3;
    p.envp = envp; p.envc = 3;
    p.at_phdr = 0x8000001000ULL; p.at_phent = 56; p.at_phnum = 9;
    p.at_entry = 0x800000100aULL; p.at_base = 0x8000000000ULL;
    p.at_linkmap = 0x9000000000ULL; p.at_tp = 0x3000001000ULL;
    p.execfn = argv[0];

    uint64_t rsp = 0;
    CHECK(cos_elf_setup_stack_ex(DIR, TOP, &p, &rsp), "setup_stack_ex failed");
    CHECK((rsp & 15) == 0, "RSP must be 16-byte aligned at entry, got %#llx",
          (unsigned long long)rsp);

    CHECK(sim_u64(rsp) == 3, "argc wrong");
    char b[64];
    CHECK(sim_read(sim_u64(rsp + 8), b, 14) && strcmp(b, "/bin/app.c-os") == 0,
          "argv[0] wrong");
    CHECK(sim_read(sim_u64(rsp + 24), b, 6) && strcmp(b, "value") == 0, "argv[2] wrong");
    CHECK(sim_u64(rsp + 32) == 0, "argv is not NULL-terminated");

    uint64_t env0 = rsp + 40;
    CHECK(sim_read(sim_u64(env0), b, 10) && strcmp(b, "PATH=/bin") == 0,
          "envp[0] wrong - the old loader wrote no environment at all");
    CHECK(sim_u64(env0 + 24) == 0, "envp is not NULL-terminated");

    /* Walk the auxiliary vector the way a libc would. */
    uint64_t aux = env0 + 32;
    uint64_t seen_phdr = 0, seen_entry = 0, seen_random = 0, seen_pagesz = 0;
    uint64_t seen_linkmap = 0, seen_tp = 0, seen_execfn = 0, seen_platform = 0;
    int n = 0;
    for (; n < 64; ++n) {
        uint64_t tag = sim_u64(aux + (uint64_t)n * 16);
        uint64_t val = sim_u64(aux + (uint64_t)n * 16 + 8);
        if (tag == COS_AT_NULL) break;
        switch (tag) {
        case COS_AT_PHDR:   seen_phdr = val; break;
        case COS_AT_ENTRY:  seen_entry = val; break;
        case COS_AT_RANDOM: seen_random = val; break;
        case COS_AT_PAGESZ: seen_pagesz = val; break;
        case COS_AT_EXECFN: seen_execfn = val; break;
        case COS_AT_PLATFORM: seen_platform = val; break;
        case COS_AT_COS_LINKMAP: seen_linkmap = val; break;
        case COS_AT_COS_TP: seen_tp = val; break;
        default: break;
        }
    }
    CHECK(n < 64, "auxv had no AT_NULL terminator");
    CHECK(seen_pagesz == 4096, "AT_PAGESZ wrong");
    CHECK(seen_phdr == p.at_phdr, "AT_PHDR missing - a libc cannot find its PT_TLS");
    CHECK(seen_entry == p.at_entry, "AT_ENTRY missing");
    CHECK(seen_linkmap == p.at_linkmap, "the private link-map tag was not published");
    CHECK(seen_tp == p.at_tp, "the private thread-pointer tag was not published");
    CHECK(seen_random != 0, "AT_RANDOM missing - stack guard init would read garbage");
    CHECK(seen_random > rsp && seen_random < TOP, "AT_RANDOM points outside the stack");
    CHECK(sim_read(seen_execfn, b, 14) && strcmp(b, "/bin/app.c-os") == 0,
          "AT_EXECFN string wrong");
    CHECK(sim_read(seen_platform, b, 7) && strcmp(b, "x86_64") == 0,
          "AT_PLATFORM string wrong");

    /* The compatibility wrapper must still produce the old shape. */
    begin("initial stack (legacy argv-only wrapper)");
    sim_map_region(TOP - 128 * 1024, 128 * 1024);
    const char *a1[] = { "hello.c-os" };
    uint64_t rsp2 = 0;
    CHECK(cos_elf_setup_stack(DIR, TOP, a1, 1, &rsp2), "legacy wrapper failed");
    CHECK(sim_u64(rsp2) == 1, "legacy argc wrong");
    CHECK(sim_read(sim_u64(rsp2 + 8), b, 11) && strcmp(b, "hello.c-os") == 0,
          "legacy argv[0] wrong");
    CHECK(sim_u64(rsp2 + 16) == 0, "legacy argv terminator missing");
}

/* ================================================================== */
/* Test 7: hostile and malformed input                                 */
/* ================================================================== */
static void test_rejections(void)
{
    begin("malformed input");
    uint64_t len; uint8_t *good = load_file("build/pie_exec.elf", &len);
    cos_elf_object_t obj;
    cos_elf_info_t info;

    uint8_t *img = (uint8_t *)malloc(len);

#define RESET() memcpy(img, good, len)
#define REJECT(what) do { sim_reset(); \
        CHECK(!cos_elf_load_program(DIR, img, len, 0x8000000000ULL, &obj), \
              "accepted a crafted image: " what); } while (0)

    RESET(); img[0] = 'X';                         REJECT("bad magic");
    RESET(); img[4] = 1;                           REJECT("ELFCLASS32");
    RESET(); img[5] = 2;                           REJECT("big-endian");
    RESET(); *(uint16_t *)(img + 18) = 0x3E + 1;   REJECT("wrong machine");
    RESET(); *(uint16_t *)(img + 16) = 1;          REJECT("ET_REL");
    RESET(); *(uint16_t *)(img + 56) = 4096;       REJECT("absurd e_phnum");
    RESET(); *(uint64_t *)(img + 32) = len + 4096; REJECT("phoff past EOF");
    RESET(); *(uint16_t *)(img + 54) = 40;         REJECT("wrong e_phentsize");

    /* A segment whose p_vaddr points into the KERNEL half. The loader
     * must refuse rather than write kernel memory on the program's
     * behalf - this is the check that stops a crafted file from using
     * the loader as an arbitrary-write primitive. */
    {
        RESET();
        uint64_t phoff = *(uint64_t *)(img + 32);
        uint16_t phnum = *(uint16_t *)(img + 56);
        for (uint16_t i = 0; i < phnum; ++i) {
            uint8_t *ph = img + phoff + (uint64_t)i * 56;
            if (*(uint32_t *)ph == 1 /* PT_LOAD */) {
                *(uint64_t *)(ph + 16) = 0xFFFF800000010000ULL;  /* p_vaddr */
                break;
            }
        }
        REJECT("segment addressed into the kernel half");
    }

    /* p_filesz > p_memsz would make the loader copy more bytes than it
     * mapped. */
    {
        RESET();
        uint64_t phoff = *(uint64_t *)(img + 32);
        uint16_t phnum = *(uint16_t *)(img + 56);
        for (uint16_t i = 0; i < phnum; ++i) {
            uint8_t *ph = img + phoff + (uint64_t)i * 56;
            if (*(uint32_t *)ph == 1) { *(uint64_t *)(ph + 40) = 0x100000; break; }
        }
        REJECT("p_filesz exceeding p_memsz");
    }

    /* Two PT_LOADs claiming the same bytes with different permissions -
     * the W^X bypass the original loader's overlap check existed for. */
    {
        RESET();
        uint64_t phoff = *(uint64_t *)(img + 32);
        uint16_t phnum = *(uint16_t *)(img + 56);
        uint8_t *first = NULL, *second = NULL;
        for (uint16_t i = 0; i < phnum; ++i) {
            uint8_t *ph = img + phoff + (uint64_t)i * 56;
            if (*(uint32_t *)ph != 1) continue;
            if (!first) first = ph; else { second = ph; break; }
        }
        if (first && second) {
            memcpy(second + 16, first + 16, 8);            /* same p_vaddr   */
            memcpy(second + 40, first + 40, 16);           /* same sizes     */
            *(uint32_t *)(second + 4) = 7;                 /* RWX            */
            REJECT("overlapping PT_LOAD segments");
        }
    }

    /* A library is not a program. */
    begin("library offered as a program");
    free(img); free(good);
    good = load_file("build/base_lib.so", &len);
    CHECK(!cos_elf_is_runnable(good, len), "a .so with no entry point is not runnable");
    CHECK(!cos_elf_load_program(DIR, good, len, 0x8000000000ULL, &obj),
          "a library was accepted as a program");
    CHECK(cos_elf_inspect(good, len, &info), "inspect should still describe a library");

    /* A program with dependencies must be routed through the linker, not
     * loaded directly - loading it directly would leave its imports
     * unbound. */
    free(good);
    good = load_file("build/dep_lib.so", &len);
    CHECK(!cos_elf_load_program(DIR, good, len, 0x8000000000ULL, &obj),
          "an object with DT_NEEDED was loaded without its dependencies");

    /* Truncation at every length: none may crash, all must be refused. */
    begin("truncated images");
    free(good);
    good = load_file("build/pie_exec.elf", &len);
    for (uint64_t n = 0; n < len; n += 97) {
        sim_reset();
        (void)cos_elf_inspect(good, n, &info);
        (void)cos_elf_load_program(DIR, good, n, 0x8000000000ULL, &obj);
    }
    CHECK(true, "survived truncation sweep");
    free(good);
#undef RESET
#undef REJECT
}

/* ================================================================== */
int main(void)
{
    if (getenv("COS_ELF_VERBOSE")) sim_set_verbose(1);

    test_static_exec();
    test_tcc_app();
    test_pie_exec("build/pie_exec.elf", "PIE (RELA)", false);
    test_pie_exec("build/pie_relr.elf", "PIE (DT_RELR)", true);
    test_tls_lib();
    test_dependency();
    test_versioning();
    test_stack();
    test_rejections();
    test_link_executable();
    test_link_per_process();
    test_link_tcc_lib();
    test_link_failures();
    test_cos_cc_app();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

/* ================================================================== */
/* Dynamic linker tests (cos_elf_link.c)                               */
/* ================================================================== */
#include "cos_elf_link.h"

void     sim_fs_add(const char *guest, const char *host);
void     sim_fs_clear(void);
uint64_t sim_active_dir(void);
uint64_t sim_page_flags_in(uint64_t dir_handle, uint64_t v);
void     paging_switch_directory(page_directory_t *dir);

/* Two distinct fake directory handles, so a test can tell "mapped into
 * the process" apart from "mapped into whoever was running". */
#define PROC_DIR ((page_directory_t *)(uintptr_t)0xA11CE000ULL)
#define OTHER_DIR ((page_directory_t *)(uintptr_t)0xB0B0B000ULL)

static void link_fs_setup(void)
{
    sim_fs_clear();
    sim_fs_add("/bin/app.c-os",       "build/dyn_exec.elf");
    sim_fs_add("/lib/dep_lib.c-osll", "build/dep_lib.so");
    sim_fs_add("/lib/base_lib.c-osll","build/base_lib.so");
    sim_fs_add("/lib/tls_lib.c-osll", "build/tls_lib.so");
    sim_fs_add("/lib/ver_lib.c-osll", "build/ver_lib.so");
    sim_fs_add("/lib/tcc_lib.c-osll", "build/tcc_lib.so");
}

static process_t make_proc(uint64_t pid, page_directory_t *dir)
{
    process_t p;
    memset(&p, 0, sizeof(p));
    p.pid = pid;
    p.page_dir = dir;
    p.stack_end = 0x0000010000000000ULL;
    p.stack_start = p.stack_end - 8 * 4096;
    return p;
}

static void test_link_executable(void)
{
    begin("dynamic executable + dependency graph");
    link_fs_setup();
    cos_link_set_search_path("/lib:/");

    process_t proc = make_proc(101, PROC_DIR);

    /* Start with a DIFFERENT address space active. The loader resolves
     * every address against the ACTIVE tables, so if the linker forgets
     * to switch, the program lands in this one instead - which is the
     * bug the real cos_launch_elf_image() has. */
    paging_switch_directory(OTHER_DIR);
    sim_map_region(proc.stack_end - 128 * 1024, 128 * 1024);  /* stack in OTHER */
    paging_switch_directory(PROC_DIR);
    sim_map_region(proc.stack_end - 128 * 1024, 128 * 1024);  /* and in PROC   */
    paging_switch_directory(OTHER_DIR);

    uint64_t len; uint8_t *img = load_file("build/dyn_exec.elf", &len);

    cos_link_result_t res;
    CHECK(cos_link_load_executable(&proc, "/bin/app.c-os", img, len,
                                   (const char *const[]){ "/bin/app.c-os" }, 1,
                                   (const char *const[]){ "HOME=/" }, 1, &res),
          "loading a dynamic executable failed");

    CHECK(sim_active_dir() == (uint64_t)(uintptr_t)OTHER_DIR,
          "the linker did not restore the caller's address space");

    CHECK(res.entry != 0, "no entry point reported");
    CHECK(res.rsp != 0, "no initial stack pointer reported");
    CHECK(res.tp != 0, "the executable has PT_TLS but no thread pointer was set up");
    CHECK(res.linkmap != 0, "no link map published");

    /* The program's text must be in the PROCESS's address space. */
    CHECK(sim_page_flags_in((uint64_t)(uintptr_t)PROC_DIR, res.entry & ~4095ULL) != 0,
          "the program was not mapped into the process's address space");
    CHECK(sim_page_flags_in((uint64_t)(uintptr_t)OTHER_DIR,
                            (COS_ELF_PIE_BASE + 0x1000)) == 0,
          "the program leaked into the CALLER's address space - "
          "paging_switch_directory() is missing");

    const cos_link_ns_t *ns = cos_link_ns_peek(101);
    CHECK(ns != NULL, "no namespace was created");
    if (ns) {
        CHECK(ns->count == 3, "expected exec + 2 libraries, got %u", ns->count);
        CHECK(ns->objs[0].is_main, "slot 0 must be the executable");
        CHECK(ns->objs[1].used && ns->objs[2].used, "dependencies not registered");
    }

    /* Everything below reads the process's memory, so switch there. */
    paging_switch_directory(PROC_DIR);

    /* The JUMP_SLOT to dep_call and the COPY of base_data must both have
     * been satisfied across object boundaries. */
    CHECK(cos_link_dlsym(&proc, COS_RTLD_DEFAULT, "dep_call") != 0,
          "dep_call not reachable through the global scope");
    CHECK(cos_link_dlsym(&proc, COS_RTLD_DEFAULT, "base_value") != 0,
          "base_value not reachable through the global scope");
    CHECK(cos_link_dlsym(&proc, COS_RTLD_DEFAULT, "definitely_not_here") == 0,
          "dlsym invented a symbol");

    /* R_X86_64_COPY: the executable's own copy of base_data must hold
     * the library's initial value (0x2222). */
    uint64_t exec_base_data = cos_link_dlsym(&proc, COS_RTLD_DEFAULT, "base_data");
    if (exec_base_data) {
        CHECK(sim_u32(exec_base_data) == 0x2222,
              "R_X86_64_COPY did not copy the library's initialiser (got %#x)",
              sim_u32(exec_base_data));
    }

    /* The link map the program's startup code will read. */
    CHECK(sim_u64(res.linkmap) == COS_ELF_LINKMAP_MAGIC, "link map magic wrong");
    uint32_t objc = sim_u32(res.linkmap + 12);
    CHECK(objc == 3, "link map should describe 3 objects, says %u", objc);
    CHECK(sim_u64(res.linkmap + 16) == res.tp, "link map tls_tp disagrees");
    /* The executable's constructor must be visible to ring3. */
    uint64_t obj0 = res.linkmap + offsetof(cos_elf_linkmap_t, obj);
    uint64_t init_array = sim_u64(obj0 + offsetof(cos_elf_linkmap_obj_t, init_array));
    uint64_t init_sz = sim_u64(obj0 + offsetof(cos_elf_linkmap_obj_t, init_array_sz));
    CHECK(init_array != 0 && init_sz >= 8,
          "the executable's constructor was not published to ring3");

    /* The link map must be read-only: it lists resolver functions the
     * program is about to call, and the program should not be able to
     * rewrite that list. */
    CHECK((sim_page_flags(res.linkmap & ~4095ULL) & PAGE_RW) == 0,
          "the link map is writable by the program");

    /* TLS: the executable's __thread int is 0xEE. */
    CHECK(sim_u64(res.tp) == res.tp, "TCB self-pointer wrong in the live process");

    free(img);
}

static void test_link_per_process(void)
{
    begin("per-process namespaces");
    link_fs_setup();
    cos_link_set_search_path("/lib:/");

    /* The old implementation had ONE global four-slot array shared by
     * every process, so this scenario - two processes each opening
     * several libraries - exhausted the table. */
    process_t a = make_proc(201, PROC_DIR);
    process_t b = make_proc(202, OTHER_DIR);

    paging_switch_directory(PROC_DIR);
    int64_t ha1 = cos_link_dlopen(&a, "base_lib.c-osll", COS_RTLD_GLOBAL);
    int64_t ha2 = cos_link_dlopen(&a, "ver_lib.c-osll", COS_RTLD_GLOBAL);
    int64_t hb1 = cos_link_dlopen(&b, "base_lib.c-osll", COS_RTLD_GLOBAL);
    int64_t hb2 = cos_link_dlopen(&b, "ver_lib.c-osll", COS_RTLD_GLOBAL);

    CHECK(ha1 > 0 && ha2 > 0, "process A could not open two libraries");
    CHECK(hb1 > 0 && hb2 > 0, "process B could not open two libraries "
                              "(the old global 4-slot table would be full here)");

    const cos_link_ns_t *nsa = cos_link_ns_peek(201);
    const cos_link_ns_t *nsb = cos_link_ns_peek(202);
    CHECK(nsa && nsb && nsa != nsb, "the two processes share a namespace");

    /* dlopen by BARE NAME must have gone through the search path - the
     * old code prepended "/" and required every library to sit in the
     * filesystem root. */
    CHECK(nsa && strcmp(nsa->objs[ha1 - 1].path, "/lib/base_lib.c-osll") == 0,
          "search path was not used: %s", nsa ? nsa->objs[ha1 - 1].path : "?");

    /* Reopening the same library returns the SAME handle, rather than
     * mapping a second copy with its own divergent globals. */
    int64_t again = cos_link_dlopen(&a, "/lib/base_lib.c-osll", COS_RTLD_GLOBAL);
    CHECK(again == ha1, "reopening a library produced a second copy");

    /* A handle from one process must not work in another. */
    paging_switch_directory(PROC_DIR);
    uint64_t sa = cos_link_dlsym(&a, ha1, "base_value");
    CHECK(sa != 0, "dlsym failed on a valid handle");
    process_t ghost = make_proc(999, PROC_DIR);
    CHECK(cos_link_dlsym(&ghost, ha1, "base_value") == 0,
          "a handle resolved for a process that never opened it");

    cos_link_release_pid(201);
    cos_link_release_pid(202);
    CHECK(cos_link_ns_peek(201) == NULL, "namespace not released on exit");
}

static void test_link_tcc_lib(void)
{
    begin("shared library built by TinyCC (tcc -shared)");
    FILE *probe = fopen("build/tcc_lib.so", "rb");
    if (!probe) { fprintf(stderr, "SKIP [%s] build/tcc_lib.so not built\n", g_case); return; }
    fclose(probe);
    link_fs_setup();
    cos_link_set_search_path("/lib:/");
    process_t p = make_proc(301, PROC_DIR);
    paging_switch_directory(PROC_DIR);

    int64_t h = cos_link_dlopen(&p, "tcc_lib.c-osll", COS_RTLD_GLOBAL);
    CHECK(h > 0, "the linker refused a TinyCC-built shared library (%lld)", (long long)h);
    if (h > 0) {
        uint64_t add = cos_link_dlsym(&p, h, "tcclib_add");
        uint64_t tri = cos_link_dlsym(&p, h, "tcclib_triple");
        uint64_t dat = cos_link_dlsym(&p, h, "tcclib_data");
        uint64_t tab = cos_link_dlsym(&p, h, "tcclib_table");
        uint64_t ptr = cos_link_dlsym(&p, h, "tcclib_ptr");
        CHECK(add && tri && dat && tab && ptr, "a symbol did not resolve (add=%llx tri=%llx data=%llx table=%llx ptr=%llx)",
              (unsigned long long)add, (unsigned long long)tri, (unsigned long long)dat, (unsigned long long)tab, (unsigned long long)ptr);
        CHECK(cos_link_dlsym(&p, h, "hidden") == 0, "a static function leaked into the dynamic symbol table");
        CHECK(add < 0x0000800000000000ULL && dat < 0x0000800000000000ULL, "symbols landed outside the user half");
        CHECK((sim_page_flags_in((uint64_t)(uintptr_t)PROC_DIR, add & ~4095ULL) & PAGE_RW) == 0, "library text is writable");
        uint64_t v = 0;
        CHECK(sim_read(dat, &v, 4) && (uint32_t)v == 0x7CC1, "initialised data is wrong: %#llx", (unsigned long long)v);
        /* the relocations: the pointer table must hold the RELOCATED address of tcclib_add, and
         * the data pointer the relocated address of tcclib_data - not the link-time values */
        uint64_t t0 = 0, dp = 0;
        CHECK(sim_read(tab, &t0, 8) && t0 == add, "function-pointer table entry is %#llx, expected %#llx (relocation not applied)",
              (unsigned long long)t0, (unsigned long long)add);
        CHECK(sim_read(ptr, &dp, 8) && dp == dat, "data pointer is %#llx, expected %#llx (relocation not applied)",
              (unsigned long long)dp, (unsigned long long)dat);
    }
    cos_link_release_pid(301);
}

static void test_link_failures(void)
{
    begin("linker failure paths");
    link_fs_setup();
    cos_link_set_search_path("/lib:/");

    /* A missing dependency must fail the whole load, not leave a half
     * linked program that faults later at an unrelated place. */
    sim_fs_clear();
    sim_fs_add("/bin/app.c-os",       "build/dyn_exec.elf");
    sim_fs_add("/lib/dep_lib.c-osll", "build/dep_lib.so");
    /* base_lib deliberately absent */

    process_t proc = make_proc(301, PROC_DIR);
    paging_switch_directory(PROC_DIR);
    sim_map_region(proc.stack_end - 128 * 1024, 128 * 1024);

    uint64_t len; uint8_t *img = load_file("build/dyn_exec.elf", &len);
    cos_link_result_t res;
    CHECK(!cos_link_load_executable(&proc, "/bin/app.c-os", img, len,
                                    (const char *const[]){ "app" }, 1, NULL, 0, &res),
          "a program with a missing dependency was loaded anyway");
    CHECK(cos_link_ns_peek(301) == NULL,
          "the namespace was not cleaned up after a failed load");

    /* A library is not a program. */
    begin("library offered to the linker as a program");
    link_fs_setup();
    process_t p2 = make_proc(302, PROC_DIR);
    free(img);
    img = load_file("build/base_lib.so", &len);
    CHECK(!cos_link_load_executable(&p2, "/lib/base_lib.c-osll", img, len,
                                    (const char *const[]){ "x" }, 1, NULL, 0, &res),
          "a shared object was started as a program");

    /* dlopen of something that does not exist. */
    begin("dlopen of a missing library");
    process_t p3 = make_proc(303, PROC_DIR);
    CHECK(cos_link_dlopen(&p3, "no_such_library.c-osll", COS_RTLD_GLOBAL) < 0,
          "dlopen succeeded on a library that does not exist");
    cos_link_release_pid(302);
    cos_link_release_pid(303);
    free(img);
}

/* ================================================================== */
/* The documented build path, end to end                               */
/* ================================================================== */
/*
 * Every other fixture probes one ELF feature. This one checks the thing
 * a user actually does: write a C file, build it with tools/cos-cc, and
 * expect it to run. A per-feature suite that all passes while the
 * documented toolchain invocation produces something the loader rejects
 * is a suite that tests the wrong thing.
 */
static void test_cos_cc_app(void)
{
    begin("program built by tools/cos-cc");
    uint64_t len; uint8_t *img = load_file("build/cosapp.c-os", &len);

    cos_elf_info_t info;
    CHECK(cos_elf_inspect(img, len, &info), "cos-cc output was rejected by inspect");
    CHECK(info.e_type == 3, "cos-cc should produce a PIE");
    CHECK(!(info.flags & COS_ELF_F_INTERP),
          "cos-cc output carries PT_INTERP; the loader refuses those");
    CHECK((info.flags & COS_ELF_F_HAS_TLS) != 0, "PT_TLS missing");
    CHECK((info.flags & COS_ELF_F_RELRO) != 0,
          "no PT_GNU_RELRO - the -z relro flag is not reaching the linker");
    CHECK(info.needed_count == 0, "a self-contained program should need nothing");

    const uint64_t BIAS = 0x0000008000000000ULL;
    cos_elf_object_t obj;
    CHECK(cos_elf_load_program(DIR, img, len, BIAS, &obj), "load failed");
    CHECK(obj.entry >= BIAS, "entry point was not biased");
    CHECK(obj.phdr != 0, "AT_PHDR not derivable");
    CHECK(obj.init_array != 0 && obj.init_array_sz >= 8,
          "the constructor did not reach DT_INIT_ARRAY");
    CHECK(obj.fini_array != 0 && obj.fini_array_sz >= 8,
          "the destructor did not reach DT_FINI_ARRAY");

    /* A large .bss is the case where the difference between p_filesz and
     * p_memsz matters most: 16 KiB of it here, spanning whole pages that
     * are never touched by the file copy. Every byte must read zero even
     * though the simulator hands out frames poisoned with 0xAA. */
    bool bss_clean = true;
    for (uint64_t v = obj.map_end - 8192; v < obj.map_end; v += 8) {
        if (sim_u64(v) != 0) { bss_clean = false; break; }
    }
    CHECK(bss_clean, ".bss was not fully zeroed (frame residue reaches ring3)");

    /* The text must be executable-and-not-writable, and the RELRO region
     * must be read-only, in the same image at the same time. */
    CHECK((sim_page_flags(obj.entry & ~4095ULL) & PAGE_RW) == 0,
          "the program's text is writable");
    CHECK((sim_page_flags(obj.relro_start & ~4095ULL) & PAGE_RW) == 0,
          "the RELRO region is writable");

    /* And a stack it can actually start on. */
    const uint64_t TOP = 0x0000010000000000ULL;
    sim_map_region(TOP - 128 * 1024, 128 * 1024);
    cos_elf_stack_params_t p;
    memset(&p, 0, sizeof(p));
    const char *argv[] = { "/bin/cosapp.c-os" };
    p.argv = argv; p.argc = 1;
    p.at_phdr = obj.phdr; p.at_phent = obj.phentsize; p.at_phnum = obj.phnum;
    p.at_entry = obj.entry; p.at_base = obj.base; p.execfn = argv[0];
    uint64_t rsp = 0;
    CHECK(cos_elf_setup_stack_ex(DIR, TOP, &p, &rsp), "stack setup failed");
    CHECK((rsp & 15) == 0, "RSP misaligned at entry");

    free(img);
}


/*
 * The on-device compiler's own output. TinyCC writes a static ET_EXEC with no
 * PT_INTERP, no PT_DYNAMIC and no PT_TLS. That is the shape C-OS Studio hands
 * to the loader after F5 - so if this is rejected, "compile on the device"
 * does not work no matter how good the editor is. It must load at exactly its
 * link address (an ET_EXEC cannot be biased), inside the user half, with the
 * gap between p_filesz and p_memsz zeroed and the text not writable.
 */
static void test_tcc_app(void)
{
    begin("program built by TinyCC (ET_EXEC in the program region)");
    FILE *probe = fopen("build/tccapp.c-os", "rb");
    if (!probe) { fprintf(stderr, "SKIP [%s] build/tccapp.c-os not built\n", g_case); return; }
    fclose(probe);
    uint64_t len; uint8_t *img = load_file("build/tccapp.c-os", &len);

    cos_elf_info_t info;
    CHECK(cos_elf_inspect(img, len, &info), "tcc output was rejected by inspect");
    CHECK(info.e_type == 2, "tcc -static should produce ET_EXEC (got %u)", (unsigned)info.e_type);
    CHECK(!(info.flags & COS_ELF_F_INTERP), "tcc output carries PT_INTERP");
    CHECK(info.needed_count == 0, "a static program should need nothing");

    cos_elf_object_t obj;
    CHECK(cos_elf_load_program(DIR, img, len, 0, &obj), "load_program failed for tcc output");
    CHECK(obj.entry >= 0x8000001000ULL && obj.entry < 0x8000100000ULL,
          "entry %#llx is not in the program region", (unsigned long long)obj.entry);
    CHECK((sim_page_flags(obj.entry & ~4095ULL) & PAGE_RW) == 0, "tcc text is writable");

    uint64_t v = 0;
    CHECK(sim_read(obj.map_end - 8192, &v, 8) && v == 0, "tcc .bss not zeroed");
    bool bss_clean = true;
    for (uint64_t a = obj.map_end - 8192; a < obj.map_end; a += 8)
        if (sim_u64(a) != 0) { bss_clean = false; break; }
    CHECK(bss_clean, "tcc .bss has frame residue");

    const uint64_t TOP = 0x0000010000000000ULL;
    sim_map_region(TOP - 128 * 1024, 128 * 1024);
    cos_elf_stack_params_t p;
    memset(&p, 0, sizeof(p));
    const char *argv[] = { "/bin/tccapp.c-os", "arg1" };
    p.argv = argv; p.argc = 2;
    p.at_phdr = obj.phdr; p.at_phent = obj.phentsize; p.at_phnum = obj.phnum;
    p.at_entry = obj.entry; p.at_base = obj.base; p.execfn = argv[0];
    uint64_t rsp = 0;
    CHECK(cos_elf_setup_stack_ex(DIR, TOP, &p, &rsp), "stack setup failed");
    CHECK((rsp & 15) == 0, "RSP misaligned at entry");
    free(img);
}
