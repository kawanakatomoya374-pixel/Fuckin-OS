/**
 * arp.h - ARP Protocol
 */

#ifndef ARP_H
#define ARP_H

#include "types.h"
#include "net.h"

#define ARP_TABLE_SIZE  16

/* ARP entry */
typedef struct {
    ip_addr_t ip;
    eth_addr_t mac;
    uint64_t timestamp;
    int valid;
} arp_entry_t;

/* ARP operations (defined in net.h) */
#define ARP_OP_REQUEST    1
#define ARP_OP_REPLY      2

/* Function prototypes */
void arp_init(void);
int arp_lookup(ip_addr_t* ip, eth_addr_t* mac);
void arp_insert(ip_addr_t* ip, eth_addr_t* mac);
void arp_send_request(ip_addr_t* target_ip);
void arp_send_reply(ip_addr_t* target_ip, eth_addr_t* target_mac);
void arp_handle_packet(arp_packet_t* packet, size_t len);
void arp_table_dump(void);

#endif /* ARP_H */
