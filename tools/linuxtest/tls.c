/* thread-local storage must stay with its thread across preemption, system calls, page faults and CPU migration */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#define NT 8
static __thread long tid_tls; static __thread char big[3000]; static pthread_key_t key; static volatile int bad;
static void *worker(void *a) {
    long t = (long)a; tid_tls = t * 1000 + 7; big[17] = (char)t; pthread_setspecific(key, (void *)(t + 100));
    for (int i = 0; i < 40000; ++i) {
        if (i % 3 == 0) syscall(SYS_getpid);
        if (i % 7 == 0) sched_yield();
        if (i % 50 == 0) { char *m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); m[5000] = 1; munmap(m, 8192); }
        if (i % 500 == 0) usleep(1);
        errno = (int)t;
        if (tid_tls != t * 1000 + 7 || big[17] != (char)t || (long)pthread_getspecific(key) != t + 100 || errno != (int)t) { bad++; break; }
    }
    return 0;
}
int main(void) {
    pthread_key_create(&key, 0); pthread_t th[NT]; for (long t = 0; t < NT; ++t) pthread_create(&th[t], 0, worker, (void *)t);
    for (int t = 0; t < NT; ++t) pthread_join(th[t], 0);
    printf("tls: %d threads saw wrong thread-local data\n", bad); return bad != 0;
}
