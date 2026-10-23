/**
 * fuzz_loader.c - throws damaged ELF files at the real loader (src/kernel/cos_elf.c and cos_elf_link.c, compiled against the simulated MMU) under
 * AddressSanitizer and UBSan, through every way a program or a library gets loaded:
 *   cos_elf_inspect / cos_elf_is_runnable / cos_elf_read_needed   (what the shell and the launcher ask first)
 *   cos_elf_load_program                                          (a program by itself)
 *   cos_link_load_executable                                      (a program with its libraries, TLS, stack)
 *   cos_link_dlopen                                               (a .c-osll opened by a running program)
 * Inputs are the fixtures the conformance test uses, changed by bit flips, field-targeted overwrites (offsets, sizes, counts set to values that
 * sit on the edge of what a parser trips over), truncation and splicing. A run is repeatable: fuzz_loader [iterations] [seed].
 * A finding is a sanitizer report, a crash, or a loop that does not end; the input being tried is always in /tmp/cos_fuzz_cur.bin.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include "cos_elf.h"
#include "cos_elf_link.h"

void     sim_reset(void);
#define KVA_OFFSET 0xFFFF800000000000ULL
uint64_t paging_alloc_physical(void);
static void sim_map_region(uint64_t start, uint64_t bytes) {                /* a scratch region (stack, TLS, link map), as paging_setup_user_stack() would map it */
    for (uint64_t v = start; v < start + bytes; v += 4096) {
        if (paging_virt_to_phys(v)) continue;
        uint64_t p = paging_alloc_physical(); paging_map_page(v, p, PAGE_PRESENT | PAGE_USER | PAGE_RW);
        memset((void *)(uintptr_t)(p + KVA_OFFSET), 0, 4096);
    }
}
void     sim_fs_add(const char *guest, const char *host);
void     sim_fs_clear(void);
void     paging_switch_directory(page_directory_t *dir);
#define PROC_DIR ((page_directory_t *)(uintptr_t)0xA11CE000ULL)
#define OTHER_DIR ((page_directory_t *)(uintptr_t)0xB0B0B000ULL)
static page_directory_t g_dir_storage;
#define DIR (&g_dir_storage)

static const char *replay_file;
static uint64_t rs = 88172645463325252ULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint64_t below(uint64_t n) { return n ? rnd() % n : 0; }

static uint8_t *load_file(const char *path, uint64_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n); if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2); fclose(f); *len = (uint64_t)n; return b;
}
static const uint64_t INTERESTING[] = { 0, 1, 2, 3, 4, 7, 8, 0x38, 0x40, 0xFF, 0x100, 0xFFF, 0x1000, 0x1001, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0xFFFFFFFFULL,
    0x100000000ULL, 0x7FFFFFFFULL, 0x80000000ULL, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFF8ULL,
    0x0000800000000000ULL, 0x00007FFFFFFFF000ULL, 0xFFFF800000000000ULL, 0xFFFFFFFF80000000ULL, 0x8000000000ULL, 0x8000001000ULL };
#define NINT (sizeof INTERESTING / sizeof INTERESTING[0])

