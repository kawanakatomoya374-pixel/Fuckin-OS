/* cos_socket.c - Berkeley-style sockets for ring-3 programs.
 *
 * Every call here is NON-BLOCKING and returns at once: a blocking syscall holds the syscall path and freezes every ring-3
 * window, so waiting is done by the program (cos_sock_poll() in the user library sleeps between polls). A descriptor is a
 * small integer owned by one process; a process that exits has all of its sockets closed.
 *
 *   TCP  socket / bind / listen / accept / connect / send / recv / close   (accept queue, SYN backlog, half-open limit)
 *   UDP  socket / bind / connect / sendto / recvfrom / close               (8-datagram queue, sender address kept)
 *   poll over any mix, getsockname / getpeername, and asynchronous DNS and interface information.
 */
#include "sync.h"
#include "serial.h"
#include "string.h"
#include "memory.h"
#include "drivers/net.h"
#include "drivers/tcp.h"
#include "drivers/udp.h"
#include "drivers/ip6.h"
#include "drivers/dns.h"
#include "drivers/dhcp.h"
#include "cos_socket.h"

extern void net_poll(void);
extern net_iface_t* net_get_default_iface(void);

#define CS_MAX      64
#define CS_PER_PROC 24

enum { CS_TCP = 1, CS_UDP = 2 };

typedef struct {
    uint8_t   used, type;
    uint8_t   listening, connecting, connected, v6;
    uint32_t  owner;
    void*     obj;                    /* tcp_socket_t* or udp_socket_t* */
} cs_t;

static cs_t g_cs[CS_MAX];
static volatile uint32_t g_cs_lock;

static void cs_lock(void) { cos_spin_lock(&g_cs_lock); }
static void cs_unlock(void) { cos_spin_unlock(&g_cs_lock); }

static cs_t* cs_get(int sd, uint32_t pid) {
    if (sd < 0 || sd >= CS_MAX) return NULL;
    cs_t* c = &g_cs[sd];
    return (c->used && c->owner == pid) ? c : NULL;
}

static int cs_alloc(uint32_t pid, int type, void* obj) {
    cs_lock();
    int mine = 0;
    for (int i = 0; i < CS_MAX; i++) if (g_cs[i].used && g_cs[i].owner == pid) mine++;
    int sd = -COS_EMFILE;
    if (mine < CS_PER_PROC) {
        for (int i = 0; i < CS_MAX; i++) {
            if (!g_cs[i].used) {
                memset(&g_cs[i], 0, sizeof g_cs[i]);
                g_cs[i].used = 1; g_cs[i].type = (uint8_t)type; g_cs[i].owner = pid; g_cs[i].obj = obj;
                sd = i;
                break;
            }
        }
    }
    cs_unlock();
    return sd;
}

static ip_addr_t from_u32(uint32_t v) { ip_addr_t a; a.addr[0] = (uint8_t)(v >> 24); a.addr[1] = (uint8_t)(v >> 16); a.addr[2] = (uint8_t)(v >> 8); a.addr[3] = (uint8_t)v; return a; }
static uint32_t to_u32(const ip_addr_t* a) { return ((uint32_t)a->addr[0] << 24) | ((uint32_t)a->addr[1] << 16) | ((uint32_t)a->addr[2] << 8) | a->addr[3]; }

int cos_sock_create(uint32_t pid, int type) {
    net_iface_t* iface = net_get_default_iface();
    if (!iface) return -COS_ENETDOWN;
    void* obj = NULL;
    int t;
    if (type == COS_SOCK_STREAM) { obj = tcp_socket_create(); t = CS_TCP; }
    else if (type == COS_SOCK_DGRAM) {
        /* an unbound UDP socket still needs a local port to receive replies: the ephemeral one is picked here */
        obj = udp_socket_create(0); t = CS_UDP;
    } else return -COS_EINVAL;
    if (!obj) return -COS_ENOMEM;
    int sd = cs_alloc(pid, t, obj);
    if (sd < 0) { if (t == CS_TCP) tcp_socket_destroy(obj); else udp_socket_close(obj); }
    return sd;
}

int cos_sock_bind(uint32_t pid, int sd, uint32_t ip, uint16_t port) {
    (void)ip;                                      /* one interface: the address is always the interface's own */
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    if (c->type == CS_TCP) {
        tcp_socket_t* s = c->obj;
        if (tcp_socket_state(s) != TCP_STATE_CLOSED) return -COS_EINVAL;
        return tcp_socket_bind(s, port) == 0 ? 0 : -COS_EADDRINUSE;
    }
    return udp_socket_bind(c->obj, port) == 0 ? 0 : -COS_EADDRINUSE;
}

