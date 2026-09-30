/* cos_libc.c - the standard C library interface for .c-os programs.
 *
 * libcos already provides the primitives (allocator, buffered file
 * streams, formatter, math, setjmp, time) under cos_* names. This file
 * gives them their standard names and fills in what was missing, so an
 * ordinary C program - <stdio.h>, <stdlib.h>, <string.h>, <ctype.h>,
 * <math.h>, <time.h> - compiles and links unchanged with tools/cos-cc.
 *
 * Deliberately NOT redefined here: memcpy, memmove, memset, memcmp.
 * cos_lib.c already exports those under their standard names (the
 * compiler emits calls to them by itself), and a second definition would
 * be a duplicate-symbol error for every program.
 */
#include "cos.h"
#include "libc/_cos_libc_cfg.h"
#include "libc/string.h"
#include "libc/strings.h"
#include "libc/ctype.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"
#include "libc/errno.h"
#include "libc/math.h"
#include "libc/time.h"
#include "libc/unistd.h"

int errno = 0;

/* ======================================================================
 * string.h
 * ==================================================================== */
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; ++i) if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}
size_t strlen(const char *s) { return cos_strlen(s); }
size_t strnlen(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) ++n; return n; }
char *strcpy(char *d, const char *s) { return cos_strcpy(d, s); }
char *strncpy(char *d, const char *s, size_t n) { return cos_strncpy(d, s, n); }
char *strcat(char *d, const char *s) { return cos_strcat(d, s); }
char *strncat(char *d, const char *s, size_t n) {
    char *p = d + strlen(d);
    while (n-- && *s) *p++ = *s++;
    *p = '\0';
    return d;
}
int strcmp(const char *a, const char *b) { return cos_strcmp(a, b); }
int strncmp(const char *a, const char *b, size_t n) { return cos_strncmp(a, b, n); }
int strcoll(const char *a, const char *b) { return cos_strcmp(a, b); }
char *strchr(const char *s, int c) { return cos_strchr(s, c); }
char *strrchr(const char *s, int c) { return cos_strrchr(s, c); }
char *strstr(const char *h, const char *n) { return cos_strstr(h, n); }
char *strdup(const char *s) { return cos_strdup(s); }
char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *p = (char *)cos_malloc(len + 1);
    if (!p) { errno = ENOMEM; return NULL; }
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}
size_t strspn(const char *s, const char *a) {
    size_t n = 0;
    while (s[n] && strchr(a, s[n])) ++n;
    return n;
}
size_t strcspn(const char *s, const char *r) {
    size_t n = 0;
    while (s[n] && !strchr(r, s[n])) ++n;
    return n;
}
char *strpbrk(const char *s, const char *a) {
    for (; *s; ++s) if (strchr(a, *s)) return (char *)s;
    return NULL;
}
char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    if (!s) return NULL;
    s += strspn(s, delim);
    if (!*s) { *save = NULL; return NULL; }
    char *end = s + strcspn(s, delim);
    if (*end) { *end = '\0'; *save = end + 1; }
    else *save = NULL;
    return s;
}
char *strtok(char *s, const char *delim) {
    static char *save;
    return strtok_r(s, delim, &save);
}
char *strerror(int err) {
    switch (err) {
    case 0: return "Success";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case EIO: return "I/O error";
    case EBADF: return "Bad file descriptor";
    case ENOMEM: return "Out of memory";
    case EACCES: return "Permission denied";
    case EEXIST: return "File exists";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case ENOSPC: return "No space left on device";
    case EDOM: return "Numerical argument out of domain";
    case ERANGE: return "Result out of range";
    case ENOSYS: return "Function not implemented";
    default: return "Unknown error";
    }
}
size_t strlcpy(char *d, const char *s, size_t size) {
    size_t len = strlen(s);
    if (size) { size_t n = len < size - 1 ? len : size - 1; memcpy(d, s, n); d[n] = '\0'; }
    return len;
}
size_t strlcat(char *d, const char *s, size_t size) {
    size_t dl = strnlen(d, size);
    if (dl == size) return size + strlen(s);
    return dl + strlcpy(d + dl, s, size - dl);
}

