
/**
 * net.c - Network Stack Glue / Socket Abstraction
 */

#ifndef COS_ENABLE_NETWORK
/* Keep this translation unit aligned with kernel.c.  The E1000 path uses
 * page-backed DMA rings and is enabled by default. */
#define COS_ENABLE_NETWORK 1
#endif

#include "net.h"
#include "arp.h"
#include "udp.h"
#include "tcp.h"
#include "dns.h"
#include "dhcp.h"
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
    dhcp_init();

    if (e1000_init() == 0) {
        serial_puts("[NET] NIC initialized\n");
    } else {
        serial_puts("[NET] No supported NIC found\n");
    }

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
    dhcp_poll();
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
    (void)domain; (void)protocol;
    socket_t* sock = (socket_t*)kmalloc(sizeof(socket_t));
    if (!sock) return NULL;
    memset(sock, 0, sizeof(socket_t));
    sock->type = type;
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
    if (sock->type == SOCK_STREAM && sock->private_data) {
        int ret = tcp_socket_connect((tcp_socket_t*)sock->private_data, &addr->addr, addr->port);
        if (ret == 0) sock->state = SS_CONNECTED;
        return ret;
    }
    if (sock->type == SOCK_DGRAM) {
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

uint16_t ip_checksum(void* data, size_t len) {
    uint64_t sum = 0;
    uint16_t* ptr = (uint16_t*)data;
    while (len > 1) { sum += *ptr++; len -= 2; }
    if (len == 1) sum += *(uint8_t*)ptr;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

void eth_send(net_iface_t* iface, eth_addr_t* dst, uint16_t type, void* data, size_t len) {
    if (!iface || !dst || !iface->send) return;
    uint8_t* frame = (uint8_t*)kmalloc(ETH_HDR_LEN + len);
    if (!frame) return;
    eth_hdr_t* hdr = (eth_hdr_t*)frame;
    hdr->dst = *dst;
    hdr->src = iface->mac;
    hdr->type = htons(type);
    if (len > 0 && data) memcpy(frame + ETH_HDR_LEN, data, len);
    iface->send(iface, frame, ETH_HDR_LEN + len);
    kfree(frame);
}

void eth_recv(net_iface_t* iface, void* data, size_t len) {
    if (!iface || !data || len < ETH_HDR_LEN) return;
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
        ip_recv(ip, payload + ihl, ip_total_len - ihl);
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

int ip_send(ip_addr_t* dst, uint8_t proto, void* data, size_t len) {
    if (!dst || len > 1480) return -1;
    net_iface_t* iface = net_get_default_iface();
    if (!iface || !iface->send) return -1;
    uint8_t* packet = (uint8_t*)kmalloc(sizeof(ip_hdr_t) + len);
    if (!packet) return -1;
    ip_hdr_t* ip = (ip_hdr_t*)packet;
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->tot_len = htons((uint16_t)(sizeof(ip_hdr_t) + len));
    ip->id = 0;
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = proto;
    ip->checksum = 0;
    memcpy(&ip->src_addr, iface->ip.addr, 4);
    memcpy(&ip->dst_addr, dst->addr, 4);
    ip->checksum = ip_checksum(ip, sizeof(ip_hdr_t));
    if (len > 0 && data) memcpy(packet + sizeof(ip_hdr_t), data, len);
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
            arp_send_request(&next_hop);
            kfree(packet);
            return -1;
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
    if (hdr->protocol == IP_PROTO_TCP) {
        if (payload_len < sizeof(tcp_hdr_t)) return;

        /* A TCP header is not always the fixed 20-byte base header.  Remote
         * Internet peers commonly attach timestamp/window-scale options.
         * Counting those option bytes as payload produces an ACK beyond the
         * peer's sequence range, so the peer retransmits its response and no
         * HTTP body reaches the socket. */
        tcp_hdr_t* tcp = (tcp_hdr_t*)payload;
        size_t tcp_header_len = (size_t)((tcp->data_off >> 4) & 0x0Fu) * 4u;
        if (tcp_header_len < sizeof(tcp_hdr_t) || tcp_header_len > payload_len) return;
        tcp_handle_packet(hdr, tcp, payload + tcp_header_len,
                          payload_len - tcp_header_len);
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
