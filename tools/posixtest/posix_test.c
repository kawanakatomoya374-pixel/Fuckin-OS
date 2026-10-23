/* posix_test.c - runtime checks of the POSIX layer of libcos (userland/lib/cos_posix.c): posixtest. Every check prints "POSIXTEST PASS|FAIL <what>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <getopt.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int fails;
static void check(int ok, const char *what) {
    printf("POSIXTEST %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static void test_cwd(void) {
    char buf[256];
    check(chdir("/") == 0, "chdir /");
    check(getcwd(buf, sizeof(buf)) && strcmp(buf, "/") == 0, "getcwd after chdir /");
    mkdir("/posixdir", 0755);
    check(chdir("/posixdir") == 0, "chdir into a directory");
    check(getcwd(buf, sizeof(buf)) && strcmp(buf, "/posixdir") == 0, "getcwd reports the directory");
    int fd = open("rel.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "open a relative path for writing");
    if (fd >= 0) { write(fd, "abc", 3); close(fd); }
    struct stat st;
    check(stat("/posixdir/rel.txt", &st) == 0 && st.st_size == 3, "relative path resolved against the working directory");
    check(chdir("/nonexistent_dir_xyz") != 0, "chdir to a missing directory fails");
    check(getcwd(buf, sizeof(buf)) && strcmp(buf, "/posixdir") == 0, "failed chdir leaves the directory unchanged");
    unlink("rel.txt");
    chdir("/");
}

static void test_files(void) {
    int fd = open("/posix_f.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(fd >= 3, "open returns a descriptor >= 3");
    char w[256], r[256];
    for (int i = 0; i < 256; i++) w[i] = (char)i;
    check(write(fd, w, 256) == 256, "write 256 bytes");
    check(lseek(fd, 0, SEEK_CUR) == 256, "lseek SEEK_CUR after write");
    check(lseek(fd, 0, SEEK_SET) == 0, "lseek SEEK_SET");
    check(read(fd, r, 256) == 256 && memcmp(r, w, 256) == 0, "read back is byte-exact");
    check(lseek(fd, 100, SEEK_SET) == 100 && read(fd, r, 4) == 4 && memcmp(r, w + 100, 4) == 0, "read at an offset");
    check(lseek(fd, -10, SEEK_END) == 246, "lseek SEEK_END");
    check(read(fd, r, 100) == 10, "read stops at EOF");
    check(read(fd, r, 10) == 0, "read at EOF returns 0");
    struct stat st;
    check(fstat(fd, &st) == 0 && st.st_size == 256, "fstat size");
    check(ftruncate(fd, 100) == 0, "ftruncate shortens");
    check(fstat(fd, &st) == 0 && st.st_size == 100, "size after ftruncate");
    check(close(fd) == 0, "close");
    check(close(fd) != 0, "closing twice fails");
    check(read(fd, r, 1) < 0, "read on a closed descriptor fails");
    check(open("/posix_missing_file", O_RDONLY) < 0, "open of a missing file fails");
    check(access("/posix_f.bin", F_OK) == 0, "access finds the file");
    check(access("/posix_missing_file", F_OK) != 0, "access misses a missing file");
    check(unlink("/posix_f.bin") == 0, "unlink");
    check(access("/posix_f.bin", F_OK) != 0, "file is gone after unlink");

    mkdir("/posix_dir2", 0755);
    int a = open("/posix_dir2/a", O_WRONLY | O_CREAT, 0644); if (a >= 0) close(a);
    int b = open("/posix_dir2/b", O_WRONLY | O_CREAT, 0644); if (b >= 0) close(b);
    DIR *d = opendir("/posix_dir2");
    check(d != NULL, "opendir");
    int seen_a = 0, seen_b = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) { if (!strcmp(e->d_name, "a")) seen_a = 1; if (!strcmp(e->d_name, "b")) seen_b = 1; }
        closedir(d);
    }
    check(seen_a && seen_b, "readdir lists both entries");
    unlink("/posix_dir2/a"); unlink("/posix_dir2/b");
}

static void test_dup_fcntl(void) {
    int fd = dup(1);
    check(fd >= 3 || fd < 0, "dup of the console gives a new descriptor or fails cleanly");
    if (fd >= 0) close(fd);
    check(isatty(1) == 1, "isatty on stdout");
    int f = open("/posix_isatty", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(f >= 0 && isatty(f) == 0, "isatty is false for a file");
    if (f >= 0) { errno = 0; int d2 = dup(f); check(d2 < 0 && errno == ENOSYS, "dup of a file returns ENOSYS (documented limit)"); close(f); }
    unlink("/posix_isatty");
}

static void test_pipe(void) {
    int p[2];
    check(pipe(p) == 0, "pipe");
    check(write(p[1], "hello", 5) == 5, "write to the pipe");
    char b[16] = {0};
    check(read(p[0], b, 16) == 5 && !memcmp(b, "hello", 5), "read from the pipe");
    close(p[1]);
    check(read(p[0], b, 16) == 0, "read returns 0 once the write end is closed");
    close(p[0]);
}

static void *thr(void *arg) { int *v = arg; (*v)++; return (void *)(long)(*v * 2); }
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int counter;
static void *inc(void *arg) { (void)arg; for (int i = 0; i < 1000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); } return 0; }
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int ready;
static void *waiter(void *arg) { (void)arg; pthread_mutex_lock(&mu); while (!ready) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu); return (void *)7; }
static pthread_key_t key;
static void *keyfn(void *arg) { pthread_setspecific(key, arg); usleep(20000); return pthread_getspecific(key); }
static int once_n;
static void once_fn(void) { once_n++; }


static void test_pthread(void) {
    pthread_t t;
    int v = 20; void *ret = 0;
    check(pthread_create(&t, 0, thr, &v) == 0, "pthread_create");
    check(pthread_join(t, &ret) == 0 && v == 21 && (long)ret == 42, "pthread_join returns the value");
    pthread_t ts[4];
    for (int i = 0; i < 4; i++) pthread_create(&ts[i], 0, inc, 0);
    for (int i = 0; i < 4; i++) pthread_join(ts[i], 0);
    check(counter == 4000, "mutex protects a shared counter");
    pthread_t w;
    pthread_create(&w, 0, waiter, 0);
    usleep(50000);
    pthread_mutex_lock(&mu); ready = 1; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
    ret = 0; pthread_join(w, &ret);
    check((long)ret == 7, "cond_wait wakes on cond_signal");
    pthread_key_create(&key, 0);
    pthread_t k1, k2; void *r1 = 0, *r2 = 0;
    pthread_create(&k1, 0, keyfn, (void *)11); pthread_create(&k2, 0, keyfn, (void *)22);
    pthread_join(k1, &r1); pthread_join(k2, &r2);
    check((long)r1 == 11 && (long)r2 == 22, "thread-specific keys are per thread");
    pthread_once_t o = PTHREAD_ONCE_INIT;
    pthread_once(&o, once_fn); pthread_once(&o, once_fn);
    check(once_n == 1, "pthread_once runs once");
    check(pthread_equal(pthread_self(), pthread_self()), "pthread_self is stable");
}

static void test_env(void) {
    check(setenv("POSIXTEST_VAR", "one", 1) == 0, "setenv");
    check(getenv("POSIXTEST_VAR") && !strcmp(getenv("POSIXTEST_VAR"), "one"), "getenv sees it");
    setenv("POSIXTEST_VAR", "two", 0);
    check(!strcmp(getenv("POSIXTEST_VAR"), "one"), "setenv without overwrite keeps the value");
    setenv("POSIXTEST_VAR", "three", 1);
    check(!strcmp(getenv("POSIXTEST_VAR"), "three"), "setenv with overwrite replaces it");
    unsetenv("POSIXTEST_VAR");
    check(getenv("POSIXTEST_VAR") == NULL, "unsetenv");
}

static int cmp_r(const void *a, const void *b, void *arg) {
    int x = *(const int *)a, y = *(const int *)b;
    return *(int *)arg ? (y > x) - (y < x) : (x > y) - (x < y);
}
static void test_misc(void) {
    int a[] = { 5, 3, 9, 1, 7, 3 }, desc = 1;
    qsort_r(a, 6, sizeof(int), cmp_r, &desc);
    check(a[0] == 9 && a[1] == 7 && a[5] == 1, "qsort_r passes its argument (descending)");

    char *av[] = { "prog", "-a", "-b", "val", "-cX", "--long=5", "rest", NULL };
    optind = 1;
    int got_a = 0, got_c = 0; const char *bv = 0;
    int c;
    while ((c = getopt(7, av, "ab:c:")) != -1) {
        if (c == 'a') got_a = 1; else if (c == 'b') bv = optarg; else if (c == 'c') got_c = optarg && optarg[0] == 'X';
    }
    check(got_a && got_c && bv && !strcmp(bv, "val"), "getopt parses flags and arguments");
    check(optind >= 1 && optind <= 7, "getopt leaves optind in range");

    struct timespec ts = { 0, 30 * 1000000 };
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    nanosleep(&ts, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check(ms >= 25 && ms < 2000, "nanosleep sleeps about as long as asked");

    void *m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(m != MAP_FAILED, "mmap anonymous");
    if (m != MAP_FAILED) { memset(m, 0xAB, 8192); check(((unsigned char *)m)[8191] == 0xAB, "mapped memory is writable"); check(munmap(m, 8192) == 0, "munmap"); }
}

static volatile int got_sig;
static void on_usr1(int s) { got_sig = s; }
static void test_signal(void) {
    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    for (int i = 0; i < 50 && !got_sig; i++) usleep(10000);
    check(got_sig == SIGUSR1, "raise delivers a handled signal");
}

static void test_socket(void) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    check(srv >= 0, "socket");
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(47123); a.sin_addr.s_addr = htonl(INADDR_ANY);
    int one = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    check(bind(srv, (struct sockaddr *)&a, sizeof(a)) == 0, "bind");
    check(listen(srv, 4) == 0, "listen");
    int cli = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in lo = a; lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = connect(cli, (struct sockaddr *)&lo, sizeof(lo));
    check(rc == 0, "connect to a listener on loopback");
    if (rc != 0) { close(cli); close(srv); return; }
    struct pollfd pf = { srv, POLLIN, 0 };
    check(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLIN), "poll sees the pending connection");
    int cn = accept(srv, 0, 0);
    check(cn >= 0, "accept");
    check(send(cli, "ping", 4, 0) == 4, "send");
    pf.fd = cn;
    check(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLIN), "poll sees readable data");
    char b[16] = {0};
    check(recv(cn, b, sizeof(b), 0) == 4 && !memcmp(b, "ping", 4), "recv gets the data");
    check(send(cn, "pong", 4, 0) == 4, "reply");
    memset(b, 0, sizeof(b));
    check(recv(cli, b, sizeof(b), 0) == 4 && !memcmp(b, "pong", 4), "client receives the reply");
    char ip[32];
    check(inet_pton(AF_INET, "192.168.1.5", &a.sin_addr) == 1 && inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip)) && !strcmp(ip, "192.168.1.5"), "inet_pton / inet_ntop round trip");
    close(cn); close(cli); close(srv);
}


extern int cos_cpu_load(unsigned char *pct, int max);
static int cpus_online(void) { unsigned char pct[64]; return cos_cpu_load(pct, 64); }

/* Threads really run on several CPUs: each one spins and notes which CPU it is on. */
extern int cos_getcpu(void);
extern unsigned long long cos_time_ms(void);
static volatile unsigned long long cpu_mask;
static volatile int smp_done;
static void *smp_spin(void *arg) {
    unsigned long long end = cos_time_ms() + 400;
    volatile unsigned long x = (unsigned long)arg;
    while (cos_time_ms() < end) {
        for (int i = 0; i < 2000; i++) x = x * 6364136223846793005ull + 1442695040888963407ull;
        __atomic_fetch_or(&cpu_mask, 1ull << cos_getcpu(), __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&smp_done, 1, __ATOMIC_RELAXED);
    return (void *)(unsigned long)x;
}
static void test_smp(void) {
    pthread_t t[4];
    unsigned long long t0 = cos_time_ms();
    for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, smp_spin, (void *)(unsigned long)(i + 1));
    for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
    int n = 0; for (int c = 0; c < 64; c++) if (cpu_mask & (1ull << c)) n++;
    printf("POSIXTEST INFO 4 spinning threads ran on %d CPUs (mask 0x%llx) in %llu ms\n", n, (unsigned long long)cpu_mask, cos_time_ms() - t0);
    check(smp_done == 4, "all spinning threads finished");
    check(cpus_online() <= 1 || n >= 2, "threads ran on at least two CPUs (when more than one is online)");
}

