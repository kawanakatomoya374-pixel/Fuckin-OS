/**
 * tcp.c - TCP Protocol Implementation (Simplified)
 */

#include "tcp.h"
#include "arp.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "task.h"

extern void net_poll(void);
/* Byte order conversion functions */
static uint16_t htons(uint16_t n);
static uint16_t ntohs(uint16_t n);
static uint32_t htonl(uint32_t n);
static uint32_t ntohl(uint32_t n);

static tcp_socket_t* tcp_sockets = NULL;
static uint64_t next_port = 1024;
/* NIC receive handling and future AP-owned transport workers can access the
 * socket registry concurrently. Keep this lock limited to registry/port
 * updates: no network poll, scheduler wait or TLS work is performed while it
 * is held. */
static volatile uint32_t tcp_registry_lock = 0;

static void tcp_registry_lock_acquire(void) {
    while (__atomic_exchange_n(&tcp_registry_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile("pause");
    }
}

static void tcp_registry_lock_release(void) {
    __atomic_store_n(&tcp_registry_lock, 0u, __ATOMIC_RELEASE);
}

void tcp_init(void) {
    tcp_sockets = NULL;
    next_port = 1024;
    __atomic_store_n(&tcp_registry_lock, 0u, __ATOMIC_RELEASE);
    serial_puts("[TCP] TCP initialized\n");
}

tcp_socket_t* tcp_socket_create(void) {
    tcp_socket_t* sock = (tcp_socket_t*)kmalloc(sizeof(tcp_socket_t));
    if (!sock) return NULL;
    
    memset(sock, 0, sizeof(tcp_socket_t));
    sock->state = TCP_STATE_CLOSED;
    sock->seq_num = get_timer_ticks();  // Initial sequence number
    
    sock->rx_buffer = (uint8_t*)kmalloc(TCP_WINDOW_SIZE);
    if (!sock->rx_buffer) {
        kfree(sock);
        return NULL;
    }
    sock->tx_buffer = (uint8_t*)kmalloc(TCP_WINDOW_SIZE);
    if (!sock->tx_buffer) {
        kfree(sock->rx_buffer);
        kfree(sock);
        return NULL;
    }
    sock->rx_max = TCP_WINDOW_SIZE;
    sock->tx_max = TCP_WINDOW_SIZE;
    
    tcp_registry_lock_acquire();
    sock->next = tcp_sockets;
    tcp_sockets = sock;
    tcp_registry_lock_release();
    
    return sock;
}

void tcp_socket_destroy(tcp_socket_t* sock) {
    if (!sock) return;
    
    /* Remove from the registry before freeing buffers. The packet handler
     * holds this same lock for lookup plus state mutation, so it cannot keep a
     * stale socket pointer across the free. */
    tcp_registry_lock_acquire();
    tcp_socket_t* prev = NULL;
    tcp_socket_t* curr = tcp_sockets;
    while (curr) {
        if (curr == sock) {
            if (prev) prev->next = curr->next;
            else tcp_sockets = curr->next;
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    tcp_registry_lock_release();
    
    if (sock->rx_buffer) kfree(sock->rx_buffer);
    if (sock->tx_buffer) kfree(sock->tx_buffer);
    kfree(sock);
}

int tcp_socket_bind(tcp_socket_t* sock, uint16_t port) {
    if (!sock) return -1;
    tcp_registry_lock_acquire();
    sock->local_port = port;
    tcp_registry_lock_release();
    return 0;
}

static void tcp_build_header(tcp_hdr_t* hdr, tcp_socket_t* sock, 
                             uint8_t flags, uint64_t payload_len) {
    hdr->src_port = htons(sock->local_port);
    hdr->dst_port = htons(sock->remote_port);
    hdr->seq_num = htonl((uint32_t)sock->seq_num);
    hdr->ack_num = htonl((uint32_t)sock->ack_num);
    hdr->data_off = (5 << 4);  // 5 * 4 = 20 bytes header
    hdr->flags = flags;
    hdr->window = htons((uint16_t)(sock->rx_max - sock->rx_len));
    hdr->checksum = 0;
    hdr->urgent = 0;
    
    /* SYN and FIN each consume one sequence number in addition to payload.
     * Without advancing after SYN, the first HTTP segment reuses the SYN
     * sequence number and the peer discards its initial byte ("GET" became
     * "ET" at the server). */
    sock->seq_num += payload_len + ((flags & (TCP_SYN | TCP_FIN)) ? 1u : 0u);
}

static uint64_t tcp_checksum(ip_addr_t* src, ip_addr_t* dst,
                             tcp_hdr_t* tcp, void* data, uint64_t len) {
    /* Pseudo header */
    uint32_t sum = 0;
    uint16_t* w;

    /* Source and destination IP (byte pairs) */
    for (int i = 0; i < 4; i += 2) {
        sum += ((uint32_t)src->addr[i] << 8) | src->addr[i + 1];
        sum += ((uint32_t)dst->addr[i] << 8) | dst->addr[i + 1];
    }

    /* Protocol and TCP length */
    sum += IP_PROTO_TCP;
    sum += (uint64_t)sizeof(tcp_hdr_t) + len;

    /* TCP header */
    w = (uint16_t*)tcp;
    for (size_t i = 0; i < sizeof(tcp_hdr_t) / 2; i++) {
        sum += ntohs(w[i]);
    }

    /* Payload */
    if (data && len > 0) {
        w = (uint16_t*)data;
        for (size_t i = 0; i < len / 2; i++) {
            sum += ntohs(w[i]);
        }
        if (len & 1) {
            sum += *((uint8_t*)data + len - 1) << 8;
        }
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum & 0xFFFF);
}

static int tcp_send_packet(tcp_socket_t* sock, uint8_t flags, 
                           void* data, uint64_t len) {
    uint8_t packet[sizeof(tcp_hdr_t) + TCP_MSS];
    tcp_hdr_t* hdr = (tcp_hdr_t*)packet;
    
    if (!sock) return -1;
    if (len > TCP_MSS) len = TCP_MSS;
    tcp_build_header(hdr, sock, flags, len);
    
    if (data && len > 0) {
        memcpy(packet + sizeof(tcp_hdr_t), data, len);
    }
    
    /* Calculate checksum */
    extern net_iface_t* net_get_default_iface(void);
    net_iface_t* iface = net_get_default_iface();
    if (!iface) return -1;
    
    /* tcp_checksum() returns a host-endian 16-bit value; the wire header
     * must contain its network-byte-order representation.  The previous
     * direct assignment produced invalid SYN checksums, so QEMU usernet
     * correctly discarded every connection attempt. */
    hdr->checksum = htons((uint16_t)tcp_checksum(&iface->ip,
                                                   &sock->remote_addr,
                                                   hdr, data, len));
    
    /* Send via IP */
    return ip_send(&sock->remote_addr, IP_PROTO_TCP, packet, sizeof(tcp_hdr_t) + len);
}

int tcp_socket_connect(tcp_socket_t* sock, ip_addr_t* addr, uint16_t port) {
    if (!sock || !addr) return -1;

    tcp_registry_lock_acquire();
    sock->remote_addr = *addr;
    sock->remote_port = port;
    sock->local_port = next_port++;
    if (next_port > 65535) next_port = 1024;
    sock->state = TCP_STATE_SYN_SENT;
    tcp_registry_lock_release();

    /* ip_send() initiates ARP and deliberately reports no transmission
     * until the gateway MAC is cached.  Retry the SYN after polling ARP;
     * preserve seq_num on each unsent attempt because tcp_build_header()
     * advances it for a SYN. */
    bool syn_sent = false;
    uint64_t deadline = get_timer_ticks() + 5000u;
    int established = 0;
    while (get_timer_ticks() < deadline) {
        tcp_registry_lock_acquire();
        established = (sock->state == TCP_STATE_ESTABLISHED);
        if (!established && !syn_sent) {
            uint64_t seq_before_send = sock->seq_num;
            serial_puts("[TCP] Sending SYN\n");
            if (tcp_send_packet(sock, TCP_SYN, NULL, 0) == 0) {
                syn_sent = true;
            } else {
                sock->seq_num = seq_before_send;
            }
        }
        tcp_registry_lock_release();
        if (established) break;
        net_poll();
        /* ARP/SYNまたは送信可能待ちでCPUを空回しせず、GUI・DNS・NIC
         * 処理へ実行権を譲る。次のpollで状態遷移を確認する。 */
        thread_yield();
    }

    tcp_registry_lock_acquire();
    established = (sock->state == TCP_STATE_ESTABLISHED);
    if (!established) sock->state = TCP_STATE_CLOSED;
    tcp_registry_lock_release();
    if (established) {
        serial_puts("[TCP] Connection established\n");
        return 0;
    }

    serial_puts("[TCP] Connection failed\n");
    return -1;
}

int tcp_socket_send(tcp_socket_t* sock, void* data, size_t len) {
    if (!sock || (len > 0 && !data)) return -1;
    if (len > TCP_MSS) len = TCP_MSS;  // Fragmentation not supported

    /* The first data record may immediately follow a SYN-ACK while ARP has
     * not yet committed its gateway entry. State and sequence updates are
     * serialized with the RX path, but the lock is released before net_poll
     * or yield so another connection can make forward progress. */
    uint64_t deadline = get_timer_ticks() + 1500u;
    while (get_timer_ticks() < deadline) {
        tcp_registry_lock_acquire();
        int established = (sock->state == TCP_STATE_ESTABLISHED);
        uint64_t seq_before_send = sock->seq_num;
        int sent = established && tcp_send_packet(sock, TCP_ACK | TCP_PSH, data, len) == 0;
        if (!sent) sock->seq_num = seq_before_send;
        tcp_registry_lock_release();
        if (sent) return (int)len;
        if (!established) break;
        net_poll();
        thread_yield();
    }
    serial_puts("[TCP] Data send failed\n");
    return -1;
}

int tcp_socket_recv(tcp_socket_t* sock, void* buf, size_t max_len) {
    if (!sock || !buf || max_len == 0 || !sock->rx_buffer) return -1;

    /* Pull fresh packets from the NIC before checking receive buffers. */
    net_poll();

    /* A peer may send its final HTTP data segment and FIN in the same NIC
     * poll. Preserve buffered data while in CLOSE_WAIT; only report EOF after
     * the buffer is drained. Take the same short lock as tcp_handle_packet()
     * before inspecting or moving the RX ring. */
    tcp_registry_lock_acquire();
    if (sock->state != TCP_STATE_ESTABLISHED && sock->state != TCP_STATE_CLOSE_WAIT) {
        tcp_registry_lock_release();
        return -1;
    }
    if (sock->rx_len == 0) {
        int eof = sock->state == TCP_STATE_CLOSE_WAIT;
        tcp_registry_lock_release();
        return eof ? -1 : 0;
    }

    uint64_t to_copy = sock->rx_len;
    if (to_copy > max_len) to_copy = max_len;

    memcpy(buf, sock->rx_buffer, to_copy);

    /* Move remaining data */
    if (sock->rx_len > to_copy) {
        memmove(sock->rx_buffer, sock->rx_buffer + to_copy,
                sock->rx_len - to_copy);
    }
    sock->rx_len -= to_copy;

    /* Send ACK while sequence state is still protected. */
    tcp_send_packet(sock, TCP_ACK, NULL, 0);
    tcp_registry_lock_release();
    return to_copy;
}

int tcp_socket_close(tcp_socket_t* sock) {
    if (!sock) return -1;

    tcp_registry_lock_acquire();
    int established = sock->state == TCP_STATE_ESTABLISHED;
    if (established) {
        sock->state = TCP_STATE_FIN_WAIT1;
        tcp_send_packet(sock, TCP_FIN | TCP_ACK, NULL, 0);
    }
    tcp_registry_lock_release();

    if (established) {
        /* Wait for FIN-ACK while servicing the network without consuming a
         * full CPU in a delay loop. */
        uint64_t deadline = get_timer_ticks() + 500u;
        for (;;) {
            tcp_registry_lock_acquire();
            int closed = sock->state == TCP_STATE_CLOSED;
            tcp_registry_lock_release();
            if (closed || get_timer_ticks() >= deadline) break;
            net_poll();
            thread_yield();
        }
    }

    tcp_registry_lock_acquire();
    sock->state = TCP_STATE_CLOSED;
    tcp_registry_lock_release();
    return 0;
}

void tcp_handle_packet(ip_hdr_t* ip, tcp_hdr_t* tcp, void* data, size_t len) {
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t seq = ntohl(tcp->seq_num);
    uint8_t flags = tcp->flags;
    
    /* Hold the registry lock through lookup and short state mutation. This
     * prevents tcp_socket_destroy() from freeing the selected socket while an
     * RX path is appending data or sending its ACK. */
    tcp_registry_lock_acquire();
    tcp_socket_t* sock = tcp_sockets;
    while (sock) {
        if (sock->local_port == dst_port &&
            sock->remote_port == src_port &&
            ip_addr_cmp(&sock->remote_addr, 
                       &(ip_addr_t){.addr = {(uint8_t)(ip->src_addr),
                                             (uint8_t)(ip->src_addr >> 8),
                                             (uint8_t)(ip->src_addr >> 16),
                                             (uint8_t)(ip->src_addr >> 24)}}) == 0) {
            break;
        }
        sock = sock->next;
    }
    
    if (!sock) {
        /* Unexpected after a completed TLS handshake. Keep a concise marker so
         * transport diagnostics can distinguish peer data loss from a socket
         * tuple matching failure. */
#if COS_HTTP_RUNTIME_SMOKE
        serial_puts("[TCP] RX unmatched tuple src=");
        serial_putdec(src_port);
        serial_puts(" dst=");
        serial_putdec(dst_port);
        serial_puts(" flags=");
        serial_putdec(flags);
        serial_puts(" len=");
        serial_putdec(len);
        serial_puts("\n");
#endif
        tcp_registry_lock_release();
        return;
    }
    
    switch (sock->state) {
        case TCP_STATE_SYN_SENT:
            if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
                /* SYN-ACK received */
                sock->ack_num = seq + 1;
                sock->remote_seq = seq;
                sock->state = TCP_STATE_ESTABLISHED;
                
                /* Send ACK */
                tcp_send_packet(sock, TCP_ACK, NULL, 0);
            }
            break;
            
        case TCP_STATE_ESTABLISHED:
            /* Internet peers retransmit SYN-ACK when the first pure ACK is
             * lost around ARP/NIC bring-up. Re-acknowledge it explicitly;
             * treating it as a zero-length ordinary ACK left ack_num one byte
             * short and forced the peer to wait for its multi-second retry. */
            if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                sock->ack_num = seq + 1u;
                sock->remote_seq = seq;
                (void)tcp_send_packet(sock, TCP_ACK, NULL, 0);
                break;
            }
            if (flags & TCP_ACK) {
                sock->ack_num = seq + (uint32_t)len;
                sock->remote_seq = seq;
                
                /* Process data */
                if (len > 0 && data) {
                    if (sock->rx_len + len < sock->rx_max) {
#if COS_HTTP_RUNTIME_SMOKE
                        serial_puts("[TCP] RX payload len=");
                        serial_putdec(len);
                        serial_puts(" buffered_before=");
                        serial_putdec(sock->rx_len);
                        serial_puts("\n");
#endif
                        memcpy(sock->rx_buffer + sock->rx_len, data, len);
                        sock->rx_len += len;
                        if (tcp_send_packet(sock, TCP_ACK, NULL, 0) != 0) {
                            serial_puts("[TCP] RX ACK transmit failed\n");
                        }
                    } else {
                        serial_puts("[TCP] RX buffer full; dropping payload len=");
                        serial_putdec(len);
                        serial_puts(" buffered=");
                        serial_putdec(sock->rx_len);
                        serial_puts("\n");
                    }
                }
                
                if (flags & TCP_FIN) {
                    /* Remote closed connection */
                    sock->ack_num++;
                    tcp_send_packet(sock, TCP_ACK, NULL, 0);
                    sock->state = TCP_STATE_CLOSE_WAIT;
                }
            }
            break;
            
        case TCP_STATE_FIN_WAIT1:
            if (flags & TCP_ACK) {
                sock->state = TCP_STATE_FIN_WAIT2;
            }
            if (flags & TCP_FIN) {
                sock->ack_num++;
                tcp_send_packet(sock, TCP_ACK, NULL, 0);
                sock->state = TCP_STATE_TIME_WAIT;
            }
            break;
            
        case TCP_STATE_FIN_WAIT2:
            if (flags & TCP_FIN) {
                sock->ack_num++;
                tcp_send_packet(sock, TCP_ACK, NULL, 0);
                sock->state = TCP_STATE_CLOSED;
            }
            break;
            
        default:
            break;
    }
    tcp_registry_lock_release();
}

void tcp_poll(void) {
    /* Poll would be called by network stack to process incoming packets */
}

/* Byte order conversion implementations */
static uint16_t htons(uint16_t n) {
    return ((n & 0xFF) << 8) | ((n >> 8) & 0xFF);
}

static uint16_t ntohs(uint16_t n) {
    return htons(n);
}

static uint32_t htonl(uint32_t n) {
    return ((n & 0xFF) << 24) |
           ((n & 0xFF00) << 8) |
           ((n >> 8) & 0xFF00) |
           ((n >> 24) & 0xFF);
}

static uint32_t ntohl(uint32_t n) {
    return htonl(n);
}
