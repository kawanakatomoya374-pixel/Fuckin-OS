/* timed waits: futex with a monotonic absolute timeout, nanosleep, poll and epoll timeouts must end on time */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
static long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }
int main(void) {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; pthread_condattr_t ca; pthread_cond_t c;
    pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC); pthread_cond_init(&c, &ca);
    long t0 = ms(); struct timespec dl; clock_gettime(CLOCK_MONOTONIC, &dl); dl.tv_nsec += 300000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&m); int r = pthread_cond_timedwait(&c, &m, &dl); pthread_mutex_unlock(&m);
    printf("cond_timedwait(300ms) -> %d (%s) after %ld ms\n", r, r == ETIMEDOUT ? "timeout" : "other", ms() - t0);
    t0 = ms(); struct timespec s = { 0, 200000000 }; nanosleep(&s, 0); printf("nanosleep(200ms) after %ld ms\n", ms() - t0);
    t0 = ms(); int p = poll(0, 0, 250); printf("poll(250ms) = %d after %ld ms\n", p, ms() - t0);
    return 0;
}
