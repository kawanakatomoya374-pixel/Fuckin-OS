/**
 * icmp.c - ICMPv4 echo (ping) and error reporting.
 */
#include "icmp.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "task.h"

extern void net_poll(void);

typedef struct __attribute__((packed)) {
    uint8_t type, code;
    uint16_t checksum;
    uint16_t id, seq;
} icmp_echo_hdr_t;

static icmp_stats_t g_icmp;
/* One ping may be outstanding at a time (the id/seq pair identifies it). */
static volatile uint32_t ping_busy;
static volatile uint16_t ping_id, ping_seq;
static volatile int ping_state;            /* 0 waiting, 1 reply, 2 unreachable */
static volatile uint64_t ping_sent_at;
static volatile uint64_t ping_rtt;
static uint16_t next_ping_id = 0x4352;     /* "CR" */

static uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

void icmp_init(void) {
    memset(&g_icmp, 0, sizeof g_icmp);
    __atomic_store_n(&ping_busy, 0u, __ATOMIC_RELEASE);
}

void icmp_get_stats(icmp_stats_t* out) { if (out) *out = g_icmp; }

static ip_addr_t addr_of(uint32_t wire) {
    ip_addr_t a = {.addr = {(uint8_t)wire, (uint8_t)(wire >> 8), (uint8_t)(wire >> 16), (uint8_t)(wire >> 24)}};
    return a;
}

void icmp_recv(ip_hdr_t* ip, void* data, size_t len) {
    if (!ip || !data || len < sizeof(icmp_echo_hdr_t)) return;
    if (ip_checksum(data, len) != 0) return;                      /* corrupted */
    icmp_echo_hdr_t* h = (icmp_echo_hdr_t*)data;

    if (h->type == ICMP_ECHO_REQUEST && h->code == 0) {
        g_icmp.requests_rcvd++;
        if (len > 1400) return;
        uint8_t reply[1400];
        memcpy(reply, data, len);                                  /* echo the payload back unchanged */
        icmp_echo_hdr_t* r = (icmp_echo_hdr_t*)reply;
        r->type = ICMP_ECHO_REPLY;
        r->checksum = 0;
        r->checksum = ip_checksum(reply, len);
        ip_addr_t to = addr_of(ip->src_addr);
        if (ip_send(&to, IP_PROTO_ICMP, reply, len) == 0) g_icmp.replies_sent++;
        return;
    }
    if (h->type == ICMP_ECHO_REPLY) {
        if (__atomic_load_n(&ping_busy, __ATOMIC_ACQUIRE) && h->id == bswap16(ping_id) && h->seq == bswap16(ping_seq) &&
            ping_state == 0) {
            g_icmp.echo_replies++;
            ping_rtt = get_timer_ticks() - ping_sent_at;
            ping_state = 1;
        }
        return;
    }
    if ((h->type == ICMP_DEST_UNREACH || h->type == ICMP_TIME_EXCEEDED) && len >= 8 + IP_HDR_LEN + 8) {
        g_icmp.errors_rcvd++;
        /* The error quotes our original IP header + the first 8 bytes of the echo request. */
        ip_hdr_t* inner = (ip_hdr_t*)((uint8_t*)data + 8);
        if (inner->protocol == IP_PROTO_ICMP && __atomic_load_n(&ping_busy, __ATOMIC_ACQUIRE)) {
            icmp_echo_hdr_t* q = (icmp_echo_hdr_t*)((uint8_t*)inner + (inner->version_ihl & 0x0F) * 4u);
            if ((uint8_t*)q + 8 <= (uint8_t*)data + len && q->type == ICMP_ECHO_REQUEST && q->id == bswap16(ping_id) &&
                q->seq == bswap16(ping_seq) && ping_state == 0) ping_state = 2;
        }
    }
}

int icmp_ping(const ip_addr_t* dst, uint16_t seq, uint32_t timeout_ms, uint32_t* rtt_ms) {
    if (!dst) return -1;
    if (!net_get_default_iface()) return -3;
    if (__atomic_exchange_n(&ping_busy, 1u, __ATOMIC_ACQUIRE) != 0u) return -1;

    uint8_t pkt[sizeof(icmp_echo_hdr_t) + 32];
    icmp_echo_hdr_t* h = (icmp_echo_hdr_t*)pkt;
    h->type = ICMP_ECHO_REQUEST;
    h->code = 0;
    h->checksum = 0;
    ping_id = next_ping_id++;
    ping_seq = seq;
    h->id = bswap16(ping_id);
    h->seq = bswap16(seq);
    for (int i = 0; i < 32; ++i) pkt[sizeof(icmp_echo_hdr_t) + i] = (uint8_t)('a' + (i % 23));
    h->checksum = ip_checksum(pkt, sizeof pkt);
    ping_state = 0;

    int result = -1;
    uint64_t start = get_timer_ticks(), deadline = start + timeout_ms;
    bool sent = false;
    uint64_t last_try = 0;
    while (get_timer_ticks() < deadline) {
        /* ip_send() drops the first packet while the next hop's MAC is being resolved by ARP: repeat until it leaves. */
        if (!sent && get_timer_ticks() - last_try >= 20) {
            last_try = get_timer_ticks();
            ping_sent_at = last_try;
            if (ip_send((ip_addr_t*)dst, IP_PROTO_ICMP, pkt, sizeof pkt) == 0) { sent = true; g_icmp.echo_sent++; }
        }
        net_poll();
        if (ping_state == 1) { result = 0; if (rtt_ms) *rtt_ms = (uint32_t)ping_rtt; break; }
        if (ping_state == 2) { result = -2; break; }
        thread_yield();
    }
    __atomic_store_n(&ping_busy, 0u, __ATOMIC_RELEASE);
    return result;
}
