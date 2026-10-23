/* IPv6 over the Linux layer, against tools/ip6_peer.py (TCP service on 18082: "DOWN n\n" sends n pattern bytes; UDP echo on port 7). argv[1]: the peer (default fec0::2) */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static int fails;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); if (!ok_) fails++; } while (0)
static int pat(int i) { return (i * 131 + (i >> 8) * 7) & 0xFF; }
int main(int argc, char **argv) {
    struct sockaddr_in6 a; memset(&a, 0, sizeof a); a.sin6_family = AF_INET6; a.sin6_port = htons(18082);
    inet_pton(AF_INET6, argc > 1 ? argv[1] : "fec0::2", &a.sin6_addr);
    int s = socket(AF_INET6, SOCK_STREAM, 0);
    CHECK("socket(AF_INET6, SOCK_STREAM)", s >= 0);
    int rc = connect(s, (struct sockaddr *)&a, sizeof a);
    CHECK("IPv6 TCP connect", rc == 0);
    if (rc == 0) {
        struct sockaddr_in6 pn; socklen_t pl = sizeof pn;
        CHECK("getpeername", getpeername(s, (struct sockaddr *)&pn, &pl) == 0 && pn.sin6_family == AF_INET6 && ntohs(pn.sin6_port) == 18082 && !memcmp(&pn.sin6_addr, &a.sin6_addr, 16));
        struct sockaddr_in6 me; pl = sizeof me;
        CHECK("getsockname has a global address", getsockname(s, (struct sockaddr *)&me, &pl) == 0 && me.sin6_addr.s6_addr[0] == 0xfe && ntohs(me.sin6_port) != 0);
        CHECK("send command", send(s, "DOWN 6000\n", 10, 0) == 10);
        int total = 0, bad = 0; unsigned char buf[2048]; int n;
        while (total < 6000 && (n = recv(s, buf, sizeof buf, 0)) > 0) { for (int i = 0; i < n; i++) if (buf[i] != pat(total + i)) bad++; total += n; }
        CHECK("6000 pattern bytes received intact", total == 6000 && bad == 0);
        close(s);
    }
    int u = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 ua = a; ua.sin6_port = htons(7);
    CHECK("IPv6 UDP sendto", sendto(u, "dgram6", 6, 0, (struct sockaddr *)&ua, sizeof ua) == 6);
    struct pollfd up = { u, POLLIN, 0 };
    if (poll(&up, 1, 5000) == 1) {
        struct sockaddr_in6 from; socklen_t fl = sizeof from; char b[64];
        int n = recvfrom(u, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        CHECK("IPv6 UDP echo", n == 6 && !memcmp(b, "dgram6", 6) && from.sin6_family == AF_INET6 && ntohs(from.sin6_port) == 7);
    } else CHECK("IPv6 UDP reply arrives", 0);
    printf("%s\n", fails ? "SOCKS6 FAILED" : "SOCKS6 OK");
    return fails != 0;
}
