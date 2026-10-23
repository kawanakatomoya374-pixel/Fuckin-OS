/**
 * tcp.c - TCP Protocol Implementation (Simplified)
 */

#include "sync.h"
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

static void tcp_registry_lock_acquire(void) { cos_spin_lock(&tcp_registry_lock); }

static void tcp_registry_lock_release(void) { cos_spin_unlock(&tcp_registry_lock); }

void tcp_init(void) {
    tcp_sockets = NULL;
    next_port = 1024;
    __atomic_store_n(&tcp_registry_lock, 0u, __ATOMIC_RELEASE);
    serial_puts("[TCP] TCP initialized\n");
}

/* Allocates a socket with its buffers but does not register it; the caller links it under the registry lock. */
static tcp_socket_t* tcp_alloc(void) {
    tcp_socket_t* sock = (tcp_socket_t*)kmalloc(sizeof(tcp_socket_t));
    if (!sock) return NULL;
    memset(sock, 0, sizeof(tcp_socket_t));
    sock->state = TCP_STATE_CLOSED;
    sock->seq_num = net_rand32();
    sock->rx_buffer = (uint8_t*)kmalloc(TCP_WINDOW_SIZE);
    if (!sock->rx_buffer) { kfree(sock); return NULL; }
    sock->tx_buffer = (uint8_t*)kmalloc(TCP_WINDOW_SIZE);
    if (!sock->tx_buffer) { kfree(sock->rx_buffer); kfree(sock); return NULL; }
    sock->rx_max = TCP_WINDOW_SIZE;
    sock->tx_max = TCP_WINDOW_SIZE;
    return sock;
}

tcp_socket_t* tcp_socket_create(void) {
    tcp_socket_t* sock = tcp_alloc();
    if (!sock) return NULL;
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
    for (tcp_socket_t* c = tcp_sockets; c; c = c->next)          /* a listener's unaccepted connections go with it */
        if (c->parent == sock) { c->parent = NULL; c->state = TCP_STATE_CLOSED; }
    tcp_registry_lock_release();
    
    if (sock->rx_buffer) kfree(sock->rx_buffer);
    if (sock->tx_buffer) kfree(sock->tx_buffer);
    kfree(sock);
}

/* A port is taken while any live socket (listener or connection that is not closed) uses it. */
static bool tcp_port_used_locked(uint16_t port, const tcp_socket_t* except) {
    for (tcp_socket_t* o = tcp_sockets; o; o = o->next)
        if (o != except && o->local_port == port && (o->state != TCP_STATE_CLOSED || o->bound) && !o->unaccepted && !o->detached) return true;
    return false;
}

int tcp_port_in_use(uint16_t port) {
    tcp_registry_lock_acquire();
    bool used = tcp_port_used_locked(port, NULL);
    tcp_registry_lock_release();
    return used;
}

/* Ephemeral ports are chosen at random so a blind attacker cannot predict the next connection's port. */
static uint16_t tcp_pick_port_locked(const tcp_socket_t* sock) {
    for (int i = 0; i < 128; i++) {
        uint16_t p = (uint16_t)(49152u + net_rand32() % 16384u);
        if (!tcp_port_used_locked(p, sock)) return p;
    }
    return 0;
}

int tcp_socket_bind(tcp_socket_t* sock, uint16_t port) {
    if (!sock) return -1;
    tcp_registry_lock_acquire();
    int rc = 0;
    if (port == 0) { port = tcp_pick_port_locked(sock); if (!port) rc = -1; }
    else if (tcp_port_used_locked(port, sock)) rc = -1;
    if (rc == 0) { sock->local_port = port; sock->bound = 1; }
    tcp_registry_lock_release();
    return rc;
}

int tcp_socket_listen(tcp_socket_t* sock, int backlog) {
    if (!sock) return -1;
    tcp_registry_lock_acquire();
    int rc = -1;
    if (sock->local_port && sock->state == TCP_STATE_CLOSED) {
        sock->backlog = (uint8_t)(backlog < 1 ? 1 : backlog > 8 ? 8 : backlog);
        sock->state = TCP_STATE_LISTEN;
        rc = 0;
    }
    tcp_registry_lock_release();
    return rc;
}


static tcp_stats_t g_stats;
void tcp_get_stats(tcp_stats_t* out) { if (out) *out = g_stats; }

/* ------------------------------------------------------------------ wire helpers */
/* Who a segment goes to / comes from: an IPv4 or an IPv6 host. */
typedef struct { uint8_t v6; ip_addr_t a4; ip6_addr_t a6; } tcp_peer_t;

static bool peer_eq_sock(const tcp_peer_t* p, const tcp_socket_t* s) {
    if (p->v6 != s->v6) return false;
    return p->v6 ? ip6_same(&p->a6, &s->remote6) : ip_addr_cmp((ip_addr_t*)&p->a4, (ip_addr_t*)&s->remote_addr) == 0;
}
static void sock_peer(const tcp_socket_t* s, tcp_peer_t* p) { p->v6 = s->v6; p->a4 = s->remote_addr; p->a6 = s->remote6; }

