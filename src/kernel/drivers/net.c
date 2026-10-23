#include <stdint.h>
uint64_t get_timer_ticks(void);

/**
 * net.c - Network Stack Glue / Socket Abstraction
 */

#ifndef COS_ENABLE_NETWORK
/* Keep this translation unit aligned with kernel.c.  The E1000 path uses
 * page-backed DMA rings and is enabled by default. */
#define COS_ENABLE_NETWORK 1
#endif

#include "sync.h"
#include "cos_kevent.h"
#include "net.h"
#include "arp.h"
#include "udp.h"
#include "tcp.h"
#include "dns.h"
#include "dhcp.h"
#include "icmp.h"
#include "net_fw.h"
#include "ip6.h"
#include "e1000.h"
#include "serial.h"
#include "string.h"
#include "memory.h"

static uint16_t htons(uint16_t n) { return (uint16_t)(((n & 0xFFu) << 8) | ((n >> 8) & 0xFFu)); }
static uint16_t ntohs(uint16_t n) { return htons(n); }

static net_iface_t* net_interfaces = NULL;
static net_iface_t* default_iface = NULL;

/* net_poll() is reachable from GUI service, synchronous DNS/TCP waits and
 * TLS handshakes.  It is therefore a dispatcher, not a re-entrant utility:
 * exactly one caller may drain E1000/DHCP/TCP at a time.  A caller that loses
 * ownership simply returns to its own yield loop; the periodic GUI/service
 * paths will request the next bounded pass.  This avoids a high-frequency GUI
 * poller keeping the dispatcher in an unbounded coalescing loop. */
static volatile uint32_t net_dispatch_busy = 0;
static volatile uint64_t net_dispatch_passes = 0;
static volatile uint64_t net_dispatch_contended = 0;

extern void e1000_poll(void);
void ip_loop_deliver(void);

void net_init(void) {
#if COS_ENABLE_NETWORK
    serial_puts("[NET] Network stack initializing...\n");
    net_interfaces = NULL;
    default_iface = NULL;
    __atomic_store_n(&net_dispatch_busy, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&net_dispatch_passes, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&net_dispatch_contended, 0u, __ATOMIC_RELEASE);

    arp_init();
    udp_init();
    tcp_init();
    icmp_init();
    fw_init();
    dhcp_init();

    if (e1000_init() == 0) {
        serial_puts("[NET] NIC initialized\n");
    } else {
        serial_puts("[NET] No supported NIC found\n");
    }

    ip6_init();
    dns_init();
    serial_puts("[NET] Network stack ready\n");
#else
    serial_puts("[NET] Network stack disabled for stability.\n");
    net_interfaces = NULL;
    default_iface = NULL;
#endif
}