int cos_sock_listen(uint32_t pid, int sd, int backlog) {
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    if (c->type != CS_TCP) return -COS_EOPNOTSUPP;
    tcp_socket_t* s = c->obj;
    if (tcp_socket_listen(s, backlog) != 0) return -COS_EINVAL;     /* not bound, or already in use */
    c->listening = 1;
    return 0;
}

int cos_sock_accept(uint32_t pid, int sd, cos_sockaddr_t* peer) {
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    if (c->type != CS_TCP || !c->listening) return -COS_EINVAL;
    net_poll();
    tcp_socket_t* child = tcp_socket_accept(c->obj);
    if (!child) return -COS_EAGAIN;
    int nsd = cs_alloc(pid, CS_TCP, child);
    if (nsd < 0) { tcp_socket_abandon(child); return nsd; }
    g_cs[nsd].connected = 1;
    if (peer) { peer->ip = to_u32(&child->remote_addr); peer->port = child->remote_port; peer->family = 4; }
    return nsd;
}

int cos_sock_connect(uint32_t pid, int sd, uint32_t ip, uint16_t port) {
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    net_poll();
    ip_addr_t a = from_u32(ip);
    if (c->type == CS_UDP) return udp_socket_connect(c->obj, &a, port) == 0 ? 0 : -COS_EINVAL;
    tcp_socket_t* s = c->obj;
    if (c->listening) return -COS_EINVAL;
    if (c->connected) return -COS_EISCONN;
    if (!c->connecting) {
        if (tcp_socket_connect_start(s, &a, port) != 0) return -COS_EINVAL;
        c->connecting = 1;
        return -COS_EINPROGRESS;
    }
    int st = tcp_socket_state(s);
    if (st == TCP_STATE_ESTABLISHED || st == TCP_STATE_CLOSE_WAIT) { c->connecting = 0; c->connected = 1; return 0; }
    if (st == TCP_STATE_CLOSED) { c->connecting = 0; return tcp_socket_was_reset(s) ? -COS_ECONNREFUSED : -COS_ETIMEDOUT; }
    return -COS_EINPROGRESS;
}

int cos_sock_send(uint32_t pid, int sd, const void* buf, size_t len) {
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    net_poll();
    if (c->type == CS_UDP) {
        udp_socket_t* u = c->obj;
        if (!u->connected) return -COS_EDESTADDRREQ;
        if (len > UDP_MAX_PACKET) return -COS_EMSGSIZE;
        if (u->v6) return udp6_send(&u->remote6, u->local_port, u->remote_port, buf, len) == 0 ? (int)len : -COS_ENETUNREACH;
        ip_addr_t d = u->remote_addr;
        return udp_socket_sendto(u, &d, u->remote_port, buf, len) == 0 ? (int)len : -COS_ENETUNREACH;
    }
    tcp_socket_t* s = c->obj;
    int st = tcp_socket_state(s);
    if (st == TCP_STATE_SYN_SENT || st == TCP_STATE_SYN_RECEIVED) return -COS_EAGAIN;
    if (st != TCP_STATE_ESTABLISHED && st != TCP_STATE_CLOSE_WAIT) return tcp_socket_was_reset(s) ? -COS_ECONNRESET : -COS_EPIPE;
    int n = tcp_socket_send_nb(s, buf, len);
    if (n < 0) return -COS_EPIPE;
    return n == 0 && len > 0 ? -COS_EAGAIN : n;
}

int cos_sock_recv(uint32_t pid, int sd, void* buf, size_t len) {
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    net_poll();
    if (c->type == CS_UDP) {
        int n = udp_socket_recvfrom(c->obj, buf, len, NULL, NULL, NULL, NULL);
        return n < 0 ? -COS_EAGAIN : n;
    }
    tcp_socket_t* s = c->obj;
    int n = tcp_socket_recv(s, buf, len);
    if (n > 0) return n;
    int st = tcp_socket_state(s);
    if (st == TCP_STATE_SYN_SENT || st == TCP_STATE_SYN_RECEIVED || st == TCP_STATE_ESTABLISHED || st == TCP_STATE_FIN_WAIT1 || st == TCP_STATE_FIN_WAIT2) return -COS_EAGAIN;
    if (tcp_socket_was_reset(s)) return -COS_ECONNRESET;
    return 0;                                      /* the peer closed: end of stream */
}

