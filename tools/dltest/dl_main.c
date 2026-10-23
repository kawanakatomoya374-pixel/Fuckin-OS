/* dl_main.c - checks of the program and library loader that need a running system: dltest <leak|share|...>. Every check prints "DLTEST PASS|FAIL <what>". */
#include "cos.h"
#include <dlfcn.h>

static int fails;
static void check(int ok, const char *what) {
    cos_printf("DLTEST %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}
static cos_sysinfo_t si;
static int64_t free_pages(void) { cos_sysinfo(&si); return (int64_t)si.pages_free; }

static int64_t spawn_child(const char *mode) {
    const char *av[3] = { "/bin/dlchild.c-os", mode, 0 };
    const char *ev[1] = { 0 };
    return cos_spawn_argv(av[0], av, ev);
}
static void run_child(const char *mode) {
    int64_t pid = spawn_child(mode);
    int st = 0;
    if (pid > 0) cos_waitpid(pid, &st, true);
}

/* A process that exits gives back every frame it had: its image, its libraries, the TLS block and the link map used to stay allocated for good. */
static void test_leak(void) {
    for (int i = 0; i < 3; i++) run_child("quick");          /* warm-up: caches and tables that grow once */
    cos_sleep_ms(300);
    int64_t before = free_pages();
    for (int i = 0; i < 40; i++) run_child("quick");
    cos_sleep_ms(300);
    int64_t lost = before - free_pages();
    cos_printf("DLTEST INFO 40 programs started and ended, pages lost: %ld\n", (long)lost);
    check(lost <= 64, "ending a process returns its image and library frames (no leak)");
}

/* Identical read-only pages are shared between processes. */
static void test_share(void) {
    int64_t base = free_pages();
    int64_t pids[6];
    pids[0] = spawn_child("sleep");
    cos_sleep_ms(1500);
    int64_t one = base - free_pages();
    for (int i = 1; i < 6; i++) pids[i] = spawn_child("sleep");
    cos_sleep_ms(1500);
    free_pages();
    int64_t six = base - (int64_t)si.pages_free;
    uint32_t frames = si.pcache_frames, maps = si.pcache_maps;
    cos_printf("DLTEST INFO one child costs %ld pages, six cost %ld; shared frames %u, mappings %u\n", (long)one, (long)six, frames, maps);
    check(frames > 0, "read-only pages are registered in the shared page cache");
    check(maps >= frames + 40, "several processes map the same frames (at least 40 pages saved)");
    check(six < 6 * one, "six processes use less memory than six times one");
    for (int i = 0; i < 6; i++) if (pids[i] > 0) { int st; cos_waitpid(pids[i], &st, true); }
    cos_sleep_ms(300);
    int64_t after = free_pages();
    cos_printf("DLTEST INFO after they ended: %ld pages below the start\n", (long)(base - after));
    check(base - after <= 64, "after they end the memory is back");
    free_pages();
    check(si.pcache_maps <= si.pcache_frames + 4, "and no shared page is still mapped by a dead process");
}


typedef int (*fn_ii)(int, int);
typedef int (*fn_v)(void);
typedef void (*fn_setflag)(volatile int *);

static int64_t spawn_env(const char *path, const char *env) {
    const char *av[2] = { path, 0 };
    const char *ev[2] = { env, 0 };
    return cos_spawn_argv(path, av, ev);
}

/* dlopen runs constructors, dlclose runs destructors and unmaps. */
static void test_dlopen(void) {
    int64_t before = free_pages();
    static volatile int flag;
    flag = 0;
    int64_t h = cos_dlopen_flags("/lib/dla.c-osll", 0);
    check(h > 0, "dlopen opens a library");
    fn_v value = (fn_v)cos_dlsym(h, "dla_value"), runs = (fn_v)cos_dlsym(h, "dla_ctor_runs");
    check(value && runs, "dlsym finds its functions");
    check(runs && runs() == 1, "its constructor has run (once) when dlopen returns");
    check(value && value() == 42, "...and its effect is visible (a global it set)");
    ((fn_setflag)cos_dlsym(h, "dla_set_flag"))(&flag);

    int64_t h2 = cos_dlopen_flags("/lib/dla.c-osll", 0);
    check(h2 > 0 && h2 == h, "opening it again gives the same copy");
    check(runs() == 1, "...and does not run the constructor again");
    check(cos_dlclose(h2) == 0 && flag == 0, "the first dlclose keeps it loaded (a second handle is open)");
    check(cos_dlsym(h, "dla_value") != 0, "...it still resolves symbols");
    check(cos_dlclose(h) == 0 && flag == 1, "the last dlclose runs the destructor");
    check(cos_dlsym(h, "dla_value") == 0, "a closed handle names nothing (it is not mistaken for whatever loads into the slot next)");
    check(cos_dlerror() != 0, "dlerror says why");
    check(cos_dlclose(h) == -1, "closing it again is refused");
    int64_t after = free_pages();
    check(before - after <= 8, "the library's pages are unmapped and their frames returned");
    int64_t h3 = cos_dlopen_flags("/lib/dla.c-osll", 0);
    fn_v runs3 = (fn_v)cos_dlsym(h3, "dla_ctor_runs");
    check(h3 > 0 && runs3 && runs3() == 1, "opened again it is a fresh copy: its constructor runs once more");
    cos_dlclose(h3);
}

/* dependencies: construction order, cascade on close, a library another one uses stays */
static void test_deps(void) {
    int64_t before = free_pages();
    static volatile int fa, fb;
    fa = fb = 0;
    int64_t hb = cos_dlopen_flags("/lib/dlb.c-osll", 0);
    check(hb > 0, "a library with a dependency opens (the dependency is loaded with it)");
    check(((fn_v)cos_dlsym(hb, "dlb_saw"))() == 42, "the dependency's constructor ran before the dependent's");
    check(((fn_ii)cos_dlsym(hb, "dla_add")) != 0, "dlsym on a handle also searches what the library depends on");
    ((fn_setflag)cos_dlsym(hb, "dlb_set_flag"))(&fb);
    ((fn_setflag)cos_dlsym(hb, "dla_set_flag"))(&fa);
    check(cos_dlclose(hb) == 0, "closing it");
    check(fb == 2 && fa == 1, "unloads it and the dependency nobody else used: both destructors ran, dependent first");
    check(before - free_pages() <= 8, "both libraries' frames are back");

    /* dla opened by the program, dlb on top; closing dla must not pull the rug from under dlb */
    fa = fb = 0;
    int64_t ha = cos_dlopen_flags("/lib/dla.c-osll", 0);
    ((fn_setflag)cos_dlsym(ha, "dla_set_flag"))(&fa);
    hb = cos_dlopen_flags("/lib/dlb.c-osll", 0);
    ((fn_setflag)cos_dlsym(hb, "dlb_set_flag"))(&fb);
    cos_dlclose(ha);
    check(fa == 0, "closing a library another loaded library depends on keeps it");
    fn_v twice = (fn_v)0;
    int (*t2)(int) = (int (*)(int))cos_dlsym(hb, "dlb_twice");
    (void)twice;
    check(t2 && t2(21) == 42, "...and the dependent still works");
    cos_dlclose(hb);
    check(fa == 1 && fb == 2, "closing the dependent then unloads both");
    check(before - free_pages() <= 8, "no frames left behind");
}

static void test_fail(void) {
    int64_t before = free_pages();
    int64_t h = cos_dlopen_flags("/lib/dlbad.c-osll", 0);
    const char *e = cos_dlerror();
    check(h == -1, "a library with an unresolvable import does not load");
    check(e && cos_strstr(e, "dl_does_not_exist"), "dlerror names the missing symbol");
    check(before - free_pages() <= 4, "and nothing it mapped is left behind");
    check(cos_dlopen_flags("/lib/nonexistent.c-osll", 0) == -1 && cos_dlerror() != 0, "a library that does not exist is an error with a message");
    check(cos_dlopen_flags("/lib/dla.c-osll", COS_RTLD_NOLOAD) == -1, "RTLD_NOLOAD: not loaded, so no handle");
    int64_t ha = cos_dlopen_flags("/lib/dla.c-osll", 0);
    int64_t hn = cos_dlopen_flags("/lib/dla.c-osll", COS_RTLD_NOLOAD);
    check(ha > 0 && hn == ha, "RTLD_NOLOAD: loaded, so the same handle");
    cos_dlclose(hn); cos_dlclose(ha);
}

static void test_flags(void) {
    static volatile int flag;
    flag = 0;
    int64_t h = cos_dlopen_flags("/lib/dla.c-osll", COS_RTLD_NODELETE);
    ((fn_setflag)cos_dlsym(h, "dla_set_flag"))(&flag);
    check(cos_dlclose(h) == 0 && flag == 0, "RTLD_NODELETE: close does not run the destructor or unload");
    int64_t again = cos_dlopen_flags("/lib/dla.c-osll", COS_RTLD_NOLOAD);
    check(again > 0 && ((fn_v)cos_dlsym(again, "dla_value"))() == 42, "...the library is still there, state intact");
    cos_dlclose(again);
}

static int count_cb(const cos_dlobj_t *o, void *ctx) { (void)o; (*(int *)ctx)++; return 0; }
static int find_cb(const cos_dlobj_t *o, void *ctx) { return cos_strstr(o->name, (const char *)ctx) ? 1 : 0; }
static void test_api(void) {
    int64_t h = cos_dlopen_flags("/lib/dla.c-osll", 0);
    void *add = cos_dlsym(h, "dla_add");
    cos_dlinfo_t info;
    check(add && cos_dladdr(add, &info) == 0, "dladdr finds the object that holds an address");
    check(cos_strstr(info.path, "dla") != 0, "...its path");
    check(cos_strcmp(info.sym, "dla_add") == 0 && info.sym_addr == (uint64_t)(uintptr_t)add, "...and the symbol that covers it");
    check(cos_dladdr((void *)0x10, &info) == -1, "an address in no object has no answer");
    int n = 0;
    cos_dl_iterate(count_cb, &n);
    check(n >= 2, "dl_iterate walks the program and the library");
    check(cos_dl_iterate(find_cb, (void *)"dla") == 1, "...and finds the library by name");
    int64_t hd = cos_dlopen_flags("/lib/dlb.c-osll", COS_RTLD_GLOBAL);
    void *next = cos_dlsym(COS_RTLD_NEXT, "dla_add");
    check(next == add, "RTLD_NEXT from the program finds the definition in the libraries");
    cos_dlclose(hd); cos_dlclose(h);

    /* a library found through cos_dl_set_path (copy of dla under another name in another directory) */
    static char buf[65536];
    long len = cos_read_file("/lib/dla.c-osll", buf, sizeof buf);
    cos_mkdir("/tmp/dlx");
    check(len > 0 && cos_write_file("/tmp/dlx/dlq.c-osll", buf, (unsigned long)len) == len, "(a copy of the library is written to /tmp/dlx)");
    check(cos_dlopen_flags("dlq.c-osll", 0) == -1, "a bare name is not found outside the search path");
    check(cos_dl_set_path("/tmp/dlx") == 0, "cos_dl_set_path");
    int64_t hq = cos_dlopen_flags("dlq.c-osll", 0);
    check(hq > 0, "...and now it is found");
    cos_dlclose(hq);
    cos_unlink("/tmp/dlx/dlq.c-osll");
}

/* COS_PRELOAD puts a library first in the symbol scope of a new process. */
static void test_env(void) {
    int64_t pid = spawn_env("/bin/dluse.c-os", "X=1"); int st = -1;
    if (pid > 0) cos_waitpid(pid, &st, true);
    check(st == 5, "a program linked against libdla calls dla_add normally");
    pid = spawn_env("/bin/dluse.c-os", "COS_PRELOAD=/lib/dlp.c-osll"); st = -1;
    if (pid > 0) cos_waitpid(pid, &st, true);
    check(st == (1005 & 0xFF), "with COS_PRELOAD the preload library's dla_add wins");
    pid = spawn_env("/bin/dluse.c-os", "COS_PRELOAD=relative.c-osll:/lib/nonexistent.c-osll"); st = -1;
    if (pid > 0) cos_waitpid(pid, &st, true);
    check(st == 5, "a bad COS_PRELOAD entry is skipped and the program still runs");
}

/* open and close over and over: no frames, no address space lost */
static void test_soak(void) {
    cos_dlclose(cos_dlopen_flags("/lib/dlb.c-osll", 0));
    int64_t before = free_pages();
    int ok = 1;
    for (int i = 0; i < 150; i++) {
        int64_t h = cos_dlopen_flags("/lib/dlb.c-osll", i & 1 ? COS_RTLD_GLOBAL : 0);
        int (*t2)(int) = h > 0 ? (int (*)(int))cos_dlsym(h, "dlb_twice") : 0;
        if (!t2 || t2(i) != 2 * i) { ok = 0; break; }
        if (cos_dlclose(h) != 0) { ok = 0; break; }
    }
    check(ok, "150 open/use/close cycles of a library with a dependency");
    check(before - free_pages() <= 8, "...and the memory is the same afterwards");
}

/* the POSIX spelling over the same loader */
static void test_posix(void) {
    void *h = dlopen("/lib/dla.c-osll", RTLD_NOW | RTLD_LOCAL);
    check(h != NULL, "dlopen (dlfcn.h)");
    int (*add)(int, int) = h ? (int (*)(int, int))dlsym(h, "dla_add") : 0;
    check(add && add(40, 2) == 42, "dlsym and a call through it");
    Dl_info info;
    check(add && dladdr((void *)add, &info) != 0 && info.dli_sname && info.dli_fname && info.dli_saddr == (void *)add, "dladdr (Dl_info)");
    check(dlsym(h, "no_such_symbol") == NULL && dlerror() != NULL, "dlsym of an unknown name: NULL, and dlerror has the message");
    check(dlerror() == NULL, "dlerror is cleared by reading it");
    check(dlclose(h) == 0, "dlclose");
    check(dlopen("/lib/nonexistent.c-osll", RTLD_NOW) == NULL && dlerror() != NULL, "dlopen of a missing library: NULL and a message");
}

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "all";
    int all = cos_strcmp(what, "all") == 0;
    if (all || !cos_strcmp(what, "leak")) test_leak();
    if (all || !cos_strcmp(what, "share")) test_share();
    if (all || !cos_strcmp(what, "open")) test_dlopen();
    if (all || !cos_strcmp(what, "deps")) test_deps();
    if (all || !cos_strcmp(what, "fail")) test_fail();
    if (all || !cos_strcmp(what, "flags")) test_flags();
    if (all || !cos_strcmp(what, "api")) test_api();
    if (all || !cos_strcmp(what, "env")) test_env();
    if (all || !cos_strcmp(what, "posix")) test_posix();
    if (all || !cos_strcmp(what, "soak")) test_soak();
    cos_printf("DLTEST DONE failures=%d\n", fails);
    return fails ? 1 : 0;
}
