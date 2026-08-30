/**
 * dhcp.c - DHCP Client Implementation
 */

#include "dhcp.h"
#include "arp.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "udp.h"
#include "dns.h"

/* Byte order conversion functions */
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

static dhcp_client_t dhcp_client;

static uint32_t dhcp_generate_xid(void) {
    static uint32_t xid = 0x12345678u;
    return xid++;
}

void dhcp_init(void) {
    memset(&dhcp_client, 0, sizeof(dhcp_client));
    dhcp_client.state = DHCP_STATE_INIT;
    serial_puts("[DHCP] Client initialized\n");
}

int dhcp_discover(void) {
    dhcp_packet_t packet;
    memset(&packet, 0, sizeof(packet));
    
    packet.op = 1;      // Boot request
    packet.htype = 1;   // Ethernet
    packet.hlen = 6;    // MAC length
    packet.hops = 0;
    packet.xid = htonl(dhcp_generate_xid());
    packet.secs = 0;
    packet.flags = htons(0x8000);  // Broadcast
    packet.ciaddr = 0;
    packet.yiaddr = 0;
    packet.siaddr = 0;
    packet.giaddr = 0;
    
    /* Our MAC address */
    extern net_iface_t* net_get_default_iface(void);
    net_iface_t* iface = net_get_default_iface();
    if (!iface) return -1;
    
    memcpy(packet.chaddr, iface->mac.addr, 6);
    packet.magic = htonl(DHCP_MAGIC_COOKIE);
    
    /* Options */
    uint8_t* opt = packet.options;
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_DISCOVER;
    
    /* Parameter request list */
    *opt++ = DHCP_OPT_PARAM_REQ;
    *opt++ = 3;
    *opt++ = DHCP_OPT_SUBNET;
    *opt++ = DHCP_OPT_ROUTER;
    *opt++ = DHCP_OPT_DNS;
    
    *opt++ = DHCP_OPT_END;
    
    dhcp_client.xid = ntohl(packet.xid);
    dhcp_client.state = DHCP_STATE_SELECTING;
    
    /* Send broadcast */
    ip_addr_t broadcast = ip_make_addr(255, 255, 255, 255);
    udp_send(&broadcast, 68, 67, &packet, sizeof(dhcp_packet_t));
    
    return 0;
}

int dhcp_request(ip_addr_t* server, ip_addr_t* requested) {
    if (!server || !requested) return -1;

    dhcp_packet_t packet;
    memset(&packet, 0, sizeof(packet));

    /* A REQUEST must repeat the BOOTP identity fields from DISCOVER.
     * Servers discard option-only packets because they lack the client
     * hardware address, transaction ID and DHCP magic cookie. */
    packet.op = 1;
    packet.htype = 1;
    packet.hlen = 6;
    packet.xid = htonl(dhcp_client.xid);
    packet.flags = htons(0x8000);
    net_iface_t* iface = net_get_default_iface();
    if (!iface) return -1;
    memcpy(packet.chaddr, iface->mac.addr, 6);
    packet.magic = htonl(DHCP_MAGIC_COOKIE);

    /* Options */
    uint8_t* opt = packet.options;
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_REQUEST;
    
    /* Requested IP */
    *opt++ = DHCP_OPT_REQ_IP;
    *opt++ = 4;
    memcpy(opt, requested->addr, 4);
    opt += 4;
    
    /* Server identifier */
    *opt++ = DHCP_OPT_SERVER;
    *opt++ = 4;
    memcpy(opt, server->addr, 4);
    opt += 4;
    
    *opt++ = DHCP_OPT_END;
    
    dhcp_client.state = DHCP_STATE_REQUESTING;
    
    /* A client without a lease must broadcast.  Sending unicast would
     * require ARP first, while this minimal stack intentionally does not
     * queue a DHCP Request behind ARP resolution. */
    ip_addr_t broadcast = ip_make_addr(255, 255, 255, 255);
    udp_send(&broadcast, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
             &packet, sizeof(dhcp_packet_t));
    
    return 0;
}