/* ======================================================================
 * ctype.h (ASCII / "C" locale)
 * ==================================================================== */
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isblank(int c) { return c == ' ' || c == '\t'; }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int strcasecmp(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, size_t n) {
    if (!n) return 0;
    while (--n && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* ======================================================================
 * stdlib.h
 * ==================================================================== */
void *malloc(size_t n) { void *p = cos_malloc(n); if (!p && n) errno = ENOMEM; return p; }
void *calloc(size_t n, size_t s) {
    if (s && n > (size_t)-1 / s) { errno = ENOMEM; return NULL; }
    void *p = cos_calloc(n, s);
    if (!p && n && s) errno = ENOMEM;
    return p;
}
void *realloc(void *p, size_t n) { void *q = cos_realloc(p, n); if (!q && n) errno = ENOMEM; return q; }
void free(void *p) { cos_free(p); }
void *aligned_alloc(size_t align, size_t n) {
    /* Over-allocate and stash the original pointer just below the aligned
     * block. Only valid to release with aligned_free-style knowledge, so
     * keep alignments <= 16 on the fast path (cos_malloc already gives
     * 16-byte alignment) and document the rest as leaking on free(). */
    if (align <= 16) return malloc(n);
    uintptr_t raw = (uintptr_t)malloc(n + align);
    if (!raw) return NULL;
    return (void *)((raw + align - 1) & ~(uintptr_t)(align - 1));
}
_Noreturn void abort(void) {
    cos_write("abort()\n", 8);
    cos_exit(134);
}
_Noreturn void exit(int status) {
    cos_run_atexit();
    cos_exit(status);
}
_Noreturn void _Exit(int status) { cos_exit(status); }
int atexit(void (*fn)(void)) { return cos_atexit(fn); }
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
long long llabs(long long v) { return v < 0 ? -v : v; }
div_t div(int a, int b) { div_t r = { a / b, a % b }; return r; }
ldiv_t ldiv(long a, long b) { ldiv_t r = { a / b, a % b }; return r; }

static int digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}
unsigned long long strtoull(const char *s, char **end, int base) {
    const char *p = s;
    while (isspace((unsigned char)*p)) ++p;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); ++p; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2; base = 16;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    unsigned long long v = 0;
    int any = 0, overflow = 0;
    for (;; ++p) {
        int d = digit_val((unsigned char)*p);
        if (d >= base) break;
        any = 1;
        if (v > (~0ull - (unsigned)d) / (unsigned)base) overflow = 1;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (end) *end = (char *)(any ? p : s);
    if (overflow) { errno = ERANGE; return ~0ull; }
    return neg ? (unsigned long long)(-(long long)v) : v;
}
long long strtoll(const char *s, char **end, int base) {
    const char *p = s;
    while (isspace((unsigned char)*p)) ++p;
    int neg = (*p == '-');
    unsigned long long v = strtoull(s, end, base);
    if (neg) {
        unsigned long long mag = (unsigned long long)(-(long long)v);
        if (mag > (1ull << 63)) { errno = ERANGE; return (long long)(1ull << 63); }
        return (long long)v;
    }
    if (v > 0x7fffffffffffffffull) { errno = ERANGE; return 0x7fffffffffffffffll; }
    return (long long)v;
}
long strtol(const char *s, char **end, int base) { return (long)strtoll(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

/* Decimal (and "inf"/"nan"/hex-prefixed integer) parsing. Accumulates the
 * mantissa as an integer of up to 19 significant digits and applies the
 * decimal exponent once with a power-of-ten table, which keeps ordinary
 * inputs ("3.14", "1e-5", "12345.678") exact to the last printed digit. */
double strtod(const char *s, char **end) {
    const char *p = s;
    while (isspace((unsigned char)*p)) ++p;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); ++p; }
    if (strncasecmp(p, "inf", 3) == 0) {
        p += 3;
        if (strncasecmp(p, "inity", 5) == 0) p += 5;
        if (end) *end = (char *)p;
        return neg ? -__builtin_inf() : __builtin_inf();
    }
    if (strncasecmp(p, "nan", 3) == 0) {
        if (end) *end = (char *)(p + 3);
        return __builtin_nan("");
    }
    unsigned long long mant = 0;
    int digits = 0, exp10 = 0, any = 0;
    while (isdigit((unsigned char)*p)) {
        any = 1;
        if (digits < 19) { mant = mant * 10 + (unsigned)(*p - '0'); if (mant) ++digits; }
        else ++exp10;
        ++p;
    }
    if (*p == '.') {
        ++p;
        while (isdigit((unsigned char)*p)) {
            any = 1;
            if (digits < 19) { mant = mant * 10 + (unsigned)(*p - '0'); if (mant) ++digits; --exp10; }
            ++p;
        }
    }
    if (!any) { if (end) *end = (char *)s; return 0.0; }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0;
        if (*q == '+' || *q == '-') { eneg = (*q == '-'); ++q; }
        if (isdigit((unsigned char)*q)) {
            int e = 0;
            while (isdigit((unsigned char)*q)) { if (e < 10000) e = e * 10 + (*q - '0'); ++q; }
            exp10 += eneg ? -e : e;
            p = q;
        }
    }
    if (end) *end = (char *)p;
    double v = (double)mant;
    static const double p10[] = { 1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256 };
    int e = exp10 < 0 ? -exp10 : exp10;
    double scale = 1.0;
    for (int i = 0; i < 9 && e; ++i, e >>= 1) if (e & 1) scale *= p10[i];
    if (e) { errno = ERANGE; v = exp10 < 0 ? 0.0 : __builtin_inf(); }
    else v = exp10 < 0 ? v / scale : v * scale;
    return neg ? -v : v;
}
float strtof(const char *s, char **end) { return (float)strtod(s, end); }
double atof(const char *s) { return strtod(s, NULL); }

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    cos_qsort(base, n, size, cmp);
}
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *)) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void *elem = (const char *)base + mid * size;
        int c = cmp(key, elem);
        if (c == 0) return (void *)elem;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
