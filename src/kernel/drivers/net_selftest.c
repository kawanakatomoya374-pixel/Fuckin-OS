/**
 * net_selftest.c - validation-only network exerciser (COS_VALIDATION_NET=1). Never part of a normal image.
 *
 * Pings the gateway, then moves pattern data to and from tools/net_test_server.py over TCP, first on a clean link and
 * then with packets dropped at random (net_set_loss_pct), so the retransmission, fast-retransmit, ordering and
 * window logic in tcp.c is exercised for real. Results go to the serial log as [NETTEST] lines.
 */
#include "net.h"
#include "tcp.h"
#include "icmp.h"
#include "dhcp.h"
#include "http.h"
#include "net_fw.h"
#include "ip6.h"
#include "dns.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "task.h"

#ifndef COS_NET_TEST_PORT
#define COS_NET_TEST_PORT 18080
#endif

#if COS_VALIDATION_NET
extern void net_set_loss_pct(uint32_t pct);

static uint8_t pat(uint64_t i) { return (uint8_t)(i * 131u + (i >> 8) * 7u); }

static int line_to(char* out, const char* verb, uint64_t n) {
    int k = 0;
    for (const char* p = verb; *p; ++p) out[k++] = *p;
    out[k++] = ' ';
    char tmp[24]; int t = 0;
    uint64_t v = n;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t) out[k++] = tmp[--t];
    out[k++] = '\n';
    return k;
}

static socket_t* connect_server(void) {
    socket_t* s = socket_create(AF_INET, SOCK_STREAM, 0);
    if (!s) return NULL;
    sock_addr_t sa; memset(&sa, 0, sizeof sa);
    sa.family = AF_INET; sa.port = COS_NET_TEST_PORT; sa.addr = net_get_default_iface()->gateway;   /* QEMU user-mode networking: the gateway address is the host */
    if (socket_connect(s, &sa) != 0) { socket_close(s); return NULL; }
    return s;
}

static int send_all(socket_t* s, const void* buf, size_t n, uint64_t deadline) {
    size_t sent = 0;
    while (sent < n && get_timer_ticks() < deadline) {
        int r = socket_send(s, (void*)((const uint8_t*)buf + sent), n - sent, 0);
        if (r < 0) return -1;
        sent += (size_t)r;
        if (r == 0) { net_poll(); thread_yield(); }
    }
    return sent == n ? 0 : -1;
}

static int test_down(uint64_t n, uint64_t budget_ms) {
    socket_t* s = connect_server();
    if (!s) return -1;
    char line[40]; int k = line_to(line, "DOWN", n);
    uint64_t deadline = get_timer_ticks() + budget_ms;
    int ok = send_all(s, line, (size_t)k, deadline) == 0;
    uint64_t got = 0; int bad = 0;
    static uint8_t buf[8192];
    while (ok && got < n && get_timer_ticks() < deadline) {
        int r = socket_recv(s, buf, sizeof buf, 0);
        if (r < 0) break;
        if (r == 0) { thread_yield(); continue; }
        for (int i = 0; i < r; ++i) if (buf[i] != pat(got + (uint64_t)i)) { bad++; break; }
        got += (uint64_t)r;
    }
    socket_close(s);
    return (ok && got == n && !bad) ? 0 : -1;
}

static int test_up(uint64_t n, uint64_t budget_ms) {
    socket_t* s = connect_server();
    if (!s) return -1;
    char line[40]; int k = line_to(line, "UP", n);
    uint64_t deadline = get_timer_ticks() + budget_ms;
    int ok = send_all(s, line, (size_t)k, deadline) == 0;
    static uint8_t chunk[4096];
    uint64_t done = 0;
    while (ok && done < n) {
        size_t c = n - done < sizeof chunk ? (size_t)(n - done) : sizeof chunk;
        for (size_t i = 0; i < c; ++i) chunk[i] = pat(done + i);
        ok = send_all(s, chunk, c, deadline) == 0;
        done += c;
    }
    char reply[8] = {0}; size_t rl = 0;
    while (ok && rl < 3 && get_timer_ticks() < deadline) {
        int r = socket_recv(s, reply + rl, 3 - rl, 0);
        if (r < 0) break;
        if (r == 0) { thread_yield(); continue; }
        rl += (size_t)r;
    }
    socket_close(s);
    return (ok && rl >= 2 && reply[0] == 'O' && reply[1] == 'K') ? 0 : -1;
}

