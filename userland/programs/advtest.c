/* advtest.c - exercises the newly exposed advanced capabilities:
 * threads, time, filesystem mutation, and setjmp/longjmp. */
#include "cos.h"

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) { failures++; cos_printf("ADV_FAIL %s\n", what); }
}

/* ---- threads ---- */
static volatile int worker_ran = 0;
static volatile long worker_sum = 0;

static void worker(void *arg) {
    long n = (long)(uintptr_t)arg;
    long s = 0;
    for (long i = 1; i <= n; ++i) s += i;
    worker_sum = s;
    worker_ran = 1;
    cos_thread_exit(0);
}

/* ---- setjmp/longjmp ---- */
static cos_jmp_buf jb;
static int deep_calls = 0;

static void deep(int depth) {
    deep_calls++;
    if (depth > 0) { deep(depth - 1); return; }
    cos_longjmp(jb, 42);   /* unwind several frames at once */
}

int main(void) {
    /* --- time: monotonic clock must advance across a sleep --- */
    uint64_t t0 = cos_time_ms();
    cos_sleep_ms(60);
    uint64_t t1 = cos_time_ms();
    check(t1 > t0, "time_ms advances");
    /* Bound it too: a clock that jumped wildly would also satisfy
     * "advances", so check the elapsed time is in a sane range. */
    check((t1 - t0) >= 30 && (t1 - t0) < 5000, "time_ms elapsed is plausible");
    cos_printf("ADV elapsed=%lu ms unix=%lu\n",
               (unsigned long)(t1 - t0), (unsigned long)cos_time_unix());

    /* --- filesystem mutation --- */
    check(cos_mkdir("/advdir") == 0, "mkdir");
    cos_stat_t st;
    check(cos_stat("/advdir", &st) == 0 && st.is_dir, "stat sees the new directory");

    const char *payload = "hello from advtest";
    check(cos_write_file("/advdir/a.txt", payload, cos_strlen(payload))
              == (ssize_t)cos_strlen(payload), "write into the new directory");
    check(cos_stat("/advdir/a.txt", &st) == 0 && !st.is_dir
              && st.size == cos_strlen(payload), "stat reports the right size");

    check(cos_rename("/advdir/a.txt", "/advdir/b.txt") == 0, "rename");
    check(cos_stat("/advdir/a.txt", &st) != 0, "old name is gone after rename");
    check(cos_stat("/advdir/b.txt", &st) == 0, "new name exists after rename");

    check(cos_unlink("/advdir/b.txt") == 0, "unlink");
    check(cos_stat("/advdir/b.txt", &st) != 0, "file is gone after unlink");
    check(cos_unlink("/") != 0, "unlink of root is refused");

    /* --- threads: run a real computation on another thread --- */
    int64_t tid = cos_thread_create(worker, (void *)(uintptr_t)1000);
    check(tid > 0, "thread_create");
    if (tid > 0) {
        check(cos_thread_join(tid, 5000) == 0, "thread_join");
        check(worker_ran == 1, "worker actually ran");
        /* 1+2+...+1000 = 500500 - checking the VALUE proves the thread
         * did the work, not merely that it started and exited. */
        check(worker_sum == 500500, "worker computed the right result");
        cos_printf("ADV thread sum=%ld\n", worker_sum);
    }

    /* --- setjmp/longjmp across several frames --- */
    int rc = cos_setjmp(jb);
    if (rc == 0) {
        deep(5);
        check(false, "longjmp did not transfer control");
    } else {
        check(rc == 42, "longjmp delivered its value");
        check(deep_calls == 6, "unwound all frames");
    }

    if (failures == 0) cos_printf("ADV_PASS threads, time, fs mutation, setjmp all verified\n");
    else cos_printf("ADV_FAIL %d checks failed\n", failures);
    return failures;
}