void dhcp_handle_packet(dhcp_packet_t* packet, size_t len) {
    /* DHCP messages are variable length.  The fixed BOOTP header plus the
     * magic cookie is 240 bytes; QEMU's valid Offer is shorter than this
     * implementation's 312-byte maximum option buffer. */
    const size_t fixed_len = sizeof(dhcp_packet_t) - sizeof(packet->options);
    if (len < fixed_len) {
        return;
    }
    
    /* Check transaction ID */
    uint32_t xid = ntohl(packet->xid);
    if (xid != dhcp_client.xid) {
        return;
    }
    
    /* Check magic cookie */
    if (ntohl(packet->magic) != DHCP_MAGIC_COOKIE) {
        return;
    }
    
    /* Parse options safely */
    uint8_t* opt = packet->options;
    size_t option_len = len - fixed_len;
    if (option_len > sizeof(packet->options)) option_len = sizeof(packet->options);
    uint8_t* opt_end = packet->options + option_len;
    uint8_t msg_type = 0;

    while (opt < opt_end && *opt != DHCP_OPT_END) {
        uint8_t type = *opt++;
        if (type == DHCP_OPT_PAD) {
            continue;
        }
        if (opt >= opt_end) break;

        uint8_t opt_len = *opt++;
        if ((size_t)(opt_end - opt) < opt_len) {
            break;
        }

        switch (type) {
            case DHCP_OPT_MSG_TYPE:
                if (opt_len >= 1) msg_type = opt[0];
                break;
            case DHCP_OPT_SUBNET:
                if (opt_len >= 4) memcpy(dhcp_client.subnet_mask.addr, opt, 4);
                break;
            case DHCP_OPT_ROUTER:
                if (opt_len >= 4) memcpy(dhcp_client.gateway.addr, opt, 4);
                break;
            case DHCP_OPT_DNS:
                if (opt_len >= 4) memcpy(dhcp_client.dns[0].addr, opt, 4);
                break;
            case DHCP_OPT_LEASE:
                if (opt_len >= 4) {
                    uint32_t lease = 0;
                    memcpy(&lease, opt, sizeof(lease));
                    dhcp_client.lease_time = ntohl(lease);
                    dhcp_client.renewal_time = dhcp_client.lease_time / 2;
                    dhcp_client.rebinding_time = dhcp_client.lease_time * 7 / 8;
                }
                break;
            case DHCP_OPT_SERVER:
                if (opt_len >= 4) memcpy(dhcp_client.server_ip.addr, opt, 4);
                break;
        }

        opt += opt_len;
    }

    switch (msg_type) {
        case DHCP_OFFER:
            if (dhcp_client.state == DHCP_STATE_SELECTING) {
                memcpy(dhcp_client.offered_ip.addr, &packet->yiaddr, 4);
                /* Send request */
                dhcp_request(&dhcp_client.server_ip, &dhcp_client.offered_ip);
            }
            break;
            
        case DHCP_ACK:
            if (dhcp_client.state == DHCP_STATE_REQUESTING) {
                /* Configure interface */
                extern net_iface_t* net_get_default_iface(void);
                net_iface_t* iface = net_get_default_iface();
                if (iface) {
                    memcpy(iface->ip.addr, &packet->yiaddr, 4);
                    iface->netmask = dhcp_client.subnet_mask;
                    iface->gateway = dhcp_client.gateway;
                }

                /* dns_init() ran before DHCP completed; replace its fallback
                 * resolver with the option supplied in this lease. */
                dns_set_server(&dhcp_client.dns[0]);
                dhcp_client.state = DHCP_STATE_BOUND;
                
            }
            break;
            
        case DHCP_NAK:
            serial_puts("[DHCP] NAK received\n");
            dhcp_client.state = DHCP_STATE_INIT;
            break;
    }
}

int dhcp_poll(void) {
    switch (dhcp_client.state) {
        case DHCP_STATE_INIT:
            dhcp_discover();
            return 1;
            
        case DHCP_STATE_SELECTING:
        case DHCP_STATE_REQUESTING:
            /* Waiting for response */
            return 1;
            
        case DHCP_STATE_BOUND:
            return 0;  /* Success */
            
        default:
            return -1;
    }
}

ip_addr_t dhcp_get_ip(void) {
    return dhcp_client.offered_ip;
}

ip_addr_t dhcp_get_gateway(void) {
    return dhcp_client.gateway;
}

ip_addr_t dhcp_get_dns(void) {
    return dhcp_client.dns[0];
}
