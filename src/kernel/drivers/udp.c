/**
 * udp.c - UDP Protocol Implementation
 *
 * Sockets own a small ring of datagrams (with the sender's address and port) so a burst is not collapsed into the last
 * packet, a socket can serve many peers (recvfrom), and the receive path verifies the checksum.
 */

#include "sync.h"
#include "udp.h"
#include "arp.h"
#include "memory.h"
#include "string.h"
#include "dns.h"

extern uint32_t net_rand32(void);

/* Byte order conversion functions (forward declarations) */
static uint16_t htons(uint16_t n);
static uint16_t ntohs(uint16_t n);

static udp_socket_t* udp_sockets = NULL;
static volatile uint32_t udp_lock = 0;

static void udp_lock_acquire(void) { cos_spin_lock(&udp_lock); }
static void udp_lock_release(void) { cos_spin_unlock(&udp_lock); }

void udp_init(void) {
    udp_sockets = NULL;
    __atomic_store_n(&udp_lock, 0u, __ATOMIC_RELEASE);
}

static bool port_in_use_locked(uint16_t port, const udp_socket_t* except) {
    for (udp_socket_t* s = udp_sockets; s; s = s->next) if (s != except && s->local_port == port) return true;
    return false;
}

/* Ephemeral ports are random (an attacker cannot guess the next one) and never collide with a live socket. */
static uint16_t pick_ephemeral_locked(void) {
    for (int i = 0; i < 64; i++) {
        uint16_t p = (uint16_t)(49152u + net_rand32() % 16384u);
        if (!port_in_use_locked(p, NULL)) return p;
    }
    return 0;
}

udp_socket_t* udp_socket_create(uint16_t local_port) {
    udp_socket_t* sock = (udp_socket_t*)kmalloc(sizeof(udp_socket_t));
    if (!sock) return NULL;
    memset(sock, 0, sizeof(udp_socket_t));
    sock->queue = (udp_dgram_t*)kmalloc(sizeof(udp_dgram_t) * UDP_QUEUE);
    if (!sock->queue) { kfree(sock); return NULL; }

    udp_lock_acquire();
    if (local_port == 0) local_port = pick_ephemeral_locked();
    else if (port_in_use_locked(local_port, NULL)) local_port = 0;
    if (local_port == 0) { udp_lock_release(); kfree(sock->queue); kfree(sock); return NULL; }
    sock->local_port = local_port;
    sock->next = udp_sockets;
    udp_sockets = sock;
    udp_lock_release();
    return sock;
}

int udp_socket_bind(udp_socket_t* sock, uint16_t port) {
    if (!sock || port == 0) return -1;
    udp_lock_acquire();
    int rc = port_in_use_locked(port, sock) ? -1 : 0;
    if (rc == 0) sock->local_port = port;
    udp_lock_release();
    return rc;
}

int udp_socket_connect(udp_socket_t* sock, const ip_addr_t* peer, uint16_t port) {
    if (!sock || !peer) return -1;
    udp_lock_acquire();
    sock->v6 = 0;
    sock->remote_addr = *peer;
    sock->remote_port = port;
    sock->connected = 1;
    udp_lock_release();
    return 0;
}

int udp_socket_sendto(udp_socket_t* sock, const ip_addr_t* dst, uint16_t dport, const void* data, size_t len) {
    if (!sock || !dst || (len && !data) || len > UDP_MAX_PACKET) return -1;
    ip_addr_t d = *dst;
    return udp_send(&d, sock->local_port, dport, (void*)data, len);
}

int udp_socket_send(udp_socket_t* sock, ip_addr_t* dst, void* data, size_t len) {
    if (!sock || !dst || !data) return -1;
    return udp_send(dst, sock->local_port, sock->remote_port, data, len);
}

int udp_socket_pending(udp_socket_t* sock) {
    if (!sock) return 0;
    udp_lock_acquire();
    int n = sock->q_count;
    udp_lock_release();
    return n;
}