int rand(void) { return cos_rand(); }
void srand(unsigned seed) { cos_srand(seed); }
char *getenv(const char *name) { return cos_getenv(name); }
int system(const char *cmd) { (void)cmd; errno = ENOSYS; return -1; }

void __cos_assert_fail(const char *expr, const char *file, int line, const char *func) {
    cos_printf("Assertion failed: %s (%s:%d, %s)\n", expr, file, line, func);
    abort();
}

/* ======================================================================
 * stdio.h
 *
 * stdout/stderr write straight through cos_write() (unbuffered - nothing
 * is lost if a program crashes, and interleaving with cos_printf() output
 * stays in order). Files wrap a cos_FILE and remember their path so
 * SEEK_END can be resolved with cos_stat(). Every open file is on a list
 * flushed by an atexit hook, because cos_FILE streams are buffered and a
 * program that returns from main without fclose() would otherwise lose
 * its last writes.
 * ==================================================================== */
#define F_STD_OUT 1
#define F_STD_IN  2
#define F_FILE    3

struct __cos_libc_FILE {
    int kind;
    int err;
    int eof;
    int unget;          /* pushed-back char for stdin/stdout-kind streams, -1 if none */
    cos_FILE *f;
    char path[FILENAME_MAX];
    struct __cos_libc_FILE *next;
};

static struct __cos_libc_FILE s_stdin  = { F_STD_IN,  0, 0, -1, NULL, "", NULL };
static struct __cos_libc_FILE s_stdout = { F_STD_OUT, 0, 0, -1, NULL, "", NULL };
static struct __cos_libc_FILE s_stderr = { F_STD_OUT, 0, 0, -1, NULL, "", NULL };
FILE *stdin = &s_stdin, *stdout = &s_stdout, *stderr = &s_stderr;

