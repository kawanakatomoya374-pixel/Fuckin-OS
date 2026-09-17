/**
 * udp.c - UDP Protocol Implementation
 */

#include "udp.h"
#include "arp.h"
#include "memory.h"
#include "string.h"
#include "dns.h"

/* Byte order conversion functions (forward declarations) */
static uint16_t htons(uint16_t n);
static uint16_t ntohs(uint16_t n);

static udp_socket_t* udp_sockets = NULL;
static uint64_t next_ephemeral_port = 49152;

void udp_init(void) {
    udp_sockets = NULL;
    next_ephemeral_port = 49152;
}

udp_socket_t* udp_socket_create(uint16_t local_port) {
    udp_socket_t* sock = (udp_socket_t*)kmalloc(sizeof(udp_socket_t));
    if (!sock) return NULL;
    
    memset(sock, 0, sizeof(udp_socket_t));
    
    if (local_port == 0) {
        sock->local_port = next_ephemeral_port++;
    } else {
        sock->local_port = local_port;
    }
    
    sock->rx_buffer = (uint8_t*)kmalloc(UDP_MAX_PACKET);
    if (!sock->rx_buffer) {
        kfree(sock);
        return NULL;
    }
    sock->rx_len = 0;
    
    sock->next = udp_sockets;
    udp_sockets = sock;
    
    return sock;
}

int udp_socket_send(udp_socket_t* sock, ip_addr_t* dst, void* data, size_t len) {
    if (!sock || !dst || !data) return -1;
    return udp_send(dst, sock->local_port, sock->remote_port, data, len);
}

int udp_socket_recv(udp_socket_t* sock, void* buf, size_t max_len, ip_addr_t* from) {
    if (!sock || !buf || max_len == 0) return -1;
    if (!sock->rx_buffer) return -1;
    if (sock->rx_len == 0) return 0;

    size_t copy_len = sock->rx_len < max_len ? sock->rx_len : max_len;
    memcpy(buf, sock->rx_buffer, copy_len);

    if (from) {
        memcpy(from->addr, sock->remote_addr.addr, 4);
    }

    sock->rx_len = 0;
    return (int)copy_len;
}

void udp_socket_close(udp_socket_t* sock) {
    if (!sock) return;
    
    /* Remove from list */
    udp_socket_t* prev = NULL;
    udp_socket_t* curr = udp_sockets;
    while (curr) {
        if (curr == sock) {
            if (prev) {
                prev->next = curr->next;
            } else {
                udp_sockets = curr->next;
            }
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    
    if (sock->rx_buffer) {
        kfree(sock->rx_buffer);
    }
    kfree(sock);
}

int udp_send(ip_addr_t* dst, uint16_t src_port, uint16_t dst_port, 
             void* data, size_t len) {
    if (len > UDP_MAX_PACKET) return -1;
    
    /* Allocate packet buffer */
    uint8_t* packet = (uint8_t*)kmalloc(sizeof(udp_hdr_t) + len);
    if (!packet) return -1;
    
    /* Build UDP header */
    udp_hdr_t* hdr = (udp_hdr_t*)packet;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->len = htons((uint16_t)(sizeof(udp_hdr_t) + len));
    hdr->checksum = 0;  /* Optional, set to 0 for now */
    
    /* Copy data */
    if (len > UDP_MAX_PACKET - sizeof(udp_hdr_t)) len = UDP_MAX_PACKET - sizeof(udp_hdr_t);
    memcpy(packet + sizeof(udp_hdr_t), data, len);
    
    /* Send via IP */
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

    /* Find matching socket */
    udp_socket_t* sock = udp_sockets;
    while (sock) {
        if (sock->local_port == dst_port && sock->rx_buffer) {
            if (payload_len > UDP_MAX_PACKET) payload_len = UDP_MAX_PACKET;
            if (payload_len > 0 && data) {
                memcpy(sock->rx_buffer, data, payload_len);
            }
            sock->rx_len = payload_len;
            sock->remote_port = src_port;
            memcpy(sock->remote_addr.addr, &ip->src_addr, 4);
            return;
        }
        sock = sock->next;
    }

    if (src_port == DNS_PORT || dst_port == DNS_PORT) {
        extern void dns_handle_response(void* data, size_t len);
        dns_handle_response(data, payload_len);
        return;
    }

    if (dst_port == 67 || src_port == 67) {
        extern void dhcp_handle_packet(void* packet, size_t len);
        dhcp_handle_packet(data, payload_len);
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