int udp_socket_recvfrom(udp_socket_t* sock, void* buf, size_t max_len, ip_addr_t* from4, ip6_addr_t* from6,
                        uint16_t* from_port, int* from_v6) {
    if (!sock || !sock->queue) return -1;
    udp_lock_acquire();
    if (sock->q_count == 0) { udp_lock_release(); return -1; }
    udp_dgram_t* d = &sock->queue[sock->q_head];
    size_t n = d->len < max_len ? d->len : max_len;
    if (buf && n) memcpy(buf, d->data, n);
    if (from4) *from4 = d->a4;
    if (from6) *from6 = d->a6;
    if (from_port) *from_port = d->port;
    if (from_v6) *from_v6 = d->v6;
    sock->q_head = (uint8_t)((sock->q_head + 1u) % UDP_QUEUE);
    sock->q_count--;
    udp_lock_release();
    return (int)n;
}

int udp_socket_recv(udp_socket_t* sock, void* buf, size_t max_len, ip_addr_t* from) {
    if (!sock || !buf || max_len == 0) return -1;
    int n = udp_socket_recvfrom(sock, buf, max_len, from, NULL, NULL, NULL);
    return n < 0 ? 0 : n;                      /* legacy contract: 0 = nothing waiting */
}

int udp_socket_recv6(udp_socket_t* sock, void* buf, size_t max_len, ip6_addr_t* from) {
    if (!sock || !buf || !max_len) return -1;
    int n = udp_socket_recvfrom(sock, buf, max_len, NULL, from, NULL, NULL);
    return n < 0 ? 0 : n;
}

