#ifndef _COS_NETINET_IN_H
#define _COS_NETINET_IN_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
#include <stdint.h>
#include "../sys/socket.h"
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in { sa_family_t sin_family; in_port_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
#define INADDR_ANY      ((in_addr_t)0)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define htons(x) ((uint16_t)((((x) & 0xff) << 8) | (((x) >> 8) & 0xff)))
#define ntohs(x) htons(x)
#define htonl(x) ((uint32_t)((((x) & 0xffu) << 24) | (((x) & 0xff00u) << 8) | (((x) >> 8) & 0xff00u) | (((x) >> 24) & 0xffu)))
#define ntohl(x) htonl(x)
_COS_END
#endif