/* structure-aware damage: finds the program headers, the dynamic section and the tables it points to in the (already valid) image and damages one field of them */
typedef struct { uint8_t *b; uint64_t len; } img_t;
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static bool vaddr_to_off(img_t *im, uint64_t va, uint64_t *off) {
    if (im->len < 64) return false; uint64_t phoff = rd64(im->b + 32); uint16_t phn; memcpy(&phn, im->b + 56, 2);
    for (uint16_t i = 0; i < phn && i < 64; i++) {
        uint64_t o = phoff + 56ULL * i; if (o + 56 > im->len) return false; const uint8_t *ph = im->b + o; uint32_t t; memcpy(&t, ph, 4);
        uint64_t po = rd64(ph + 8), pv = rd64(ph + 16), pf = rd64(ph + 32);
        if (t == 1 && va >= pv && va - pv < pf && po + (va - pv) < im->len) { *off = po + (va - pv); return true; }
    }
    return false;
}
static const char *EVIL_NAMES[] = { "../../etc/passwd", "../base_lib.c-osll", "/lib/../lib/base_lib.c-osll", "//////", "", "./", "..", "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p", "%s%s%s%n", "base_lib.c-osll/", "\xff\xfe" };
static void mutate_struct(img_t *im) {
    uint8_t *b = im->b; uint64_t len = im->len; if (len < 64 || b[0] != 0x7F) return;
    uint64_t phoff = rd64(b + 32); uint16_t phn; memcpy(&phn, b + 56, 2); if (phn > 64) phn = 64;
    uint64_t v = INTERESTING[below(NINT)];
    switch (below(7)) {
    case 0: { if (!phn) return; uint64_t o = phoff + 56ULL * below(phn) + 8 * below(7); if (o + 8 <= len) memcpy(b + o, &v, 8); break; }          /* a program header field */
    case 1: { if (!phn) return; uint64_t o = phoff + 56ULL * below(phn), c = phoff + 56ULL * below(phn); if (o + 56 <= len && c + 56 <= len) memcpy(b + o, b + c, 56); break; }   /* duplicate a header */
    case 2: { if (!phn) return; uint64_t o = phoff + 56ULL * below(phn); uint32_t t = (uint32_t)below(10) | (below(4) == 0 ? 0x60000000u + (uint32_t)below(0x20000000) : 0); if (o + 4 <= len) memcpy(b + o, &t, 4); break; }   /* a new segment type */
    case 3: case 4: case 5: {                                                                                                         /* a dynamic entry, or one of the tables it points at */
        for (uint16_t i = 0; i < phn; i++) {
            uint64_t o = phoff + 56ULL * i; if (o + 56 > len) return; uint32_t t; memcpy(&t, b + o, 4); if (t != 2) continue;
            uint64_t dof = rd64(b + o + 8), dsz = rd64(b + o + 32); if (dof >= len) return; if (dsz > len - dof) dsz = len - dof; uint64_t n = dsz / 16; if (!n) return;
            uint64_t e = dof + 16 * below(n); uint64_t val = rd64(b + e + 8); uint64_t r = below(3), to;
            if (r == 0) { memcpy(b + e + (below(2) ? 8 : 0), &v, 8); return; }
            if (r == 1) { uint64_t tag = below(40); memcpy(b + e, &tag, 8); return; }
            /* follow the entry's pointer into a table and damage 8 bytes there, or put a hostile name in the string table */
            if (vaddr_to_off(im, val, &to) && to + 64 < len) {
                if (below(3) == 0) { const char *nm = EVIL_NAMES[below(sizeof EVIL_NAMES / sizeof *EVIL_NAMES)]; size_t l = strlen(nm); memcpy(b + to, nm, l + 1); if (below(3) == 0) { uint64_t k = below(len - to); memset(b + to, 'A', k < 5000 ? k : 5000); } }
                else { uint64_t o2 = to + (below(64) & ~7ULL); memcpy(b + o2, &v, 8); }
            } else memcpy(b + e + 8, &v, 8);
            return;
        }
        break; }
    case 6: { uint64_t fields[] = { 24, 32, 40, 48, 52, 54, 56, 58, 60, 62 }; uint64_t o = fields[below(10)]; memcpy(b + o, &v, o < 48 ? 8 : 2); break; }  /* ELF header: entry, phoff, shoff, sizes, counts */
    }
}

/* one damaged copy of seed[0..len) in out (len may change) */
static uint64_t mutate(const uint8_t *seed, uint64_t len, uint8_t *out, uint64_t cap) {
    if (len > cap) len = cap; memcpy(out, seed, len);
    int rounds = 1 + (int)below(4); if (below(2) == 0) { img_t im = { out, len }; for (int k = 1 + (int)below(3); k--; ) mutate_struct(&im); }
    for (int r = 0; r < rounds && len > 16; r++) {
        switch (below(6)) {
        case 0: { int k = 1 + (int)below(8); while (k--) out[below(len)] ^= (uint8_t)(1u << below(8)); break; }                 /* bit flips */
        case 1: { uint64_t o = below(len); out[o] = (uint8_t)rnd(); break; }                                                     /* one byte */
        case 2: { uint64_t reach = len < 4096 ? len : 4096; uint64_t o = below(reach) & ~7ULL; uint64_t v = INTERESTING[below(NINT)];   /* a 64-bit field near the start */
                  if (o + 8 <= len) memcpy(out + o, &v, 8); break; }
        case 3: { uint64_t o = below(len) & ~3ULL; uint32_t v = (uint32_t)INTERESTING[below(NINT)]; if (o + 4 <= len) memcpy(out + o, &v, 4); break; }   /* a 32-bit field anywhere */
        case 4: if (below(3) == 0) len = 1 + below(len); break;                                                                    /* truncate */
        case 5: { uint64_t a = below(len), b = below(len), n = 1 + below(64); if (a + n <= len && b + n <= len) memmove(out + a, out + b, n); break; }   /* splice */
        }
    }
    return len;
}

static void save_current(const uint8_t *img, uint64_t len) { FILE *f = fopen("/tmp/cos_fuzz_cur.bin", "wb"); if (f) { fwrite(img, 1, len, f); fclose(f); } }
static const char *g_stage = "";
static void on_alarm(int s) { (void)s; static const char m[] = "\nFUZZ: the loader did not return (loop?) - input in /tmp/cos_fuzz_cur.bin, stage: "; (void)!write(2, m, sizeof m - 1); (void)!write(2, g_stage, strlen(g_stage)); (void)!write(2, "\n", 1); _exit(3); }