void net_poll(void) {
#if COS_ENABLE_NETWORK
    /* Single ownership is sufficient: all callers invoke this from a bounded
     * wait/yield loop or a periodic service pass. Returning when another owner
     * is active prevents concurrent RX/TCP/DHCP parsing without allowing the
     * GUI's high-frequency polling to monopolize dispatch forever. */
    if (__atomic_exchange_n(&net_dispatch_busy, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __atomic_fetch_add(&net_dispatch_contended, 1u, __ATOMIC_RELAXED);
        return;
    }
    __atomic_fetch_add(&net_dispatch_passes, 1u, __ATOMIC_RELAXED);
    e1000_poll();
    ip_loop_deliver();
    dhcp_poll();
    ip6_poll();
    tcp_poll();
    __atomic_store_n(&net_dispatch_busy, 0u, __ATOMIC_RELEASE);
#else
    return;
#endif
}

void net_get_dispatch_stats(uint64_t* completed_passes, uint64_t* contended_calls) {
    if (completed_passes) {
        *completed_passes = __atomic_load_n(&net_dispatch_passes, __ATOMIC_RELAXED);
    }
    if (contended_calls) {
        *contended_calls = __atomic_load_n(&net_dispatch_contended, __ATOMIC_RELAXED);
    }
}

net_iface_t* net_get_default_iface(void) {
    return default_iface;
}

bool net_is_connected(void) {
    return default_iface != NULL;
}

net_iface_t* net_iface_register(const char* name, eth_addr_t* mac) {
    net_iface_t* iface = (net_iface_t*)kmalloc(sizeof(net_iface_t));
    if (!iface) return NULL;
    memset(iface, 0, sizeof(net_iface_t));
    strncpy(iface->name, name ? name : "eth0", sizeof(iface->name) - 1);
    iface->name[sizeof(iface->name) - 1] = '\0';
    if (mac) iface->mac = *mac;
    iface->next = net_interfaces;
    net_interfaces = iface;
    if (!default_iface) default_iface = iface;
    serial_puts("[NET] Interface registered: ");
    serial_puts(iface->name);
    serial_puts("\n");
    return iface;
}

socket_t* socket_create(int domain, int type, int protocol) {
    (void)protocol;
    socket_t* sock = (socket_t*)kmalloc(sizeof(socket_t));
    if (!sock) return NULL;
    memset(sock, 0, sizeof(socket_t));
    sock->type = type;
    sock->family = domain == AF_INET6 ? AF_INET6 : AF_INET;
    sock->state = SS_UNCONNECTED;
    if (type == SOCK_STREAM) {
        sock->private_data = tcp_socket_create();
        if (!sock->private_data) {
            kfree(sock);
            return NULL;
        }
    } else if (type == SOCK_DGRAM) {
        sock->private_data = udp_socket_create(0);
        if (!sock->private_data) {
            kfree(sock);
            return NULL;
        }
        ((udp_socket_t*)sock->private_data)->v6 = (domain == AF_INET6);
    }
    return sock;
}

int socket_bind(socket_t* sock, sock_addr_t* addr) {
    if (!sock || !addr) return -1;
    sock->local_addr = addr->addr;
    sock->local_port = addr->port;
    return 0;
}

int socket_listen(socket_t* sock, int backlog) {
    (void)backlog;
    if (!sock) return -1;
    sock->state = SS_UNCONNECTED;
    return 0;
}

socket_t* socket_accept(socket_t* sock, sock_addr_t* addr) {
    (void)sock; (void)addr; return NULL;
}

int socket_connect(socket_t* sock, sock_addr_t* addr) {
    if (!sock || !addr) return -1;
    sock->remote_addr = addr->addr;
    sock->remote_port = addr->port;
    if (sock->type == SOCK_STREAM && sock->private_data && addr->family == AF_INET6) {
        int ret = tcp_socket_connect6((tcp_socket_t*)sock->private_data, &addr->addr6, addr->port);
        if (ret == 0) sock->state = SS_CONNECTED;
        return ret;
    }
    if (sock->type == SOCK_STREAM && sock->private_data) {
        int ret = tcp_socket_connect((tcp_socket_t*)sock->private_data, &addr->addr, addr->port);
        if (ret == 0) sock->state = SS_CONNECTED;
        return ret;
    }
    if (sock->type == SOCK_DGRAM) {
        if (addr->family == AF_INET6 && sock->private_data) {
            udp_socket_t* u = (udp_socket_t*)sock->private_data;
            u->v6 = 1; u->remote6 = addr->addr6; u->remote_port = addr->port;
        }
        sock->state = SS_CONNECTED;
        return 0;
    }
    serial_puts("[NET] socket_connect rejected unsupported or uninitialised socket\n");
    return -1;
}

int socket_send(socket_t* sock, void* buf, size_t len, int flags) {
    (void)flags;
    if (!sock || !buf) return -1;
    if (sock->type == SOCK_STREAM && sock->private_data) {
        return tcp_socket_send((tcp_socket_t*)sock->private_data, buf, len);
    }
    if (sock->type == SOCK_DGRAM && sock->private_data) {
        udp_socket_t* udp = (udp_socket_t*)sock->private_data;
        if (udp->v6) return udp->remote_port ? udp6_send(&udp->remote6, udp->local_port, udp->remote_port, buf, len) : -1;
        if (sock->remote_port == 0) return -1;
        return udp_send(&sock->remote_addr, udp->local_port, sock->remote_port, buf, len);
    }
    return -1;
}

int socket_recv(socket_t* sock, void* buf, size_t len, int flags) {
    (void)flags;
    if (!sock || !buf) return -1;
    if (sock->type == SOCK_STREAM && sock->private_data) {
        return tcp_socket_recv((tcp_socket_t*)sock->private_data, buf, len);
    }
    if (sock->type == SOCK_DGRAM && sock->private_data) {
        udp_socket_t* udp = (udp_socket_t*)sock->private_data;
        if (udp->v6) { net_poll(); ip6_addr_t from6; return udp_socket_recv6(udp, buf, len, &from6); }
        ip_addr_t from;
        return udp_socket_recv(udp, buf, len, &from);
    }
    return 0;
}

int socket_close(socket_t* sock) {
    if (!sock) return -1;
    if (sock->type == SOCK_STREAM && sock->private_data) tcp_socket_destroy((tcp_socket_t*)sock->private_data);
    else if (sock->type == SOCK_DGRAM && sock->private_data) udp_socket_close((udp_socket_t*)sock->private_data);
    sock->state = SS_FREE;
    kfree(sock);
    return 0;
}

/* Unpredictable 32-bit values for initial sequence numbers, ephemeral ports, DNS ids and the IP id. xorshift64* whose
 * state is re-mixed with the TSC on every call, so the sequence cannot be reconstructed from a few observed values. */
uint32_t net_rand32(void) {
    static uint64_t state = 0x9E3779B97F4A7C15ull;
    uint64_t t = __builtin_ia32_rdtsc();
    uint64_t x = state ^ (t * 0xD6E8FEB86659FD93ull) ^ (t << 17);
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    state = x ? x : 0x9E3779B97F4A7C15ull;
    return (uint32_t)((x * 0x2545F4914F6CDD1Dull) >> 32);
}

uint16_t ip_checksum(void* data, size_t len) {
    uint64_t sum = 0;
    uint16_t* ptr = (uint16_t*)data;
    while (len > 1) { sum += *ptr++; len -= 2; }
    if (len == 1) sum += *(uint8_t*)ptr;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

#ifndef COS_VALIDATION_NET
#define COS_VALIDATION_NET 0
#endif
#if COS_VALIDATION_NET
/* Validation only: drop a configurable share of frames in both directions to exercise TCP loss recovery. */
static volatile uint32_t net_loss_pct = 0;
static volatile uint32_t net_loss_rng = 0x2545F491u;
void net_set_loss_pct(uint32_t pct) { net_loss_pct = pct; }
static int net_frame_lost(void) {
    if (!net_loss_pct) return 0;
    uint32_t x = net_loss_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; net_loss_rng = x;
    return (x % 100u) < net_loss_pct;
}
#else
#define net_frame_lost() 0
#endif

void eth_send(net_iface_t* iface, eth_addr_t* dst, uint16_t type, void* data, size_t len) {
    if (!iface || !dst || !iface->send) return;
    if (net_frame_lost()) return;
    size_t cap = ETH_HDR_LEN + len + COSYS_NET_SLACK;
    uint8_t* frame = (uint8_t*)kmalloc(cap);
    if (!frame) return;
    eth_hdr_t* hdr = (eth_hdr_t*)frame;
    hdr->dst = *dst;
    hdr->src = iface->mac;
    hdr->type = htons(type);
    if (len > 0 && data) memcpy(frame + ETH_HDR_LEN, data, len);
    size_t flen = ETH_HDR_LEN + len;
    if (!cos_kmod_net_rewrite(COSYS_NET_TX, frame, &flen, cap)) { kfree(frame); return; }          /* a kernel module (.c-osys) dropped it */
    if (!cos_kmod_net_pass(COSYS_NET_TX, frame, flen)) { kfree(frame); return; }
    iface->send(iface, frame, flen);
    kfree(frame);
}

static void eth_recv_stack(net_iface_t* iface, void* data, size_t len);
void eth_recv(net_iface_t* iface, void* data, size_t len) {
    if (!iface || !data || len < ETH_HDR_LEN) return;
    if (net_frame_lost()) return;
    uint8_t *copy = NULL;
    if (cos_kmod_net_rewrite_active()) {                    /* the driver's buffer has no room to grow: rewriters get a copy */
        size_t cap = len + COSYS_NET_SLACK;
        copy = (uint8_t *)kmalloc(cap);
        if (!copy) return;
        memcpy(copy, data, len);
        if (!cos_kmod_net_rewrite(COSYS_NET_RX, copy, &len, cap)) { kfree(copy); return; }
        data = copy;
    }
    if (!cos_kmod_net_pass(COSYS_NET_RX, data, len)) { if (copy) kfree(copy); return; }          /* a kernel module (.c-osys) dropped it */
    eth_recv_stack(iface, data, len);
    if (copy) kfree(copy);
}

static void eth_recv_stack(net_iface_t* iface, void* data, size_t len) {
    (void)iface;
    eth_hdr_t* hdr = (eth_hdr_t*)data;
    uint16_t type = ntohs(hdr->type);
    uint8_t* payload = (uint8_t*)data + ETH_HDR_LEN;
    size_t payload_len = len - ETH_HDR_LEN;
    if (type == 0x0800u) {
        if (payload_len < IP_HDR_LEN) return;
        ip_hdr_t* ip = (ip_hdr_t*)payload;
        size_t ihl = (size_t)(ip->version_ihl & 0x0Fu) * 4u;
        if (ihl < IP_HDR_LEN || payload_len < ihl) return;
        /* Ethernet pads short frames to 60 bytes.  Network layers must use
         * IPv4 total length, not the raw Ethernet payload length, otherwise
         * six padding bytes in a 40-byte TCP ACK look like application data
         * and create an endless ACK loop. */
        size_t ip_total_len = (size_t)ntohs(ip->tot_len);
        if (ip_total_len < ihl || ip_total_len > payload_len) return;
        if (ip_checksum(ip, ihl) != 0) return;                  /* header checksum folds to zero when valid */
        ip_recv(ip, payload + ihl, ip_total_len - ihl);
    } else if (type == 0x86DDu) {
        ip6_recv(payload, payload_len, hdr->src.addr);
    } else if (type == 0x0806u) {
        arp_handle_packet((arp_packet_t*)payload, payload_len);
    }
}

void net_handle_packet(net_iface_t* iface, void* data, size_t len) {
    if (!iface || !data) return;
    eth_recv(iface, data, len);
}

void net_arp_request(ip_addr_t* ip) { arp_send_request(ip); }
void net_arp_reply(arp_packet_t* req) { if (req) arp_send_reply(&req->spa, &req->sha); }


/* ---- packets waiting for ARP ----------------------------------------------------------------------------------------- */
#define IP_PENDING 6
#define IP_PENDING_MAX_AGE 3000u        /* ticks: a reply that takes longer than this is not worth waiting for */
typedef struct { uint8_t used; ip_addr_t hop; uint16_t len; uint64_t queued; uint8_t pkt[1500]; } ip_pending_t;
static ip_pending_t g_ip_pending[IP_PENDING];
static volatile uint32_t g_ip_pending_lock;
static void ip_pending_lock(void) { cos_spin_lock(&g_ip_pending_lock); }
static void ip_pending_unlock(void) { cos_spin_unlock(&g_ip_pending_lock); }

static void ip_pending_queue(ip_addr_t* hop, const void* pkt, size_t len) {
    if (len > sizeof(g_ip_pending[0].pkt)) return;
    uint64_t now = get_timer_ticks();
    ip_pending_lock();
    int slot = -1; uint64_t oldest = ~0ull;
    for (int i = 0; i < IP_PENDING; i++) {
        if (!g_ip_pending[i].used) { slot = i; break; }
        if (g_ip_pending[i].queued < oldest) { oldest = g_ip_pending[i].queued; slot = i; }
    }
    g_ip_pending[slot].used = 1; g_ip_pending[slot].hop = *hop; g_ip_pending[slot].len = (uint16_t)len; g_ip_pending[slot].queued = now;
    memcpy(g_ip_pending[slot].pkt, pkt, len);
    ip_pending_unlock();
}

/* Called by the ARP module when a MAC address becomes known. */
void ip_flush_pending(ip_addr_t* hop, eth_addr_t* mac) {
    net_iface_t* iface = net_get_default_iface();
    if (!iface) return;
    uint64_t now = get_timer_ticks();
    for (int i = 0; i < IP_PENDING; i++) {
        static uint8_t out[1500];
        uint16_t len = 0;
        ip_pending_lock();
        if (g_ip_pending[i].used && memcmp(g_ip_pending[i].hop.addr, hop->addr, 4) == 0) {
            if (now - g_ip_pending[i].queued <= IP_PENDING_MAX_AGE) { len = g_ip_pending[i].len; memcpy(out, g_ip_pending[i].pkt, len); }
            g_ip_pending[i].used = 0;
        }
        ip_pending_unlock();
        if (len) eth_send(iface, mac, 0x0800u, out, len);
    }
}

/* ---- loopback: a packet addressed to our own IP never reaches the NIC (the LAN would not send it back). It waits in this ring
 * and is delivered by net_poll(), never from inside ip_send(), so no socket lock is re-entered. ---------------------------- */
#define IP_LOOP 32
static struct { uint16_t len; uint8_t pkt[1500]; } g_ip_loop[IP_LOOP];
static volatile uint32_t g_ip_loop_lock; static uint32_t g_ip_loop_head, g_ip_loop_tail;
static void ip_loop_lock(void) { cos_spin_lock(&g_ip_loop_lock); }
static void ip_loop_unlock(void) { cos_spin_unlock(&g_ip_loop_lock); }
static int ip_loop_put(const void* pkt, size_t len) {
    int ok = 0;
    ip_loop_lock();
    if (len <= sizeof g_ip_loop[0].pkt && g_ip_loop_head - g_ip_loop_tail < IP_LOOP) {
        uint32_t i = g_ip_loop_head++ % IP_LOOP; g_ip_loop[i].len = (uint16_t)len; memcpy(g_ip_loop[i].pkt, pkt, len); ok = 1;
    }
    ip_loop_unlock();
    return ok;
}
void ip_loop_deliver(void) {
    for (int n = 0; n < IP_LOOP; n++) {
        static uint8_t buf[1500]; size_t len = 0;
        ip_loop_lock();
        if (g_ip_loop_tail != g_ip_loop_head) { uint32_t i = g_ip_loop_tail++ % IP_LOOP; len = g_ip_loop[i].len; memcpy(buf, g_ip_loop[i].pkt, len); }
        ip_loop_unlock();
        if (!len) return;
        if (len > sizeof(ip_hdr_t)) ip_recv((ip_hdr_t*)buf, buf + sizeof(ip_hdr_t), len - sizeof(ip_hdr_t));
    }
}

int ip_send(ip_addr_t* dst, uint8_t proto, void* data, size_t len) {
    if (!dst || len > 1480) return -1;
    if (!fw_check(FW_OUT, proto, dst, data, len)) return -1;            /* blocked by the packet filter */
    net_iface_t* iface = net_get_default_iface();
    if (!iface || !iface->send) return -1;
    uint8_t* packet = (uint8_t*)kmalloc(sizeof(ip_hdr_t) + len);
    if (!packet) return -1;
    ip_hdr_t* ip = (ip_hdr_t*)packet;
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->tot_len = htons((uint16_t)(sizeof(ip_hdr_t) + len));
    ip->id = htons((uint16_t)net_rand32());
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = proto;
    ip->checksum = 0;
    memcpy(&ip->src_addr, dst->addr[0] == 127 ? dst->addr : iface->ip.addr, 4);
    memcpy(&ip->dst_addr, dst->addr, 4);
    ip->checksum = ip_checksum(ip, sizeof(ip_hdr_t));
    if (len > 0 && data) memcpy(packet + sizeof(ip_hdr_t), data, len);
    if (dst->addr[0] == 127 || (memcmp(dst->addr, iface->ip.addr, 4) == 0 && iface->ip.addr[0])) {          /* to ourselves (127.0.0.0/8 or our own address) */
        ip_loop_put(packet, sizeof(ip_hdr_t) + len); kfree(packet); return 0;
    }
    eth_addr_t dst_mac;
    if (ip_is_broadcast(dst)) {
        /* DHCP uses 255.255.255.255 before it owns an address.  ARP is
         * inapplicable for this destination; transmit directly to the
         * Ethernet broadcast address instead of dropping the first packet. */
        memset(dst_mac.addr, 0xFF, sizeof(dst_mac.addr));
    } else {
        /* ARP is only valid for a local-LAN peer.  Internet destinations
         * must be delivered to the DHCP-provided gateway; ARPing the remote
         * web server caused every TCP SYN to be dropped under QEMU usernet. */
        ip_addr_t next_hop = *dst;
        int on_link = 1;
        for (int i = 0; i < IP_ADDR_LEN; ++i) {
            if ((dst->addr[i] & iface->netmask.addr[i]) !=
                (iface->ip.addr[i] & iface->netmask.addr[i])) {
                on_link = 0;
                break;
            }
        }
        if (!on_link && (iface->gateway.addr[0] || iface->gateway.addr[1] ||
                         iface->gateway.addr[2] || iface->gateway.addr[3])) {
            next_hop = iface->gateway;
        }
        if (arp_lookup(&next_hop, &dst_mac) < 0) {
            /* The next hop's MAC is unknown: keep the packet and send it the moment the ARP reply arrives instead of
             * dropping it (the first packet to a new peer used to be lost every time). */
            ip_pending_queue(&next_hop, packet, sizeof(ip_hdr_t) + len);
            arp_send_request(&next_hop);
            kfree(packet);
            return 0;
        }
    }
    eth_send(iface, &dst_mac, 0x0800u, packet, sizeof(ip_hdr_t) + len);
    kfree(packet);
    return 0;
}

void ip_recv(ip_hdr_t* hdr, void* data, size_t len) {
    if (!hdr || !data) return;
    uint8_t ihl = (uint8_t)(hdr->version_ihl & 0x0Fu);
    size_t header_len = (size_t)ihl * 4u;
    if (ihl < 5 || len < header_len - IP_HDR_LEN) return;
    uint8_t* payload = (uint8_t*)data;
    size_t payload_len = len;
    {
        ip_addr_t src = {.addr = {(uint8_t)hdr->src_addr, (uint8_t)(hdr->src_addr >> 8), (uint8_t)(hdr->src_addr >> 16), (uint8_t)(hdr->src_addr >> 24)}};
        if (!fw_check(FW_IN, hdr->protocol, &src, payload, payload_len)) return;
    }
    if (hdr->protocol == IP_PROTO_TCP) {
        if (payload_len < sizeof(tcp_hdr_t)) return;

        /* A TCP header is not always the fixed 20-byte base header.  Remote
         * Internet peers commonly attach timestamp/window-scale options.
         * Counting those option bytes as payload produces an ACK beyond the
         * peer's sequence range, so the peer retransmits its response and no
         * HTTP body reaches the socket. */
        tcp_hdr_t* tcp = (tcp_hdr_t*)payload;
        if (!tcp_checksum_ok(hdr, tcp, payload_len)) return;   /* corrupted in transit: the peer retransmits */
        size_t tcp_header_len = (size_t)((tcp->data_off >> 4) & 0x0Fu) * 4u;
        if (tcp_header_len < sizeof(tcp_hdr_t) || tcp_header_len > payload_len) return;
        tcp_handle_packet(hdr, tcp, payload + tcp_header_len,
                          payload_len - tcp_header_len);
    } else if (hdr->protocol == IP_PROTO_ICMP) {
        icmp_recv(hdr, payload, payload_len);
    } else if (hdr->protocol == IP_PROTO_UDP) {
        if (payload_len < sizeof(udp_hdr_t)) return;
        udp_recv(hdr, (udp_hdr_t*)payload, payload + sizeof(udp_hdr_t), payload_len - sizeof(udp_hdr_t));
    }
}

ip_addr_t ip_make_addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    ip_addr_t addr; addr.addr[0] = a; addr.addr[1] = b; addr.addr[2] = c; addr.addr[3] = d; return addr;
}

int ip_addr_cmp(ip_addr_t* a, ip_addr_t* b) {
    if (!a || !b) return -1;
    return memcmp(a->addr, b->addr, IP_ADDR_LEN);
}

int ip_is_broadcast(ip_addr_t* addr) {
    if (!addr) return 0;
    return addr->addr[0] == 255 && addr->addr[1] == 255 && addr->addr[2] == 255 && addr->addr[3] == 255;
}


/* ---- host name / literal -> address (dual stack) */
/* Dotted IPv4 / IPv6 literal only; never touches the network. */
int net_resolve_literal(const char* host, sock_addr_t* out) {
    if (!host || !host[0] || !out) return -1;
    memset(out, 0, sizeof *out);
    {
        const char* p = host; int ok = 1;
        for (int part = 0; part < 4 && ok; ++part) {
            int v = 0, d = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); if (v > 255) { ok = 0; break; } ++p; ++d; }
            if (!d || !ok) { ok = 0; break; }
            out->addr.addr[part] = (uint8_t)v;
            if (part < 3) { if (*p != '.') { ok = 0; break; } ++p; }
        }
        if (ok && *p == '\0') { out->family = AF_INET; return 0; }
    }
    if (strchr(host, ':') && ip6_parse(host, &out->addr6)) { out->family = AF_INET6; return 0; }
    return -1;
}

