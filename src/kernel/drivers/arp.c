/**
 * arp.c - ARP Protocol Implementation
 */

#include "arp.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"

/* Byte order conversion functions (declarations) */
static uint16_t htons(uint16_t n);
static uint16_t ntohs(uint16_t n);

static arp_entry_t arp_table[ARP_TABLE_SIZE];
static int arp_entry_count = 0;
/* Transport workers perform lookups while packet ingress inserts resolved
 * peers. Keep cache reads and replacement atomic, without holding the lock
 * across Ethernet frame construction or transmission. */
static volatile uint32_t arp_table_lock = 0;

static void arp_lock_acquire(void) {
    while (__atomic_exchange_n(&arp_table_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile("pause");
    }
}

static void arp_lock_release(void) {
    __atomic_store_n(&arp_table_lock, 0u, __ATOMIC_RELEASE);
}

void arp_init(void) {
    __atomic_store_n(&arp_table_lock, 0u, __ATOMIC_RELEASE);
    memset(arp_table, 0, sizeof(arp_table));
    arp_entry_count = 0;
    serial_puts("[ARP] ARP table initialized\n");
}

int arp_lookup(ip_addr_t* ip, eth_addr_t* mac) {
    if (!ip || !mac) return -1;
    arp_lock_acquire();
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && ip_addr_cmp(&arp_table[i].ip, ip) == 0) {
            *mac = arp_table[i].mac;
            arp_lock_release();
            return 0;
        }
    }
    arp_lock_release();
    return -1;  // Not found
}

void arp_insert(ip_addr_t* ip, eth_addr_t* mac) {
    if (!ip || !mac) return;
    arp_lock_acquire();
    /* Check if entry already exists */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && ip_addr_cmp(&arp_table[i].ip, ip) == 0) {
            /* Update existing entry */
            arp_table[i].mac = *mac;
            arp_table[i].timestamp = get_timer_ticks();
            arp_lock_release();
            return;
        }
    }
    
    /* Find empty slot or oldest entry */
    int slot = -1;
    uint64_t oldest = 0xFFFFFFFF;
    
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            slot = i;
            break;
        }
        if (arp_table[i].timestamp < oldest) {
            oldest = arp_table[i].timestamp;
            slot = i;
        }
    }
    
    if (slot >= 0) {
        arp_table[slot].ip = *ip;
        arp_table[slot].mac = *mac;
        arp_table[slot].timestamp = get_timer_ticks();
        arp_table[slot].valid = 1;
        arp_entry_count++;
    }
    arp_lock_release();
}

void arp_send_request(ip_addr_t* target_ip) {
    arp_packet_t packet;
    
    packet.htype = htons(1);       // Ethernet
    packet.ptype = htons(0x0800);  // IPv4
    packet.hlen = 6;
    packet.plen = 4;
    packet.oper = htons(ARP_OP_REQUEST);
    
    /* Get our MAC and IP */
    extern net_iface_t* net_get_default_iface(void);
    net_iface_t* iface = net_get_default_iface();
    if (!iface) return;
    
    packet.sha = iface->mac;
    memcpy(packet.spa.addr, iface->ip.addr, IP_ADDR_LEN);
    
    /* Target */
    memset(&packet.tha, 0, sizeof(eth_addr_t));
    memcpy(packet.tpa.addr, target_ip->addr, IP_ADDR_LEN);
    
    /* Broadcast */
    eth_addr_t broadcast;
    memset(&broadcast, 0xFF, sizeof(broadcast));
    
    eth_send(iface, &broadcast, 0x0806, &packet, sizeof(packet));
}

void arp_send_reply(ip_addr_t* target_ip, eth_addr_t* target_mac) {
    arp_packet_t packet;
    
    packet.htype = htons(1);
    packet.ptype = htons(0x0800);
    packet.hlen = 6;
    packet.plen = 4;
    packet.oper = htons(ARP_OP_REPLY);
    
    extern net_iface_t* net_get_default_iface(void);
    net_iface_t* iface = net_get_default_iface();
    if (!iface) return;
    
    packet.sha = iface->mac;
    memcpy(packet.spa.addr, iface->ip.addr, IP_ADDR_LEN);
    packet.tha = *target_mac;
    memcpy(packet.tpa.addr, target_ip->addr, IP_ADDR_LEN);
    
    eth_send(iface, target_mac, 0x0806, &packet, sizeof(packet));
}

void arp_handle_packet(arp_packet_t* packet, size_t len) {
    if (len < sizeof(arp_packet_t)) return;
    
    uint16_t oper = ntohs(packet->oper);
    
    ip_addr_t sender_ip = packet->spa;
    
    /* Add sender to ARP table */
    arp_insert(&sender_ip, &packet->sha);
    
    if (oper == ARP_OP_REQUEST) {
        /* Check if request is for us */
        extern net_iface_t* net_get_default_iface(void);
        net_iface_t* iface = net_get_default_iface();
        if (!iface) return;
        
        ip_addr_t target_ip = packet->tpa;
        
        if (ip_addr_cmp(&target_ip, &iface->ip) == 0) {
            /* Send reply */
            arp_send_reply(&sender_ip, &packet->sha);
        }
    }
}

void arp_table_dump(void) {
    serial_puts("=== ARP Table ===\n");
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid) {
            serial_puts("  ");
            for (int j = 0; j < 4; j++) {
                serial_putdec(arp_table[i].ip.addr[j]);
                if (j < 3) serial_puts(".");
            }
            serial_puts(" -> ");
            for (int j = 0; j < 6; j++) {
                serial_puthex(arp_table[i].mac.addr[j]);
                if (j < 5) serial_puts(":");
            }
            serial_puts("\n");
        }
    }
    serial_puts("=================\n");
}

/* Helper: htons */
static uint16_t htons(uint16_t n) {
    return ((n & 0xFF) << 8) | ((n >> 8) & 0xFF);
}

/* Helper: ntohs */
static uint16_t ntohs(uint16_t n) {
    return htons(n);
}

/* Helper: htonl */
uint32_t htonl(uint32_t n) {
    return ((n & 0xFF) << 24) |
           ((n & 0xFF00) << 8) |
           ((n >> 8) & 0xFF00) |
           ((n >> 24) & 0xFF);
}

/* Helper: ntohl */
uint32_t ntohl(uint32_t n) {
    return htonl(n);
}