/* The real HTTP client over the new TCP: status, Content-Length framing and a 300 KB body. */
static int test_http(void) {
    http_client_t* h = http_create();
    if (!h) return -1;
    char url[64]; net_iface_t* f = net_get_default_iface();
    int k = 0; const char* pre = "http://";
    while (*pre) url[k++] = *pre++;
    for (int i = 0; i < 4; ++i) {
        unsigned v = f->gateway.addr[i]; char t[4]; int n = 0;
        do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) url[k++] = t[--n];
        if (i < 3) url[k++] = '.';
    }
    const char* tail = ":" ; url[k++] = *tail;
    const char* port = "18081/big"; while (*port) url[k++] = *port++;
    url[k] = 0;
    int rc = http_get(h, url);
    int ok = rc == 0 && http_status_code(h) == 200 && http_response_length(h) >= 300000;
    http_destroy(h);
    return ok ? 0 : -1;
}

/* ---- IPv6: the same pattern transfers straight over tcp_socket_* with an IPv6 peer */
static tcp_socket_t* connect6(void) {
    tcp_socket_t* t = tcp_socket_create();
    if (!t) return NULL;
    ip6_addr_t host; ip6_parse("fec0::2", &host);               /* QEMU user-mode networking: the host */
    if (tcp_socket_connect6(t, &host, COS_NET_TEST_PORT + 2) != 0) { tcp_socket_destroy(t); return NULL; }
    return t;
}
static int t6_send_all(tcp_socket_t* t, const void* buf, size_t n, uint64_t deadline) {
    size_t sent = 0;
    while (sent < n && get_timer_ticks() < deadline) {
        int r = tcp_socket_send(t, (void*)((const uint8_t*)buf + sent), n - sent);
        if (r < 0) return -1;
        sent += (size_t)r;
        if (r == 0) { net_poll(); thread_yield(); }
    }
    return sent == n ? 0 : -1;
}
static int test_down6(uint64_t n, uint64_t budget_ms) {
    tcp_socket_t* t = connect6();
    if (!t) return -1;
    char line[40]; int k = line_to(line, "DOWN", n);
    uint64_t deadline = get_timer_ticks() + budget_ms;
    int ok = t6_send_all(t, line, (size_t)k, deadline) == 0;
    uint64_t got = 0; int bad = 0;
    static uint8_t buf[8192];
    while (ok && got < n && get_timer_ticks() < deadline) {
        int r = tcp_socket_recv(t, buf, sizeof buf);
        if (r < 0) break;
        if (r == 0) { thread_yield(); continue; }
        for (int i = 0; i < r; ++i) if (buf[i] != pat(got + (uint64_t)i)) { bad++; break; }
        got += (uint64_t)r;
    }
    tcp_socket_close(t); tcp_socket_destroy(t);
    return (ok && got == n && !bad) ? 0 : -1;
}
static int test_up6(uint64_t n, uint64_t budget_ms) {
    tcp_socket_t* t = connect6();
    if (!t) return -1;
    char line[40]; int k = line_to(line, "UP", n);
    uint64_t deadline = get_timer_ticks() + budget_ms;
    int ok = t6_send_all(t, line, (size_t)k, deadline) == 0;
    static uint8_t chunk[4096];
    uint64_t done = 0;
    while (ok && done < n) {
        size_t c = n - done < sizeof chunk ? (size_t)(n - done) : sizeof chunk;
        for (size_t i = 0; i < c; ++i) chunk[i] = pat(done + i);
        ok = t6_send_all(t, chunk, c, deadline) == 0;
        done += c;
    }
    char reply[8] = {0}; size_t rl = 0;
    while (ok && rl < 3 && get_timer_ticks() < deadline) {
        int r = tcp_socket_recv(t, reply + rl, 3 - rl);
        if (r < 0) break;
        if (r == 0) { thread_yield(); continue; }
        rl += (size_t)r;
    }
    tcp_socket_close(t); tcp_socket_destroy(t);
    return (ok && rl >= 2 && reply[0] == 'O' && reply[1] == 'K') ? 0 : -1;
}

