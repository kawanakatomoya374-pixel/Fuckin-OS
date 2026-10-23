/**
 * ip6.c - IPv6 (RFC 8200), neighbour discovery (RFC 4861), SLAAC (RFC 4862) and ICMPv6 echo (RFC 4443).
 */
#include "sync.h"
#include "ip6.h"
#include "tcp.h"
#include "udp.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "task.h"

extern void eth_send(net_iface_t* iface, eth_addr_t* dst, uint16_t type, void* data, size_t len);
extern void net_poll(void);

#define ND_NS 135
#define ND_NA 136
#define ND_RS 133
#define ND_RA 134
#define ICMP6_ECHO_REQ 128
#define ICMP6_ECHO_REP 129
#define NH_ICMP6 58

typedef struct { ip6_addr_t ip; uint8_t mac[6]; bool valid; uint64_t stamp; } nbr_t;
#define NBR_MAX 16

static nbr_t g_nbr[NBR_MAX];
static ip6_addr_t g_ll, g_global, g_router;
static bool g_have_ll, g_have_global, g_have_router;
static uint8_t g_prefix[8];
static uint64_t g_last_rs;
static bool g_tentative;                     /* link-local address under duplicate address detection */
static uint64_t g_dad_start;
static bool g_dad_failed;
static int g_rs_count;
static ip6_stats_t g_st;
static volatile uint32_t g_lock;
static uint8_t g_mac[6];

static void lock(void) { cos_spin_lock(&g_lock); }
static void unlock(void) { cos_spin_unlock(&g_lock); }

static uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

bool ip6_same(const ip6_addr_t* a, const ip6_addr_t* b) { return memcmp(a->b, b->b, 16) == 0; }
bool ip6_ready(void) { return g_have_global && g_have_router; }
bool ip6_link_local_ready(void) { return g_have_ll && !g_tentative; }
ip6_addr_t ip6_link_local(void) { return g_ll; }
ip6_addr_t ip6_global(void) { return g_global; }
ip6_addr_t ip6_router(void) { return g_router; }
void ip6_get_stats(ip6_stats_t* o) { if (o) *o = g_st; }

static bool is_multicast(const ip6_addr_t* a) { return a->b[0] == 0xFF; }
static bool is_link_local(const ip6_addr_t* a) { return a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0x80; }

static ip6_addr_t solicited_node(const ip6_addr_t* t) {
    ip6_addr_t a; memset(&a, 0, sizeof a);
    a.b[0] = 0xFF; a.b[1] = 0x02; a.b[11] = 0x01; a.b[12] = 0xFF; a.b[13] = t->b[13]; a.b[14] = t->b[14]; a.b[15] = t->b[15];
    return a;
}
static void mcast_mac(const ip6_addr_t* a, uint8_t mac[6]) { mac[0] = 0x33; mac[1] = 0x33; memcpy(mac + 2, a->b + 12, 4); }

/* ---- checksum */
static uint32_t sum_bytes(const uint8_t* p, size_t n, uint32_t acc) {
    for (size_t i = 0; i + 1 < n; i += 2) acc += ((uint32_t)p[i] << 8) | p[i + 1];
    if (n & 1) acc += (uint32_t)p[n - 1] << 8;
    return acc;
}
uint16_t ip6_checksum(const ip6_addr_t* src, const ip6_addr_t* dst, uint8_t nh, const void* seg, size_t len) {
    uint32_t s = 0;
    s = sum_bytes(src->b, 16, s); s = sum_bytes(dst->b, 16, s);
    s += (uint32_t)(len >> 16) + (uint32_t)(len & 0xFFFF);
    s += nh;
    s = sum_bytes((const uint8_t*)seg, len, s);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)(~s & 0xFFFF);
}

/* ---- neighbour cache */
static void nbr_put(const ip6_addr_t* ip, const uint8_t mac[6]) {
    lock();
    int slot = -1;
    for (int i = 0; i < NBR_MAX; ++i) if (g_nbr[i].valid && ip6_same(&g_nbr[i].ip, ip)) { slot = i; break; }
    if (slot < 0) {
        uint64_t oldest = ~0ull;
        for (int i = 0; i < NBR_MAX; ++i) {
            if (!g_nbr[i].valid) { slot = i; break; }
            if (g_nbr[i].stamp < oldest) { oldest = g_nbr[i].stamp; slot = i; }
        }
    }
    g_nbr[slot].ip = *ip; memcpy(g_nbr[slot].mac, mac, 6); g_nbr[slot].valid = true; g_nbr[slot].stamp = get_timer_ticks();
    unlock();
}
static bool nbr_get(const ip6_addr_t* ip, uint8_t mac[6]) {
    bool ok = false;
    lock();
    for (int i = 0; i < NBR_MAX; ++i) if (g_nbr[i].valid && ip6_same(&g_nbr[i].ip, ip)) { memcpy(mac, g_nbr[i].mac, 6); ok = true; break; }
    unlock();
    return ok;
}

