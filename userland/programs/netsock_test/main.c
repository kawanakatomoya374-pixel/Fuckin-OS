/* netsock_test.c-os - regression target for the ring-3 socket API (SYS_SOCKET .. SYS_NET_INFO). Prints [NETSOCK] lines that
 * tools/run_netsock_test.sh reads. Launched by the kernel with VALIDATION_NETSOCK=1; the host side is tools/net_test_server.py. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cos.h"

#define HOST_PORT 18080            /* net_test_server.py: TCP DOWN/UP service */
#define UDP_ECHO_PORT 18082        /* net_test_server.py: UDP echo */
#define LISTEN_PORT 7000           /* forwarded from the host by QEMU hostfwd */

static int fails;
static void say(const char *s) { cos_puts(s); }
static void check(const char *name, int ok) {
    char b[200];
    snprintf(b, sizeof b, "[NETSOCK] %s %s\n", ok ? "PASS" : "FAIL", name);
    say(b);
    if (!ok) fails++;
}
static uint8_t pat(uint64_t i) { return (uint8_t)(i * 131u + (i >> 8) * 7u); }

int main(void) {
    char b[200];
    cos_netinfo_t ni;
    check("net_info works", cos_net_info(&ni) == 0);
    for (int i = 0; i < 300 && !ni.has_lease; i++) { cos_sleep_ms(100); cos_net_info(&ni); }     /* the lease takes a moment after boot */
    check("interface has a DHCP lease", ni.has_lease);
    uint32_t gw = COS_IP(ni.gateway[0], ni.gateway[1], ni.gateway[2], ni.gateway[3]);
    snprintf(b, sizeof b, "[NETSOCK] info ip=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u\n", ni.ip[0], ni.ip[1], ni.ip[2], ni.ip[3],
             ni.gateway[0], ni.gateway[1], ni.gateway[2], ni.gateway[3], ni.dns[0], ni.dns[1], ni.dns[2], ni.dns[3]);
    say(b);

    /* ---- argument checking ---- */
    check("recv on a bad descriptor -> EBADF", cos_recv(55, b, 4) == -COS_EBADF);
    check("close on a bad descriptor -> EBADF", cos_sock_close(-1) == -COS_EBADF);
    check("unknown socket type -> EINVAL", cos_socket(9) == -COS_EINVAL);
    int l1 = cos_socket(COS_SOCK_STREAM);
    check("accept on a socket that is not listening -> EINVAL", cos_accept(l1, NULL) == -COS_EINVAL);
    check("listen without bind -> EINVAL", cos_listen(l1, 4) == -COS_EINVAL);
    check("bind works", cos_bind(l1, 0, 7777) == 0);
    int l2 = cos_socket(COS_SOCK_STREAM);
    check("second bind to the same port -> EADDRINUSE", cos_bind(l2, 0, 7777) == -COS_EADDRINUSE);
    check("listen works", cos_listen(l1, 4) == 0);
    check("a listening port stays taken -> EADDRINUSE", cos_bind(l2, 0, 7777) == -COS_EADDRINUSE);
    check("accept with nobody connecting -> EAGAIN", cos_accept(l1, NULL) == -COS_EAGAIN);
    cos_sock_close(l1); cos_sock_close(l2);
    int l3 = cos_tcp_listen(7777, 2);
    check("the port is free again after close", l3 >= 0);
    cos_sock_close(l3);

    /* ---- descriptor limit ---- */
    int many[40], got = 0;
    for (int i = 0; i < 40; i++) { many[i] = cos_socket(COS_SOCK_DGRAM); if (many[i] >= 0) got++; }
    check("per-process descriptor limit -> EMFILE", got > 4 && got < 40 && many[39] == -COS_EMFILE);
    for (int i = 0; i < 40; i++) if (many[i] >= 0) cos_sock_close(many[i]);

    /* ---- UDP echo through the host ---- */
    {
        int u = cos_socket(COS_SOCK_DGRAM);
        char msg[600]; for (int i = 0; i < (int)sizeof msg; i++) msg[i] = (char)pat((uint64_t)i);
        int ok = 1, replies = 0;
        cos_sockaddr_t me; cos_getsockname(u, &me);
        check("UDP socket got an ephemeral port", me.port >= 49152);
        for (int k = 0; k < 5; k++) {              /* five datagrams back to back: all must be queued, none overwritten */
            msg[0] = (char)('A' + k);
            if (cos_sendto(u, msg, 100 + k * 100, gw, UDP_ECHO_PORT) < 0) ok = 0;
        }
        char rb[700];
        for (int k = 0; k < 5; k++) {
            if (cos_sock_wait(u, COS_POLLIN, 3000) < 0) break;
            cos_sockaddr_t from;
            ssize_t n = cos_recvfrom(u, rb, sizeof rb, &from);
            if (n < 0) break;
            replies++;
            if (from.ip != gw || from.port != UDP_ECHO_PORT) ok = 0;
            if (n != 100 + k * 100 || rb[0] != (char)('A' + k)) ok = 0;
            if (n > 1 && memcmp(rb + 1, msg + 1, (size_t)n - 1) != 0) ok = 0;
        }
        snprintf(b, sizeof b, "[NETSOCK] udp replies=%d\n", replies); say(b);
        check("UDP sendto/recvfrom keeps five datagrams in order with their sender", ok && replies == 5);
        cos_sock_close(u);
    }

    /* ---- TCP client: refused port, then a real transfer ---- */
    {
        int c = cos_tcp_connect(gw, 1, 4000);
        check("connect to a closed port -> ECONNREFUSED", c == -COS_ECONNREFUSED);
        c = cos_tcp_connect(gw, HOST_PORT, 8000);
        check("TCP connect to the host", c >= 0);
        if (c >= 0) {
            cos_sockaddr_t pn; cos_getpeername(c, &pn);
            check("getpeername reports the host", pn.ip == gw && pn.port == HOST_PORT);
            check("send the request", cos_send_all(c, "DOWN 300000\n", 12, 3000) == 12);
            static uint8_t buf[8192];
            size_t total = 0; int bad = 0; ssize_t n;
            uint64_t t0 = cos_time_ms();
            while (total < 300000 && (n = cos_recv_wait(c, buf, sizeof buf, 15000)) > 0) {
                for (ssize_t i = 0; i < n; i++) if (buf[i] != pat(total + (size_t)i)) bad++;
                total += (size_t)n;
            }
            snprintf(b, sizeof b, "[NETSOCK] download %zu bytes in %llu ms, %d bad\n", total, (unsigned long long)(cos_time_ms() - t0), bad); say(b);
            check("download 300000 bytes intact", total == 300000 && bad == 0);
            n = cos_recv_wait(c, buf, sizeof buf, 5000);
            check("end of stream is reported as 0", n == 0);
            cos_sock_close(c);
        }
        c = cos_tcp_connect(gw, HOST_PORT, 8000);
        if (c >= 0) {
            static uint8_t out[200000];
            for (size_t i = 0; i < sizeof out; i++) out[i] = pat(i);
            check("send UP request", cos_send_all(c, "UP 200000\n", 10, 3000) == 10);
            check("upload 200000 bytes", cos_send_all(c, out, sizeof out, 20000) == (ssize_t)sizeof out);
            char rep[8] = {0};
            ssize_t n = cos_recv_wait(c, rep, sizeof rep - 1, 15000);
            check("host confirms the upload", n >= 2 && rep[0] == 'O' && rep[1] == 'K');
            cos_sock_close(c);
        } else check("TCP connect for upload", 0);
    }

    /* ---- TCP server: echo whatever the host sends ---- */
    {
        int l = cos_tcp_listen(LISTEN_PORT, 4);
        check("server socket listening", l >= 0);
        if (l >= 0) {
            snprintf(b, sizeof b, "[NETSOCK] listening %d\n", LISTEN_PORT); say(b);
            cos_sockaddr_t peer;
            int a = cos_tcp_accept_wait(l, &peer, 90000);
            check("accepted the host's connection", a >= 0);
            if (a >= 0) {
                static uint8_t buf[4096];
                size_t echoed = 0; ssize_t n;
                while ((n = cos_recv_wait(a, buf, sizeof buf, 20000)) > 0) {
                    if (cos_send_all(a, buf, (size_t)n, 10000) != n) break;
                    echoed += (size_t)n;
                }
                snprintf(b, sizeof b, "[NETSOCK] echoed %zu bytes\n", echoed); say(b);
                check("echoed everything until the host closed", echoed >= 100000 && n == 0);
                cos_sock_close(a);
            }
            /* a second client while we keep listening, to show the accept queue works more than once */
            int a2 = cos_tcp_accept_wait(l, &peer, 30000);
            check("accepted a second connection", a2 >= 0);
            if (a2 >= 0) {
                char hello[] = "hello from C-OS\n";
                cos_send_all(a2, hello, strlen(hello), 3000);
                cos_sock_close(a2);
            }
            cos_sock_close(l);
        }
    }

    snprintf(b, sizeof b, "[NETSOCK] DONE failed=%d\n", fails);
    say(b);
    return fails ? 1 : 0;
}
