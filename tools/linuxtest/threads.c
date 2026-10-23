#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int counter, ready;
static void *worker(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < 20000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
    pthread_mutex_lock(&mu); ready++; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    return (void *)(id * 10);
}
int main(void) {
    pthread_t t[4];
    for (long i = 0; i < 4; i++) if (pthread_create(&t[i], NULL, worker, (void *)i)) { printf("create failed\n"); return 1; }
    pthread_mutex_lock(&mu);
    while (ready < 4) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    long sum = 0;
    for (int i = 0; i < 4; i++) { void *rv; pthread_join(t[i], &rv); sum += (long)rv; }
    printf("counter=%d (expect 80000) sum=%ld (expect 60) cpus=%ld\n", counter, sum, sysconf(_SC_NPROCESSORS_ONLN));
    return counter == 80000 && sum == 60 ? 0 : 2;
}
