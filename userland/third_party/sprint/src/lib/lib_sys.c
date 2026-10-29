/* Modules io, sys, time and gc. */
#include "lib_int.h"

/* ---------------------------------------------------------------- */
/* io                                                                */
/* ---------------------------------------------------------------- */
static int io_write(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)ret;
    if (!(vm->caps & CAP_STDIO_WRITE)) return vm_set_error(vm, E_PERM, "stdout is not permitted");
    Out o = {0};
    o.sink = sink_stdout;
    o.ud = vm;
    for (int i = 0; i < argc; i++) fmt_value(vm, argv[i], 0, 0, &o);
    return 0;
}
static void sink_stderr(void *ud, const char *s, size_t n) {
    VM *vm = ud;
    if (vm && vm->hwrite) (void)vm->hwrite(vm->hio_ud, 2, s, n);
    else (void)fwrite(s, 1, n, stderr);
}
static int io_eprint(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)ret;
    if (!(vm->caps & CAP_STDIO_WRITE)) return vm_set_error(vm, E_PERM, "stderr is not permitted");
    vm_out_flush(vm);
    Out o = {0};
    o.sink = sink_stderr;
    o.ud = vm;
    for (int i = 0; i < argc; i++) {
        if (i) out_put(&o, " ", 1);
        fmt_value(vm, argv[i], 0, 0, &o);
    }
    out_put(&o, "\n", 1);
    return 0;
}
static int io_flush(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)argv; (void)ret;
    vm_out_flush(vm);
    return 0;
}

static int read_stdin(VM *vm, int whole, SpValue *ret) {
    if (!(vm->caps & CAP_STDIO_READ)) return vm_set_error(vm, E_PERM, "stdin is not permitted");
    vm_out_flush(vm);
    size_t cap = 256, n = 0;
    char *buf = malloc(cap);
    if (!buf) vm_fatal(vm, RS_MEMORY, "out of memory");
    int c;
    int any = 0;
    for (;;) {
        if (vm->hread) {
            char ch;
            ptrdiff_t got = vm->hread(vm->hio_ud, &ch, 1);
            if (got < 0) { free(buf); return lib_io_fail(vm, EIO, "read", "stdin"); }
            if (got == 0) break;
            c = (unsigned char)ch;
        } else if ((c = getc(stdin)) == EOF) {
            break;
        }
        any = 1;
        if (!whole && c == '\n') break;
        if (n + 2 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); vm_fatal(vm, RS_MEMORY, "out of memory"); }
            buf = nb;
        }
        buf[n++] = (char)c;
    }
    if (!whole && !any) { free(buf); *ret = v_nil(); return 0; }
    if (!whole && n > 0 && buf[n - 1] == '\r') n--;
    size_t bad;
    if (!utf8_validate(buf, n, &bad)) { free(buf); return vm_set_error(vm, E_IO, "stdin is not valid UTF-8"); }
    *ret = vstr(vm, buf, n);
    free(buf);
    return 0;
}
static int io_read_line(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)argv; return read_stdin(vm, 0, ret); }
static int io_read_all(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)argv; return read_stdin(vm, 1, ret); }

const NativeDesc io_fns[] = {
    {"write", 0, -1, io_write}, {"eprint", 0, -1, io_eprint}, {"flush", 0, 0, io_flush},
    {"read_line", 0, 0, io_read_line}, {"read_all", 0, 0, io_read_all}, {NULL, 0, 0, NULL}};

/* ---------------------------------------------------------------- */
/* sys, time, gc                                                     */
/* ---------------------------------------------------------------- */
static int sys_exit(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)ret;
    int64_t code = 0;
    if (argc > 0 && need_int(vm, argv[0], "exit code", &code)) return 1;
    if (code < 0 || code > 255) return vm_set_error(vm, E_VALUE, "exit code must be 0..255");
    vm->exit_code = (int)code;
    vm_fatal(vm, RS_EXIT, "exit");
}
const NativeDesc sys_fns[] = {{"exit", 0, 1, sys_exit}, {NULL, 0, 0, NULL}};

static double now_wall(void) {
    struct timespec ts;
    (void)timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static double now_mono(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static int time_now(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)argv;
    if (!(vm->caps & CAP_CLOCK)) return vm_set_error(vm, E_PERM, "clock is not permitted");
    *ret = v_float(vm->deterministic ? vm->vclock : now_wall());
    return 0;
}
static int time_monotonic(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)argv;
    if (!(vm->caps & CAP_CLOCK)) return vm_set_error(vm, E_PERM, "clock is not permitted");
    *ret = v_float(vm->deterministic ? vm->vclock : now_mono());
    return 0;
}
static int time_sleep(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)ret;
    double s;
    if (!(vm->caps & CAP_CLOCK)) return vm_set_error(vm, E_PERM, "clock is not permitted");
    if (need_num(vm, argv[0], "sleep() argument", &s)) return 1;
    if (s < 0 || isnan(s)) return vm_set_error(vm, E_VALUE, "sleep() duration must be >= 0");
    if (vm->deterministic) { vm->vclock += s; return 0; }
    vm_out_flush(vm);
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - floor(s)) * 1e9);
    (void)nanosleep(&ts, NULL);
    return 0;
}
const NativeDesc time_fns[] = {
    {"now", 0, 0, time_now}, {"monotonic", 0, 0, time_monotonic}, {"sleep", 1, 1, time_sleep},
    {NULL, 0, 0, NULL}};

static int gc_collect(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)argv; (void)ret;
    vm_gc(vm);
    return 0;
}
static int gc_stats(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; (void)argv;
    STable *t = vm_new_table(vm, 4);
    int z = vm->deterministic;
    table_set_str(vm, t, "heap_bytes", 10, v_int(z ? 0 : (int64_t)vm->bytes));
    table_set_str(vm, t, "collections", 11, v_int(z ? 0 : vm->gc_runs));
    *ret = v_obj(T_TABLE, t);
    return 0;
}
const NativeDesc gc_fns[] = {{"collect", 0, 0, gc_collect}, {"stats", 0, 0, gc_stats}, {NULL, 0, 0, NULL}};