/* ---- low-level send: IPv6 header + payload in one Ethernet frame */
static int send_frame(const ip6_addr_t* src, const ip6_addr_t* dst, uint8_t nh, uint8_t hop, const void* payload, size_t len, const uint8_t mac[6]) {
    net_iface_t* iface = net_get_default_iface();
    if (!iface || len > 1500 - 40) return -1;
    uint8_t* pkt = (uint8_t*)kmalloc(40 + len);
    if (!pkt) return -1;
    pkt[0] = 0x60; pkt[1] = pkt[2] = pkt[3] = 0;
    pkt[4] = (uint8_t)(len >> 8); pkt[5] = (uint8_t)len; pkt[6] = nh; pkt[7] = hop;
    memcpy(pkt + 8, src->b, 16); memcpy(pkt + 24, dst->b, 16);
    if (len) memcpy(pkt + 40, payload, len);
    eth_addr_t m; memcpy(m.addr, mac, 6);
    eth_send(iface, &m, 0x86DD, pkt, 40 + len);
    kfree(pkt);
    g_st.tx++;
    return 0;
}

static int send_icmp6(const ip6_addr_t* src, const ip6_addr_t* dst, uint8_t type, uint8_t code, const void* body, size_t blen, uint8_t hop, const uint8_t mac[6]) {
    uint8_t msg[1280];
    if (blen + 4 > sizeof msg) return -1;
    msg[0] = type; msg[1] = code; msg[2] = msg[3] = 0;
    memcpy(msg + 4, body, blen);
    uint16_t c = ip6_checksum(src, dst, NH_ICMP6, msg, blen + 4);
    msg[2] = (uint8_t)(c >> 8); msg[3] = (uint8_t)c;
    return send_frame(src, dst, NH_ICMP6, hop, msg, blen + 4, mac);
}

static void send_ns(const ip6_addr_t* target) {
    ip6_addr_t dst = solicited_node(target);
    ip6_addr_t src = is_link_local(target) || !g_have_global ? g_ll : g_global;
    uint8_t body[4 + 16 + 8];
    memset(body, 0, 4); memcpy(body + 4, target->b, 16);
    body[20] = 1; body[21] = 1; memcpy(body + 22, g_mac, 6);            /* source link-layer address option */
    uint8_t mac[6]; mcast_mac(&dst, mac);
    send_icmp6(&src, &dst, ND_NS, 0, body, sizeof body, 255, mac);
    g_st.ns_sent++;
}

static void send_na(const ip6_addr_t* to, const uint8_t to_mac[6], const ip6_addr_t* target, bool solicited) {
    uint8_t body[4 + 16 + 8];
    memset(body, 0, 4); body[0] = (uint8_t)((solicited ? 0x40 : 0) | 0x20);     /* S, O */
    memcpy(body + 4, target->b, 16);
    body[20] = 2; body[21] = 1; memcpy(body + 22, g_mac, 6);                    /* target link-layer address option */
    send_icmp6(target, to, ND_NA, 0, body, sizeof body, 255, to_mac);
}

static void send_rs(void) {
    ip6_addr_t dst; memset(&dst, 0, sizeof dst); dst.b[0] = 0xFF; dst.b[1] = 0x02; dst.b[15] = 0x02;      /* all routers */
    uint8_t body[4 + 8];
    memset(body, 0, 4); body[4] = 1; body[5] = 1; memcpy(body + 6, g_mac, 6);
    uint8_t mac[6]; mcast_mac(&dst, mac);
    send_icmp6(&g_ll, &dst, ND_RS, 0, body, sizeof body, 255, mac);
    g_last_rs = get_timer_ticks(); g_rs_count++;
}