static FILE *s_open_files = NULL;
static int s_atexit_registered = 0;

static void flush_all_files(void) {
    for (FILE *f = s_open_files; f; f = f->next) if (f->f) cos_fflush(f->f);
}

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) { errno = EINVAL; return NULL; }
    /* cos_fopen understands "r", "w", "a", "r+"; map the rest ("rb",
     * "wb", "w+", "a+" ...) onto the closest of those - C-OS files have
     * no text/binary distinction. */
    char m[3] = { mode[0], 0, 0 };
    if (strchr(mode, '+')) {
        if (mode[0] == 'r') { m[1] = '+'; }
        else if (mode[0] == 'w') { m[0] = 'w'; }
        else if (mode[0] == 'a') { m[0] = 'a'; }
    }
    cos_FILE *cf = cos_fopen(path, m);
    if (!cf) { errno = (mode[0] == 'r') ? ENOENT : EIO; return NULL; }
    FILE *f = (FILE *)cos_calloc(1, sizeof(FILE));
    if (!f) { cos_fclose(cf); errno = ENOMEM; return NULL; }
    f->kind = F_FILE;
    f->unget = -1;
    f->f = cf;
    strlcpy(f->path, path, sizeof(f->path));
    f->next = s_open_files;
    s_open_files = f;
    if (!s_atexit_registered) { s_atexit_registered = 1; cos_atexit(flush_all_files); }
    return f;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    if (f->kind != F_FILE) return 0;
    for (FILE **pp = &s_open_files; *pp; pp = &(*pp)->next) {
        if (*pp == f) { *pp = f->next; break; }
    }
    int r = cos_fclose(f->f);
    cos_free(f);
    return r == 0 ? 0 : EOF;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f) {
    if (!f || !size || !n) return 0;
    if (f->kind == F_STD_OUT) {
        ssize_t w = cos_write((const char *)p, size * n);
        if (w < 0) { f->err = 1; return 0; }
        return (size_t)w / size;
    }
    if (f->kind != F_FILE) { f->err = 1; return 0; }
    size_t r = cos_fwrite(p, size, n, f->f);
    if (r < n) f->err = 1;
    return r;
}

size_t fread(void *p, size_t size, size_t n, FILE *f) {
    if (!f || !size || !n) return 0;
    if (f->kind != F_FILE) { f->eof = 1; return 0; }
    size_t r = cos_fread(p, size, n, f->f);
    if (r < n) f->eof = 1;
    return r;
}

int fputc(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    return fwrite(&ch, 1, 1, f) == 1 ? ch : EOF;
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputs(const char *s, FILE *f) { size_t n = strlen(s); return fwrite(s, 1, n, f) == n ? 0 : EOF; }
int puts(const char *s) { if (fputs(s, stdout) == EOF) return EOF; return fputc('\n', stdout) == EOF ? EOF : 0; }

int fgetc(FILE *f) {
    if (!f) return EOF;
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (f->kind != F_FILE) { f->eof = 1; return EOF; }
    int c = cos_fgetc(f->f);
    if (c == COS_EOF) f->eof = 1;
    return c == COS_EOF ? EOF : c;
}
int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }
int ungetc(int c, FILE *f) {
    if (!f || c == EOF) return EOF;
    if (f->kind == F_FILE) { int r = cos_ungetc(c, f->f); if (r != COS_EOF) f->eof = 0; return r == COS_EOF ? EOF : r; }
    f->unget = (unsigned char)c;
    f->eof = 0;
    return (unsigned char)c;
}
char *fgets(char *buf, int n, FILE *f) {
    if (!buf || n <= 0 || !f) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = '\0';
    return buf;
}

