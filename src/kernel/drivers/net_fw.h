/**
 * net_fw.h - small stateless packet filter for IPv4 (TCP / UDP / ICMP), checked on every received and sent packet.
 * Rules are evaluated in order, the first match decides; no match = the default policy (allow unless changed).
 */
#ifndef NET_FW_H
#define NET_FW_H
#include "types.h"
#include "net.h"

#define FW_MAX_RULES 32
enum { FW_IN = 1, FW_OUT = 2, FW_BOTH = 3 };
enum { FW_ANY = 0, FW_TCP = 6, FW_UDP = 17, FW_ICMP = 1 };

typedef struct {
    bool deny;
    uint8_t dir;                 /* FW_IN / FW_OUT / FW_BOTH */
    uint8_t proto;               /* FW_ANY or an IP protocol number */
    uint16_t port_lo, port_hi;   /* remote port range for TCP/UDP; 0..0 = any */
    ip_addr_t net, mask;         /* remote address match; mask 0.0.0.0 = any */
} fw_rule_t;

void fw_init(void);
int  fw_add_rule(const fw_rule_t* rule);          /* appends; returns its index or -1 */
void fw_clear(void);
void fw_set_default_deny(bool deny);
/* 1 = let the packet through. 'remote' is the peer's address; 'payload' points at the TCP/UDP header (or NULL). */
int  fw_check(uint8_t dir, uint8_t proto, const ip_addr_t* remote, const void* payload, size_t len);
void fw_get_counters(uint32_t* allowed, uint32_t* denied);
#endif
