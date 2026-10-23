/*
 * p2c_elfcheck: binutils（readelf / nm / file）に依存しない ELF64 検査器。
 *
 * TinyCC だけで Python Code to C をビルド・検証できるように、Makefile と
 * テストスクリプトの ELF 検査はこのツールで行う。対象は ELF64 リトルエンディアン
 * （x86-64 / AArch64）。外部コマンドを一切呼ばないため、どのホストでも同じ判定になる。
 *
 * 使い方:
 *   p2c_elfcheck [検査オプション...] FILE   マニフェストを表示し、指定した検査を行う
 *   p2c_elfcheck --undef FILE               未定義の大域シンボル名を1行ずつ表示する
 *
 * 検査オプション（指定したものだけを検査する。違反があれば終了コード 1）:
 *   --exec            ET_EXEC（実行形式）であること
 *   --rel             ET_REL（再配置可能オブジェクト）であること
 *   --static          PT_INTERP / PT_DYNAMIC を持たないこと（動的リンクでないこと）
 *   --no-undef        未定義の大域シンボルが無いこと（シンボル表が無ければ違反）
 *
 * W と X を同時に持つ PT_LOAD（W^X 違反）は、オプションに関係なく常に違反とする。
 *   --entry-sym NAME  e_entry が NAME の値と一致すること（複数指定可、いずれか一致で合格）
 *
 * 終了コード: 0 = 合格, 1 = 検査違反, 2 = 使い方の誤り・解析不能
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u
#define ET_REL 1u
#define ET_EXEC 2u
#define ET_DYN 3u
#define SHT_SYMTAB 2u
#define SHN_UNDEF 0u
#define STB_WEAK 2u
#define STT_SECTION 3u
#define STT_FILE 4u

#define EHDR_SIZE 64u
#define PHDR_SIZE 56u
#define SHDR_SIZE 64u
#define SYM_SIZE 24u

typedef struct {
    unsigned char *buf;
    size_t size;
    const char *path;
} Image;

typedef struct {
    uint16_t type;
    uint16_t machine;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint16_t phnum;
    uint16_t shnum;
    uint16_t shstrndx;
} Ehdr;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t memsz;
} Phdr;

typedef struct {
    uint32_t name;
    uint32_t type;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
} Shdr;

typedef struct {
    int exec;
    int rel;
    int is_static;
    int no_undef;
    int undef_only;
    size_t n_entry_sym;
    const char *entry_sym[16];
} Options;

static int report_error(const char *msg, const char *detail) {
    if (detail) fprintf(stderr, "p2c_elfcheck: %s: %s\n", msg, detail);
    else fprintf(stderr, "p2c_elfcheck: %s\n", msg);
    return 2;
}

static int range_ok(const Image *im, uint64_t off, uint64_t len) {
    return off <= (uint64_t)im->size && len <= (uint64_t)im->size - off;
}

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int load_image(const char *path, Image *im) {
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return report_error("cannot open", path);
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return report_error("cannot determine size", path);
    }
    im->size = (size_t)n;
    im->buf = (unsigned char *)malloc(im->size ? im->size : 1u);
    if (!im->buf) {
        fclose(f);
        return report_error("out of memory", path);
    }
    if (im->size > 0 && fread(im->buf, 1, im->size, f) != im->size) {
        fclose(f);
        free(im->buf);
        im->buf = NULL;
        return report_error("short read", path);
    }
    fclose(f);
    im->path = path;
    return 0;
}

static int parse_ehdr(const Image *im, Ehdr *e) {
    const unsigned char *b = im->buf;
    if (im->size < EHDR_SIZE || memcmp(b, "\177ELF", 4) != 0)
        return report_error("not an ELF file", im->path);
    if (b[4] != 2 || b[5] != 1)
        return report_error("only ELF64 little-endian is supported", im->path);
    e->type = rd16(b + 16);
    e->machine = rd16(b + 18);
    e->entry = rd64(b + 24);
    e->phoff = rd64(b + 32);
    e->shoff = rd64(b + 40);
    e->phnum = rd16(b + 56);
    e->shnum = rd16(b + 60);
    e->shstrndx = rd16(b + 62);
    return 0;
}

static int read_phdr(const Image *im, const Ehdr *e, unsigned i, Phdr *ph) {
    uint64_t off = e->phoff + (uint64_t)i * PHDR_SIZE;
    const unsigned char *p;
    if (!range_ok(im, e->phoff, (uint64_t)e->phnum * PHDR_SIZE) || !range_ok(im, off, PHDR_SIZE))
        return report_error("program header table out of range", im->path);
    p = im->buf + off;
    ph->type = rd32(p);
    ph->flags = rd32(p + 4);
    ph->offset = rd64(p + 8);
    ph->vaddr = rd64(p + 16);
    ph->filesz = rd64(p + 32);
    ph->memsz = rd64(p + 40);
    return 0;
}

static int read_shdr(const Image *im, const Ehdr *e, unsigned i, Shdr *sh) {
    uint64_t off = e->shoff + (uint64_t)i * SHDR_SIZE;
    const unsigned char *p;
    if (!range_ok(im, off, SHDR_SIZE))
        return report_error("section header out of range", im->path);
    p = im->buf + off;
    sh->name = rd32(p);
    sh->type = rd32(p + 4);
    sh->offset = rd64(p + 24);
    sh->size = rd64(p + 32);
    sh->link = rd32(p + 40);
    return 0;
}

/* 文字列表の位置 idx から NUL 終端の名前を返す。範囲外・未終端は "?" */
static const char *str_at(const Image *im, const Shdr *strtab, uint32_t idx) {
    uint64_t i;
    if (idx >= strtab->size) return "?";
    for (i = idx; i < strtab->size; i++) {
        if (im->buf[strtab->offset + i] == '\0') break;
    }
    if (i >= strtab->size) return "?";
    return (const char *)(im->buf + strtab->offset + idx);
}