/* A process that exits while its threads still run on other CPUs: its address space must stay until they are off, and nothing may crash. */
extern long long cos_spawn_argv(const char *path, const char *const *argv, const char *const *envp);
extern int cos_waitpid(long long pid, int *status, int block);
static void *spin_forever(void *arg) { volatile unsigned long x = 0; (void)arg; for (;;) x++; return 0; }
static void test_exit_with_threads(void) {
    int ok = 0;
    for (int i = 0; i < 8; i++) {
        const char *av[3] = { "/bin/posixtest.c-os", "exitspin", 0 };
        const char *ev[1] = { 0 };
        long long pid = cos_spawn_argv(av[0], av, ev);
        int st = 0;
        if (pid > 0 && cos_waitpid(pid, &st, 1) >= 0) ok++;
    }
    check(ok == 8, "a process may exit while its threads run on other CPUs");
}

/* A ring-3 compute job (cos_par_*) still gets a core while threads keep the other CPUs busy. */
extern int cos_par_start(void (*fn)(void *), void *arg, void *stack, unsigned long stack_size);
extern int cos_par_poll(int id, void *fault);
extern int cos_par_free(int id);
static volatile int par_flag, stop_spin;
static void par_fn(void *a) { volatile unsigned long x = 0; for (unsigned long i = 0; i < 3000000ul; i++) x += i; *(volatile int *)a = 1; }
static void *spin_until(void *a) { (void)a; while (!stop_spin) __asm__ volatile("pause"); return 0; }
static void test_par_job(void) {
    if (cpus_online() <= 1) return;                 /* a compute job needs another core */
    pthread_t t[3];
    stop_spin = 0; par_flag = 0;
    for (int i = 0; i < 3; i++) pthread_create(&t[i], 0, spin_until, 0);
    usleep(100000);
    unsigned long ss = 256ul * 1024;
    char *stack = malloc(ss);
    if (stack) for (unsigned long o = 0; o < ss; o += 4096) stack[o] = 0;
    int id = stack ? cos_par_start(par_fn, (void *)&par_flag, stack, ss) : -1;
    check(id >= 0, "a compute job starts while threads occupy the other CPUs");
    int st = 0;
    if (id >= 0) {
        for (int i = 0; i < 100; i++) { st = cos_par_poll(id, 0); if (st >= 3) break; usleep(50000); }
        check(st == 3 && par_flag == 1, "the compute job ran to the end");
        cos_par_free(id);
    }
    stop_spin = 1;
    for (int i = 0; i < 3; i++) pthread_join(t[i], 0);
    free(stack);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "mknotimer")) { int f = open("/etc/smp_notimer", O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f >= 0) { write(f, "1", 1); close(f); } printf("POSIXTEST INFO mknotimer %d\n", f >= 0); return 0; }
    if (argc > 1 && !strcmp(argv[1], "rmnotimer")) { unlink("/etc/smp_notimer"); printf("POSIXTEST INFO rmnotimer\n"); return 0; }
    if (argc > 1 && !strcmp(argv[1], "exitspin")) {
        pthread_t t[3];
        for (int i = 0; i < 3; i++) pthread_create(&t[i], 0, spin_forever, 0);
        usleep(30000);
        return 0;
    }
    (void)argc; (void)argv;
    test_cwd();
    test_files();
    test_dup_fcntl();
    test_pipe();
    test_pthread();
    test_env();
    test_misc();
    test_signal();
    test_socket();
    test_smp();
    test_exit_with_threads();
    test_par_job();
    printf("POSIXTEST DONE failures=%d\n", fails);
    return fails ? 1 : 0;
}
