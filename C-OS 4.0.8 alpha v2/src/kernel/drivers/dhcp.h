/**
 * dhcp.h - DHCP Client
 */

#ifndef DHCP_H
#define DHCP_H

#include "types.h"
#include "net.h"

#define DHCP_CLIENT_PORT    68
#define DHCP_SERVER_PORT    67

#define DHCP_MAGIC_COOKIE   0x63825363

/* DHCP message types */
#define DHCP_DISCOVER   1
#define DHCP_OFFER      2
#define DHCP_REQUEST    3
#define DHCP_DECLINE    4
#define DHCP_ACK        5
#define DHCP_NAK        6
#define DHCP_RELEASE    7
#define DHCP_INFORM     8

/* DHCP options */
#define DHCP_OPT_PAD        0
#define DHCP_OPT_SUBNET     1
#define DHCP_OPT_ROUTER     3
#define DHCP_OPT_DNS        6
#define DHCP_OPT_HOSTNAME   12
#define DHCP_OPT_DOMAIN     15
#define DHCP_OPT_REQ_IP     50
#define DHCP_OPT_LEASE      51
#define DHCP_OPT_MSG_TYPE   53
#define DHCP_OPT_SERVER     54
#define DHCP_OPT_PARAM_REQ  55
#define DHCP_OPT_RENEWAL    58
#define DHCP_OPT_REBINDING  59
#define DHCP_OPT_END        255

/* DHCP packet */
typedef struct __attribute__((packed)) {
    uint8_t op;         // Message type (1=request, 2=reply)
    uint8_t htype;      // Hardware type (1=Ethernet)
    uint8_t hlen;       // Hardware address length (6)
    uint8_t hops;       // Hops
    uint32_t xid;       // Transaction ID
    uint16_t secs;      // Seconds elapsed
    uint16_t flags;     // Flags
    uint32_t ciaddr;    // Client IP address
    uint32_t yiaddr;    // Your IP address
    uint32_t siaddr;    // Server IP address
    uint32_t giaddr;    // Gateway IP address
    uint8_t chaddr[16]; // Client hardware address
    uint8_t sname[64];  // Server name
    uint8_t file[128];  // Boot filename
    uint32_t magic;     // Magic cookie
    uint8_t options[312]; // Options (variable)
} dhcp_packet_t;

/* DHCP state */
typedef enum {
    DHCP_STATE_INIT = 0,
    DHCP_STATE_SELECTING,
    DHCP_STATE_REQUESTING,
    DHCP_STATE_BOUND,
    DHCP_STATE_RENEWING,
    DHCP_STATE_REBINDING,
} dhcp_state_t;

/* DHCP client */
typedef struct {
    dhcp_state_t state;
    uint32_t xid;
    uint64_t lease_time;
    uint64_t renewal_time;
    uint64_t rebinding_time;
    ip_addr_t server_ip;
    ip_addr_t offered_ip;
    ip_addr_t subnet_mask;
    ip_addr_t gateway;
    ip_addr_t dns[2];
} dhcp_client_t;

/* Function prototypes */
void dhcp_init(void);
int dhcp_discover(void);
int dhcp_request(ip_addr_t* server, ip_addr_t* requested);
void dhcp_handle_packet(dhcp_packet_t* packet, size_t len);
int dhcp_poll(void);
ip_addr_t dhcp_get_ip(void);
ip_addr_t dhcp_get_gateway(void);
ip_addr_t dhcp_get_dns(void);

#endif /* DHCP_H */
