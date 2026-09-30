/**
 * e1000.h - Intel E1000 NIC Driver
 * 
 * QEMU's default emulated network card.
 * Supports 1Gbps Ethernet with full TCP/IP offload.
 */

#ifndef E1000_H
#define E1000_H

#include "types.h"
#include "net.h"

/* PCI IDs */
#define E1000_VENDOR_ID     0x8086
#define E1000_DEVICE_ID     0x100E  // QEMU e1000
#define E1000_DEVICE_ID_82545GM 0x1026

/* Register offsets (BAR0) */
#define E1000_CTRL      0x0000  // Device control
#define E1000_CTRL_DUP  0x0004  // Device control duplicate
#define E1000_STATUS    0x0008  // Device status
#define E1000_EECD      0x0010  // EEPROM control
#define E1000_EERD      0x0014  // EEPROM read
#define E1000_CTRL_EXT  0x0018  // Extended device control
#define E1000_MDIC      0x0020  // MDI control
#define E1000_FCAL      0x0028  // Flow control address low
#define E1000_FCAH      0x002C  // Flow control address high
#define E1000_FCT       0x0030  // Flow control type
#define E1000_VET       0x0038  // VLAN ether type
#define E1000_ICR       0x00C0  // Interrupt cause read
#define E1000_ITR       0x00C4  // Interrupt throttling
#define E1000_ICS       0x00C8  // Interrupt cause set
#define E1000_IMS       0x00D0  // Interrupt mask set
#define E1000_IMC       0x00D8  // Interrupt mask clear
#define E1000_RCTL      0x0100  // Receive control
#define E1000_TCTL      0x0400  // Transmit control
#define E1000_TIPG      0x0410  // TX inter-packet gap
#define E1000_RDTR      0x2820  // RX delay timer
#define E1000_RDBAL     0x2800  // RX descriptor base low
#define E1000_RDBAH     0x2804  // RX descriptor base high
#define E1000_RDLEN     0x2808  // RX descriptor length
#define E1000_RDH       0x2810  // RX descriptor head
#define E1000_RDT       0x2818  // RX descriptor tail
#define E1000_TDBAL     0x3800  // TX descriptor base low
#define E1000_TDBAH     0x3804  // TX descriptor base high
#define E1000_TDLEN     0x3808  // TX descriptor length
#define E1000_TDH       0x3810  // TX descriptor head
#define E1000_TDT       0x3818  // TX descriptor tail
#define E1000_MTA       0x5200  // Multicast table array
#define E1000_RAL       0x5400  // Receive address low
#define E1000_RAH       0x5404  // Receive address high
#define E1000_MTA_LEN   128

/* Control register bits */
#define E1000_CTRL_FD       0x00000001  // Full duplex
#define E1000_CTRL_GIO_MD   0x00000004  // GIO master disable
#define E1000_CTRL_LRST     0x00000008  // Link reset
#define E1000_CTRL_SLU      0x00000040  // Set link up
#define E1000_CTRL_ILOS     0x00000080  // Invert loss-of-signal
#define E1000_CTRL_SPEED    0x00000300  // Speed mask
#define E1000_CTRL_FRCSPD   0x00000800  // Force speed
#define E1000_CTRL_FRCDPX   0x00001000  // Force duplex
#define E1000_CTRL_RST      0x04000000  // Device reset
#define E1000_CTRL_VME      0x40000000  // VLAN mode enable

/* Receive control bits */
#define E1000_RCTL_EN       0x00000002  // RX enable
#define E1000_RCTL_SBP      0x00000004  // Store bad packets
#define E1000_RCTL_UPE      0x00000008  // Unicast promiscuous
#define E1000_RCTL_MPE      0x00000010  // Multicast promiscuous
#define E1000_RCTL_LPE      0x00000020  // Long packet enable
#define E1000_RCTL_LBM      0x000000C0  // Loopback mode
#define E1000_RCTL_RDMTS    0x00000300  // RX desc min thresh
#define E1000_RCTL_BAM      0x00008000  // Broadcast accept
#define E1000_RCTL_BSIZE    0x00030000  // Buffer size
#define E1000_RCTL_BSEX     0x02000000  // Buffer size extension
#define E1000_RCTL_SECRC    0x04000000  // Strip CRC

/* Transmit control bits */
#define E1000_TCTL_EN       0x00000002  // TX enable
#define E1000_TCTL_PSP      0x00000008  // Pad short packets
#define E1000_TCTL_CT       0x00000FF0  // Collision threshold
#define E1000_TCTL_COLD     0x003FF000  // Collision distance
#define E1000_TCTL_RTLC     0x01000000  // ReTX on late collision
#define E1000_TCTL_NRTU     0x02000000  // No reTX on underrun

