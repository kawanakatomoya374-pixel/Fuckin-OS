/* many short-lived threads: each must get its own thread pointer and thread-local data, and gettid() must be unique among live threads */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <sys/syscall.h>
#define LIVE 40
static __thread long mine; static volatile int bad, started, bad_create, bad_mine, bad_dup, bad_tid; static pthread_t selfs[LIVE]; static long tids[LIVE];
static void *worker(void *a) {
    long slot = (long)a; mine = slot * 31 + 5; selfs[slot] = pthread_self(); tids[slot] = syscall(SYS_gettid);
    for (int i = 0; i < 300; ++i) {
        if (mine != slot * 31 + 5) { bad_mine++; break; }
        if (i % 10 == 0) sched_yield();
        if (i % 20 == 0) { FILE *f = fopen("/proc/self/stat", "r"); if (f) fclose(f); }
        for (long k = 0; k < LIVE; ++k) if (k != slot && selfs[k] && selfs[k] == pthread_self()) { bad_dup++; break; } else if (k != slot && tids[k] && tids[k] == syscall(SYS_gettid)) { bad_tid++; break; }
    }
    selfs[slot] = 0; tids[slot] = 0; return 0;
}
int main(void) {
    for (int round = 0; round < 60; ++round) {
        pthread_t th[LIVE]; for (long t = 0; t < LIVE; ++t) if (pthread_create(&th[t], 0, worker, (void *)t)) { bad_create++; th[t] = 0; }
        for (int t = 0; t < LIVE; ++t) if (th[t]) pthread_join(th[t], 0);
    }
    bad = bad_create + bad_mine + bad_dup + bad_tid; printf("tls2: %d problems (create failed %d, tls changed %d, same pthread_self %d, same tid %d)\n", bad, bad_create, bad_mine, bad_dup, bad_tid); return bad != 0;
}