int fflush(FILE *f) {
    if (!f) { flush_all_files(); return 0; }
    if (f->kind != F_FILE) return 0;
    return cos_fflush(f->f) == 0 ? 0 : EOF;
}
long ftell(FILE *f) {
    if (!f || f->kind != F_FILE) { errno = EBADF; return -1; }
    return cos_ftell(f->f);
}
int fseek(FILE *f, long off, int whence) {
    if (!f || f->kind != F_FILE) { errno = EBADF; return -1; }
    long target;
    if (whence == SEEK_SET) target = off;
    else if (whence == SEEK_CUR) target = cos_ftell(f->f) + off;
    else if (whence == SEEK_END) {
        cos_fflush(f->f);   /* size must include our own buffered writes */
        cos_stat_t st;
        if (cos_stat(f->path, &st) != 0) { errno = EIO; return -1; }
        target = (long)st.size + off;
    } else { errno = EINVAL; return -1; }
    if (target < 0) { errno = EINVAL; return -1; }
    f->unget = -1;
    if (cos_fseek(f->f, target) != 0) { errno = EIO; return -1; }
    f->eof = 0;
    return 0;
}
void rewind(FILE *f) { if (f) { fseek(f, 0, SEEK_SET); f->err = 0; } }
int fgetpos(FILE *f, fpos_t *pos) { long p = ftell(f); if (p < 0) return -1; *pos = p; return 0; }
int fsetpos(FILE *f, const fpos_t *pos) { return fseek(f, *pos, SEEK_SET); }
int feof(FILE *f) { return f ? (f->kind == F_FILE ? (f->eof || cos_feof(f->f)) : f->eof) : 0; }
int ferror(FILE *f) { return f ? f->err : 0; }
void clearerr(FILE *f) { if (f) { f->err = 0; f->eof = 0; } }
int remove(const char *path) { return cos_unlink(path) == 0 ? 0 : (errno = ENOENT, -1); }
int rename(const char *a, const char *b) { return cos_rename(a, b) == 0 ? 0 : (errno = EIO, -1); }
void perror(const char *s) {
    if (s && *s) { fputs(s, stderr); fputs(": ", stderr); }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) { return cos_vsnprintf(buf, n, fmt, ap); }
int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = cos_vsnprintf(buf, n, fmt, ap); va_end(ap); return r;
}
int vsprintf(char *buf, const char *fmt, va_list ap) { return cos_vsnprintf(buf, (size_t)1 << 30, fmt, ap); }
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsprintf(buf, fmt, ap); va_end(ap); return r;
}
int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char small[512];
    va_list ap2;
    va_copy(ap2, ap);
    int len = cos_vsnprintf(small, sizeof(small), fmt, ap);
    if (len < 0) { va_end(ap2); return len; }
    if ((size_t)len < sizeof(small)) {
        va_end(ap2);
        return fwrite(small, 1, (size_t)len, f) == (size_t)len ? len : -1;
    }
    char *big = (char *)cos_malloc((size_t)len + 1);
    if (!big) { va_end(ap2); errno = ENOMEM; return -1; }
    cos_vsnprintf(big, (size_t)len + 1, fmt, ap2);
    va_end(ap2);
    size_t w = fwrite(big, 1, (size_t)len, f);
    cos_free(big);
    return w == (size_t)len ? len : -1;
}
int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r;
}
int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vfprintf(stdout, fmt, ap); va_end(ap); return r;
}

/* sscanf: %d %i %u %x %o %ld %lld %lu %llu %f %lf %e %g %s %c %[...] %n %%,
 * with optional field width and '*' assignment suppression. */