int cos_sock_sendto(uint32_t pid, int sd, const void* buf, size_t len, uint32_t ip, uint16_t port) {
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    if (c->type != CS_UDP) return -COS_EOPNOTSUPP;
    if (len > UDP_MAX_PACKET) return -COS_EMSGSIZE;
    net_poll();
    ip_addr_t d = from_u32(ip);
    return udp_socket_sendto(c->obj, &d, port, buf, len) == 0 ? (int)len : -COS_ENETUNREACH;
}

int cos_sock_recvfrom(uint32_t pid, int sd, void* buf, size_t len, cos_sockaddr_t* from) {
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    if (c->type != CS_UDP) return cos_sock_recv(pid, sd, buf, len);
    net_poll();
    ip_addr_t a4; uint16_t port = 0; int v6 = 0;
    int n = udp_socket_recvfrom(c->obj, buf, len, &a4, NULL, &port, &v6);
    if (n < 0) return -COS_EAGAIN;
    if (from) { from->ip = v6 ? 0 : to_u32(&a4); from->port = port; from->family = v6 ? 6 : 4; }
    return n;
}

static int cs_revents(cs_t* c, int events) {
    int r = 0;
    if (c->type == CS_UDP) {
        if ((events & COS_POLLIN) && udp_socket_pending(c->obj) > 0) r |= COS_POLLIN;
        if (events & COS_POLLOUT) r |= COS_POLLOUT;
        return r;
    }
    tcp_socket_t* s = c->obj;
    int st = tcp_socket_state(s);
    if (c->listening) {
        if ((events & COS_POLLIN) && tcp_socket_accept_ready(s) > 0) r |= COS_POLLIN;
        return r;
    }
    if (st == TCP_STATE_SYN_SENT || st == TCP_STATE_SYN_RECEIVED) return 0;
    if (c->connecting && st == TCP_STATE_CLOSED) return COS_POLLERR | COS_POLLHUP | COS_POLLOUT;
    bool open = st == TCP_STATE_ESTABLISHED || st == TCP_STATE_FIN_WAIT1 || st == TCP_STATE_FIN_WAIT2;
    if (events & COS_POLLIN) {
        if (tcp_socket_rx_ready(s) > 0 || !open) r |= COS_POLLIN;      /* data, or end of stream / reset to be read */
    }
    if ((events & COS_POLLOUT) && tcp_socket_tx_space(s) > 0) r |= COS_POLLOUT;
    if (st == TCP_STATE_CLOSED || st == TCP_STATE_CLOSE_WAIT || st == TCP_STATE_LAST_ACK) r |= COS_POLLHUP;
    if (tcp_socket_was_reset(s)) r |= COS_POLLERR;
    return r;
}

int cos_sock_poll(uint32_t pid, cos_pollfd_t* fds, int n) {
    net_poll();
    int ready = 0;
    for (int i = 0; i < n; i++) {
        cs_t* c = cs_get(fds[i].sd, pid);
        if (!c) { fds[i].revents = COS_POLLNVAL; ready++; continue; }
        fds[i].revents = (int16_t)cs_revents(c, fds[i].events);
        if (fds[i].revents) ready++;
    }
    return ready;
}

int cos_sock_close(uint32_t pid, int sd) {
    cs_lock();
    cs_t* c = cs_get(sd, pid);
    if (!c) { cs_unlock(); return -COS_EBADF; }
    void* obj = c->obj; int type = c->type;
    memset(c, 0, sizeof *c);
    cs_unlock();
    if (type == CS_TCP) tcp_socket_abandon(obj);       /* the stack finishes the close and frees it */
    else udp_socket_close(obj);
    return 0;
}

int cos_sock_name(uint32_t pid, int sd, int peer, cos_sockaddr_t* out) {
    cs_t* c = cs_get(sd, pid);
    if (!c || !out) return -COS_EBADF;
    net_iface_t* iface = net_get_default_iface();
    memset(out, 0, sizeof *out);
    out->family = 4;
    if (c->type == CS_TCP) {
        tcp_socket_t* s = c->obj;
        if (peer) { if (!c->connected && tcp_socket_state(s) != TCP_STATE_ESTABLISHED) return -COS_ENOTCONN; out->ip = to_u32(&s->remote_addr); out->port = s->remote_port; }
        else { if (iface) out->ip = to_u32(&iface->ip); out->port = s->local_port; }
    } else {
        udp_socket_t* u = c->obj;
        if (peer) { if (!u->connected) return -COS_ENOTCONN; out->ip = to_u32(&u->remote_addr); out->port = u->remote_port; }
        else { if (iface) out->ip = to_u32(&iface->ip); out->port = u->local_port; }
    }
    return 0;
}