static void report(const char* name, int rc, uint64_t t0, int* pass, int* fail) {
    serial_puts("[NETTEST] "); serial_puts(name); serial_puts(rc == 0 ? " PASS" : " FAIL");
    serial_puts(" ticks="); serial_putdec(get_timer_ticks() - t0); serial_puts("\n");
    if (rc == 0) (*pass)++; else (*fail)++;
}

static void run_ipv6_tests(int* pass, int* fail, int with_tcp) {
    uint64_t t;
    {   /* IPv6: router advertisement -> SLAAC address, neighbour discovery, ping6, TCP */
        uint64_t until = get_timer_ticks() + 12000;
        while (!ip6_ready() && get_timer_ticks() < until) { net_poll(); thread_yield(); }
        char a[48]; ip6_addr_t g = ip6_global(), r6 = ip6_router();
        ip6_format(&g, a, sizeof a); serial_puts("[NETTEST] ipv6 global="); serial_puts(a);
        ip6_format(&r6, a, sizeof a); serial_puts(" router="); serial_puts(a); serial_puts("\n");
        t = get_timer_ticks(); report("ipv6 SLAAC address and router from RA", ip6_ready() ? 0 : -1, t, pass, fail);
        uint32_t rtt = 0;
        t = get_timer_ticks(); report("ping6 the router (link-local, neighbour discovery)", icmp6_ping(&r6, 1, 3000, &rtt), t, pass, fail);
        ip6_addr_t host; ip6_parse("fec0::2", &host);
        t = get_timer_ticks(); report("ping6 the host (global address via the router)", icmp6_ping(&host, 2, 3000, &rtt), t, pass, fail);
        if (with_tcp) {
            t = get_timer_ticks(); report("ipv6 tcp download 512KB", test_down6(512u << 10, 60000), t, pass, fail);
            t = get_timer_ticks(); report("ipv6 tcp upload 256KB", test_up6(256u << 10, 60000), t, pass, fail);
            net_set_loss_pct(5);
            t = get_timer_ticks(); report("ipv6 tcp download 256KB loss=5%", test_down6(256u << 10, 120000), t, pass, fail);
            t = get_timer_ticks(); report("ipv6 tcp upload 128KB loss=5%", test_up6(128u << 10, 120000), t, pass, fail);
            net_set_loss_pct(0);
            /* DNS over IPv6 (resolver from the RA), then the HTTP client by name and by bracketed literal */
            ip6_addr_t named; int dr = dns_resolve6("ipv6test.cos", &named);
            t = get_timer_ticks(); report("dns AAAA over IPv6 transport", dr == 0 && ip6_same(&named, &host) ? 0 : -1, t, pass, fail);
            {   /* UDP socket over IPv6: echo service on port 7 */
                socket_t* us = socket_create(AF_INET6, SOCK_DGRAM, 0);
                int ur = -1;
                if (us) {
                    sock_addr_t ua; memset(&ua, 0, sizeof ua); ua.family = AF_INET6; ua.port = 7; ua.addr6 = host;
                    socket_connect(us, &ua);
                    const char msg[] = "hello over udp6";
                    uint64_t until2 = get_timer_ticks() + 3000; char rb[64]; int n = 0;
                    for (int tries = 0; tries < 3 && n <= 0 && get_timer_ticks() < until2; ++tries) {
                        socket_send(us, (void*)msg, sizeof msg, 0);
                        uint64_t w = get_timer_ticks() + 800;
                        while (get_timer_ticks() < w && (n = socket_recv(us, rb, sizeof rb, 0)) <= 0) { net_poll(); thread_yield(); }
                    }
                    ur = (n == (int)sizeof msg && memcmp(rb, msg, sizeof msg) == 0) ? 0 : -1;
                    socket_close(us);
                }
                t = get_timer_ticks(); report("udp socket over IPv6 (echo)", ur, t, pass, fail);
            }
            http_client_t* h = http_create();
            int hr = h ? http_get(h, "http://ipv6test.cos:18082/page") : -1;
            t = get_timer_ticks(); report("http_get by name over IPv6", hr == 0 && http_status_code(h) == 200 && http_response_length(h) >= 20000 ? 0 : -1, t, pass, fail);
            if (h) http_destroy(h);
            h = http_create();
            hr = h ? http_get(h, "http://[fec0::2]:18082/page") : -1;
            t = get_timer_ticks(); report("http_get by bracketed IPv6 literal", hr == 0 && http_status_code(h) == 200 && http_response_length(h) >= 20000 ? 0 : -1, t, pass, fail);
            if (h) http_destroy(h);
        }
        ip6_stats_t st6; ip6_get_stats(&st6);
        serial_puts("[NETTEST] ipv6 rx="); serial_putdec(st6.rx); serial_puts(" tx="); serial_putdec(st6.tx);
        serial_puts(" ra="); serial_putdec(st6.ra_rcvd); serial_puts(" na="); serial_putdec(st6.na_rcvd); serial_puts(" bad_csum="); serial_putdec(st6.bad_checksum); serial_puts("\n");
    }
}


