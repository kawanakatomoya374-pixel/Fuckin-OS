/* cos_socket.h - ring-3 socket layer (src/kernel/cos_socket.c). The structs are mirrored in userland/include/cos.h. */
#ifndef COS_SOCKET_H
#define COS_SOCKET_H

#include <stdint.h>
#include <stddef.h>

enum { COS_SOCK_STREAM = 1, COS_SOCK_DGRAM = 2 };

/* errno-style codes, returned negated */
enum { COS_EPERM = 1, COS_EBADF = 9, COS_EAGAIN = 11, COS_ENOMEM = 12, COS_EINVAL = 22, COS_EMFILE = 24, COS_EPIPE = 32,
       COS_EDESTADDRREQ = 89, COS_EMSGSIZE = 90, COS_EOPNOTSUPP = 95, COS_EADDRINUSE = 98, COS_ENETDOWN = 100,
       COS_ENETUNREACH = 101, COS_ECONNRESET = 104, COS_EISCONN = 106, COS_ENOTCONN = 107, COS_ETIMEDOUT = 110,
       COS_ECONNREFUSED = 111, COS_EINPROGRESS = 115 };

enum { COS_POLLIN = 1, COS_POLLOUT = 4, COS_POLLERR = 8, COS_POLLHUP = 16, COS_POLLNVAL = 32 };

typedef struct { uint32_t ip; uint16_t port; uint16_t family; } cos_sockaddr_t;          /* ip: host order, a.b.c.d = a<<24|... */
typedef struct { int32_t sd; int16_t events, revents; } cos_pollfd_t;
typedef struct { int32_t id, state; uint8_t ip[4]; } cos_dns_result_t;                    /* state: 0 pending, 1 resolved, -1 failed */
typedef struct {
    uint8_t ip[4], mask[4], gateway[4], dns[4], mac[6];
    uint8_t link_up, has_lease;
    uint32_t tcp_retransmits, tcp_fast_retransmits, tcp_timeouts, tcp_resets_sent, tcp_resets_rcvd, tcp_out_of_order;
    uint32_t sockets_open, pad;
} cos_netinfo_t;

/* IPv6 and shutdown (used by the Linux layer): addresses are 16 raw bytes */
typedef struct { uint8_t a[16]; uint16_t port; uint8_t v6; uint8_t pad; } cos_sockaddr6_t;           /* v6 = 0: a[0..3] is an IPv4 address */
int  cos_sock_create6(uint32_t pid, int type);
int  cos_sock_connect6(uint32_t pid, int sd, const uint8_t addr[16], uint16_t port);
int  cos_sock_sendto6(uint32_t pid, int sd, const void* buf, size_t len, const uint8_t addr[16], uint16_t port);
int  cos_sock_recvfrom6(uint32_t pid, int sd, void* buf, size_t len, cos_sockaddr6_t* from);
int  cos_sock_accept6(uint32_t pid, int sd, cos_sockaddr6_t* peer);
int  cos_sock_name6(uint32_t pid, int sd, int peer, cos_sockaddr6_t* out);
int  cos_sock_shutdown(uint32_t pid, int sd, int how);       /* how: 0 read, 1 write, 2 both (read side is accepted and ignored) */
int  cos_sock_create(uint32_t pid, int type);
int  cos_sock_bind(uint32_t pid, int sd, uint32_t ip, uint16_t port);
int  cos_sock_listen(uint32_t pid, int sd, int backlog);
int  cos_sock_accept(uint32_t pid, int sd, cos_sockaddr_t* peer);
int  cos_sock_connect(uint32_t pid, int sd, uint32_t ip, uint16_t port);
int  cos_sock_send(uint32_t pid, int sd, const void* buf, size_t len);
int  cos_sock_recv(uint32_t pid, int sd, void* buf, size_t len);
int  cos_sock_sendto(uint32_t pid, int sd, const void* buf, size_t len, uint32_t ip, uint16_t port);
int  cos_sock_recvfrom(uint32_t pid, int sd, void* buf, size_t len, cos_sockaddr_t* from);
int  cos_sock_poll(uint32_t pid, cos_pollfd_t* fds, int n);
int  cos_sock_close(uint32_t pid, int sd);
int  cos_sock_name(uint32_t pid, int sd, int peer, cos_sockaddr_t* out);
void cos_sock_close_all_for_pid(uint32_t pid);
int  cos_dns_start(const char* host, cos_dns_result_t* out);
int  cos_dns_poll(cos_dns_result_t* io);
int  cos_net_info(cos_netinfo_t* out);

#endif
