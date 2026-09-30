/**
 * tcp.h - TCP Protocol
 */

#ifndef TCP_H
#define TCP_H

#include "types.h"
#include "net.h"

#define TCP_STATE_CLOSED        0
#define TCP_STATE_LISTEN        1
#define TCP_STATE_SYN_SENT      2
#define TCP_STATE_SYN_RECEIVED  3
#define TCP_STATE_ESTABLISHED   4
#define TCP_STATE_FIN_WAIT1     5
#define TCP_STATE_FIN_WAIT2     6
#define TCP_STATE_CLOSE_WAIT    7
#define TCP_STATE_CLOSING       8
#define TCP_STATE_LAST_ACK      9
#define TCP_STATE_TIME_WAIT     10

#define TCP_MAX_SOCKETS         16
/* TCP_WINDOW_SIZE is defined in net.h */
#define TCP_MSS                 1460

/* Forward declaration */
void tcp_poll(void);

typedef struct tcp_socket {
    uint16_t local_port;
    uint16_t remote_port;
    ip_addr_t remote_addr;
    
    int state;
    
    uint64_t seq_num;
    uint64_t ack_num;
    uint64_t remote_seq;
    
    uint8_t* rx_buffer;
    uint64_t rx_len;
    uint64_t rx_max;
    
    uint8_t* tx_buffer;
    uint64_t tx_len;
    uint64_t tx_max;
    
    struct tcp_socket* next;
} tcp_socket_t;

void tcp_init(void);
tcp_socket_t* tcp_socket_create(void);
void tcp_socket_destroy(tcp_socket_t* sock);
int tcp_socket_bind(tcp_socket_t* sock, uint16_t port);
int tcp_socket_connect(tcp_socket_t* sock, ip_addr_t* addr, uint16_t port);
int tcp_socket_listen(tcp_socket_t* sock, int backlog);
tcp_socket_t* tcp_socket_accept(tcp_socket_t* sock);
int tcp_socket_send(tcp_socket_t* sock, void* data, size_t len);
int tcp_socket_recv(tcp_socket_t* sock, void* buf, size_t max_len);
int tcp_socket_close(tcp_socket_t* sock);

void tcp_handle_packet(ip_hdr_t* ip, tcp_hdr_t* tcp, void* data, size_t len);

#endif /* TCP_H */
