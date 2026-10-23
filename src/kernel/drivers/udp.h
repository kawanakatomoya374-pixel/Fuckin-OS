/**
 * udp.h - UDP Protocol
 */

#ifndef UDP_H
#define UDP_H

#include "types.h"
#include "net.h"
#include "ip6.h"

#define UDP_MAX_PACKET  1472  // 1500 - 20 (IP) - 8 (UDP)
#define UDP_QUEUE       8     /* datagrams buffered per socket; a full queue drops the newest */

/* One buffered datagram with where it came from. */
typedef struct {
    uint16_t len;
    uint16_t port;               /* sender's port */
    uint8_t  v6;
    ip_addr_t a4;
    ip6_addr_t a6;
    uint8_t  data[UDP_MAX_PACKET];
} udp_dgram_t;

/* UDP socket (udp_hdr_t is defined in net.h) */
typedef struct udp_socket {
    uint16_t local_port;
    uint16_t remote_port;        /* set by connect: only this peer's datagrams are accepted, and send() goes there */
    ip_addr_t remote_addr;
    uint8_t   v6;                /* IPv6 socket: remote6 is the peer */
    ip6_addr_t remote6;
    uint8_t   connected;
    uint32_t  dropped;           /* datagrams lost to a full queue */

    udp_dgram_t* queue;          /* UDP_QUEUE entries, used as a ring */
    uint8_t  q_head, q_count;

    struct udp_socket* next;
} udp_socket_t;

void udp_init(void);
/* local_port 0 picks an unused ephemeral port; a specific port that is already in use fails (NULL). */
udp_socket_t* udp_socket_create(uint16_t local_port);
int udp_socket_bind(udp_socket_t* sock, uint16_t port);            /* 0, or -1 when the port is taken */
int udp_socket_connect(udp_socket_t* sock, const ip_addr_t* peer, uint16_t port);
int udp_socket_sendto(udp_socket_t* sock, const ip_addr_t* dst, uint16_t dport, const void* data, size_t len);
/* Pops one datagram. Returns its length (truncated to max_len), or -1 when the queue is empty. */
int udp_socket_recvfrom(udp_socket_t* sock, void* buf, size_t max_len, ip_addr_t* from4, ip6_addr_t* from6,
                        uint16_t* from_port, int* from_v6);
int udp_socket_pending(udp_socket_t* sock);                         /* datagrams waiting */
int udp_socket_send(udp_socket_t* sock, ip_addr_t* dst, void* data, size_t len);
int udp_socket_recv(udp_socket_t* sock, void* buf, size_t max_len, ip_addr_t* from);
void udp_socket_close(udp_socket_t* sock);
int udp_socket_recv6(udp_socket_t* sock, void* buf, size_t max_len, ip6_addr_t* from);

/* Raw UDP send/receive - uses definitions from net.h */
int udp_send(ip_addr_t* dst, uint16_t src_port, uint16_t dst_port,
             void* data, size_t len);
void udp_recv(ip_hdr_t* ip, udp_hdr_t* udp, void* data, size_t len);

/* IPv6: send a datagram (mandatory checksum) / handle one received by ip6_recv. */
int udp6_send(const ip6_addr_t* dst, uint16_t src_port, uint16_t dst_port, const void* data, size_t len);
void udp6_recv(const ip6_addr_t* src, const ip6_addr_t* dst, const uint8_t* udp, size_t len);

#endif /* UDP_H */