int vsscanf(const char *s, const char *fmt, va_list ap) {
    const char *p = s;
    int assigned = 0;
    for (; *fmt; ++fmt) {
        if (isspace((unsigned char)*fmt)) { while (isspace((unsigned char)*p)) ++p; continue; }
        if (*fmt != '%') { if (*p != *fmt) break; ++p; continue; }
        ++fmt;
        if (*fmt == '%') { if (*p != '%') break; ++p; continue; }
        int suppress = 0, width = 0, lng = 0;
        if (*fmt == '*') { suppress = 1; ++fmt; }
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { if (*fmt == 'l') ++lng; ++fmt; }
        char conv = *fmt;
        if (conv == 'n') { if (!suppress) *va_arg(ap, int *) = (int)(p - s); continue; }
        if (conv != 'c' && conv != '[') while (isspace((unsigned char)*p)) ++p;
        if (!*p) return assigned ? assigned : EOF;
        char tmp[128];
        const char *src = p;
        if (width > 0 && conv != 'c' && conv != 's' && conv != '[') {
            int w = width < 127 ? width : 127;
            memcpy(tmp, p, (size_t)w); tmp[w] = '\0';
            src = tmp;
        }
        char *end = NULL;
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            int base = (conv == 'd' || conv == 'u') ? 10 : (conv == 'i') ? 0 : (conv == 'o') ? 8 : 16;
            long long v = (conv == 'd' || conv == 'i') ? strtoll(src, &end, base) : (long long)strtoull(src, &end, base);
            if (end == src) break;
            p += end - src;
            if (!suppress) {
                if (lng >= 2) *va_arg(ap, long long *) = v;
                else if (lng == 1) *va_arg(ap, long *) = (long)v;
                else *va_arg(ap, int *) = (int)v;
                ++assigned;
            }
        } else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G') {
            double v = strtod(src, &end);
            if (end == src) break;
            p += end - src;
            if (!suppress) {
                if (lng) *va_arg(ap, double *) = v; else *va_arg(ap, float *) = (float)v;
                ++assigned;
            }
        } else if (conv == 's') {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while (*p && !isspace((unsigned char)*p) && (!width || n < width)) { if (out) out[n] = *p; ++n; ++p; }
            if (out) { out[n] = '\0'; ++assigned; }
        } else if (conv == 'c') {
            int n = width ? width : 1;
            char *out = suppress ? NULL : va_arg(ap, char *);
            for (int i = 0; i < n && *p; ++i, ++p) if (out) out[i] = *p;
            if (out) ++assigned;
        } else if (conv == '[') {
            ++fmt;
            int invert = 0;
            if (*fmt == '^') { invert = 1; ++fmt; }
            char set[256] = { 0 };
            if (*fmt == ']') { set[(unsigned char)']'] = 1; ++fmt; }
            for (; *fmt && *fmt != ']'; ++fmt) {
                if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
                    for (int c = (unsigned char)fmt[0]; c <= (unsigned char)fmt[2]; ++c) set[c] = 1;
                    fmt += 2;
                } else set[(unsigned char)*fmt] = 1;
            }
            char *out = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while (*p && (set[(unsigned char)*p] ^ invert) && (!width || n < width)) { if (out) out[n] = *p; ++n; ++p; }
            if (!n) break;
            if (out) { out[n] = '\0'; ++assigned; }
        } else break;
    }
    return assigned;
}
int sscanf(const char *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsscanf(s, fmt, ap); va_end(ap); return r;
}

/* ======================================================================
 * math.h - standard names over cos_math.c
 * ==================================================================== */