/* ---- HTTP/2 (VALIDATION_NET=3, against tools/h2_test_server.py; see tools/run_h2_test.sh) ---- */
#define H2_PAT   1      /* body must be the pattern */
#define H2_H2    2      /* must have gone over h2 */
#define H2_H1    4      /* must have fallen back to HTTP/1.1 */
typedef struct { const char* name; const char* path; int status; int64_t len; int flags; int secs; } h2case_t;

static void h2_url(char* url, const char* path) {
    net_iface_t* f = net_get_default_iface();
    int k = 0; const char* pre = "https://";
    while (*pre) url[k++] = *pre++;
    for (int i = 0; i < 4; ++i) {
        unsigned v = f->gateway.addr[i]; char t[4]; int n = 0;
        do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) url[k++] = t[--n];
        if (i < 3) url[k++] = '.';
    }
    const char* port = ":18443"; while (*port) url[k++] = *port++;
    while (*path) url[k++] = *path++;
    url[k] = 0;
}

static int h2_run(const h2case_t* c) {
    http_client_t* h = http_create();
    if (!h) return -1;
    char url[96]; h2_url(url, c->path);
    uint64_t t0 = get_timer_ticks();
    int rc = http_get(h, url);
    uint64_t ms = get_timer_ticks() - t0;
    int ok = 1;
    if (c->status == 0) {
        /* only has to come back (cleanly) in time */
    } else {
        ok = rc == 0 && http_status_code(h) == c->status;
        uint64_t n = ok ? http_response_length(h) : 0;
        if (ok && c->len >= 0 && n != (uint64_t)c->len) ok = 0;
        if (ok && (c->flags & H2_PAT)) {
            const uint8_t* b = (const uint8_t*)http_response_body(h);
            for (uint64_t i = 0; ok && i < n; ++i) if (b[i] != pat(i)) { ok = 0; serial_puts("[NETTEST]   body differs at "); serial_putdec(i); serial_puts("\n"); }
        }
        if (ok && (c->flags & H2_H2) && !h->used_http2) ok = 0;
        if (ok && (c->flags & H2_H1) && h->used_http2) ok = 0;
    }
    if (ms > (uint64_t)c->secs * 1000u) ok = 0;
    if (!ok) {
        serial_puts("[NETTEST]   rc="); serial_putdec((uint64_t)(int64_t)rc); serial_puts(" status="); serial_putdec((uint64_t)http_status_code(h));
        serial_puts(" len="); serial_putdec(http_response_length(h)); serial_puts(" h2="); serial_putdec((uint64_t)h->used_http2);
        serial_puts(" ms="); serial_putdec(ms); serial_puts("\n");
    }
    http_destroy(h);
    return ok ? 0 : -1;
}