void ip6_init(void) {
    net_iface_t* iface = net_get_default_iface();
    memset(g_nbr, 0, sizeof g_nbr);
    g_have_ll = g_have_global = g_have_router = false;
    g_rs_count = 0;
    if (!iface) return;
    memcpy(g_mac, iface->mac.addr, 6);
    memset(&g_ll, 0, sizeof g_ll);
    g_ll.b[0] = 0xFE; g_ll.b[1] = 0x80;
    g_ll.b[8] = g_mac[0] ^ 0x02; g_ll.b[9] = g_mac[1]; g_ll.b[10] = g_mac[2]; g_ll.b[11] = 0xFF; g_ll.b[12] = 0xFE;
    g_ll.b[13] = g_mac[3]; g_ll.b[14] = g_mac[4]; g_ll.b[15] = g_mac[5];
    /* Duplicate address detection (RFC 4862): ask whether anyone already uses this address, from the unspecified address,
     * and stay silent for a second; the router solicitation goes out once the address is ours. */
    g_have_ll = true; g_tentative = true; g_dad_failed = false; g_dad_start = get_timer_ticks();
    {
        ip6_addr_t sn = solicited_node(&g_ll), unspec; memset(&unspec, 0, sizeof unspec);
        uint8_t body[4 + 16]; memset(body, 0, 4); memcpy(body + 4, g_ll.b, 16);
        uint8_t mac[6]; mcast_mac(&sn, mac);
        send_icmp6(&unspec, &sn, ND_NS, 0, body, sizeof body, 255, mac);
    }
    serial_puts("[IP6] link-local address tentative, duplicate address detection started\n");
}

void ip6_poll(void) {
    if (g_tentative && !g_dad_failed && get_timer_ticks() - g_dad_start >= 1000) {
        g_tentative = false;
        serial_puts("[IP6] address is unique, sending router solicitation\n");
        send_rs();
        return;
    }
    if (!g_have_ll || g_tentative || g_have_router || g_rs_count >= 6) return;
    if (get_timer_ticks() - g_last_rs > 2000) send_rs();
}

ip6_addr_t ip6_select_src(const ip6_addr_t* dst) {
    if (is_link_local(dst) || is_multicast(dst) || !g_have_global) return g_ll;
    return g_global;
}

int ip6_send(const ip6_addr_t* dst, uint8_t nh, const void* payload, size_t len) {
    if (!g_have_ll) return -1;
    uint8_t mac[6];
    if (is_multicast(dst)) mcast_mac(dst, mac);
    else {
        ip6_addr_t hop = *dst;
        bool on_link = is_link_local(dst) || (g_have_global && memcmp(dst->b, g_prefix, 8) == 0);
        if (!on_link) { if (!g_have_router) return -1; hop = g_router; }
        if (!nbr_get(&hop, mac)) { send_ns(&hop); return -1; }
    }
    ip6_addr_t src = ip6_select_src(dst);
    return send_frame(&src, dst, nh, 64, payload, len, mac);
}

/* ---- receive */
static bool is_ours(const ip6_addr_t* d) {
    if (is_multicast(d)) {
        if (d->b[1] == 0x02 && d->b[15] == 0x01 && d->b[2] == 0 && d->b[14] == 0) return true;      /* ff02::1 */
        ip6_addr_t sn = solicited_node(&g_ll);
        if (ip6_same(d, &sn)) return true;
        if (g_have_global) { sn = solicited_node(&g_global); if (ip6_same(d, &sn)) return true; }
        return false;
    }
    return (g_have_ll && ip6_same(d, &g_ll)) || (g_have_global && ip6_same(d, &g_global));
}

/* ping state */
static volatile uint32_t ping_busy; static volatile uint16_t ping_id, ping_seq; static volatile int ping_state;
static volatile uint64_t ping_sent, ping_rtt;