/* Interrupt bits */
#define E1000_ICR_TXDW      0x00000001  // TX descriptor written
#define E1000_ICR_TXQE      0x00000002  // TX queue empty
#define E1000_ICR_LSC       0x00000004  // Link status change
#define E1000_ICR_RXSEQ     0x00000008  // RX sequence error
#define E1000_ICR_RXDMT0   0x00000010  // RX desc min threshold
#define E1000_ICR_RXO      0x00000040  // RX overrun
#define E1000_ICR_RXT0      0x00000080  // RX timer interrupt
#define E1000_ICR_MDAC     0x00000200  // MDIO access complete
#define E1000_ICR_RXCFG    0x00000400  // RX config interrupt
#define E1000_ICR_GPI      0x00000800  // General purpose
#define E1000_ICR_PHYINT   0x00001000  // PHY interrupt
#define E1000_ICR_ACK      0x00020000  // ACK interrupt

/* Descriptor flags */
#define E1000_TXD_CMD_EOP   0x01  // End of packet
#define E1000_TXD_CMD_IFCS  0x02  // Insert FCS/CRC
#define E1000_TXD_CMD_IC    0x04  // Insert checksum
#define E1000_TXD_CMD_RS    0x08  // Report status
#define E1000_TXD_CMD_RPS   0x10  // Report packet sent
#define E1000_TXD_CMD_DEXT  0x20  // Desc extension
#define E1000_TXD_CMD_VLE   0x40  // VLAN packet enable
#define E1000_TXD_CMD_IDE   0x80  // Interrupt delay enable

#define E1000_TXD_STAT_DD   0x01  // Descriptor done
#define E1000_TXD_STAT_EC   0x02  // Excess collisions
#define E1000_TXD_STAT_LC   0x04  // Late collision
#define E1000_TXD_STAT_TU   0x08  // Transmit underrun

#define E1000_RXD_STAT_DD   0x01  // Descriptor done
#define E1000_RXD_STAT_EOP  0x02  // End of packet
#define E1000_RXD_STAT_IXSM 0x04  // Ignore checksum
#define E1000_RXD_STAT_VP   0x08  // VLAN packet
#define E1000_RXD_STAT_UDPCS 0x10  // UDP checksum
#define E1000_RXD_STAT_TCPCS 0x20  // TCP checksum
#define E1000_RXD_STAT_IPCS 0x40  // IP checksum
#define E1000_RXD_STAT_PIF  0x80  // Passed in filter

/* Ring sizes */
#define E1000_TX_RING_SIZE  256
#define E1000_RX_RING_SIZE  256
#define E1000_TX_BUFF_SIZE  2048
#define E1000_RX_BUFF_SIZE  2048

/* TX descriptor */
typedef struct __attribute__((packed)) {
    uint64_t addr;      // Buffer address
    uint16_t length;    // Data length
    uint8_t  cso;       // Checksum offset
    uint8_t  cmd;       // Command
    uint8_t  status;    // Status
    uint8_t  css;       // Checksum start
    uint16_t special;   // Special
} e1000_tx_desc_t;

/* RX descriptor */
typedef struct __attribute__((packed)) {
    uint64_t addr;      // Buffer address
    uint16_t length;    // Data length
    uint16_t csum;      // Packet checksum
    uint8_t  status;    // Status
    uint8_t  errors;    // Errors
    uint16_t special;   // Special
} e1000_rx_desc_t;

/* E1000 device structure */
typedef struct {
    uint64_t io_base;       // MMIO base
    uint64_t irq;           // IRQ line
    
    eth_addr_t mac;         // MAC address
    
    /* TX ring */
    e1000_tx_desc_t* tx_descs;
    uint8_t* tx_buffers[E1000_TX_RING_SIZE];
    uint64_t tx_tail;
    
    /* RX ring */
    e1000_rx_desc_t* rx_descs;
    uint8_t* rx_buffers[E1000_RX_RING_SIZE];
    uint64_t rx_tail;
    
    /* Stats */
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
} e1000_device_t;

/* Function prototypes */
int e1000_init(void);
int e1000_probe(uint64_t io_base, uint8_t irq);
int e1000_reset(e1000_device_t* dev);
void e1000_start(e1000_device_t* dev);
void e1000_stop(e1000_device_t* dev);

/* MAC address */
void e1000_read_mac(e1000_device_t* dev);
void e1000_set_mac(e1000_device_t* dev, eth_addr_t* mac);

/* Packet I/O */
int e1000_send_packet(e1000_device_t* dev, void* data, size_t len);
int e1000_recv_packet(e1000_device_t* dev, void* buf, size_t max_len);
void e1000_handle_interrupt(e1000_device_t* dev);
/* Arm legacy INTx only after the GUI scheduler's first visible frame. */
void e1000_enable_runtime_irq(void);

/* Register access */
uint32_t e1000_read_reg(e1000_device_t* dev, uint32_t reg);
void e1000_write_reg(e1000_device_t* dev, uint32_t reg, uint32_t value);

/* Link status */
int e1000_link_up(e1000_device_t* dev);
uint32_t e1000_link_speed(e1000_device_t* dev);

#endif /* E1000_H */