static const char *machine_name(uint16_t m) {
    switch (m) {
    case 62: return "x86-64";
    case 183: return "AArch64";
    default: return "other";
    }
}

static const char *type_name(uint16_t t) {
    switch (t) {
    case ET_REL: return "REL (relocatable object)";
    case ET_EXEC: return "EXEC (static/position-fixed executable)";
    case ET_DYN: return "DYN (shared object or PIE)";
    default: return "other";
    }
}

static void flags_text(uint32_t f, char out[4]) {
    out[0] = (f & PF_R) ? 'R' : '-';
    out[1] = (f & PF_W) ? 'W' : '-';
    out[2] = (f & PF_X) ? 'X' : '-';
    out[3] = '\0';
}

/* 大域シンボル表（SHT_SYMTAB）を探す。見つからなければ *symtab を NULL にする。 */
static int find_symtab(const Image *im, const Ehdr *e, Shdr *symtab, Shdr *strtab, int *found) {
    unsigned i;
    *found = 0;
    for (i = 0; i < e->shnum; i++) {
        Shdr sh;
        if (read_shdr(im, e, i, &sh) != 0) return 2;
        if (sh.type == SHT_SYMTAB) {
            Shdr st;
            if (read_shdr(im, e, sh.link, &st) != 0) return 2;
            if (!range_ok(im, st.offset, st.size) || !range_ok(im, sh.offset, sh.size))
                return report_error("symbol table out of range", im->path);
            *symtab = sh;
            *strtab = st;
            *found = 1;
            return 0;
        }
    }
    return 0;
}

