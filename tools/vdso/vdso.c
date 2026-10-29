/* vdso.c - the vDSO of the Linux layer (src/kernel/cos_linux.c, "the vDSO"): clock_gettime, gettimeofday, time and clock_getres answered in user mode.
 * glibc finds it through AT_SYSINFO_EHDR and calls it instead of the system call. Without it every read of the clock was a #UD trap into the kernel (a browser asks
 * for the time hundreds of thousands of times a minute). The kernel writes the time into the data page that follows this page (vvar_data, at +0x1000) at every
 * timer tick; the calculation below is the one timer_get_ms_exact() does, so the two paths give the same milliseconds. Built by the Makefile (build/vdso.so). */
typedef unsigned long u64; typedef unsigned int u32; typedef long i64;

struct vdso_data {
    volatile u32 seq;                  /* odd while the kernel is writing */
    u32 pad;
    volatile u64 hz;                   /* TSC rate (0: not calibrated yet, the tick count is used) */
    volatile u64 base_tsc, base_ms;    /* the TSC value and the uptime in ms at the last calibration */
    volatile u64 tick_ms;              /* the uptime by the 1000 Hz tick count: the floor */
    volatile u64 wall_base;            /* wall-clock ms minus uptime ms */
};
extern const struct vdso_data vvar_data __attribute__((visibility("hidden")));

struct timespec { i64 tv_sec, tv_nsec; };
struct timeval { i64 tv_sec, tv_usec; };

static inline u64 rdtsc(void) { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }

static u64 uptime_ms(u64 *wall) {
    const struct vdso_data *d = &vvar_data;
    u32 s; u64 hz, bt, bm, tm, wb;
    do {
        while ((s = d->seq) & 1u) __builtin_ia32_pause();
        __asm__ volatile("" ::: "memory");
        hz = d->hz; bt = d->base_tsc; bm = d->base_ms; tm = d->tick_ms; wb = d->wall_base;
        __asm__ volatile("" ::: "memory");
    } while (s != d->seq);
    u64 ms = tm;
    if (hz) { u64 dt = rdtsc() - bt; u64 m = bm + (dt / hz) * 1000 + (dt % hz) * 1000 / hz; if (m > ms) ms = m; }
    if (wall) *wall = wb + ms;
    return ms;
}

int __vdso_clock_gettime(int clk, struct timespec *ts) {
    u64 wall, ms = uptime_ms(&wall);
    u64 t = (clk == 0 || clk == 5 || clk == 8) ? wall : ms;          /* the same choice as the system call: these are wall-clock, everything else uptime */
    ts->tv_sec = (i64)(t / 1000); ts->tv_nsec = (i64)(t % 1000) * 1000000;
    return 0;
}
int __vdso_gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (tv) { u64 wall; uptime_ms(&wall); tv->tv_sec = (i64)(wall / 1000); tv->tv_usec = (i64)(wall % 1000) * 1000; }
    return 0;
}
i64 __vdso_time(i64 *t) {
    u64 wall; uptime_ms(&wall);
    i64 s = (i64)(wall / 1000); if (t) *t = s; return s;
}
int __vdso_clock_getres(int clk, struct timespec *res) {
    (void)clk;
    if (res) { res->tv_sec = 0; res->tv_nsec = 1000000; }
    return 0;
}
/* the names a libc looks for (versioned LINUX_2.6 on a real kernel; this object has no version table, which a libc accepts) */
int clock_gettime(int clk, struct timespec *ts) __attribute__((alias("__vdso_clock_gettime")));
int gettimeofday(struct timeval *tv, void *tz) __attribute__((alias("__vdso_gettimeofday")));
i64 time(i64 *t) __attribute__((alias("__vdso_time")));
int clock_getres(int clk, struct timespec *res) __attribute__((alias("__vdso_clock_getres")));