static void libs_setup(void) {
    sim_fs_clear();
    sim_fs_add("/bin/app.c-os", "build/dyn_exec.elf");
    sim_fs_add("/lib/dep_lib.c-osll", "build/dep_lib.so"); sim_fs_add("/lib/base_lib.c-osll", "build/base_lib.so");
    sim_fs_add("/lib/tls_lib.c-osll", "build/tls_lib.so"); sim_fs_add("/lib/ver_lib.c-osll", "build/ver_lib.so");
}
static process_t make_proc(uint64_t pid, page_directory_t *dir) {
    process_t p; memset(&p, 0, sizeof p); p.pid = pid; p.page_dir = dir; p.stack_end = 0x0000010000000000ULL; p.stack_start = p.stack_end - 8 * 4096; return p;
}
static void map_stacks(process_t *proc) {
    paging_switch_directory(OTHER_DIR); sim_map_region(proc->stack_end - 128 * 1024, 128 * 1024);
    paging_switch_directory(PROC_DIR);  sim_map_region(proc->stack_end - 128 * 1024, 128 * 1024);
    paging_switch_directory(OTHER_DIR);
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--replay")) { replay_file = argv[2]; argc = 1; }
    long iters = argc > 1 ? atol(argv[1]) : 20000; if (argc > 2) rs ^= (uint64_t)atoll(argv[2]) * 0x9E3779B97F4A7C15ULL;
    static const char *seeds[] = { "build/pie_exec.elf", "build/static_exec.elf", "build/dyn_exec.elf", "build/pie_relr.elf", "build/ifunc_exec.elf", "build/cosapp.c-os",
                                   "build/tls_lib.so", "build/ver_lib.so", "build/base_lib.so", "build/dep_lib.so" };
    int nseed = (int)(sizeof seeds / sizeof seeds[0]); uint8_t *seed[16]; uint64_t slen[16];
    for (int i = 0; i < nseed; i++) seed[i] = load_file(seeds[i], &slen[i]);
    static uint8_t buf[1 << 20]; signal(SIGALRM, on_alarm);
    long accepted = 0, tried = 0;
    for (long it = 0; it < iters; it++) {
        uint64_t len; if (replay_file) { uint64_t n; uint8_t *r = load_file(replay_file, &n); memcpy(buf, r, n); len = n; if (it) break; }
        else { int s = (int)below((uint64_t)nseed); len = mutate(seed[s], slen[s], buf, sizeof buf); }
        if (!replay_file) save_current(buf, len); alarm(replay_file ? 60 : 10); tried++;
        cos_elf_info_t info; cos_elf_object_t obj; char names[8][256];
        g_stage = "inspect"; sim_reset(); (void)cos_elf_inspect(buf, len, &info); (void)cos_elf_is_runnable(buf, len); (void)cos_elf_read_needed(DIR, buf, len, names, 8);
        g_stage = "load_program (static base)"; sim_reset(); if (cos_elf_load_program(DIR, buf, len, 0x8000000000ULL, &obj)) accepted++;
        g_stage = "load_program (default base)"; sim_reset(); (void)cos_elf_load_program(DIR, buf, len, 0, &obj);
        /* a program with libraries (the mutated file is the program; or a library that the good program pulls in) */
        g_stage = "link_load_executable"; sim_reset(); libs_setup(); cos_link_set_search_path("/lib:/");
        {   process_t proc = make_proc(101, PROC_DIR); map_stacks(&proc); cos_link_result_t res;
            (void)cos_link_load_executable(&proc, "/bin/app.c-os", buf, len, (const char *const[]){ "/bin/app.c-os", "arg" }, 2, (const char *const[]){ "HOME=/" }, 1, &res); }
        /* the damaged file as a library: written where the linker looks, then opened with dlopen */
        g_stage = "dlopen"; sim_reset(); libs_setup(); cos_link_set_search_path("/lib:/");
        {   FILE *f = fopen("/tmp/cos_fuzz_lib.so", "wb"); if (f) { fwrite(buf, 1, len, f); fclose(f); }
            sim_fs_add("/lib/fuzz.c-osll", "/tmp/cos_fuzz_lib.so"); sim_fs_add("/lib/ver_lib.c-osll", "/tmp/cos_fuzz_lib.so");
            process_t proc = make_proc(102, PROC_DIR); map_stacks(&proc);
            int64_t h = cos_link_dlopen(&proc, "fuzz.c-osll", COS_RTLD_GLOBAL); if (h > 0) { (void)cos_link_dlsym(&proc, h, "ver_fn"); (void)cos_link_dlclose(&proc, h); }
            (void)cos_link_dlopen(&proc, "ver_lib.c-osll", COS_RTLD_LOCAL); }
        alarm(0);
    }
    printf("fuzz: %ld inputs tried, %ld accepted as programs, no sanitizer report\n", tried, accepted);
    return 0;
}
