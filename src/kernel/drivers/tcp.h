/**
 * tcp.h - TCP Protocol
 */

#ifndef TCP_H
#define TCP_H

#include "types.h"
#include "net.h"
#include "ip6.h"

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
#define TCP_MAX_RETRIES         8

/* Forward declaration */
void tcp_poll(void);

typedef struct tcp_socket {
    uint16_t local_port;
    uint16_t remote_port;
    ip_addr_t remote_addr;
    uint8_t   v6;                /* 1: the peer is an IPv6 host, remote6 holds its address */
    ip6_addr_t remote6;
    
    int state;
    
    /* Sequence state (RFC 793): snd_una = oldest unacknowledged byte (tx_buffer[0] is that byte), snd_nxt = next
     * byte to send, rcv_nxt = next byte expected from the peer. seq_num/ack_num mirror snd_nxt/rcv_nxt. */
    uint64_t seq_num;
    uint64_t ack_num;
    uint64_t remote_seq;
    uint32_t snd_una, snd_nxt, snd_max, rcv_nxt;   /* snd_max: highest sequence ever sent (+1) */
    uint64_t tx_sent;            /* bytes of tx_buffer already transmitted (in flight) */
    uint32_t peer_wnd;           /* peer's advertised receive window */
    uint16_t peer_mss;           /* segment size to use towards the peer */
    uint32_t cwnd, ssthresh;     /* congestion control (slow start / congestion avoidance) */
    uint32_t rto, srtt, rttvar;  /* retransmission timeout state, milliseconds */
    uint64_t rtx_at;             /* absolute tick of the next retransmission, 0 = timer off */
    uint8_t  rtx_count;
    uint8_t  dup_acks;
    uint8_t  fin_queued, fin_sent, syn_sent, reset_seen;
    uint32_t rtt_seq;            /* RTT sample: ack must reach this byte ... */
    uint64_t rtt_start;          /* ... measured from this tick (0 = no sample pending) */
    uint64_t time_wait_until;
    uint32_t ooo_seq, ooo_len;   /* one buffered out-of-order block: bytes [ooo_seq, ooo_seq+ooo_len) sit in rx_buffer
                                  * at rx_len + (ooo_seq - rcv_nxt) */
    
    /* Server side. A listener (state LISTEN) owns children created by incoming SYNs; a child stays 'unaccepted' (parent
     * set) until tcp_socket_accept() hands it over. */
    struct tcp_socket* parent;
    uint8_t  backlog;            /* listener: how many unaccepted children it may hold */
    uint8_t  unaccepted;         /* child: created by a listener and not yet accepted */
    uint8_t  bound;              /* explicitly bound: the port stays reserved even before listen/connect */
    uint8_t  detached;           /* the owner has let go: the stack finishes the close and frees the socket itself */
    uint64_t detach_deadline;    /* a detached socket that has not finished closing by this tick is dropped */

    uint8_t* rx_buffer;
    uint64_t rx_len;
    uint64_t rx_max;
    
    uint8_t* tx_buffer;
    uint64_t tx_off;             /* the unacknowledged data starts here: an ACK advances this offset instead of shifting the buffer */
    uint64_t tx_len;
    uint64_t tx_max;
    
    struct tcp_socket* next;
} tcp_socket_t;

void tcp_init(void);
tcp_socket_t* tcp_socket_create(void);
void tcp_socket_destroy(tcp_socket_t* sock);
int tcp_socket_bind(tcp_socket_t* sock, uint16_t port);
int tcp_socket_connect(tcp_socket_t* sock, ip_addr_t* addr, uint16_t port);
int tcp_socket_connect6(tcp_socket_t* sock, const ip6_addr_t* addr, uint16_t port);
int tcp_socket_listen(tcp_socket_t* sock, int backlog);
/* Returns an established connection from the accept queue, or NULL when none is ready. Never blocks. */
tcp_socket_t* tcp_socket_accept(tcp_socket_t* sock);
int tcp_socket_accept_ready(tcp_socket_t* listener);   /* connections waiting in the accept queue */

/* Non-blocking forms for the ring-3 socket layer. connect_start returns 0 once the handshake is under way (poll the state
 * for ESTABLISHED / CLOSED); send_nb returns the bytes queued, 0 when the send queue is full, -1 on a dead connection. */
int tcp_socket_connect_start(tcp_socket_t* sock, const ip_addr_t* addr, uint16_t port);
int tcp_socket_connect_start6(tcp_socket_t* sock, const ip6_addr_t* addr, uint16_t port);
int tcp_socket_shutdown_wr(tcp_socket_t* sock);
int tcp_socket_send_nb(tcp_socket_t* sock, const void* data, size_t len);
int tcp_socket_state(tcp_socket_t* sock);
size_t tcp_socket_rx_ready(tcp_socket_t* sock);        /* bytes waiting to be read */
size_t tcp_socket_tx_space(tcp_socket_t* sock);        /* bytes that send_nb would accept right now */
int tcp_socket_was_reset(tcp_socket_t* sock);
/* Starts an orderly close and hands the socket to the stack, which frees it when the close finishes. The caller must not
 * touch the pointer afterwards. */
void tcp_socket_abandon(tcp_socket_t* sock);
int tcp_port_in_use(uint16_t port);
int tcp_socket_send(tcp_socket_t* sock, void* data, size_t len);
int tcp_socket_recv(tcp_socket_t* sock, void* buf, size_t max_len);
int tcp_socket_close(tcp_socket_t* sock);

/* Counters since boot, for diagnostics and tests. */
typedef struct { uint32_t retransmits, fast_retransmits, timeouts, out_of_order, dup_segments, resets_sent, resets_rcvd; } tcp_stats_t;
void tcp_get_stats(tcp_stats_t* out);

int tcp_checksum_ok(ip_hdr_t* ip, tcp_hdr_t* tcp, size_t seglen);   /* 1 = segment checksum valid */
void tcp_handle_packet(ip_hdr_t* ip, tcp_hdr_t* tcp, void* data, size_t len);
void tcp_handle_packet6(const ip6_addr_t* src, tcp_hdr_t* tcp, void* data, size_t len);

#endif /* TCP_H */