int net_resolve_host(const char* host, sock_addr_t* out) {
    if (!host || !host[0] || !out) return -1;
    memset(out, 0, sizeof *out);
    /* dotted IPv4 literal */
    {
        const char* p = host; int ok = 1;
        for (int part = 0; part < 4 && ok; ++part) {
            int v = 0, d = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); if (v > 255) { ok = 0; break; } ++p; ++d; }
            if (!d || !ok) { ok = 0; break; }
            out->addr.addr[part] = (uint8_t)v;
            if (part < 3) { if (*p != '.') { ok = 0; break; } ++p; }
        }
        if (ok && *p == '\0') { out->family = AF_INET; return 0; }
    }
    if (strchr(host, ':')) {                                         /* IPv6 literal */
        if (ip6_parse(host, &out->addr6)) { out->family = AF_INET6; return 0; }
        return -1;
    }
    extern int dns_resolve(const char* hostname, ip_addr_t* result);
    extern int dns_resolve6(const char* hostname, ip6_addr_t* result);
    memset(out, 0, sizeof *out);
    if (dns_resolve(host, &out->addr) == 0) { out->family = AF_INET; return 0; }
    if (ip6_ready() || ip6_link_local_ready()) {
        if (dns_resolve6(host, &out->addr6) == 0) { out->family = AF_INET6; return 0; }
    }
    return -1;
}
