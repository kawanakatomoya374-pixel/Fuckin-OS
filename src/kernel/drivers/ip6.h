/**
 * ip6.h - IPv6: link-local + SLAAC addressing, neighbour discovery, ICMPv6 echo, and the packet path for TCP.
 *
 * Addresses come from Router Advertisements (stateless autoconfiguration); duplicate address detection is not done.
 * Extension headers other than hop-by-hop / destination options are not parsed.
 */
#ifndef IP6_H
#define IP6_H
#include "types.h"
#include "net.h"

/* ip6_addr_t is defined in net.h so socket addresses can carry it */

void ip6_init(void);                       /* after the NIC is up: derive the link-local address, start router discovery */
void ip6_poll(void);                       /* from net_poll(): router solicitation retries */
void ip6_recv(const uint8_t* pkt, size_t len, const uint8_t src_mac[6]);   /* an IPv6 packet from an Ethernet frame */
bool ip6_ready(void);                      /* a global (SLAAC) address and a router are known */
bool ip6_link_local_ready(void);
ip6_addr_t ip6_link_local(void);
ip6_addr_t ip6_global(void);
ip6_addr_t ip6_router(void);
bool ip6_same(const ip6_addr_t* a, const ip6_addr_t* b);
/* Sends an IPv6 payload (e.g. a TCP segment or ICMPv6 message). 0 = sent; -1 = dropped (typically the next hop's MAC is
 * still being resolved by neighbour discovery: retry). Source = the right one of our addresses for 'dst'. */
int  ip6_send(const ip6_addr_t* dst, uint8_t next_header, const void* payload, size_t len);
ip6_addr_t ip6_select_src(const ip6_addr_t* dst);
/* Checksum over the IPv6 pseudo header + 'len' bytes at 'seg'; 0 when a received segment is valid. */
uint16_t ip6_checksum(const ip6_addr_t* src, const ip6_addr_t* dst, uint8_t next_header, const void* seg, size_t len);
/* ICMPv6 echo. 0 = reply (rtt in ms), -1 timeout, -3 no IPv6 address yet. */
int  icmp6_ping(const ip6_addr_t* dst, uint16_t seq, uint32_t timeout_ms, uint32_t* rtt_ms);
bool ip6_parse(const char* text, ip6_addr_t* out);          /* "fe80::1", "fec0::2" ... */
void ip6_format(const ip6_addr_t* a, char* out, size_t cap);

typedef struct { uint32_t rx, tx, ns_sent, na_rcvd, ra_rcvd, echo_replies, bad_checksum; } ip6_stats_t;
void ip6_get_stats(ip6_stats_t* out);
#endif