double fabs(double x) { return cos_fabs(x); }
double floor(double x) { return cos_floor(x); }
double ceil(double x) { return cos_ceil(x); }
double trunc(double x) { return cos_trunc(x); }
double round(double x) { return cos_round(x); }
double fmod(double a, double b) { return cos_fmod(a, b); }
double modf(double x, double *i) { return cos_modf(x, i); }
double frexp(double x, int *e) { return cos_frexp(x, e); }
double ldexp(double x, int e) { return cos_ldexp(x, e); }
double sqrt(double x) { return cos_sqrt(x); }
double cbrt(double x) { return cos_cbrt(x); }
double hypot(double a, double b) { return cos_hypot(a, b); }
double exp(double x) { return cos_exp(x); }
double exp2(double x) { return cos_exp2(x); }
double log(double x) { return cos_log(x); }
double log2(double x) { return cos_log2(x); }
double log10(double x) { return cos_log10(x); }
double pow(double a, double b) { return cos_pow(a, b); }
double sin(double x) { return cos_sin(x); }
double cos(double x) { return cos_cos(x); }
double tan(double x) { return cos_tan(x); }
double asin(double x) { return cos_asin(x); }
double acos(double x) { return cos_acos(x); }
double atan(double x) { return cos_atan(x); }
double atan2(double y, double x) { return cos_atan2(y, x); }
double sinh(double x) { return cos_sinh(x); }
double cosh(double x) { return cos_cosh(x); }
double tanh(double x) { return cos_tanh(x); }
double copysign(double a, double b) { return cos_copysign(a, b); }
double fmin(double a, double b) { return cos_fmin(a, b); }
double fmax(double a, double b) { return cos_fmax(a, b); }
double fma(double a, double b, double c) { return a * b + c; }
double rint(double x) { return cos_round(x); }
double nearbyint(double x) { return cos_round(x); }
long lround(double x) { return (long)cos_round(x); }
long lroundf(float x) { return (long)cos_round((double)x); }
float fabsf(float x) { return (float)cos_fabs(x); }
float floorf(float x) { return (float)cos_floor(x); }
float ceilf(float x) { return (float)cos_ceil(x); }
float truncf(float x) { return (float)cos_trunc(x); }
float roundf(float x) { return (float)cos_round(x); }
float fmodf(float a, float b) { return (float)cos_fmod(a, b); }
float sqrtf(float x) { return (float)cos_sqrt(x); }
float powf(float a, float b) { return (float)cos_pow(a, b); }
float expf(float x) { return (float)cos_exp(x); }
float logf(float x) { return (float)cos_log(x); }
float log10f(float x) { return (float)cos_log10(x); }
float log2f(float x) { return (float)cos_log2(x); }
float sinf(float x) { return (float)cos_sin(x); }
float cosf(float x) { return (float)cos_cos(x); }
float tanf(float x) { return (float)cos_tan(x); }
float atanf(float x) { return (float)cos_atan(x); }
float atan2f(float y, float x) { return (float)cos_atan2(y, x); }
float hypotf(float a, float b) { return (float)cos_hypot(a, b); }
float fminf(float a, float b) { return (float)cos_fmin(a, b); }
float fmaxf(float a, float b) { return (float)cos_fmax(a, b); }
float copysignf(float a, float b) { return (float)cos_copysign(a, b); }

/* ======================================================================
 * time.h
 * ==================================================================== */
time_t time(time_t *t) { time_t v = (time_t)cos_time_unix(); if (t) *t = v; return v; }
clock_t clock(void) { return (clock_t)cos_time_ms(); }
double difftime(time_t a, time_t b) { return (double)(a - b); }

static int is_leap(long long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int s_mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

struct tm *gmtime_r(const time_t *tp, struct tm *out) {
    long long t = *tp;
    long long days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; --days; }
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)((rem % 3600) / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((4 + days) % 7); if (out->tm_wday < 0) out->tm_wday += 7;
    long long y = 1970;
    for (;;) {
        long long ylen = is_leap(y) ? 366 : 365;
        if (days >= 0 && days < ylen) break;
        if (days < 0) { --y; days += is_leap(y) ? 366 : 365; }
        else { days -= ylen; ++y; }
    }
    out->tm_year = (int)(y - 1900);
    out->tm_yday = (int)days;
    int m = 0;
    for (; m < 12; ++m) {
        int ml = s_mdays[m] + (m == 1 && is_leap(y));
        if (days < ml) break;
        days -= ml;
    }
    out->tm_mon = m;
    out->tm_mday = (int)days + 1;
    out->tm_isdst = 0;
    return out;
}
struct tm *gmtime(const time_t *t) { static struct tm tm; return gmtime_r(t, &tm); }
/* C-OS keeps no timezone database: local time is UTC. */
struct tm *localtime_r(const time_t *t, struct tm *out) { return gmtime_r(t, out); }
struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t mktime(struct tm *tm) {
    long long y = 1900LL + tm->tm_year;
    long long mon = tm->tm_mon;
    y += mon / 12; mon %= 12; if (mon < 0) { mon += 12; --y; }
    long long days = 0;
    if (y >= 1970) for (long long i = 1970; i < y; ++i) days += is_leap(i) ? 366 : 365;
    else for (long long i = y; i < 1970; ++i) days -= is_leap(i) ? 366 : 365;
    for (int m = 0; m < mon; ++m) days += s_mdays[m] + (m == 1 && is_leap(y));
    days += tm->tm_mday - 1;
    time_t t = (time_t)(days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec);
    gmtime_r(&t, tm);   /* normalise fields, fill wday/yday */
    return t;
}

