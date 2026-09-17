/**
 * dns.h - DNS Client
 */

#ifndef DNS_H
#define DNS_H

#include "types.h"
#include "net.h"

#define DNS_PORT        53

#define DNS_TYPE_A      1   // IPv4 address
#define DNS_TYPE_NS     2   // Name server
#define DNS_TYPE_CNAME  5   // Canonical name
#define DNS_TYPE_SOA    6   // Start of authority
#define DNS_TYPE_PTR    12  // Pointer
#define DNS_TYPE_MX     15  // Mail exchange
#define DNS_TYPE_TXT    16  // Text
#define DNS_TYPE_AAAA   28  // IPv6 address

#define DNS_CLASS_IN    1   // Internet

/* DNS header */


typedef struct {
    ip_addr_t ip;
    uint16_t port;
    uint8_t active;
    uint8_t available;
    uint32_t last_used;
} dns_server_t;

extern dns_server_t g_dns_servers[3];
extern int g_primary_dns;
extern int g_secondary_dns;

typedef struct __attribute__((packed)) {
    uint16_t id;        // Transaction ID
    uint16_t flags;     // Flags
    uint16_t questions; // Number of questions
    uint16_t answers;   // Number of answer RRs
    uint16_t authority; // Number of authority RRs
    uint16_t additional;// Number of additional RRs
} dns_header_t;

/* DNS flags */
#define DNS_FLAG_QR     0x8000  // Query/Response
#define DNS_FLAG_AA     0x0400  // Authoritative answer
#define DNS_FLAG_TC     0x0200  // Truncated
#define DNS_FLAG_RD     0x0100  // Recursion desired
#define DNS_FLAG_RA     0x0080  // Recursion available

/* DNS response codes */
#define DNS_RCODE_NOERROR   0
#define DNS_RCODE_FORMERR   1
#define DNS_RCODE_SERVFAIL  2
#define DNS_RCODE_NXDOMAIN  3
#define DNS_RCODE_NOTIMP    4
#define DNS_RCODE_REFUSED   5

#define DNS_MAX_NAME_LEN    256
#define DNS_MAX_PACKET      512

/* DNS cache entry */
typedef struct {
    char name[DNS_MAX_NAME_LEN];
    ip_addr_t ip;
    uint32_t ttl;
    uint32_t expires;
    uint32_t timestamp;
    int valid;
    int negative;   /* NXDOMAIN: valid name lookup with no A record */
} dns_cache_entry_t;

#define DNS_CACHE_SIZE  16

void dns_init(void);
/* Updates the active resolver after a DHCP lease is acknowledged. */
void dns_set_server(const ip_addr_t* server);
int dns_resolve(const char* hostname, ip_addr_t* result);
int dns_query(const char* hostname, uint16_t type);
void dns_handle_response(void* data, size_t len);

#endif /* DNS_H */
