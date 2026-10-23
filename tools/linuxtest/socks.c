#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static int fails;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); if (!ok_) fails++; } while (0)
int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "192.168.70.2";
    char buf[512];
    int sv[2];
    CHECK("socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK("socketpair a->b", write(sv[0], "ping", 4) == 4 && read(sv[1], buf, sizeof buf) == 4 && !memcmp(buf, "ping", 4));
    CHECK("socketpair b->a", send(sv[1], "pong", 4, 0) == 4 && recv(sv[0], buf, sizeof buf, 0) == 4 && !memcmp(buf, "pong", 4));
    pid_t pid = fork();
    if (pid == 0) { char b[16]; int n = read(sv[1], b, sizeof b); write(sv[1], b, n > 0 ? n : 0); _exit(0); }
    write(sv[0], "echo", 4); int n = read(sv[0], buf, sizeof buf); wait(NULL);
    CHECK("socketpair across fork", n == 4 && !memcmp(buf, "echo", 4));
    close(sv[1]); CHECK("socketpair EOF after peer close", read(sv[0], buf, sizeof buf) == 0);

    int f = open("/tmp/sf_in", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    for (int i = 0; i < 1000; i++) dprintf(f, "line %04d of the sendfile source\n", i);
    close(f);
    int in = open("/tmp/sf_in", O_RDONLY), out = open("/tmp/sf_out", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    ssize_t sent = sendfile(out, in, NULL, 1 << 20);
    CHECK("sendfile file->file copies everything", sent == 33000);
    close(out); close(in);
    in = open("/tmp/sf_in", O_RDONLY); off_t off = 33; int p[2]; pipe(p);
    sent = sendfile(p[1], in, &off, 33);
    CHECK("sendfile with offset", sent == 33 && off == 66 && read(p[0], buf, sizeof buf) == 33 && !memcmp(buf, "line 0001", 9));

    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK("socket(AF_INET, SOCK_STREAM)", s >= 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(8099) };
    inet_pton(AF_INET, host, &a.sin_addr);
    int rc = connect(s, (struct sockaddr *)&a, sizeof a);
    CHECK("connect to the host", rc == 0);
    if (rc == 0) {
        struct sockaddr_in me; socklen_t l = sizeof me;
        CHECK("getsockname", getsockname(s, (struct sockaddr *)&me, &l) == 0 && me.sin_addr.s_addr != 0 && ntohs(me.sin_port) != 0);
        const char *req = "GET /hello.txt HTTP/1.0\r\nHost: x\r\n\r\n";
        CHECK("send", send(s, req, strlen(req), 0) == (ssize_t)strlen(req));
        struct pollfd pf = { s, POLLIN, 0 };
        CHECK("poll sees the reply", poll(&pf, 1, 5000) == 1 && (pf.revents & POLLIN));
        int total = 0; char all[2048];
        while ((n = recv(s, all + total, sizeof all - 1 - total, 0)) > 0) total += n;
        all[total] = 0;
        CHECK("HTTP reply", strncmp(all, "HTTP/1.", 7) == 0 && strstr(all, "hello from the host"));
        close(s);
    }
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = { .sin_family = AF_INET, .sin_port = htons(8098) }; inet_pton(AF_INET, host, &ua.sin_addr);
    CHECK("UDP sendto", sendto(u, "datagram", 8, 0, (struct sockaddr *)&ua, sizeof ua) == 8);
    struct pollfd up = { u, POLLIN, 0 };
    if (poll(&up, 1, 5000) == 1) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        n = recvfrom(u, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        CHECK("UDP recvfrom (echo)", n == 8 && !memcmp(buf, "datagram", 8) && ntohs(from.sin_port) == 8098);
    } else CHECK("UDP reply arrives", 0);
    /* half-close: send the request, shut the write side, the reply still arrives */
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        const char *req = "GET /hello.txt HTTP/1.0\r\n\r\n"; send(s, req, strlen(req), 0);
        CHECK("shutdown(SHUT_WR)", shutdown(s, SHUT_WR) == 0);
        int total = 0; char all[1024];
        while ((n = recv(s, all + total, sizeof all - 1 - total, 0)) > 0) total += n;
        all[total] = 0;
        CHECK("reply readable after SHUT_WR", strstr(all, "hello from the host") != NULL);
        close(s);
    } else CHECK("second connect", 0);
    int e = socket(AF_INET6, SOCK_STREAM, 0);
    CHECK("AF_INET6 socket can be created", e >= 0);
    printf("%s\n", fails ? "SOCKS FAILED" : "SOCKS OK");
    return fails ? 1 : 0;
}