static void run_h2_tests(int* pass, int* fail) {
    static const h2case_t cases[] = {
        { "h2 small GET",                       "/small",        200, 9,        H2_H2,           20 },
        { "h2 3MB body (flow control)",          "/big/3000000",  200, 3000000,  H2_H2 | H2_PAT,  90 },
        { "h2 8MB body",                         "/big/8388608",  200, 8388608,  H2_H2 | H2_PAT, 180 },
        { "h2 body trickling for 12 s",          "/slowbody",     200, 12000,    H2_H2 | H2_PAT,  60 },
        { "h2 103 Early Hints then 200",         "/early",        200, 10,       H2_H2,           20 },
        { "h2 redirect followed",                "/redirect",     200, 9,        0,               30 },
        { "h2 gzip content-encoding",            "/gzip",         200, 200000,   H2_H2 | H2_PAT,  30 },
        { "h2 many Set-Cookie + 6 KB header",    "/multi",        200, 6,        H2_H2,           20 },
        { "h2 trailers ignored",                 "/trailers",     200, 5000,     H2_H2 | H2_PAT,  20 },
        { "h2 204 no content",                   "/empty",        204, 0,        H2_H2,           20 },
        { "h2 404 with body",                    "/missing",      404, 10,       H2_H2,           20 },
        { "h2 server waits for our SETTINGS ACK","/settings",     200, 3,        H2_H2,           20 },
        { "h2 RST_STREAM -> HTTP/1.1 fallback",  "/rst",          200, 12,       H2_H1,           40 },
        { "h2 GOAWAY mid-body -> fallback",      "/goaway",       200, 12,       H2_H1,           40 },
        { "h2 stalled response gives up",        "/stall",        200, 12,       H2_H1,           60 },
        { "h2 over-size (12MB): prefix kept",   "/huge",         200, -1,       H2_H2 | H2_PAT, 240 },
        { "h2 small GET after all that",         "/small",        200, 9,        H2_H2,           20 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint64_t t = get_timer_ticks();
        report(cases[i].name, h2_run(&cases[i]), t, pass, fail);
    }
    uint64_t t = get_timer_ticks();
    int ok = 1;
    for (int i = 0; i < 25 && ok; ++i) {
        h2case_t c = { "", "/small", 200, 9, H2_H2, 20 };
        if (h2_run(&c) != 0) ok = 0;
    }
    report("h2 25 sequential connections", ok ? 0 : -1, t, pass, fail);
}

#define H2ONLY (COS_VALIDATION_NET == 3)    /* VALIDATION_NET=3: only the HTTP/2 suite */

#define V6ONLY (COS_VALIDATION_NET == 2)    /* VALIDATION_NET=2: only IPv6, against tools/ip6_peer.py */

static void net_selftest_thread(void* arg) {
    (void)arg;
    if (V6ONLY) {
        int pass6 = 0, fail6 = 0;
        run_ipv6_tests(&pass6, &fail6, 1);
        serial_puts("[NETTEST] DONE passed="); serial_putdec((uint64_t)pass6);
        serial_puts(" failed="); serial_putdec((uint64_t)fail6); serial_puts("\n");
        return;
    }
    serial_puts("[NETTEST] waiting for DHCP\n");
    for (int i = 0; i < 600; ++i) {
        net_iface_t* f = net_get_default_iface();
        if (f && dhcp_poll() == 0) break;      /* lease bound (the NIC driver pre-seeds a static 10.0.2.15) */
        net_poll(); thread_yield();
        uint64_t until = get_timer_ticks() + 20; while (get_timer_ticks() < until) thread_yield();
    }
    int pass = 0, fail = 0;
    if (H2ONLY) {
        run_h2_tests(&pass, &fail);
        serial_puts("[NETTEST] DONE passed="); serial_putdec((uint64_t)pass);
        serial_puts(" failed="); serial_putdec((uint64_t)fail); serial_puts("\n");
        return;
    }
    ip_addr_t gw = net_get_default_iface()->gateway;
    for (uint16_t i = 1; i <= 3; ++i) {
        uint32_t rtt = 0; uint64_t t0 = get_timer_ticks();
        int r = icmp_ping(&gw, i, 2000, &rtt);
        serial_puts("[NETTEST] ping seq="); serial_putdec(i); serial_puts(" rtt="); serial_putdec(rtt); serial_puts("ms\n");
        report("ping", r, t0, &pass, &fail);
    }
    uint64_t t;
    run_ipv6_tests(&pass, &fail, 0);
    t = get_timer_ticks(); report("tcp download 2MB clean", test_down(2u << 20, 60000), t, &pass, &fail);
    t = get_timer_ticks(); report("tcp upload 1MB clean", test_up(1u << 20, 60000), t, &pass, &fail);
    {   /* packet filter: block TCP to the test server's port -> connect must fail; block inbound ICMP -> ping must fail */
        fw_rule_t r; memset(&r, 0, sizeof r);
        r.deny = true; r.dir = FW_OUT; r.proto = FW_TCP; r.port_lo = r.port_hi = COS_NET_TEST_PORT;
        fw_add_rule(&r);
        t = get_timer_ticks(); socket_t* s = connect_server();
        report("firewall blocks outbound TCP to the test port", s ? -1 : 0, t, &pass, &fail);
        if (s) socket_close(s);
        fw_clear();
        t = get_timer_ticks(); s = connect_server();
        report("firewall rule removed: connection works again", s ? 0 : -1, t, &pass, &fail);
        if (s) socket_close(s);
        memset(&r, 0, sizeof r); r.deny = true; r.dir = FW_IN; r.proto = FW_ICMP;
        fw_add_rule(&r);
        uint32_t rtt = 0; t = get_timer_ticks();
        report("firewall blocks inbound ICMP (ping fails)", icmp_ping(&gw, 9, 800, &rtt) == 0 ? -1 : 0, t, &pass, &fail);
        fw_clear();
        uint32_t al, de; fw_get_counters(&al, &de);
        serial_puts("[NETTEST] firewall allowed="); serial_putdec(al); serial_puts(" denied="); serial_putdec(de); serial_puts("\n");
    }
    t = get_timer_ticks(); report("http get 300KB (http client)", test_http(), t, &pass, &fail);
    net_set_loss_pct(5);
    t = get_timer_ticks(); report("tcp download 1MB loss=5%", test_down(1u << 20, 120000), t, &pass, &fail);
    t = get_timer_ticks(); report("tcp upload 512KB loss=5%", test_up(512u << 10, 120000), t, &pass, &fail);
    net_set_loss_pct(15);
    t = get_timer_ticks(); report("tcp download 256KB loss=15%", test_down(256u << 10, 120000), t, &pass, &fail);
    t = get_timer_ticks(); report("tcp upload 256KB loss=15%", test_up(256u << 10, 120000), t, &pass, &fail);
    net_set_loss_pct(0);
    tcp_stats_t st; tcp_get_stats(&st);
    serial_puts("[NETTEST] tcp retransmits="); serial_putdec(st.retransmits);
    serial_puts(" fast="); serial_putdec(st.fast_retransmits);
    serial_puts(" timeouts="); serial_putdec(st.timeouts);
    serial_puts(" out_of_order="); serial_putdec(st.out_of_order);
    serial_puts(" dup_segments="); serial_putdec(st.dup_segments);
    serial_puts(" rst_sent="); serial_putdec(st.resets_sent); serial_puts("\n");
    icmp_stats_t is; icmp_get_stats(&is);
    serial_puts("[NETTEST] icmp echo_sent="); serial_putdec(is.echo_sent); serial_puts(" echo_replies="); serial_putdec(is.echo_replies);
    serial_puts(" requests_rcvd="); serial_putdec(is.requests_rcvd); serial_puts(" errors="); serial_putdec(is.errors_rcvd); serial_puts("\n");
    serial_puts("[NETTEST] DONE passed="); serial_putdec((uint64_t)pass);
    serial_puts(" failed="); serial_putdec((uint64_t)fail); serial_puts("\n");
}

void net_selftest_start(void) {
    thread_create_kernel_stack_size("net_selftest", (void*)net_selftest_thread, NULL, 256 * 1024);
}
#else
void net_selftest_start(void) {}
#endif