static void handle_icmp6(const ip6_addr_t* src, const ip6_addr_t* dst, const uint8_t* m, size_t len, const uint8_t src_mac[6], uint8_t hop) {
    if (len < 4) return;
    if (ip6_checksum(src, dst, NH_ICMP6, m, len) != 0) { g_st.bad_checksum++; return; }
    uint8_t type = m[0];
    if (type == ICMP6_ECHO_REQ) {
        if (len > 1200) return;
        uint8_t rep[1200];
        memcpy(rep, m, len);
        ip6_addr_t me = *dst;                          /* reply from the address that was pinged (not a multicast one) */
        if (is_multicast(&me)) me = ip6_select_src(src);
        uint8_t mac[6];
        if (!nbr_get(src, mac)) memcpy(mac, src_mac, 6);
        send_icmp6(&me, src, ICMP6_ECHO_REP, 0, rep + 4, len - 4, 64, mac);
    } else if (type == ICMP6_ECHO_REP) {
        if (len >= 8 && __atomic_load_n(&ping_busy, __ATOMIC_ACQUIRE) && bswap16(*(const uint16_t*)(m + 4)) == ping_id &&
            bswap16(*(const uint16_t*)(m + 6)) == ping_seq && ping_state == 0) {
            ping_rtt = get_timer_ticks() - ping_sent; ping_state = 1; g_st.echo_replies++;
        }
    } else if (type == ND_NS && len >= 24 && hop == 255) {
        ip6_addr_t target; memcpy(target.b, m + 8, 16);
        bool mine = (g_have_ll && ip6_same(&target, &g_ll)) || (g_have_global && ip6_same(&target, &g_global));
        /* learn the asker's MAC from its source link-layer option */
        uint8_t asker[6]; memcpy(asker, src_mac, 6);
        for (size_t o = 24; o + 8 <= len; o += (size_t)m[o + 1] * 8) {
            if (m[o + 1] == 0) break;
            if (m[o] == 1) { memcpy(asker, m + o + 2, 6); break; }
        }
        nbr_put(src, asker);
        if (mine) send_na(src, asker, &target, true);
    } else if (type == ND_NA && len >= 24) {
        ip6_addr_t target; memcpy(target.b, m + 8, 16);
        g_st.na_rcvd++;
        if (g_tentative && ip6_same(&target, &g_ll)) {              /* someone answers for OUR address: it is taken */
            g_dad_failed = true; g_have_ll = false; g_tentative = false;
            serial_puts("[IP6] duplicate address detected - IPv6 disabled on this interface\n");
            return;
        }
        const uint8_t* mac = src_mac;
        for (size_t o = 24; o + 8 <= len; o += (size_t)m[o + 1] * 8) {
            if (m[o + 1] == 0) break;
            if (m[o] == 2) { mac = m + o + 2; break; }
        }
        nbr_put(&target, mac);
    } else if (type == ND_RA && len >= 16 && is_link_local(src)) {
        g_st.ra_rcvd++;
        uint16_t lifetime = bswap16(*(const uint16_t*)(m + 6));
        const uint8_t* rmac = src_mac;
        for (size_t o = 16; o + 8 <= len && m[o + 1]; o += (size_t)m[o + 1] * 8) {
            if (m[o] == 1) rmac = m + o + 2;
            if (m[o] == 25 && m[o + 1] >= 3) {                                                  /* RDNSS: recursive DNS server */
                ip6_addr_t ds; memcpy(ds.b, m + o + 8, 16);
                extern void dns_set_server6(const ip6_addr_t* server);
                dns_set_server6(&ds);
            }
            if (m[o] == 3 && m[o + 1] >= 4 && m[o + 2] == 64 && (m[o + 3] & 0x40)) {            /* prefix info: /64, A flag */
                memcpy(g_prefix, m + o + 16, 8);
                memcpy(g_global.b, g_prefix, 8);
                memcpy(g_global.b + 8, g_ll.b + 8, 8);                                         /* EUI-64 interface id */
                g_have_global = true;
            }
        }
        if (lifetime) { g_router = *src; g_have_router = true; nbr_put(src, rmac); }
        if (g_have_global && g_have_router) {
            static bool announced;
            if (!announced) {
                announced = true;
                char a[48]; ip6_format(&g_global, a, sizeof a);
                serial_puts("[IP6] SLAAC address "); serial_puts(a); serial_puts("\n");
            }
        }
    }
}

void ip6_recv(const uint8_t* p, size_t len, const uint8_t src_mac[6]) {
    if (len < 40 || (p[0] >> 4) != 6) return;
    size_t plen = ((size_t)p[4] << 8) | p[5];
    if (plen + 40 > len) return;
    ip6_addr_t src, dst; memcpy(src.b, p + 8, 16); memcpy(dst.b, p + 24, 16);
    if (!g_have_ll || !is_ours(&dst)) return;
    g_st.rx++;
    uint8_t nh = p[6]; const uint8_t* q = p + 40; size_t qlen = plen;
    while ((nh == 0 || nh == 60) && qlen >= 8) {                  /* hop-by-hop / destination options: skip */
        size_t hl = ((size_t)q[1] + 1) * 8;
        if (hl > qlen) return;
        nh = q[0]; q += hl; qlen -= hl;
    }
    if (nh == NH_ICMP6) handle_icmp6(&src, &dst, q, qlen, src_mac, p[7]);
    else if (nh == 17) udp6_recv(&src, &dst, q, qlen);
    else if (nh == 6 && qlen >= sizeof(tcp_hdr_t)) {
        tcp_hdr_t* th = (tcp_hdr_t*)q;
        size_t hl = (size_t)((th->data_off >> 4) & 0xF) * 4;
        if (hl < sizeof(tcp_hdr_t) || hl > qlen) return;
        if (ip6_checksum(&src, &dst, 6, q, qlen) != 0) { g_st.bad_checksum++; return; }
        tcp_handle_packet6(&src, th, q + hl, qlen - hl);
    }
}