static const char *s_wday[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *s_mon[12] = { "January", "February", "March", "April", "May", "June",
                                 "July", "August", "September", "October", "November", "December" };

size_t strftime(char *buf, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;
    char tmp[64];
    for (; *fmt; ++fmt) {
        const char *add = tmp;
        if (*fmt != '%') { tmp[0] = *fmt; tmp[1] = '\0'; }
        else {
            ++fmt;
            switch (*fmt) {
            case 'Y': snprintf(tmp, sizeof tmp, "%d", tm->tm_year + 1900); break;
            case 'y': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_year + 1900) % 100); break;
            case 'm': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mon + 1); break;
            case 'd': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mday); break;
            case 'e': snprintf(tmp, sizeof tmp, "%2d", tm->tm_mday); break;
            case 'H': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour); break;
            case 'I': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
            case 'M': snprintf(tmp, sizeof tmp, "%02d", tm->tm_min); break;
            case 'S': snprintf(tmp, sizeof tmp, "%02d", tm->tm_sec); break;
            case 'p': add = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case 'j': snprintf(tmp, sizeof tmp, "%03d", tm->tm_yday + 1); break;
            case 'a': snprintf(tmp, sizeof tmp, "%.3s", s_wday[tm->tm_wday % 7]); break;
            case 'A': add = s_wday[tm->tm_wday % 7]; break;
            case 'b': case 'h': snprintf(tmp, sizeof tmp, "%.3s", s_mon[tm->tm_mon % 12]); break;
            case 'B': add = s_mon[tm->tm_mon % 12]; break;
            case 'F': snprintf(tmp, sizeof tmp, "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
            case 'T': snprintf(tmp, sizeof tmp, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'R': snprintf(tmp, sizeof tmp, "%02d:%02d", tm->tm_hour, tm->tm_min); break;
            case 'D': snprintf(tmp, sizeof tmp, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, (tm->tm_year + 1900) % 100); break;
            case 'c': snprintf(tmp, sizeof tmp, "%.3s %.3s %2d %02d:%02d:%02d %d", s_wday[tm->tm_wday % 7],
                               s_mon[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
                               tm->tm_year + 1900); break;
            case 'n': add = "\n"; break;
            case 't': add = "\t"; break;
            case '%': add = "%"; break;
            case '\0': --fmt; add = ""; break;
            default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = '\0'; break;
            }
        }
        size_t l = strlen(add);
        if (n + l >= max) return 0;
        memcpy(buf + n, add, l);
        n += l;
    }
    if (n >= max) return 0;
    buf[n] = '\0';
    return n;
}
char *asctime(const struct tm *tm) {
    static char buf[32];
    strftime(buf, sizeof buf, "%a %b %e %H:%M:%S %Y\n", tm);
    return buf;
}
char *ctime(const time_t *t) { return asctime(localtime(t)); }

/* ======================================================================
 * unistd.h
 * ==================================================================== */
unsigned sleep(unsigned seconds) { cos_sleep_ms((uint64_t)seconds * 1000u); return 0; }
int usleep(unsigned usec) { cos_sleep_ms((usec + 999u) / 1000u); return 0; }
pid_t getpid(void) { return (pid_t)cos_getpid(); }
char *strcasestr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; ++hay) {
        const char *h = hay, *n = needle;
        while (*h && *n && (*h | 32) == (*n | 32)) { ++h; ++n; }
        if (!*n) return (char *)hay;
    }
    return NULL;
}
