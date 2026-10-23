#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <unistd.h>
static int fails;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); if (!ok_) fails++; } while (0)
static int pfd[2];
static void *writer(void *arg) { (void)arg; usleep(60000); write(pfd[1], "late", 4); return NULL; }
int main(void) {
    char buf[64];
    CHECK("pipe", pipe(pfd) == 0);
    CHECK("pipe write/read", write(pfd[1], "hello", 5) == 5 && read(pfd[0], buf, sizeof buf) == 5 && !memcmp(buf, "hello", 5));
    struct pollfd p = { pfd[0], POLLIN, 0 };
    CHECK("poll times out on an empty pipe", poll(&p, 1, 20) == 0);
    write(pfd[1], "x", 1);
    CHECK("poll sees data", poll(&p, 1, 20) == 1 && (p.revents & POLLIN));
    read(pfd[0], buf, 1);
    int fl = fcntl(pfd[0], F_GETFL); fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK);
    CHECK("nonblocking read gives EAGAIN", read(pfd[0], buf, 1) == -1 && errno == EAGAIN);
    fcntl(pfd[0], F_SETFL, fl);
    int ep = epoll_create1(0); struct epoll_event ev = { .events = EPOLLIN, .data.fd = pfd[0] };
    CHECK("epoll_ctl add", epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev) == 0);
    pthread_t t; pthread_create(&t, NULL, writer, NULL);
    struct epoll_event out[4]; int n = epoll_wait(ep, out, 4, 2000);
    CHECK("epoll_wait wakes when another thread writes", n == 1 && out[0].data.fd == pfd[0]);
    CHECK("... and the data is there", read(pfd[0], buf, sizeof buf) == 4 && !memcmp(buf, "late", 4));
    pthread_join(t, NULL);
    int efd = eventfd(0, 0); uint64_t v = 5; write(efd, &v, 8); v = 0;
    CHECK("eventfd", read(efd, &v, 8) == 8 && v == 5);
    int d = dup(pfd[1]); write(d, "dup", 3);
    CHECK("dup shares the pipe", read(pfd[0], buf, sizeof buf) == 3 && !memcmp(buf, "dup", 3));
    fflush(stdout); int saved = dup(1); dup2(pfd[1], 1); printf("to the pipe"); fflush(stdout); dup2(saved, 1);
    n = read(pfd[0], buf, sizeof buf);
    CHECK("dup2 redirects stdout into a pipe", n == 11 && !memcmp(buf, "to the pipe", 11));
    fd_set rs; FD_ZERO(&rs); FD_SET(pfd[0], &rs); struct timeval tv = { 0, 10000 };
    CHECK("select times out", select(pfd[0] + 1, &rs, NULL, NULL, &tv) == 0);
    close(pfd[1]); close(d);
    CHECK("read returns 0 when every writer is closed", read(pfd[0], buf, 1) == 0);
    printf("fds done, %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