/* ---- IPv6 and shutdown (Linux layer) ----------------------------------------------------------------------------- */
int cos_sock_create6(uint32_t pid, int type) {
    int sd = cos_sock_create(pid, type);
    if (sd >= 0) { g_cs[sd].v6 = 1; if (type == COS_SOCK_DGRAM) ((udp_socket_t*)g_cs[sd].obj)->v6 = 1; }
    return sd;
}

int cos_sock_connect6(uint32_t pid, int sd, const uint8_t addr[16], uint16_t port) {
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    net_poll();
    ip6_addr_t a; memcpy(a.b, addr, 16);
    if (c->type == CS_UDP) {
        udp_socket_t* u = c->obj; u->v6 = 1; u->remote6 = a; u->remote_port = port; u->connected = 1; return 0;
    }
    tcp_socket_t* s = c->obj;
    if (c->listening) return -COS_EINVAL;
    if (c->connected) return -COS_EISCONN;
    if (!c->connecting) {
        if (tcp_socket_connect_start6(s, &a, port) != 0) return -COS_EINVAL;
        c->connecting = 1;
        return -COS_EINPROGRESS;
    }
    int st = tcp_socket_state(s);
    if (st == TCP_STATE_ESTABLISHED || st == TCP_STATE_CLOSE_WAIT) { c->connecting = 0; c->connected = 1; return 0; }
    if (st == TCP_STATE_CLOSED) { c->connecting = 0; return tcp_socket_was_reset(s) ? -COS_ECONNREFUSED : -COS_ETIMEDOUT; }
    return -COS_EINPROGRESS;
}

int cos_sock_sendto6(uint32_t pid, int sd, const void* buf, size_t len, const uint8_t addr[16], uint16_t port) {
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    if (c->type != CS_UDP) return -COS_EOPNOTSUPP;
    if (len > 1232) return -COS_EMSGSIZE;
    net_poll();
    ip6_addr_t a; memcpy(a.b, addr, 16);
    return udp6_send(&a, ((udp_socket_t*)c->obj)->local_port, port, buf, len) == 0 ? (int)len : -COS_ENETUNREACH;
}

int cos_sock_recvfrom6(uint32_t pid, int sd, void* buf, size_t len, cos_sockaddr6_t* from) {
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    if (c->type != CS_UDP) return cos_sock_recv(pid, sd, buf, len);
    net_poll();
    ip_addr_t a4; ip6_addr_t a6; uint16_t port = 0; int v6 = 0;
    int n = udp_socket_recvfrom(c->obj, buf, len, &a4, &a6, &port, &v6);
    if (n < 0) return -COS_EAGAIN;
    if (from) { memset(from, 0, sizeof *from); from->port = port; from->v6 = (uint8_t)v6; if (v6) memcpy(from->a, a6.b, 16); else memcpy(from->a, a4.addr, 4); }
    return n;
}

int cos_sock_accept6(uint32_t pid, int sd, cos_sockaddr6_t* peer) {
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    if (c->type != CS_TCP || !c->listening) return -COS_EINVAL;
    net_poll();
    tcp_socket_t* child = tcp_socket_accept(c->obj);
    if (!child) return -COS_EAGAIN;
    int nsd = cs_alloc(pid, CS_TCP, child);
    if (nsd < 0) { tcp_socket_abandon(child); return nsd; }
    g_cs[nsd].connected = 1; g_cs[nsd].v6 = c->v6;
    if (peer) { memset(peer, 0, sizeof *peer); peer->port = child->remote_port; peer->v6 = child->v6; if (child->v6) memcpy(peer->a, child->remote6.b, 16); else memcpy(peer->a, child->remote_addr.addr, 4); }
    return nsd;
}

int cos_sock_name6(uint32_t pid, int sd, int peer, cos_sockaddr6_t* out) {
    cs_t* c = cs_get(sd, pid);
    if (!c || !out) return -COS_EBADF;
    memset(out, 0, sizeof *out);
    bool v6 = false; ip6_addr_t r6; memset(&r6, 0, sizeof r6); uint16_t rport = 0, lport = 0; ip_addr_t r4; memset(&r4, 0, sizeof r4); bool conn = false;
    if (c->type == CS_TCP) { tcp_socket_t* s = c->obj; v6 = s->v6; r6 = s->remote6; r4 = s->remote_addr; rport = s->remote_port; lport = s->local_port; conn = c->connected || tcp_socket_state(s) == TCP_STATE_ESTABLISHED; }
    else { udp_socket_t* u = c->obj; v6 = u->v6 && u->connected; r6 = u->remote6; r4 = u->remote_addr; rport = u->remote_port; lport = u->local_port; conn = u->connected; }
    if (peer) {
        if (!conn) return -COS_ENOTCONN;
        out->port = rport; out->v6 = v6; if (v6) memcpy(out->a, r6.b, 16); else memcpy(out->a, r4.addr, 4);
        return 0;
    }
    out->port = lport;
    if (v6) { ip6_addr_t src = ip6_select_src(&r6); memcpy(out->a, src.b, 16); out->v6 = 1; }
    else if (c->v6 && !conn) { out->v6 = 1; if (ip6_ready()) { ip6_addr_t g = ip6_global(); memcpy(out->a, g.b, 16); } }
    else { net_iface_t* iface = net_get_default_iface(); if (iface) memcpy(out->a, iface->ip.addr, 4); }
    return 0;
}