/* 未定義の大域シンボル（弱参照は除く）の数を数え、表示する場合は name を出力する */
static int scan_undefined(const Image *im, const Shdr *symtab, const Shdr *strtab, int print,
                          size_t *count) {
    uint64_t n = symtab->size / SYM_SIZE;
    uint64_t k;
    *count = 0;
    for (k = 1; k < n; k++) {
        const unsigned char *s = im->buf + symtab->offset + k * SYM_SIZE;
        uint32_t name = rd32(s);
        unsigned info = s[4];
        uint16_t shndx = rd16(s + 6);
        unsigned bind = info >> 4;
        unsigned type = info & 0xfu;
        if (shndx != SHN_UNDEF || name == 0 || bind == 0 || bind == STB_WEAK) continue;
        if (type == STT_SECTION || type == STT_FILE) continue;
        (*count)++;
        if (print) printf("%s\n", str_at(im, strtab, name));
    }
    return 0;
}

/* NAME の定義済みシンボルの値を探す */
static int find_symbol_value(const Image *im, const Shdr *symtab, const Shdr *strtab,
                             const char *name, uint64_t *value) {
    uint64_t n = symtab->size / SYM_SIZE;
    uint64_t k;
    for (k = 1; k < n; k++) {
        const unsigned char *s = im->buf + symtab->offset + k * SYM_SIZE;
        uint32_t nm = rd32(s);
        uint16_t shndx = rd16(s + 6);
        if (shndx == SHN_UNDEF || nm == 0) continue;
        if (strcmp(str_at(im, strtab, nm), name) == 0) {
            *value = rd64(s + 8);
            return 1;
        }
    }
    return 0;
}

static int check_image(const Image *im, const Ehdr *e, const Options *opt, int *violations) {
    unsigned i;
    int has_interp = 0, has_dynamic = 0;
    int have_symtab = 0;
    Shdr symtab, strtab;
    size_t undef_count = 0;
    size_t nloads = 0;

    printf("p2c_elfcheck: %s\n", im->path);
    printf("  class: ELF64 little-endian\n");
    printf("  machine: %s (%u)\n", machine_name(e->machine), (unsigned)e->machine);
    printf("  type: %s\n", type_name(e->type));
    printf("  entry: 0x%llx\n", (unsigned long long)e->entry);
    printf("  segments (%u):\n", (unsigned)e->phnum);
    for (i = 0; i < e->phnum; i++) {
        Phdr ph;
        char fl[4];
        if (read_phdr(im, e, i, &ph) != 0) return 2;
        flags_text(ph.flags, fl);
        if (ph.type == PT_INTERP) has_interp = 1;
        if (ph.type == PT_DYNAMIC) has_dynamic = 1;
        if (ph.type == PT_LOAD) {
            nloads++;
            if ((ph.flags & PF_W) && (ph.flags & PF_X)) {
                printf("  VIOLATION: PT_LOAD #%u is writable and executable (W^X)\n", i);
                (*violations)++;
            }
        }
        printf("    %-8s off=0x%llx vaddr=0x%llx filesz=0x%llx memsz=0x%llx flags=%s\n",
               ph.type == PT_LOAD ? "LOAD" : ph.type == PT_INTERP ? "INTERP"
               : ph.type == PT_DYNAMIC ? "DYNAMIC" : "other",
               (unsigned long long)ph.offset, (unsigned long long)ph.vaddr,
               (unsigned long long)ph.filesz, (unsigned long long)ph.memsz, fl);
    }
    printf("  load segments: %u, interp: %s, dynamic: %s\n", (unsigned)nloads,
           has_interp ? "yes" : "no", has_dynamic ? "yes" : "no");

    if (find_symtab(im, e, &symtab, &strtab, &have_symtab) != 0) return 2;
    if (have_symtab) {
        scan_undefined(im, &symtab, &strtab, 0, &undef_count);
        printf("  symbols: %llu total, undefined global: %u\n",
               (unsigned long long)(symtab.size / SYM_SIZE), (unsigned)undef_count);
    } else {
        printf("  symbols: no symbol table (stripped)\n");
    }

    if (opt->exec && e->type != ET_EXEC) {
        printf("  VIOLATION: expected ET_EXEC, got %s\n", type_name(e->type));
        (*violations)++;
    }
    if (opt->rel && e->type != ET_REL) {
        printf("  VIOLATION: expected ET_REL, got %s\n", type_name(e->type));
        (*violations)++;
    }
    if (opt->is_static) {
        if (has_interp) {
            printf("  VIOLATION: PT_INTERP present (dynamically linked)\n");
            (*violations)++;
        }
        if (has_dynamic) {
            printf("  VIOLATION: PT_DYNAMIC present (dynamically linked)\n");
            (*violations)++;
        }
    }
    if (opt->no_undef) {
        if (!have_symtab) {
            printf("  VIOLATION: no symbol table; undefined symbols cannot be verified\n");
            (*violations)++;
        } else if (undef_count > 0) {
            printf("  VIOLATION: %u undefined global symbol(s):\n", (unsigned)undef_count);
            scan_undefined(im, &symtab, &strtab, 1, &undef_count);
            (*violations)++;
        }
    }
    {
        size_t k;
        for (k = 0; k < opt->n_entry_sym; k++) {
            uint64_t value = 0;
            if (!have_symtab) {
                printf("  VIOLATION: no symbol table; cannot resolve %s\n", opt->entry_sym[k]);
                (*violations)++;
            } else if (!find_symbol_value(im, &symtab, &strtab, opt->entry_sym[k], &value)) {
                printf("  VIOLATION: entry symbol %s is not defined\n", opt->entry_sym[k]);
                (*violations)++;
            } else if (value == e->entry) {
                printf("  entry: %s matches e_entry (0x%llx)\n", opt->entry_sym[k],
                       (unsigned long long)e->entry);
            } else {
                printf("  VIOLATION: e_entry is 0x%llx but %s is at 0x%llx\n",
                       (unsigned long long)e->entry, opt->entry_sym[k], (unsigned long long)value);
                (*violations)++;
            }
        }
    }
    return 0;
}

