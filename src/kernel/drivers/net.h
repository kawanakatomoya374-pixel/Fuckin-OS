/**
 * net.h - Network Stack
 * 
 * TCP/IP stack with socket API support.
 */

#ifndef NET_H
#define NET_H

#include "types.h"

/* Ethernet constants */
#define ETH_ADDR_LEN    6
#define ETH_MTU         1500
#define ETH_HDR_LEN     14

/* IP constants */
#define IP_ADDR_LEN     4
#define IP_HDR_LEN      20
#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

/* TCP constants */
#define TCP_HDR_LEN     20
#define TCP_PORT_MIN    1024
#define TCP_PORT_MAX    65535
#define TCP_WINDOW_SIZE 65535

/* Socket types */
#define SOCK_STREAM     1   // TCP
#define SOCK_DGRAM      2   // UDP
#define SOCK_RAW        3   // Raw IP

/* Address families */
#define AF_INET         2   // IPv4

/* Protocol families */
#define PF_INET         AF_INET

/* Socket states */
typedef enum {
    SS_FREE = 0,
    SS_UNCONNECTED,
    SS_CONNECTING,
    SS_CONNECTED,
    SS_DISCONNECTING,
} socket_state_t;

/* Ethernet address */
typedef struct {
    uint8_t addr[ETH_ADDR_LEN];
} eth_addr_t;

/* IP address */
typedef struct {
    uint8_t addr[IP_ADDR_LEN];
} ip_addr_t;

/* Socket address */
typedef struct {
    uint16_t family;
    uint16_t port;
    ip_addr_t addr;
} sock_addr_t;

/* Network interface */
typedef struct net_iface {
    char name[8];
    eth_addr_t mac;
    ip_addr_t ip;
    ip_addr_t netmask;
    ip_addr_t gateway;
    
    int (*send)(struct net_iface* iface, void* data, size_t len);
    int (*recv)(struct net_iface* iface, void* buf, size_t len);
    
    struct net_iface* next;
} net_iface_t;

/* Socket structure */
typedef struct socket {
    int type;
    socket_state_t state;
    
    uint16_t local_port;
    uint16_t remote_port;
    ip_addr_t local_addr;
    ip_addr_t remote_addr;
    
    /* Connection state */
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t window;
    
    /* Buffers */
    uint8_t* rx_buf;
    uint64_t rx_len;
    uint8_t* tx_buf;
    uint64_t tx_len;

    /* Protocol-specific data */
    void* private_data;

    struct socket* next;
} socket_t;

/* Ethernet header */
typedef struct __attribute__((packed)) {
    eth_addr_t dst;
    eth_addr_t src;
    uint16_t type;
} eth_hdr_t;

/* IP header */
typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;       // Version (4) + IHL (4)
    uint8_t  tos;               // Type of service
    uint16_t tot_len;           // Total length
    uint16_t id;                // Identification
    uint16_t frag_off;          // Flags + fragment offset
    uint8_t  ttl;               // Time to live
    uint8_t  protocol;          // Protocol
    uint16_t checksum;          // Header checksum
    uint32_t src_addr;          // Source address
    uint32_t dst_addr;          // Destination address
} ip_hdr_t;

/* TCP header */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_off;      // Data offset + reserved
    uint8_t  flags;         // Flags
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

/* TCP flags */
#define TCP_FIN     0x01
#define TCP_SYN     0x02
#define TCP_RST     0x04
#define TCP_PSH     0x08
#define TCP_ACK     0x10
#define TCP_URG     0x20

/* UDP header */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
} udp_hdr_t;

/* ARP */
#define ARP_HTYPE_ETH   1
#define ARP_PTYPE_IP    0x0800
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    eth_addr_t sha;
    ip_addr_t spa;
    eth_addr_t tha;
    ip_addr_t tpa;
} arp_packet_t;

/* Function prototypes */
void net_init(void);
net_iface_t* net_get_default_iface(void);
void net_poll(void);
/* Snapshot counters for diagnostics; output pointers may be NULL. */
void net_get_dispatch_stats(uint64_t* completed_passes, uint64_t* contended_calls);
bool net_is_connected(void);

/* Interface management */
net_iface_t* net_iface_register(const char* name, eth_addr_t* mac);
int net_iface_send(net_iface_t* iface, void* data, size_t len);
int net_iface_recv(net_iface_t* iface, void* buf, size_t len);

/* Socket API */
socket_t* socket_create(int domain, int type, int protocol);
int socket_bind(socket_t* sock, sock_addr_t* addr);
int socket_listen(socket_t* sock, int backlog);
socket_t* socket_accept(socket_t* sock, sock_addr_t* addr);
int socket_connect(socket_t* sock, sock_addr_t* addr);
int socket_send(socket_t* sock, void* buf, size_t len, int flags);
int socket_recv(socket_t* sock, void* buf, size_t len, int flags);
int socket_close(socket_t* sock);

/* Packet handling */
void net_handle_packet(net_iface_t* iface, void* data, size_t len);
void net_arp_request(ip_addr_t* ip);
void net_arp_reply(arp_packet_t* req);

/* IP utilities */
uint16_t ip_checksum(void* data, size_t len);
int ip_send(ip_addr_t* dst, uint8_t proto, void* data, size_t len);
void ip_recv(ip_hdr_t* hdr, void* data, size_t len);

/* TCP utilities */
int tcp_send_syn(socket_t* sock);
int tcp_send_ack(socket_t* sock);
int tcp_send_data(socket_t* sock, void* data, size_t len);
void tcp_handle_packet(ip_hdr_t* ip, tcp_hdr_t* tcp, void* data, size_t len);

/* UDP utilities */
int udp_send(ip_addr_t* dst, uint16_t src_port, uint16_t dst_port, 
             void* data, size_t len);
void udp_recv(ip_hdr_t* ip, udp_hdr_t* udp, void* data, size_t len);

/* Ethernet utilities */
void eth_send(net_iface_t* iface, eth_addr_t* dst, uint16_t type, 
              void* data, size_t len);
void eth_recv(net_iface_t* iface, void* data, size_t len);

/* Address utilities */
ip_addr_t ip_make_addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
int ip_addr_cmp(ip_addr_t* a, ip_addr_t* b);
int ip_is_broadcast(ip_addr_t* addr);

#endif /* NET_H */