static uint16_t tcp_csum(const uint8_t* src_ip, const uint8_t* dst_ip, const void* seg, size_t seglen) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i += 2) {
        sum += ((uint32_t)src_ip[i] << 8) | src_ip[i + 1];
        sum += ((uint32_t)dst_ip[i] << 8) | dst_ip[i + 1];
    }
    sum += IP_PROTO_TCP;
    sum += (uint32_t)seglen;
    const uint8_t* b = (const uint8_t*)seg;
    for (size_t i = 0; i + 1 < seglen; i += 2) sum += ((uint32_t)b[i] << 8) | b[i + 1];
    if (seglen & 1u) sum += (uint32_t)b[seglen - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

/* A received segment is valid when the checksum over pseudo header + segment folds to zero. */
int tcp_checksum_ok(ip_hdr_t* ip, tcp_hdr_t* tcp, size_t seglen) {
    uint8_t src[4], dst[4];
    memcpy(src, &ip->src_addr, 4);
    memcpy(dst, &ip->dst_addr, 4);
    return tcp_csum(src, dst, tcp, seglen) == 0;
}

#define TCP6_MSS 1440
#define TCP_MAX_UNACCEPTED 64      /* half-open + not-yet-accepted connections the whole stack will hold */

/* Builds and sends one segment. seq/ack are explicit so retransmission never disturbs socket state. */
static int tcp_xmit_raw(const tcp_peer_t* peer, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                        uint8_t flags, uint16_t window, const void* data, size_t len, int mss_opt) {
    extern net_iface_t* net_get_default_iface(void);
    net_iface_t* iface = net_get_default_iface();
    if (!iface || len > TCP_MSS) return -1;
    uint8_t packet[sizeof(tcp_hdr_t) + 4 + TCP_MSS];
    tcp_hdr_t* hdr = (tcp_hdr_t*)packet;
    size_t hlen = sizeof(tcp_hdr_t) + (mss_opt ? 4u : 0u);
    hdr->src_port = htons(sport);
    hdr->dst_port = htons(dport);
    hdr->seq_num = htonl(seq);
    hdr->ack_num = htonl(ack);
    hdr->data_off = (uint8_t)((hlen / 4u) << 4);
    hdr->flags = flags;
    hdr->window = htons(window);
    hdr->checksum = 0;
    hdr->urgent = 0;
    if (mss_opt) {
        uint16_t mss = peer->v6 ? TCP6_MSS : TCP_MSS;
        packet[20] = 2; packet[21] = 4;                 /* kind 2 (MSS), length 4 */
        packet[22] = (uint8_t)(mss >> 8); packet[23] = (uint8_t)(mss & 0xFF);
    }
    if (data && len > 0) memcpy(packet + hlen, data, len);
    if (peer->v6) {
        if (len > TCP6_MSS) return -1;
        ip6_addr_t src = ip6_select_src(&peer->a6);
        hdr->checksum = htons(ip6_checksum(&src, &peer->a6, 6, packet, hlen + len));
        return ip6_send(&peer->a6, 6, packet, hlen + len);
    }
    hdr->checksum = htons(tcp_csum(peer->a4.addr[0] == 127 ? peer->a4.addr : iface->ip.addr, peer->a4.addr, packet, hlen + len));   /* 127.x: both ends are this host, the source is the destination's address */
    return ip_send((ip_addr_t*)&peer->a4, IP_PROTO_TCP, packet, hlen + len);
}

static uint16_t tcp_rx_window(const tcp_socket_t* s) {
    uint64_t w = s->rx_max > s->rx_len ? s->rx_max - s->rx_len : 0;
    return (uint16_t)(w > 65535u ? 65535u : w);
}

static int tcp_xmit(tcp_socket_t* s, uint8_t flags, uint32_t seq, const void* data, size_t len, int mss_opt) {
    tcp_peer_t peer; sock_peer(s, &peer);
    return tcp_xmit_raw(&peer, s->local_port, s->remote_port, seq, s->rcv_nxt,
                        flags, tcp_rx_window(s), data, len, mss_opt);
}

static void tcp_ack_now(tcp_socket_t* s) { (void)tcp_xmit(s, TCP_ACK, s->snd_una + (uint32_t)s->tx_sent + s->fin_sent, NULL, 0, 0); }

/* ------------------------------------------------------------------ sending */
static uint32_t tcp_mss(const tcp_socket_t* s) { return s->peer_mss ? s->peer_mss : 536u; }

static bool tcp_pending(const tcp_socket_t* s) {
    return s->tx_len > 0 || (s->fin_queued && !s->reset_seen) || s->state == TCP_STATE_SYN_SENT ||
           s->state == TCP_STATE_SYN_RECEIVED || s->fin_sent;
}

static void tcp_arm(tcp_socket_t* s) {
    if (!s->rtx_at && tcp_pending(s)) s->rtx_at = get_timer_ticks() + s->rto;
}

/* Copies n bytes (n <= tx_max - tx_len) behind the queued data, sliding the queue back to the start of the buffer only when
 * the tail has run out of room. */
static void tcp_tx_append(tcp_socket_t* s, const void* data, size_t n) {
    if (s->tx_off + s->tx_len + n > s->tx_max) {
        memmove(s->tx_buffer, s->tx_buffer + s->tx_off, (size_t)s->tx_len);
        s->tx_off = 0;
    }
    memcpy(s->tx_buffer + s->tx_off + s->tx_len, data, n);
    s->tx_len += n;
}

/* Sends whatever the congestion and peer windows allow. tx_buffer[0] is snd_una; tx_sent bytes of it are in flight. */
static void tcp_output(tcp_socket_t* s) {
    if (s->state != TCP_STATE_ESTABLISHED && s->state != TCP_STATE_CLOSE_WAIT && s->state != TCP_STATE_FIN_WAIT1 &&
        s->state != TCP_STATE_CLOSING && s->state != TCP_STATE_LAST_ACK) return;
    uint32_t wnd = s->cwnd < s->peer_wnd ? s->cwnd : s->peer_wnd;
    if (wnd == 0) wnd = 1;                       /* zero-window probe: one byte rides on the retransmit timer */
    uint32_t mss = tcp_mss(s);
    while (s->tx_sent < s->tx_len) {
        if (s->tx_sent >= wnd) break;
        uint64_t n = s->tx_len - s->tx_sent;
        if (n > mss) n = mss;
        if (n > wnd - s->tx_sent) n = wnd - s->tx_sent;
        if (n == 0) break;
        uint32_t seq = s->snd_una + (uint32_t)s->tx_sent;
        if (tcp_xmit(s, TCP_ACK | TCP_PSH, seq, s->tx_buffer + s->tx_off + s->tx_sent, (size_t)n, 0) != 0) break;   /* ARP pending: timer retries */
        if (!s->rtx_count && !s->rtt_start) { s->rtt_seq = seq + (uint32_t)n; s->rtt_start = get_timer_ticks(); }
        s->tx_sent += n;
        uint32_t end = s->snd_una + (uint32_t)s->tx_sent;
        if ((int32_t)(end - s->snd_max) > 0) s->snd_max = end;
    }
    if (s->fin_queued && !s->fin_sent && s->tx_sent == s->tx_len) {
        uint32_t seq = s->snd_una + (uint32_t)s->tx_len;
        if (tcp_xmit(s, TCP_FIN | TCP_ACK, seq, NULL, 0, 0) == 0) {
            s->fin_sent = 1;
            if ((int32_t)(seq + 1u - s->snd_max) > 0) s->snd_max = seq + 1u;
        }
    }
    s->snd_nxt = s->snd_una + (uint32_t)s->tx_sent + s->fin_sent;
    s->seq_num = s->snd_nxt;
    tcp_arm(s);
}

/* Go-back-N from snd_una: everything unacknowledged is queued for sending again. */
static void tcp_rewind(tcp_socket_t* s) {
    s->tx_sent = 0;
    s->fin_sent = 0;
    s->rtt_start = 0;                            /* Karn: no RTT sample from retransmitted data */
}

static void tcp_enter_time_wait(tcp_socket_t* s) {
    s->state = TCP_STATE_TIME_WAIT;
    s->rtx_at = 0;
    s->time_wait_until = get_timer_ticks() + 2000u;
}

static void tcp_update_rtt(tcp_socket_t* s, uint32_t sample) {
    if (s->srtt == 0) { s->srtt = sample; s->rttvar = sample / 2; }
    else {
        uint32_t diff = sample > s->srtt ? sample - s->srtt : s->srtt - sample;
        s->rttvar = (3u * s->rttvar + diff) / 4u;
        s->srtt = (7u * s->srtt + sample) / 8u;
    }
    uint32_t rto = s->srtt + 4u * s->rttvar;
    if (rto < 200u) rto = 200u;
    if (rto > 16000u) rto = 16000u;
    s->rto = rto;
}

static void tcp_process_ack(tcp_socket_t* s, uint32_t ack, uint16_t win, bool pure) {
    int32_t adv = (int32_t)(ack - s->snd_una);
    int32_t outstanding = (int32_t)(s->snd_max - s->snd_una);
    if (adv < 0 || adv > outstanding) return;     /* ack of something we never sent, or old */
    uint32_t prev_wnd = s->peer_wnd;
    s->peer_wnd = win;
    uint32_t mss = tcp_mss(s);
    if (adv == 0) {
        if (pure && outstanding > 0 && win == prev_wnd) {
            if (++s->dup_acks == 3) {              /* fast retransmit */
                g_stats.fast_retransmits++;
                uint32_t flight = (uint32_t)outstanding;
                s->ssthresh = flight / 2u > 2u * mss ? flight / 2u : 2u * mss;
                s->cwnd = s->ssthresh;
                tcp_rewind(s);
                tcp_output(s);
            }
        } else if (win > prev_wnd) {
            tcp_output(s);                         /* window opened */
        }
        return;
    }
    s->dup_acks = 0;
    uint32_t data_acked = (uint32_t)adv < s->tx_len ? (uint32_t)adv : (uint32_t)s->tx_len;
    bool fin_acked = s->fin_sent && (uint32_t)adv > data_acked;
    if (s->rtt_start && (int32_t)(ack - s->rtt_seq) >= 0) {
        tcp_update_rtt(s, (uint32_t)(get_timer_ticks() - s->rtt_start));
        s->rtt_start = 0;
    }
    s->rtx_count = 0;
    if (data_acked) {
        s->tx_len -= data_acked;
        s->tx_off = s->tx_len ? s->tx_off + data_acked : 0;          /* O(1): compaction happens lazily in tcp_tx_append() */
        s->tx_sent = s->tx_sent > data_acked ? s->tx_sent - data_acked : 0;
        s->snd_una += data_acked;
        if (s->cwnd < s->ssthresh) s->cwnd += data_acked < mss ? data_acked : mss;
        else s->cwnd += (mss * mss) / (s->cwnd ? s->cwnd : 1u) + 1u;
        if (s->cwnd > 65535u) s->cwnd = 65535u;
    }
    if (fin_acked) {
        s->snd_una += 1u;
        s->fin_queued = s->fin_sent = 0;
        if (s->state == TCP_STATE_FIN_WAIT1) s->state = TCP_STATE_FIN_WAIT2;
        else if (s->state == TCP_STATE_CLOSING) tcp_enter_time_wait(s);
        else if (s->state == TCP_STATE_LAST_ACK) s->state = TCP_STATE_CLOSED;
    }
    s->rtx_at = 0;
    tcp_output(s);
}

/* ------------------------------------------------------------------ API */
static int tcp_connect_common(tcp_socket_t* sock, const tcp_peer_t* peer, uint16_t port);

int tcp_socket_connect(tcp_socket_t* sock, ip_addr_t* addr, uint16_t port) {
    if (!sock || !addr) return -1;
    tcp_peer_t p; memset(&p, 0, sizeof p); p.a4 = *addr;
    return tcp_connect_common(sock, &p, port);
}

int tcp_socket_connect6(tcp_socket_t* sock, const ip6_addr_t* addr, uint16_t port) {
    if (!sock || !addr) return -1;
    tcp_peer_t p; memset(&p, 0, sizeof p); p.v6 = 1; p.a6 = *addr;
    return tcp_connect_common(sock, &p, port);
}

static int tcp_connect_common(tcp_socket_t* sock, const tcp_peer_t* peer, uint16_t port) {
    uint32_t iss = net_rand32();
    tcp_registry_lock_acquire();
    sock->v6 = peer->v6;
    sock->remote_addr = peer->a4;
    sock->remote6 = peer->a6;
    sock->remote_port = port;
    if (sock->local_port == 0 || tcp_port_used_locked(sock->local_port, sock)) sock->local_port = tcp_pick_port_locked(sock);
    if (sock->local_port == 0) { tcp_registry_lock_release(); return -1; }
    sock->snd_una = iss; sock->snd_max = iss + 1u; sock->snd_nxt = iss + 1u;
    sock->seq_num = sock->snd_nxt;
    sock->rcv_nxt = 0;
    sock->tx_len = sock->tx_sent = sock->rx_len = 0; sock->tx_off = 0;
    sock->ooo_len = 0;
    sock->fin_queued = sock->fin_sent = sock->reset_seen = 0;
    sock->rto = 1000u; sock->srtt = sock->rttvar = 0;
    sock->peer_mss = 536u; sock->peer_wnd = 65535u;
    sock->cwnd = 4u * TCP_MSS; sock->ssthresh = 65535u;
    sock->rtx_count = sock->dup_acks = 0; sock->rtx_at = 0; sock->rtt_start = 0;
    sock->state = TCP_STATE_SYN_SENT;
    tcp_registry_lock_release();

    /* ip_send() reports no transmission while the gateway MAC is unresolved (it fires the ARP request itself).
     * Keep trying the first SYN until it leaves; after that the retransmission timer (tcp_poll) owns the SYN. */
    bool syn_out = false;
    uint64_t deadline = get_timer_ticks() + 12000u;
    int established = 0;
    while (get_timer_ticks() < deadline) {
        tcp_registry_lock_acquire();
        established = (sock->state == TCP_STATE_ESTABLISHED);
        int closed = (sock->state == TCP_STATE_CLOSED);
        if (!established && !closed && !syn_out) {
            if (tcp_xmit(sock, TCP_SYN, sock->snd_una, NULL, 0, 1) == 0) {
                syn_out = true;
                sock->rtt_start = get_timer_ticks(); sock->rtt_seq = sock->snd_una + 1u;
                sock->rtx_at = get_timer_ticks() + sock->rto;
            }
        }
        tcp_registry_lock_release();
        if (established || closed) break;
        net_poll();
        thread_yield();
    }

    tcp_registry_lock_acquire();
    established = (sock->state == TCP_STATE_ESTABLISHED);
    if (!established) { sock->state = TCP_STATE_CLOSED; sock->rtx_at = 0; }
    tcp_registry_lock_release();
    if (established) {
        serial_puts("[TCP] Connection established\n");
        return 0;
    }
    serial_puts("[TCP] Connection failed\n");
    return -1;
}

/* Resets the connection state and sends the first SYN; the retransmission timer owns the handshake from here. */
static int tcp_connect_start_common(tcp_socket_t* sock, const ip_addr_t* addr, const ip6_addr_t* addr6, uint16_t port) {
    if (!sock || (!addr && !addr6)) return -1;
    uint32_t iss = net_rand32();
    tcp_registry_lock_acquire();
    if (sock->state != TCP_STATE_CLOSED) { tcp_registry_lock_release(); return -1; }
    sock->v6 = addr6 != NULL;
    if (addr6) { memset(&sock->remote_addr, 0, sizeof sock->remote_addr); sock->remote6 = *addr6; }
    else { sock->remote_addr = *addr; memset(&sock->remote6, 0, sizeof sock->remote6); }
    sock->remote_port = port;
    if (sock->local_port == 0 || tcp_port_used_locked(sock->local_port, sock)) sock->local_port = tcp_pick_port_locked(sock);
    if (sock->local_port == 0) { tcp_registry_lock_release(); return -1; }
    sock->snd_una = iss; sock->snd_max = iss + 1u; sock->snd_nxt = iss + 1u;
    sock->seq_num = sock->snd_nxt;
    sock->rcv_nxt = 0;
    sock->tx_len = sock->tx_sent = sock->rx_len = 0; sock->tx_off = 0;
    sock->ooo_len = 0;
    sock->fin_queued = sock->fin_sent = sock->reset_seen = 0;
    sock->rto = 1000u; sock->srtt = sock->rttvar = 0;
    sock->peer_mss = 536u; sock->peer_wnd = 65535u;
    sock->cwnd = 4u * TCP_MSS; sock->ssthresh = 65535u;
    sock->rtx_count = sock->dup_acks = 0; sock->rtt_start = 0;
    sock->state = TCP_STATE_SYN_SENT;
    uint64_t now = get_timer_ticks();
    if (tcp_xmit(sock, TCP_SYN, sock->snd_una, NULL, 0, 1) == 0) {      /* ARP unresolved: the retry below covers it */
        sock->rtt_start = now; sock->rtt_seq = sock->snd_una + 1u;
        sock->rtx_at = now + sock->rto;
    } else {
        sock->rtx_at = now + 100u;
    }
    tcp_registry_lock_release();
    return 0;
}
int tcp_socket_connect_start(tcp_socket_t* sock, const ip_addr_t* addr, uint16_t port) { return addr ? tcp_connect_start_common(sock, addr, NULL, port) : -1; }
int tcp_socket_connect_start6(tcp_socket_t* sock, const ip6_addr_t* addr, uint16_t port) { return addr ? tcp_connect_start_common(sock, NULL, addr, port) : -1; }
/* shutdown(SHUT_WR): queue the FIN behind the pending data and keep reading until the peer closes too */
int tcp_socket_shutdown_wr(tcp_socket_t* sock) {
    if (!sock) return -1;
    tcp_registry_lock_acquire();
    if (sock->state == TCP_STATE_ESTABLISHED || sock->state == TCP_STATE_CLOSE_WAIT) {
        sock->state = sock->state == TCP_STATE_ESTABLISHED ? TCP_STATE_FIN_WAIT1 : TCP_STATE_LAST_ACK;
        sock->fin_queued = 1;
        tcp_output(sock);
    }
    tcp_registry_lock_release();
    return 0;
}

int tcp_socket_send_nb(tcp_socket_t* sock, const void* data, size_t len) {
    if (!sock || (len > 0 && !data)) return -1;
    tcp_registry_lock_acquire();
    if (sock->state != TCP_STATE_ESTABLISHED && sock->state != TCP_STATE_CLOSE_WAIT) {
        tcp_registry_lock_release();
        return -1;
    }
    uint64_t space = sock->tx_max - sock->tx_len;
    size_t n = len < space ? len : (size_t)space;
    if (n) {
        tcp_tx_append(sock, data, n);
        tcp_output(sock);
    }
    tcp_registry_lock_release();
    return (int)n;
}

int tcp_socket_state(tcp_socket_t* sock) {
    if (!sock) return TCP_STATE_CLOSED;
    tcp_registry_lock_acquire();
    int st = sock->state;
    tcp_registry_lock_release();
    return st;
}

size_t tcp_socket_rx_ready(tcp_socket_t* sock) {
    if (!sock) return 0;
    tcp_registry_lock_acquire();
    size_t n = (size_t)sock->rx_len;
    tcp_registry_lock_release();
    return n;
}

size_t tcp_socket_tx_space(tcp_socket_t* sock) {
    if (!sock) return 0;
    tcp_registry_lock_acquire();
    size_t n = (sock->state == TCP_STATE_ESTABLISHED || sock->state == TCP_STATE_CLOSE_WAIT) ? (size_t)(sock->tx_max - sock->tx_len) : 0;
    tcp_registry_lock_release();
    return n;
}

int tcp_socket_was_reset(tcp_socket_t* sock) {
    if (!sock) return 0;
    tcp_registry_lock_acquire();
    int r = sock->reset_seen;
    tcp_registry_lock_release();
    return r;
}

/* Accept queue: children of this listener that finished the handshake and nobody has taken yet. */
static tcp_socket_t* tcp_find_ready_child_locked(tcp_socket_t* l) {
    for (tcp_socket_t* c = tcp_sockets; c; c = c->next)
        if (c->parent == l && c->unaccepted && c->state != TCP_STATE_SYN_RECEIVED && c->state != TCP_STATE_LISTEN) return c;
    return NULL;
}

int tcp_socket_accept_ready(tcp_socket_t* l) {
    if (!l) return 0;
    int n = 0;
    tcp_registry_lock_acquire();
    for (tcp_socket_t* c = tcp_sockets; c; c = c->next)
        if (c->parent == l && c->unaccepted && c->state != TCP_STATE_SYN_RECEIVED && c->state != TCP_STATE_CLOSED) n++;
    tcp_registry_lock_release();
    return n;
}

tcp_socket_t* tcp_socket_accept(tcp_socket_t* l) {
    if (!l) return NULL;
    tcp_registry_lock_acquire();
    tcp_socket_t* c = l->state == TCP_STATE_LISTEN ? tcp_find_ready_child_locked(l) : NULL;
    if (c && c->state == TCP_STATE_CLOSED) {      /* reset before anyone accepted it: just discard */
        c->parent = NULL; c->unaccepted = 0; c->detached = 1; c->detach_deadline = 1;
        c = NULL;
    }
    if (c) { c->parent = NULL; c->unaccepted = 0; }
    tcp_registry_lock_release();
    return c;
}

void tcp_socket_abandon(tcp_socket_t* sock) {
    if (!sock) return;
    tcp_registry_lock_acquire();
    if (sock->state == TCP_STATE_ESTABLISHED || sock->state == TCP_STATE_CLOSE_WAIT) {
        sock->state = sock->state == TCP_STATE_ESTABLISHED ? TCP_STATE_FIN_WAIT1 : TCP_STATE_LAST_ACK;
        sock->fin_queued = 1;
        tcp_output(sock);
    } else if (sock->state == TCP_STATE_LISTEN) {
        for (tcp_socket_t* c = tcp_sockets; c; c = c->next)     /* connections nobody accepted go with the listener */
            if (c->parent == sock) { c->parent = NULL; c->unaccepted = 0; c->state = TCP_STATE_CLOSED; c->detached = 1; c->detach_deadline = 1; }
        sock->state = TCP_STATE_CLOSED;
    } else if (sock->state == TCP_STATE_SYN_SENT || sock->state == TCP_STATE_SYN_RECEIVED) {
        sock->state = TCP_STATE_CLOSED;
    }
    sock->rtx_at = sock->rtx_at ? sock->rtx_at : 0;
    sock->detached = 1;
    sock->detach_deadline = get_timer_ticks() + 30000u;       /* a peer that never finishes the close cannot pin the socket */
    tcp_registry_lock_release();
}

int tcp_socket_send(tcp_socket_t* sock, void* data, size_t len) {
    if (!sock || (len > 0 && !data)) return -1;
    if (len == 0) return 0;

    /* Data is copied into the send queue and transmitted as the windows allow; it stays queued until the peer
     * acknowledges it, so a lost segment is retransmitted by tcp_poll() instead of being lost for good. */
    uint64_t deadline = get_timer_ticks() + 3000u;
    while (get_timer_ticks() < deadline) {
        tcp_registry_lock_acquire();
        if (sock->state != TCP_STATE_ESTABLISHED && sock->state != TCP_STATE_CLOSE_WAIT) {
            tcp_registry_lock_release();
            return -1;
        }
        uint64_t space = sock->tx_max - sock->tx_len;
        if (space > 0) {
            size_t n = len < space ? len : (size_t)space;
            tcp_tx_append(sock, data, n);
            tcp_output(sock);
            tcp_registry_lock_release();
            return (int)n;
        }
        tcp_registry_lock_release();
        net_poll();
        thread_yield();
    }
    serial_puts("[TCP] Data send failed (send queue full)\n");
    return -1;
}

int tcp_socket_recv(tcp_socket_t* sock, void* buf, size_t max_len) {
    if (!sock || !buf || max_len == 0 || !sock->rx_buffer) return -1;

    /* Pull fresh packets from the NIC before checking receive buffers. */
    net_poll();

    tcp_registry_lock_acquire();
    if (sock->rx_len == 0) {
        /* Buffered data is always delivered first (a peer may send its last segment and FIN together or reset
         * afterwards); only an empty buffer reports EOF / error. */
        int open = sock->state == TCP_STATE_ESTABLISHED || sock->state == TCP_STATE_SYN_SENT ||
                   sock->state == TCP_STATE_SYN_RECEIVED;
        tcp_registry_lock_release();
        return open ? 0 : -1;
    }

    uint64_t before = sock->rx_len;
    uint64_t to_copy = sock->rx_len < max_len ? sock->rx_len : max_len;
    memcpy(buf, sock->rx_buffer, to_copy);
    uint64_t tail = sock->rx_len - to_copy;
    if (sock->ooo_len) tail += (uint64_t)(uint32_t)(sock->ooo_seq - sock->rcv_nxt) + sock->ooo_len;   /* keep the buffered block in step */
    if (tail) memmove(sock->rx_buffer, sock->rx_buffer + to_copy, (size_t)tail);
    sock->rx_len -= to_copy;

    /* Re-open a window the peer saw as nearly shut; otherwise data ACKs already carry the update. */
    if (sock->rx_max - before < 2u * TCP_MSS) tcp_ack_now(sock);
    tcp_registry_lock_release();
    return (int)to_copy;
}

int tcp_socket_close(tcp_socket_t* sock) {
    if (!sock) return -1;

    tcp_registry_lock_acquire();
    if (sock->state == TCP_STATE_ESTABLISHED || sock->state == TCP_STATE_CLOSE_WAIT) {
        sock->state = sock->state == TCP_STATE_ESTABLISHED ? TCP_STATE_FIN_WAIT1 : TCP_STATE_LAST_ACK;
        sock->fin_queued = 1;
        tcp_output(sock);                          /* FIN goes out behind any queued data */
    }
    int waiting = sock->state == TCP_STATE_FIN_WAIT1 || sock->state == TCP_STATE_LAST_ACK ||
                  sock->state == TCP_STATE_CLOSING;
    tcp_registry_lock_release();

    if (waiting) {
        /* Let queued data and the FIN be acknowledged (and retransmitted if lost) while servicing the network. */
        uint64_t deadline = get_timer_ticks() + 3000u;
        for (;;) {
            tcp_registry_lock_acquire();
            int done = sock->state != TCP_STATE_FIN_WAIT1 && sock->state != TCP_STATE_LAST_ACK &&
                       sock->state != TCP_STATE_CLOSING;
            tcp_registry_lock_release();
            if (done || get_timer_ticks() >= deadline) break;
            net_poll();
            thread_yield();
        }
    }

    tcp_registry_lock_acquire();
    sock->state = TCP_STATE_CLOSED;
    sock->rtx_at = 0;
    tcp_registry_lock_release();
    return 0;
}

/* ------------------------------------------------------------------ receive path */
static void tcp_parse_mss(tcp_socket_t* s, const uint8_t* opts, size_t optlen) {
    for (size_t i = 0; i < optlen;) {
        uint8_t kind = opts[i];
        if (kind == 0) break;
        if (kind == 1) { i++; continue; }
        if (i + 1 >= optlen || opts[i + 1] < 2 || i + opts[i + 1] > optlen) break;
        if (kind == 2 && opts[i + 1] == 4) {
            uint16_t m = (uint16_t)(((uint16_t)opts[i + 2] << 8) | opts[i + 3]);
            s->peer_mss = m < TCP_MSS ? m : TCP_MSS;
            if (s->v6 && s->peer_mss > TCP6_MSS) s->peer_mss = TCP6_MSS;
            if (s->peer_mss < 128) s->peer_mss = 536;
        }
        i += opts[i + 1];
    }
}

static void tcp_input(const tcp_peer_t* peerp, tcp_hdr_t* tcp, void* data, size_t len);

void tcp_handle_packet(ip_hdr_t* ip, tcp_hdr_t* tcp, void* data, size_t len) {
    tcp_peer_t peer; memset(&peer, 0, sizeof peer);
    peer.a4 = (ip_addr_t){.addr = {(uint8_t)ip->src_addr, (uint8_t)(ip->src_addr >> 8), (uint8_t)(ip->src_addr >> 16),
                                   (uint8_t)(ip->src_addr >> 24)}};
    tcp_input(&peer, tcp, data, len);
}

void tcp_handle_packet6(const ip6_addr_t* src, tcp_hdr_t* tcp, void* data, size_t len) {
    tcp_peer_t peer; memset(&peer, 0, sizeof peer);
    peer.v6 = 1; peer.a6 = *src;
    tcp_input(&peer, tcp, data, len);
}

static void tcp_input(const tcp_peer_t* peerp, tcp_hdr_t* tcp, void* data, size_t len) {
    const tcp_peer_t peer = *peerp;
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t flags = tcp->flags;
    uint16_t win = ntohs(tcp->window);
    size_t hdr_len = (size_t)((uint8_t*)data - (uint8_t*)tcp);

    /* The lock covers lookup plus the whole state update, so tcp_socket_destroy() cannot free the socket under us. */
    tcp_registry_lock_acquire();
    tcp_socket_t* s = tcp_sockets;
    while (s) {
        if (s->local_port == dst_port && s->remote_port == src_port && s->state != TCP_STATE_LISTEN &&
            peer_eq_sock(&peer, s)) break;
        s = s->next;
    }

    if (!s && (flags & TCP_SYN) && !(flags & (TCP_ACK | TCP_RST))) {
        /* A new connection attempt: is somebody listening on this port? */
        tcp_socket_t* l = NULL;
        for (tcp_socket_t* o = tcp_sockets; o; o = o->next)
            if (o->state == TCP_STATE_LISTEN && o->local_port == dst_port) { l = o; break; }
        if (l) {
            unsigned pending = 0, children_total = 0;
            for (tcp_socket_t* o = tcp_sockets; o; o = o->next) {
                if (o->parent == l) pending++;
                if (o->unaccepted) children_total++;
            }
            /* Backlog full, or too many half-open/unaccepted children overall (SYN flood): stay silent, the client retries. */
            if (pending < l->backlog && children_total < TCP_MAX_UNACCEPTED) {
                tcp_socket_t* c = tcp_alloc();
                if (c) {
                    uint32_t iss = net_rand32();
                    c->v6 = peer.v6; c->remote_addr = peer.a4; c->remote6 = peer.a6;
                    c->local_port = dst_port; c->remote_port = src_port;
                    c->parent = l; c->unaccepted = 1;
                    c->rcv_nxt = seq + 1u; c->remote_seq = seq; c->ack_num = c->rcv_nxt;
                    c->snd_una = iss; c->snd_nxt = iss + 1u; c->snd_max = iss + 1u; c->seq_num = c->snd_nxt;
                    c->peer_wnd = win; c->peer_mss = 536u;
                    tcp_parse_mss(c, (const uint8_t*)tcp + sizeof(tcp_hdr_t), hdr_len - sizeof(tcp_hdr_t));
                    c->rto = 1000u; c->cwnd = 4u * TCP_MSS; c->ssthresh = 65535u;
                    c->state = TCP_STATE_SYN_RECEIVED;
                    c->next = tcp_sockets; tcp_sockets = c;
                    uint64_t t0 = get_timer_ticks();
                    (void)tcp_xmit(c, TCP_SYN | TCP_ACK, c->snd_una, NULL, 0, 1);   /* lost? the timer resends it */
                    c->rtx_at = t0 + c->rto;
                }
            }
            tcp_registry_lock_release();
            return;
        }
    }

    if (!s) {
        /* Nothing owns this connection (closed, or a stray segment): answer with RST so the peer stops. */
        if (!(flags & TCP_RST)) {
            g_stats.resets_sent++;
            if (flags & TCP_ACK) tcp_xmit_raw(&peer, dst_port, src_port, ack, 0, TCP_RST, 0, NULL, 0, 0);
            else tcp_xmit_raw(&peer, dst_port, src_port, 0, seq + (uint32_t)len + ((flags & (TCP_SYN | TCP_FIN)) ? 1u : 0u),
                              TCP_RST | TCP_ACK, 0, NULL, 0, 0);
        }
        tcp_registry_lock_release();
        return;
    }

    uint64_t now = get_timer_ticks();

    if (s->state == TCP_STATE_SYN_SENT) {
        bool ack_ok = (flags & TCP_ACK) && ack == s->snd_una + 1u;
        if ((flags & TCP_ACK) && !ack_ok) {
            if (!(flags & TCP_RST)) tcp_xmit_raw(&peer, dst_port, src_port, ack, 0, TCP_RST, 0, NULL, 0, 0);
        } else if (flags & TCP_RST) {
            if (ack_ok) { s->state = TCP_STATE_CLOSED; s->reset_seen = 1; s->rtx_at = 0; g_stats.resets_rcvd++; }
        } else if ((flags & TCP_SYN) && ack_ok) {
            s->rcv_nxt = seq + 1u;
            s->remote_seq = seq;
            s->ack_num = s->rcv_nxt;
            s->snd_una = ack;
            s->snd_nxt = ack;
            s->peer_wnd = win;
            tcp_parse_mss(s, (const uint8_t*)tcp + sizeof(tcp_hdr_t), hdr_len - sizeof(tcp_hdr_t));
            if (s->rtt_start && s->rtx_count == 0) tcp_update_rtt(s, (uint32_t)(now - s->rtt_start));
            s->rtt_start = 0; s->rtx_count = 0; s->rtx_at = 0;
            s->state = TCP_STATE_ESTABLISHED;
            tcp_ack_now(s);
        }
        tcp_registry_lock_release();
        return;
    }

    if (s->state == TCP_STATE_SYN_RECEIVED) {
        if (flags & TCP_RST) {
            int32_t off = (int32_t)(seq - s->rcv_nxt);
            if (off >= 0 && off < 65535) { s->state = TCP_STATE_CLOSED; s->reset_seen = 1; s->rtx_at = 0; g_stats.resets_rcvd++; }
            tcp_registry_lock_release();
            return;
        }
        if (flags & TCP_SYN) {                      /* our SYN-ACK was lost: send it again */
            if (seq + 1u == s->rcv_nxt) (void)tcp_xmit(s, TCP_SYN | TCP_ACK, s->snd_una, NULL, 0, 1);
            tcp_registry_lock_release();
            return;
        }
        if ((flags & TCP_ACK) && ack == s->snd_una + 1u) {
            s->snd_una = ack; s->snd_nxt = ack; s->seq_num = ack;
            s->peer_wnd = win;
            s->rtx_count = 0; s->rtx_at = 0;
            s->state = TCP_STATE_ESTABLISHED;       /* handshake complete; data in this segment is handled below */
        } else {
            tcp_registry_lock_release();
            return;
        }
    }

    if (s->state == TCP_STATE_CLOSED) { tcp_registry_lock_release(); return; }

    if (flags & TCP_RST) {
        /* Only a reset whose sequence number is inside the receive window may tear the connection down. */
        int32_t off = (int32_t)(seq - s->rcv_nxt);
        if (off >= 0 && off < (int32_t)(tcp_rx_window(s) ? tcp_rx_window(s) : 1)) {
            s->state = TCP_STATE_CLOSED; s->reset_seen = 1; s->rtx_at = 0; g_stats.resets_rcvd++;
        }
        tcp_registry_lock_release();
        return;
    }

    if (flags & TCP_SYN) {
        /* A retransmitted SYN-ACK (our handshake ACK was lost): acknowledge it again. */
        if (seq + 1u == s->rcv_nxt) tcp_ack_now(s);
        tcp_registry_lock_release();
        return;
    }
    if (!(flags & TCP_ACK)) { tcp_registry_lock_release(); return; }

    bool has_fin = (flags & TCP_FIN) != 0;
    tcp_process_ack(s, ack, win, len == 0 && !has_fin);

    if (s->state == TCP_STATE_TIME_WAIT) {
        if (has_fin) tcp_ack_now(s);              /* peer missed our final ACK */
        tcp_registry_lock_release();
        return;
    }

    bool data_state = s->state == TCP_STATE_ESTABLISHED || s->state == TCP_STATE_FIN_WAIT1 ||
                      s->state == TCP_STATE_FIN_WAIT2;
    if (data_state && (len > 0 || has_fin)) {
        const uint8_t* p = (const uint8_t*)data;
        int32_t off = (int32_t)(s->rcv_nxt - seq);  /* > 0: the segment starts with bytes we already have */
        if (off < 0) {
            /* A gap: an earlier segment was lost. Drop this one and repeat our ACK so the sender retransmits (3 of
             * them trigger its fast retransmit). */
            g_stats.out_of_order++;
            uint32_t gap = (uint32_t)(-off);
            uint64_t pos = s->rx_len + gap;
            if (len > 0 && pos + len <= s->rx_max) {
                if (!s->ooo_len) { s->ooo_seq = seq; s->ooo_len = (uint32_t)len; memcpy(s->rx_buffer + pos, p, len); }
                else {
                    uint32_t bs = s->ooo_seq, be = s->ooo_seq + s->ooo_len, ns = seq, ne = seq + (uint32_t)len;
                    if ((int32_t)(ns - be) <= 0 && (int32_t)(ne - bs) >= 0) {       /* touches or overlaps the block: merge */
                        memcpy(s->rx_buffer + pos, p, len);
                        uint32_t us = (int32_t)(ns - bs) < 0 ? ns : bs;
                        uint32_t ue = (int32_t)(ne - be) > 0 ? ne : be;
                        s->ooo_seq = us; s->ooo_len = ue - us;
                    }                                                                 /* else: a second hole; dropped, the peer resends */
                }
            }
            tcp_ack_now(s);
        } else {
            size_t skip = (size_t)off;
            if (skip > len || (skip == len && !has_fin)) {
                g_stats.dup_segments++;             /* entirely old: ACK again, nothing to store */
                tcp_ack_now(s);
            } else {
                size_t n = len - skip;
                bool fin_in_order = has_fin;    /* the FIN sits at seq+len, i.e. right after the bytes taken below */
                size_t room = (size_t)(s->rx_max - s->rx_len);
                size_t take = n < room ? n : room;
                if (take > 0) { memcpy(s->rx_buffer + s->rx_len, p + skip, take); s->rx_len += take; s->rcv_nxt += (uint32_t)take; }
                if (take < n) {
                    serial_puts("[TCP] RX buffer full; segment trimmed\n");
                    fin_in_order = false;
                }
                if (s->ooo_len && (int32_t)(s->ooo_seq - s->rcv_nxt) <= 0) {
                    /* The hole in front of the buffered block is closed: its bytes are already in place. */
                    uint32_t overlap = s->rcv_nxt - s->ooo_seq;
                    if (overlap < s->ooo_len) { uint32_t fresh = s->ooo_len - overlap; s->rx_len += fresh; s->rcv_nxt += fresh; }
                    s->ooo_len = 0;
                }
                if (fin_in_order) {
                    s->rcv_nxt += 1u;
                    if (s->state == TCP_STATE_ESTABLISHED) s->state = TCP_STATE_CLOSE_WAIT;
                    else if (s->state == TCP_STATE_FIN_WAIT1) s->state = TCP_STATE_CLOSING;   /* our FIN is not acked yet */
                    else if (s->state == TCP_STATE_FIN_WAIT2) tcp_enter_time_wait(s);
                }
                s->ack_num = s->rcv_nxt;
                s->remote_seq = seq;
                tcp_ack_now(s);
            }
        }
    }
    tcp_registry_lock_release();
}

/* Timers: retransmission, SYN retry, TIME_WAIT expiry. Driven from net_poll(). */
void tcp_poll(void) {
    uint64_t now = get_timer_ticks();
    tcp_registry_lock_acquire();
    /* Free sockets nobody will use again: connections that died before they were accepted, and detached sockets whose
     * close has finished (or whose peer never finished it). */
    for (tcp_socket_t **pp = &tcp_sockets; *pp;) {
        tcp_socket_t* s = *pp;
        if (s->state == TCP_STATE_TIME_WAIT && now >= s->time_wait_until) s->state = TCP_STATE_CLOSED;
        bool dead_orphan = s->unaccepted && s->state == TCP_STATE_CLOSED;
        bool dead_detached = s->detached && (s->state == TCP_STATE_CLOSED || now >= s->detach_deadline);
        if (dead_orphan || dead_detached) {
            *pp = s->next;
            if (s->rx_buffer) kfree(s->rx_buffer);
            if (s->tx_buffer) kfree(s->tx_buffer);
            kfree(s);
            continue;
        }
        pp = &s->next;
    }
    for (tcp_socket_t* s = tcp_sockets; s; s = s->next) {
        if (s->state == TCP_STATE_TIME_WAIT && now >= s->time_wait_until) { s->state = TCP_STATE_CLOSED; continue; }
        if (!s->rtx_at || now < s->rtx_at || s->state == TCP_STATE_CLOSED) continue;
        if (!tcp_pending(s)) { s->rtx_at = 0; continue; }
        if (++s->rtx_count > ((s->state == TCP_STATE_SYN_SENT || s->state == TCP_STATE_SYN_RECEIVED) ? 5 : TCP_MAX_RETRIES)) {
            g_stats.timeouts++;
            s->state = TCP_STATE_CLOSED;
            s->reset_seen = 1;
            s->rtx_at = 0;
            continue;
        }
        g_stats.retransmits++;
        s->rto = s->rto * 2u > 16000u ? 16000u : s->rto * 2u;
        s->rtx_at = 0;
        if (s->state == TCP_STATE_SYN_RECEIVED) {
            (void)tcp_xmit(s, TCP_SYN | TCP_ACK, s->snd_una, NULL, 0, 1);
            s->rtx_at = now + s->rto;
            continue;
        }
        if (s->state == TCP_STATE_SYN_SENT) {
            (void)tcp_xmit(s, TCP_SYN, s->snd_una, NULL, 0, 1);
            s->rtt_start = 0;
            s->rtx_at = now + s->rto;
            continue;
        }
        uint32_t mss = tcp_mss(s);
        uint32_t flight = (uint32_t)(s->snd_max - s->snd_una);
        s->ssthresh = flight / 2u > 2u * mss ? flight / 2u : 2u * mss;
        s->cwnd = mss;
        tcp_rewind(s);
        tcp_output(s);
        if (!s->rtx_at) s->rtx_at = now + s->rto;
    }
    tcp_registry_lock_release();
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
