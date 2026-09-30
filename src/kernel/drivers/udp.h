/**
 * udp.h - UDP Protocol
 */

#ifndef UDP_H
#define UDP_H

#include "types.h"
#include "net.h"

#define UDP_MAX_PACKET  1472  // 1500 - 20 (IP) - 8 (UDP)

/* UDP socket (udp_hdr_t is defined in net.h) */
typedef struct udp_socket {
    uint16_t local_port;
    uint16_t remote_port;
    ip_addr_t remote_addr;
    
    uint8_t* rx_buffer;
    uint64_t rx_len;
    
    struct udp_socket* next;
} udp_socket_t;

void udp_init(void);
udp_socket_t* udp_socket_create(uint16_t local_port);
int udp_socket_send(udp_socket_t* sock, ip_addr_t* dst, void* data, size_t len);
int udp_socket_recv(udp_socket_t* sock, void* buf, size_t max_len, ip_addr_t* from);
void udp_socket_close(udp_socket_t* sock);

/* Raw UDP send/receive - uses definitions from net.h */
int udp_send(ip_addr_t* dst, uint16_t src_port, uint16_t dst_port,
             void* data, size_t len);
void udp_recv(ip_hdr_t* ip, udp_hdr_t* udp, void* data, size_t len);

#endif /* UDP_H */