static int print_undefined(const Image *im, const Ehdr *e) {
    Shdr symtab, strtab;
    int have_symtab = 0;
    size_t count = 0;
    if (find_symtab(im, e, &symtab, &strtab, &have_symtab) != 0) return 2;
    if (!have_symtab) return report_error("no symbol table", im->path);
    scan_undefined(im, &symtab, &strtab, 1, &count);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage: p2c_elfcheck [--exec] [--rel] [--static] [--no-undef]\n"
            "                    [--entry-sym NAME]... FILE\n"
            "       p2c_elfcheck --undef FILE\n");
}

int main(int argc, char **argv) {
    Options opt;
    Image im;
    Ehdr e;
    const char *file = NULL;
    int i, rc, violations = 0;

    memset(&opt, 0, sizeof opt);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--exec") == 0) opt.exec = 1;
        else if (strcmp(a, "--rel") == 0) opt.rel = 1;
        else if (strcmp(a, "--static") == 0) opt.is_static = 1;
        else if (strcmp(a, "--no-undef") == 0) opt.no_undef = 1;
        else if (strcmp(a, "--undef") == 0) opt.undef_only = 1;
        else if (strcmp(a, "--entry-sym") == 0) {
            if (i + 1 >= argc || opt.n_entry_sym >= sizeof opt.entry_sym / sizeof opt.entry_sym[0]) {
                usage();
                return 2;
            }
            opt.entry_sym[opt.n_entry_sym++] = argv[++i];
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "p2c_elfcheck: unknown option %s\n", a);
            usage();
            return 2;
        } else {
            if (file) {
                usage();
                return 2;
            }
            file = a;
        }
    }
    if (!file) {
        usage();
        return 2;
    }
    if (load_image(file, &im) != 0) return 2;
    rc = parse_ehdr(&im, &e);
    if (rc != 0) {
        free(im.buf);
        return 2;
    }
    if (opt.undef_only) {
        rc = print_undefined(&im, &e);
    } else {
        rc = check_image(&im, &e, &opt, &violations);
        if (rc == 0) {
            if (violations == 0) printf("  result: PASS\n");
            else printf("  result: FAIL (%d violation(s))\n", violations);
        }
    }
    free(im.buf);
    if (rc != 0) return rc;
    return violations ? 1 : 0;
}