int cos_sock_shutdown(uint32_t pid, int sd, int how) {
    cs_t* c = cs_get(sd, pid);
    if (!c) return -COS_EBADF;
    if (c->type == CS_TCP && (how == 1 || how == 2)) {
        if (!c->connected && tcp_socket_state(c->obj) != TCP_STATE_ESTABLISHED && tcp_socket_state(c->obj) != TCP_STATE_CLOSE_WAIT) return -COS_ENOTCONN;
        tcp_socket_shutdown_wr(c->obj);
    }
    return 0;
}

/* A process has exited: close everything it still holds. */
void cos_sock_close_all_for_pid(uint32_t pid) {
    for (int i = 0; i < CS_MAX; i++) if (g_cs[i].used && g_cs[i].owner == pid) cos_sock_close(pid, i);
}

/* ---- asynchronous DNS --------------------------------------------------------------------------------------------- */
int cos_dns_start(const char* host, cos_dns_result_t* out) {
    memset(out, 0, sizeof *out);
    out->id = -1;
    sock_addr_t literal;
    /* literals and cached names resolve at once; anything else becomes a query the caller polls */
    extern int net_resolve_literal(const char* host, sock_addr_t* out);
    if (net_resolve_literal(host, &literal) == 0 && literal.family == AF_INET) {
        memcpy(out->ip, literal.addr.addr, 4); out->state = 1; return 0;
    }
    ip_addr_t cached;
    int cr = dns_cache_lookup(host, &cached);
    if (cr == 0) { memcpy(out->ip, cached.addr, 4); out->state = 1; return 0; }
    if (cr == -2) { out->state = -1; return 0; }                  /* cached "no such name" */
    int id = dns_query(host, DNS_TYPE_A);
    if (id < 0) { out->state = -1; return -COS_ENETUNREACH; }
    out->id = id; out->state = 0;
    return 0;
}

int cos_dns_poll(cos_dns_result_t* io) {
    if (io->state != 0 || io->id < 0) return 0;
    net_poll();
    ip_addr_t r;
    int st = dns_async_poll(io->id, &r);
    if (st == 1) { memcpy(io->ip, r.addr, 4); io->state = 1; }
    else if (st < 0) io->state = -1;
    return 0;
}

/* ---- interface information ---------------------------------------------------------------------------------------- */
int cos_net_info(cos_netinfo_t* out) {
    memset(out, 0, sizeof *out);
    net_iface_t* iface = net_get_default_iface();
    if (!iface) return -COS_ENETDOWN;
    memcpy(out->ip, iface->ip.addr, 4);
    memcpy(out->mask, iface->netmask.addr, 4);
    memcpy(out->gateway, iface->gateway.addr, 4);
    memcpy(out->mac, iface->mac.addr, 6);
    ip_addr_t d; dns_get_server(&d); memcpy(out->dns, d.addr, 4);
    out->link_up = 1;
    ip_addr_t leased = dhcp_get_ip();              /* the interface keeps a static fallback address until DHCP answers */
    out->has_lease = leased.addr[0] || leased.addr[1] || leased.addr[2] || leased.addr[3];
    if (out->has_lease) memcpy(out->ip, leased.addr, 4);
    tcp_stats_t ts; tcp_get_stats(&ts);
    out->tcp_retransmits = ts.retransmits; out->tcp_fast_retransmits = ts.fast_retransmits; out->tcp_timeouts = ts.timeouts;
    out->tcp_resets_sent = ts.resets_sent; out->tcp_resets_rcvd = ts.resets_rcvd; out->tcp_out_of_order = ts.out_of_order;
    int open = 0;
    for (int i = 0; i < CS_MAX; i++) if (g_cs[i].used) open++;
    out->sockets_open = (uint32_t)open;
    return 0;
}
