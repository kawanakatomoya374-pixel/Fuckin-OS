/**
 * icmp.h - ICMPv4: answers echo requests and measures round trips (ping).
 */
#ifndef ICMP_H
#define ICMP_H

#include "types.h"
#include "net.h"

#define ICMP_ECHO_REPLY    0
#define ICMP_DEST_UNREACH  3
#define ICMP_ECHO_REQUEST  8
#define ICMP_TIME_EXCEEDED 11

typedef struct { uint32_t requests_rcvd, replies_sent, echo_sent, echo_replies, errors_rcvd; } icmp_stats_t;

void icmp_init(void);
void icmp_recv(ip_hdr_t* ip, void* data, size_t len);
/* Sends one echo request and waits for the matching reply while servicing the network.
 * Returns 0 and the round-trip time in ms (>= 0, 0 = under 1 ms), -1 on timeout, -2 if the destination was reported
 * unreachable / TTL expired, -3 when no network interface is up. */
int icmp_ping(const ip_addr_t* dst, uint16_t seq, uint32_t timeout_ms, uint32_t* rtt_ms);
void icmp_get_stats(icmp_stats_t* out);

#endif