void udp_socket_close(udp_socket_t* sock) {
    if (!sock) return;
    udp_lock_acquire();
    udp_socket_t* prev = NULL;
    udp_socket_t* curr = udp_sockets;
    while (curr) {
        if (curr == sock) {
            if (prev) prev->next = curr->next;
            else udp_sockets = curr->next;
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    udp_lock_release();
    if (sock->queue) kfree(sock->queue);
    kfree(sock);
}

/* RFC 768 checksum over the IPv4 pseudo header and the UDP segment; a segment with a valid checksum folds to zero. */
static uint16_t udp_csum(const uint8_t* src, const uint8_t* dst, const void* seg, size_t seglen) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i += 2) {
        sum += ((uint32_t)src[i] << 8) | src[i + 1];
        sum += ((uint32_t)dst[i] << 8) | dst[i + 1];
    }
    sum += IP_PROTO_UDP;
    sum += (uint32_t)seglen;
    const uint8_t* b = (const uint8_t*)seg;
    for (size_t i = 0; i + 1 < seglen; i += 2) sum += ((uint32_t)b[i] << 8) | b[i + 1];
    if (seglen & 1u) sum += (uint32_t)b[seglen - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

int udp_send(ip_addr_t* dst, uint16_t src_port, uint16_t dst_port,
             void* data, size_t len) {
    if (len > UDP_MAX_PACKET) return -1;
    extern net_iface_t* net_get_default_iface(void);
    net_iface_t* iface = net_get_default_iface();

    uint8_t* packet = (uint8_t*)kmalloc(sizeof(udp_hdr_t) + len);
    if (!packet) return -1;

    udp_hdr_t* hdr = (udp_hdr_t*)packet;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->len = htons((uint16_t)(sizeof(udp_hdr_t) + len));
    hdr->checksum = 0;
    if (len) memcpy(packet + sizeof(udp_hdr_t), data, len);
    if (iface) {
        uint16_t c = udp_csum(iface->ip.addr, dst->addr, packet, sizeof(udp_hdr_t) + len);
        hdr->checksum = htons(c ? c : 0xFFFFu);          /* a computed zero is sent as all ones; zero would mean "none" */
    }

    int ret = ip_send(dst, IP_PROTO_UDP, packet, sizeof(udp_hdr_t) + len);
    kfree(packet);
    return ret;
}

void udp_recv(ip_hdr_t* ip, udp_hdr_t* udp, void* data, size_t len) {
    if (!ip || !udp) return;

    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t udp_len = ntohs(udp->len);
    if (udp_len < sizeof(udp_hdr_t)) return;

    size_t payload_len = (size_t)udp_len - sizeof(udp_hdr_t);
    if (payload_len > len) return;

    uint8_t src[4], dst[4];
    memcpy(src, &ip->src_addr, 4);
    memcpy(dst, &ip->dst_addr, 4);
    if (udp->checksum != 0 && udp_csum(src, dst, udp, udp_len) != 0) return;     /* corrupted in transit */

    udp_lock_acquire();
    for (udp_socket_t* sock = udp_sockets; sock; sock = sock->next) {
        if (sock->v6 || sock->local_port != dst_port || !sock->queue) continue;
        if (sock->connected && (sock->remote_port != src_port || memcmp(sock->remote_addr.addr, src, 4) != 0)) continue;
        if (sock->q_count >= UDP_QUEUE) { sock->dropped++; udp_lock_release(); return; }
        udp_dgram_t* d = &sock->queue[(sock->q_head + sock->q_count) % UDP_QUEUE];
        if (payload_len > UDP_MAX_PACKET) payload_len = UDP_MAX_PACKET;
        d->len = (uint16_t)payload_len;
        d->port = src_port;
        d->v6 = 0;
        memcpy(d->a4.addr, src, 4);
        if (payload_len && data) memcpy(d->data, data, payload_len);
        sock->q_count++;
        udp_lock_release();
        return;
    }
    udp_lock_release();

    if (src_port == DNS_PORT || dst_port == DNS_PORT) {
        extern void dns_handle_response(void* data, size_t len);
        extern int dns_expects_port(uint16_t port);
        if (src_port == DNS_PORT && dns_expects_port(dst_port)) dns_handle_response(data, payload_len);
        return;
    }

    if (dst_port == 67 || src_port == 67) {
        extern void dhcp_handle_packet(void* packet, size_t len);
        dhcp_handle_packet(data, payload_len);
    }
}

#include "ip6.h"
int udp6_send(const ip6_addr_t* dst, uint16_t sport, uint16_t dport, const void* data, size_t len) {
    if (len > 1232) return -1;
    uint8_t pkt[8 + 1232];
    pkt[0] = (uint8_t)(sport >> 8); pkt[1] = (uint8_t)sport; pkt[2] = (uint8_t)(dport >> 8); pkt[3] = (uint8_t)dport;
    uint16_t total = (uint16_t)(8 + len);
    pkt[4] = (uint8_t)(total >> 8); pkt[5] = (uint8_t)total; pkt[6] = pkt[7] = 0;
    memcpy(pkt + 8, data, len);
    ip6_addr_t src = ip6_select_src(dst);
    uint16_t c = ip6_checksum(&src, dst, 17, pkt, total);
    if (c == 0) c = 0xFFFF;                                   /* a transmitted zero checksum would mean "none", illegal in IPv6 */
    pkt[6] = (uint8_t)(c >> 8); pkt[7] = (uint8_t)c;
    return ip6_send(dst, 17, pkt, total);
}

void udp6_recv(const ip6_addr_t* src, const ip6_addr_t* dst, const uint8_t* u, size_t len) {
    if (len < 8) return;
    uint16_t ulen = (uint16_t)((u[4] << 8) | u[5]);
    if (ulen < 8 || ulen > len) return;
    if (ip6_checksum(src, dst, 17, u, ulen) != 0) return;
    uint16_t sport = (uint16_t)((u[0] << 8) | u[1]), dport = (uint16_t)((u[2] << 8) | u[3]);
    udp_lock_acquire();
    for (udp_socket_t* sock = udp_sockets; sock; sock = sock->next) {
        if (!sock->v6 || sock->local_port != dport || !sock->queue) continue;
        if (sock->q_count >= UDP_QUEUE) { sock->dropped++; udp_lock_release(); return; }
        udp_dgram_t* d = &sock->queue[(sock->q_head + sock->q_count) % UDP_QUEUE];
        size_t n = (size_t)ulen - 8; if (n > UDP_MAX_PACKET) n = UDP_MAX_PACKET;
        d->len = (uint16_t)n; d->port = sport; d->v6 = 1; d->a6 = *src;
        memcpy(d->data, u + 8, n);
        sock->q_count++;
        udp_lock_release();
        return;
    }
    udp_lock_release();
    if (sport == DNS_PORT) {
        extern void dns_handle_response(void* data, size_t len);
        dns_handle_response((void*)(u + 8), (size_t)ulen - 8);
    }
}

/* Helper: htons */
static uint16_t htons(uint16_t n) {
    return ((n & 0xFF) << 8) | ((n >> 8) & 0xFF);
}

/* Helper: ntohs */
static uint16_t ntohs(uint16_t n) {
    return htons(n);
}