int icmp6_ping(const ip6_addr_t* dst, uint16_t seq, uint32_t timeout_ms, uint32_t* rtt_ms) {
    if (!g_have_ll) return -3;
    if (__atomic_exchange_n(&ping_busy, 1u, __ATOMIC_ACQUIRE) != 0u) return -1;
    static uint16_t next_id = 0x6001;
    ping_id = next_id++; ping_seq = seq; ping_state = 0;
    uint8_t body[4 + 32];
    body[0] = (uint8_t)(ping_id >> 8); body[1] = (uint8_t)ping_id; body[2] = (uint8_t)(seq >> 8); body[3] = (uint8_t)seq;
    for (int i = 0; i < 32; ++i) body[4 + i] = (uint8_t)('a' + i % 23);
    int result = -1; bool sent = false; uint64_t last = 0, dl = get_timer_ticks() + timeout_ms;
    while (get_timer_ticks() < dl) {
        if (!sent && get_timer_ticks() - last >= 30) {
            last = get_timer_ticks(); ping_sent = last;
            uint8_t mac[6]; ip6_addr_t hop = *dst;
            bool on_link = is_link_local(dst) || (g_have_global && memcmp(dst->b, g_prefix, 8) == 0);
            if (is_multicast(dst)) { mcast_mac(dst, mac); on_link = true; }
            else {
                if (!on_link) { if (!g_have_router) goto next; hop = g_router; }
                if (!nbr_get(&hop, mac)) { send_ns(&hop); goto next; }
            }
            ip6_addr_t src = ip6_select_src(dst);
            if (send_icmp6(&src, dst, ICMP6_ECHO_REQ, 0, body, sizeof body, 64, mac) == 0) sent = true;
        }
    next:
        net_poll();
        if (ping_state == 1) { result = 0; if (rtt_ms) *rtt_ms = (uint32_t)ping_rtt; break; }
        thread_yield();
    }
    __atomic_store_n(&ping_busy, 0u, __ATOMIC_RELEASE);
    return result;
}

/* ---- text */
void ip6_format(const ip6_addr_t* a, char* out, size_t cap) {
    static const char hx[] = "0123456789abcdef";
    size_t n = 0;
    int zs = -1, zl = 0;                                            /* longest run of zero groups, for "::" */
    for (int i = 0; i < 8; ) {
        int j = i; while (j < 8 && !a->b[2 * j] && !a->b[2 * j + 1]) ++j;
        if (j - i > zl && j - i >= 2) { zs = i; zl = j - i; }
        i = j > i ? j : i + 1;
    }
    for (int g = 0; g < 8 && n + 6 < cap; ++g) {
        if (g == zs) { out[n++] = ':'; if (g == 0) out[n++] = ':'; g += zl - 1; continue; }
        uint16_t v = (uint16_t)((a->b[2 * g] << 8) | a->b[2 * g + 1]);
        bool started = false;
        for (int s = 12; s >= 0; s -= 4) { int d = (v >> s) & 0xF; if (d || started || s == 0) { out[n++] = hx[d]; started = true; } }
        if (g != 7) out[n++] = ':';
    }
    out[n] = 0;
}

bool ip6_parse(const char* s, ip6_addr_t* out) {
    uint16_t g[8]; int ng = 0, gap = -1;
    memset(g, 0, sizeof g);
    const char* p = s;
    if (p[0] == ':' && p[1] == ':') { gap = 0; p += 2; }
    while (*p) {
        if (ng >= 8) return false;
        uint32_t v = 0; int d = 0;
        while (*p && *p != ':') {
            int c = *p;
            int x = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (x < 0 || ++d > 4) return false;
            v = v * 16 + (uint32_t)x; ++p;
        }
        if (!d) return false;
        g[ng++] = (uint16_t)v;
        if (*p == ':') { ++p; if (*p == ':') { if (gap >= 0) return false; gap = ng; ++p; } else if (!*p) return false; }
    }
    if (gap < 0 && ng != 8) return false;
    uint16_t full[8]; memset(full, 0, sizeof full);
    if (gap < 0) memcpy(full, g, sizeof full);
    else { for (int i = 0; i < gap; ++i) full[i] = g[i]; int tail = ng - gap; for (int i = 0; i < tail; ++i) full[8 - tail + i] = g[gap + i]; }
    for (int i = 0; i < 8; ++i) { out->b[2 * i] = (uint8_t)(full[i] >> 8); out->b[2 * i + 1] = (uint8_t)full[i]; }
    return true;
}
