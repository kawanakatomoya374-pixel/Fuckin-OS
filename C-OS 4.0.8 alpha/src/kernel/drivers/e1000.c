/**
 * e1000.c - Intel E1000 NIC Driver Implementation
 * 
 * Full driver for QEMU's e1000 emulation.
 */

#include "e1000.h"
#include "irq.h"
#include "idt.h"
#include "io.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "pci.h"
#include "mm/paging.h"
#include "memory_physical.h"

/* 64-bit I/O macros if not defined */
#ifndef outl
#define outl(port, val) \
    __asm__ __volatile__ ("outl %%eax, %%dx" : : "a"((uint32_t)(val)), "d"((uint32_t)(port)))
#endif
#ifndef inl
#define inl(port) ({ \
    uint32_t val; \
    __asm__ __volatile__ ("inl %%dx, %%eax" : "=a"(val) : "d"((uint32_t)(port))); \
    val; \
})
#endif

/* Aligned kmalloc helper */
/* kmalloc_aligned is in memory.h */
static void* e1000_kmalloc_aligned_unused(size_t size, size_t align) {
    (void)align;
    return kmalloc(size);
}

static e1000_device_t* e1000_dev = NULL;
static net_iface_t* net_iface = NULL;
/* Set by the PCI IRQ handler; packet parsing remains outside interrupt
 * context so TCP/ARP/DHCP never allocate memory or invoke callbacks on an
 * interrupt stack. */
static volatile bool e1000_rx_irq_pending = false;
static volatile bool e1000_irq_runtime_enabled = false;
/* Diagnostics are observed only from the deferred poll context; never print
 * from the hard IRQ path. */
static volatile uint64_t e1000_irq_count = 0;
static volatile uint64_t e1000_deferred_frames = 0;
static volatile uint32_t e1000_last_irq_cause = 0;

/* Transport workers may submit TX while the sole network dispatcher drains
 * RX. Protect descriptor ownership independently: these locks never cover
 * protocol parsing, scheduler waits, or callback execution. */
static volatile uint32_t e1000_tx_ring_lock = 0;
static volatile uint32_t e1000_rx_ring_lock = 0;

static void e1000_ring_lock_acquire(volatile uint32_t *lock) {
    while (__atomic_exchange_n(lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile("pause");
    }
}

static void e1000_ring_lock_release(volatile uint32_t *lock) {
    __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
}

static bool e1000_dma_addr(void* ptr, uint64_t* out_phys) {
    if (!ptr || !out_phys) return false;
    uint64_t phys = paging_virt_to_phys((uint64_t)(uintptr_t)ptr);
    if (!phys) return false;
    *out_phys = phys;
    return true;
}

static void e1000_unmap_mmio_range(uint64_t io_base);

static void e1000_unmap_mmio_range(uint64_t io_base) {
    if (io_base & 1) {
        return;
    }
    uint64_t base = io_base & ~0x0FULL;
    for (uint64_t off = 0; off < 0x20000ULL; off += PAGE_SIZE) {
        paging_unmap_page((virt_addr_t)(base + off));
    }
}

static void e1000_free_tx_ring(e1000_device_t* dev) {
    if (!dev) return;
    for (int i = 0; i < E1000_TX_RING_SIZE; i++) {
        if (dev->tx_buffers[i]) {
            kfree(dev->tx_buffers[i]);
            dev->tx_buffers[i] = NULL;
        }
    }
    if (dev->tx_descs) {
        kfree(dev->tx_descs);
        dev->tx_descs = NULL;
    }
    dev->tx_tail = 0;
}

static void e1000_free_rx_ring(e1000_device_t* dev) {
    if (!dev) return;
    for (int i = 0; i < E1000_RX_RING_SIZE; i++) {
        if (dev->rx_buffers[i]) {
            kfree(dev->rx_buffers[i]);
            dev->rx_buffers[i] = NULL;
        }
    }
    if (dev->rx_descs) {
        kfree(dev->rx_descs);
        dev->rx_descs = NULL;
    }
    dev->rx_tail = 0;
}

/* Register read/write helpers */
uint32_t e1000_read_reg(e1000_device_t* dev, uint32_t reg) {
    if (dev->io_base & 1) {
        // PIO
        uint32_t io_port = (uint32_t)(dev->io_base & ~1);
        outl(io_port, reg);
        return inl(io_port + 4);
    } else {
        // MMIO
        volatile uint32_t* addr = (volatile uint32_t*)(uintptr_t)(dev->io_base + reg);
        return *addr;
    }
}

void e1000_write_reg(e1000_device_t* dev, uint32_t reg, uint32_t value) {
    if (dev->io_base & 1) {
        // PIO
        uint32_t io_port = (uint32_t)(dev->io_base & ~1);
        outl(io_port, reg);
        outl(io_port + 4, value);
    } else {
        // MMIO
        volatile uint32_t* addr = (volatile uint32_t*)(uintptr_t)(dev->io_base + reg);
        *addr = value;
    }
}

/* Read MAC address from EEPROM */
void e1000_read_mac(e1000_device_t* dev) {
    /* Read from EEPROM */
    uint32_t ral = e1000_read_reg(dev, E1000_RAL);
    uint32_t rah = e1000_read_reg(dev, E1000_RAH);
    dev->mac.addr[0] = ral & 0xFF;
    dev->mac.addr[1] = (ral >> 8) & 0xFF;
    dev->mac.addr[2] = (ral >> 16) & 0xFF;
    dev->mac.addr[3] = (ral >> 24) & 0xFF;
    dev->mac.addr[4] = rah & 0xFF;
    dev->mac.addr[5] = (rah >> 8) & 0xFF;
    
    serial_puts("[E1000] MAC: ");
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        serial_puthex(dev->mac.addr[i]);
        if (i < ETH_ADDR_LEN - 1) serial_puts(":");
    }
    serial_puts("\n");
}

/* Reset the device */
int e1000_reset(e1000_device_t* dev) {
    if (!dev) return -1;

    uint32_t ctrl = e1000_read_reg(dev, E1000_CTRL);
    e1000_write_reg(dev, E1000_CTRL, ctrl | E1000_CTRL_RST);

    /* Wait for reset to complete, but never forever. */
    uint64_t timeout = 1000000ULL;
    while ((e1000_read_reg(dev, E1000_CTRL) & E1000_CTRL_RST) && timeout--) {
        /* Spin */
    }
    if (!timeout) {
        serial_puts("[E1000] Reset timeout\n");
        return -1;
    }

    /* Wait for EEPROM auto-read, but never forever. */
    timeout = 1000000ULL;
    while (!(e1000_read_reg(dev, E1000_EECD) & 0x01) && timeout--) {
        /* Spin */
    }
    if (!timeout) {
        serial_puts("[E1000] EEPROM auto-read timeout\n");
        return -1;
    }

    serial_puts("[E1000] Device reset complete\n");
    return 0;
}

/* Initialize TX ring */
static void e1000_init_tx(e1000_device_t* dev) {
    if (!dev) return;
    e1000_free_tx_ring(dev);

    /* Allocate descriptor ring on a page boundary so DMA gets a stable
     * physically-backed buffer instead of an arbitrary heap chunk. */
    dev->tx_descs = (e1000_tx_desc_t*)kmalloc_pages(1);
    if (!dev->tx_descs) {
        serial_puts("[E1000] TX descriptor allocation failed\n");
        return;
    }
    memset(dev->tx_descs, 0, PAGE_SIZE);

    /* Allocate TX buffers as page-backed memory as well; each packet
     * buffer is a full page so the NIC always sees a physically
     * contiguous DMA region. */
    for (int i = 0; i < E1000_TX_RING_SIZE; i++) {
        dev->tx_buffers[i] = (uint8_t*)kmalloc_pages(1);
        if (!dev->tx_buffers[i]) {
            serial_puts("[E1000] TX buffer allocation failed\n");
            e1000_free_tx_ring(dev);
            return;
        }
        memset(dev->tx_buffers[i], 0, PAGE_SIZE);

        uint64_t dma_addr = 0;
        if (!e1000_dma_addr(dev->tx_buffers[i], &dma_addr)) {
            serial_puts("[E1000] TX buffer DMA mapping failed\n");
            e1000_free_tx_ring(dev);
            return;
        }
        dev->tx_descs[i].addr = dma_addr;
        dev->tx_descs[i].status = E1000_TXD_STAT_DD;
    }

    uint64_t tx_ring_phys = 0;
    if (!e1000_dma_addr(dev->tx_descs, &tx_ring_phys)) {
        serial_puts("[E1000] TX ring DMA mapping failed\n");
        e1000_free_tx_ring(dev);
        return;
    }

    e1000_write_reg(dev, E1000_TDBAL, (uint32_t)(tx_ring_phys & 0xFFFFFFFFu));
    e1000_write_reg(dev, E1000_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    e1000_write_reg(dev, E1000_TDLEN, PAGE_SIZE);

    /* Set head and tail */
    e1000_write_reg(dev, E1000_TDH, 0);
    e1000_write_reg(dev, E1000_TDT, 0);
    dev->tx_tail = 0;

    serial_puts("[E1000] TX ring initialized\n");
}

/* Initialize RX ring */
static void e1000_init_rx(e1000_device_t* dev) {
    if (!dev) return;
    e1000_free_rx_ring(dev);

    /* Allocate descriptor ring on a page boundary. */
    dev->rx_descs = (e1000_rx_desc_t*)kmalloc_pages(1);
    if (!dev->rx_descs) {
        serial_puts("[E1000] RX descriptor allocation failed\n");
        return;
    }
    memset(dev->rx_descs, 0, PAGE_SIZE);

    /* Allocate RX buffers as page-backed memory too. */
    for (int i = 0; i < E1000_RX_RING_SIZE; i++) {
        dev->rx_buffers[i] = (uint8_t*)kmalloc_pages(1);
        if (!dev->rx_buffers[i]) {
            serial_puts("[E1000] RX buffer allocation failed\n");
            e1000_free_rx_ring(dev);
            return;
        }
        memset(dev->rx_buffers[i], 0, PAGE_SIZE);

        uint64_t dma_addr = 0;
        if (!e1000_dma_addr(dev->rx_buffers[i], &dma_addr)) {
            serial_puts("[E1000] RX buffer DMA mapping failed\n");
            e1000_free_rx_ring(dev);
            return;
        }
        dev->rx_descs[i].addr = dma_addr;
        dev->rx_descs[i].status = 0;
    }

    uint64_t rx_ring_phys = 0;
    if (!e1000_dma_addr(dev->rx_descs, &rx_ring_phys)) {
        serial_puts("[E1000] RX ring DMA mapping failed\n");
        e1000_free_rx_ring(dev);
        return;
    }

    e1000_write_reg(dev, E1000_RDBAL, (uint32_t)(rx_ring_phys & 0xFFFFFFFFu));
    e1000_write_reg(dev, E1000_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    e1000_write_reg(dev, E1000_RDLEN, PAGE_SIZE);

    /* Set head and tail */
    e1000_write_reg(dev, E1000_RDH, 0);
    e1000_write_reg(dev, E1000_RDT, E1000_RX_RING_SIZE - 1);
    dev->rx_tail = 0;

    serial_puts("[E1000] RX ring initialized\n");
}

/* The E1000's INTx line remains asserted until ICR is read.  Never
 * unmask the legacy PIC before masking the NIC and consuming any reset/link
 * causes, otherwise QEMU can deliver IRQ11 continuously during SMP startup. */
static uint32_t e1000_disable_interrupts(e1000_device_t* dev) {
    if (dev == NULL) return 0;
    e1000_write_reg(dev, E1000_IMC, 0xFFFFFFFFu);
    return e1000_read_reg(dev, E1000_ICR); /* read-to-clear */
}

/* Enable only receive causes. Link-status notification is deliberately left
 * masked during bootstrap: QEMU raises LSC as soon as IMS is written, which
 * can level-assert legacy INTx before the first packet. net_poll() samples
 * link state outside IRQ context, so omitting LSC loses no receive work. */
static void e1000_enable_interrupts(e1000_device_t* dev) {
    if (dev == NULL) return;
    (void)e1000_read_reg(dev, E1000_ICR); /* discard stale causes first */
    e1000_write_reg(dev, E1000_IMS,
        E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO);
}

/* Clear multicast filter (accept all multicast) */
static void e1000_clear_mta(e1000_device_t* dev) {
    for (int i = 0; i < E1000_MTA_LEN; i++) {
        e1000_write_reg(dev, E1000_MTA + i * 4, 0);
    }
}

/* Start the device */
void e1000_start(e1000_device_t* dev) {
    if (!dev) return;

    /* Initialize TX */
    e1000_init_tx(dev);
    if (!dev->tx_descs) {
        serial_puts("[E1000] TX initialization failed\n");
        return;
    }

    /* Program TX control */
    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP;
    tctl |= (15 << 12);  // Collision threshold
    tctl |= (64 << 6);   // Collision distance
    e1000_write_reg(dev, E1000_TCTL, tctl);

    /* Initialize RX */
    e1000_init_rx(dev);
    if (!dev->rx_descs) {
        serial_puts("[E1000] RX initialization failed\n");
        e1000_free_tx_ring(dev);
        return;
    }

    /* Set RX control */
    /* Accept broadcast DHCP plus the interface's unicast traffic.  Keeping
     * UPE/MPE enabled avoids depending on a partially initialised receive
     * address filter during early boot. */
    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_BAM |
                    E1000_RCTL_UPE | E1000_RCTL_MPE;
    rctl |= E1000_RCTL_SECRC;  // Strip CRC
    /* Legacy RCTL BSIZE=00 is 2048 bytes.  The old value 3 selected
     * 256-byte buffers, causing DHCP replies and ordinary Ethernet frames
     * to be dropped/truncated despite 4KiB DMA pages being allocated. */
    rctl |= (0u << 16);
    e1000_write_reg(dev, E1000_RCTL, rctl);

    /* Clear MTA */
    e1000_clear_mta(dev);

    /* Keep NIC interrupts masked until e1000_init() has installed its
     * IRQ11 handler and cleared reset/link causes. */
    e1000_disable_interrupts(dev);

    /* Set link up */
    uint32_t ctrl = e1000_read_reg(dev, E1000_CTRL);
    ctrl |= E1000_CTRL_SLU;
    e1000_write_reg(dev, E1000_CTRL, ctrl);

    serial_puts("[E1000] Device started\n");
}

/* Stop the device */
void e1000_stop(e1000_device_t* dev) {
    /* Disable TX/RX */
    e1000_write_reg(dev, E1000_RCTL, 0);
    e1000_write_reg(dev, E1000_TCTL, 0);
    
    serial_puts("[E1000] Device stopped\n");
}

/* Send a packet */
int e1000_send_packet(e1000_device_t* dev, void* data, size_t len) {
    if (!dev || !data || !dev->tx_descs || !dev->tx_buffers[0]) {
        serial_puts("[E1000] TX not initialized\n");
        return -1;
    }
    if (len == 0 || len > E1000_TX_BUFF_SIZE) {
        serial_puts("[E1000] Packet too large\n");
        return -1;
    }

    e1000_ring_lock_acquire(&e1000_tx_ring_lock);
    uint32_t tail = (uint32_t)dev->tx_tail;
    if (tail >= E1000_TX_RING_SIZE || !dev->tx_buffers[tail]) {
        e1000_ring_lock_release(&e1000_tx_ring_lock);
        serial_puts("[E1000] TX buffer missing\n");
        return -1;
    }

    e1000_tx_desc_t* desc = &dev->tx_descs[tail];

    /* Check if descriptor is available */
    if (!(desc->status & E1000_TXD_STAT_DD)) {
        /* TX queue full */
        e1000_ring_lock_release(&e1000_tx_ring_lock);
        return -1;
    }

    /* Copy data to TX buffer */
    memcpy(dev->tx_buffers[tail], data, len);

    /* Setup descriptor */
    desc->length = (uint16_t)len;
    desc->cso = 0;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;
    desc->css = 0;
    desc->special = 0;

    /* Update tail */
    tail = (tail + 1) % E1000_TX_RING_SIZE;
    dev->tx_tail = tail;
    e1000_write_reg(dev, E1000_TDT, tail);

    dev->tx_packets++;
    dev->tx_bytes += len;
    e1000_ring_lock_release(&e1000_tx_ring_lock);

    return 0;
}

/* Receive a packet */
int e1000_recv_packet(e1000_device_t* dev, void* buf, size_t max_len) {
    if (!dev || !buf || !dev->rx_descs || !dev->rx_buffers[0] || max_len == 0)
        return -1;

    e1000_ring_lock_acquire(&e1000_rx_ring_lock);
    uint32_t tail = (uint32_t)dev->rx_tail;
    if (tail >= E1000_RX_RING_SIZE || !dev->rx_buffers[tail]) {
        e1000_ring_lock_release(&e1000_rx_ring_lock);
        return -1;
    }

    e1000_rx_desc_t* desc = &dev->rx_descs[tail];

    /* Check if packet available */
    if (!(desc->status & E1000_RXD_STAT_DD)) {
        e1000_ring_lock_release(&e1000_rx_ring_lock);
        return 0;  /* No packet */
    }

    /* Check for errors */
    if (desc->errors) {
        desc->status = 0;  /* Clear status */
        desc->errors = 0;
        /* RDT names the last descriptor returned to the NIC.  Returning
         * `next_to_clean` would make RDT equal RDH after one packet and
         * temporarily starve the ring; return the descriptor just cleaned. */
        e1000_write_reg(dev, E1000_RDT, tail);
        dev->rx_tail = (tail + 1) % E1000_RX_RING_SIZE;
        e1000_ring_lock_release(&e1000_rx_ring_lock);
        return -1;
    }

    /* Get packet length */
    uint32_t len = desc->length;
    if (len > max_len) {
        len = (uint32_t)max_len;
    }
    /* Defense in depth: never read past the actual DMA buffer, even if a
     * corrupted or malicious descriptor claims a larger length than the
     * hardware should ever produce. */
    if (len > PAGE_SIZE) {
        len = PAGE_SIZE;
    }

    /* Copy data */
    memcpy(buf, dev->rx_buffers[tail], len);

    /* Clear status */
    desc->status = 0;
    desc->errors = 0;

    /* Return precisely the descriptor just cleaned, then advance the
     * software next-to-clean cursor.  RDT is a producer/ownership index,
     * not the index of the next descriptor the CPU will inspect. */
    e1000_write_reg(dev, E1000_RDT, tail);
    dev->rx_tail = (tail + 1) % E1000_RX_RING_SIZE;

    dev->rx_packets++;
    dev->rx_bytes += len;
    e1000_ring_lock_release(&e1000_rx_ring_lock);

    return (int)len;
}

/* Check link status */
int e1000_link_up(e1000_device_t* dev) {
    uint32_t status = e1000_read_reg(dev, E1000_STATUS);
    return (status & 0x02) != 0;
}

/* Get link speed */
uint32_t e1000_link_speed(e1000_device_t* dev) {
    uint32_t status = e1000_read_reg(dev, E1000_STATUS);
    uint32_t speed = (status >> 6) & 0x03;
    
    switch (speed) {
        case 0: return 10;      // 10 Mbps
        case 1: return 100;     // 100 Mbps
        case 2: return 1000;    // 1 Gbps
        default: return 0;
    }
}

/* NAPI-style hard IRQ entry: mask the NIC, acknowledge its cause, and
 * publish one deferred drain request. A legacy INTx line is level-triggered;
 * leaving IMS armed while the CPU is still in the interrupt path lets QEMU
 * re-deliver vector 0x2B indefinitely. net_poll() drains descriptors and
 * explicitly rearms receive causes after the line is quiet. */
void e1000_handle_interrupt(e1000_device_t* dev) {
    if (dev == NULL) return;
    /* PIC EOI is issued by _irq_handler before this callback. Mask the
     * legacy line before returning so a still-asserted INTx level cannot be
     * accepted again between EOI and the deferred descriptor drain. */
    if (dev->irq < 16u) irq_set_mask((uint8_t)dev->irq);
    uint32_t cause = e1000_disable_interrupts(dev);
    __atomic_store_n(&e1000_last_irq_cause, cause, __ATOMIC_RELAXED);
    __atomic_fetch_add(&e1000_irq_count, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&e1000_rx_irq_pending, true, __ATOMIC_RELEASE);
}

static void e1000_irq_handler(struct regs *r)
{
    (void)r;
    if (e1000_dev != NULL) e1000_handle_interrupt(e1000_dev);
}

/* Network interface send function */
static int e1000_iface_send(net_iface_t* iface, void* data, size_t len) {
    (void)iface;
    if (!e1000_dev) return -1;
    return e1000_send_packet(e1000_dev, data, len);
}

/* Network interface receive function */
static int e1000_iface_recv(net_iface_t* iface, void* buf, size_t len) {
    (void)iface;
    if (!e1000_dev) return -1;
    return e1000_recv_packet(e1000_dev, buf, len);
}

/* Probe for e1000 device */
int e1000_probe(uint64_t io_base, uint8_t irq) {
    serial_puts("[E1000] Probing device at 0x");
    serial_puthex(io_base);
    serial_puts(" IRQ ");
    serial_putdec(irq);
    serial_puts("\n");

    /* Allocate device structure */
    e1000_dev = (e1000_device_t*)kmalloc(sizeof(e1000_device_t));
    if (!e1000_dev) {
        serial_puts("[E1000] Failed to allocate device\n");
        return -1;
    }

    memset(e1000_dev, 0, sizeof(e1000_device_t));
    __atomic_store_n(&e1000_tx_ring_lock, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&e1000_rx_ring_lock, 0u, __ATOMIC_RELEASE);
    e1000_dev->io_base = io_base;
    e1000_dev->irq = irq;

    /* Map MMIO range (E1000 registers) if not using PIO */
    if (!(io_base & 1)) {
        if (!paging_map_range(io_base, io_base, 0x20000, PAGE_PRESENT | PAGE_RW | PAGE_NOCACHE)) {
            serial_puts("[E1000] Failed to map MMIO range\n");
            kfree(e1000_dev);
            e1000_dev = NULL;
            return -1;
        }
        phys_memory_reserve_range((phys_addr_t)io_base, 0x20000);
    }

    /* Reset device */
    if (e1000_reset(e1000_dev) < 0) {
        e1000_unmap_mmio_range(io_base);
        phys_memory_unreserve_range((phys_addr_t)io_base, 0x20000);
        kfree(e1000_dev);
        e1000_dev = NULL;
        return -1;
    }

    /* Read MAC address */
    e1000_read_mac(e1000_dev);

    /* Start device */
    e1000_start(e1000_dev);
    if (!e1000_dev->tx_descs || !e1000_dev->rx_descs) {
        serial_puts("[E1000] Device failed to start\n");
        e1000_stop(e1000_dev);
        e1000_free_tx_ring(e1000_dev);
        e1000_free_rx_ring(e1000_dev);
        e1000_unmap_mmio_range(io_base);
        kfree(e1000_dev);
        e1000_dev = NULL;
        return -1;
    }

    /* Register network interface */
    net_iface = net_iface_register("eth0", &e1000_dev->mac);
    if (net_iface) {
        net_iface->send = e1000_iface_send;
        net_iface->recv = e1000_iface_recv;

        /* Set default IP (can be changed via DHCP later) */
        net_iface->ip = ip_make_addr(10, 0, 2, 15);  // QEMU default
        net_iface->netmask = ip_make_addr(255, 255, 255, 0);
        net_iface->gateway = ip_make_addr(10, 0, 2, 2);  // QEMU gateway
    }

    serial_puts("[E1000] Device initialized successfully\n");
    return 0;
}

/* Initialize e1000 driver */
int e1000_init(void) {
    serial_puts("[E1000] E1000 driver initializing...\n");
    
    /* Look for e1000 device in PCI */
    pci_dev_t* pci_dev = pci_get_device(E1000_VENDOR_ID, E1000_DEVICE_ID);
    if (!pci_dev) {
        pci_dev = pci_get_device(E1000_VENDOR_ID, E1000_DEVICE_ID_82545GM);
    }
    
    if (!pci_dev) {
        serial_puts("[E1000] No E1000 device found\n");
        return -1;
    }
    
    serial_puts("[E1000] Found device on bus ");
    serial_putdec(pci_dev->bus);
    serial_puts(" slot ");
    serial_putdec(pci_dev->slot);
    serial_puts("\n");
    
    /* Get IO base from BAR0. Check if it's IO or MMIO. */
    uint64_t io_base = pci_dev->bar[0];
    if (io_base & 1) {
        io_base &= ~0x03; // PIO
    } else {
        io_base &= ~0x0F; // MMIO
    }
    uint8_t irq = pci_dev->irq_line;
    
    /* Enable bus mastering */
    pci_enable_bus_mastering(pci_dev);
    
    int rc = e1000_probe(io_base, irq);
    if (rc == 0 && irq < 16 && e1000_dev != NULL) {
        /* The legacy PIC must remain masked while scheduler/tasking is still
         * being brought up. Register the vector and clear NIC causes now;
         * gui_init() calls e1000_enable_runtime_irq() only after the first
         * desktop frame is visible and interrupt return has a live scheduler. */
        irq_set_mask(irq);
        e1000_disable_interrupts(e1000_dev);
        register_interrupt_handler((uint8_t)(32u + irq), e1000_irq_handler);
        (void)e1000_read_reg(e1000_dev, E1000_ICR);
        e1000_rx_irq_pending = true; /* preserve bootstrap traffic for poll */
        serial_puts("[E1000] PCI RX IRQ registered; activation deferred until GUI ready\n");
    } else if (rc == 0) {
        serial_puts("[E1000] no legacy PIC IRQ; cooperative receive fallback active\n");
    }
    return rc;
}

/* Enable INTx only after the GUI scheduler has completed its first frame.
 * This isolates early-boot IRQ11 assertion from AP startup and preserves the
 * normal IRQ->pending-bit->net_poll design once desktop execution is live. */
void e1000_enable_runtime_irq(void) {
    if (e1000_dev == NULL) {
        serial_puts("[E1000] runtime IRQ skipped: no device\n");
        return;
    }
    if (e1000_dev->irq >= 16) {
        serial_puts("[E1000] runtime IRQ skipped: non-PIC route\n");
        return;
    }
    if (e1000_irq_runtime_enabled) {
        serial_puts("[E1000] runtime IRQ already active\n");
        return;
    }
    uint8_t irq = e1000_dev->irq;
    serial_puts("[E1000] runtime IRQ arm: mask\n");
    irq_set_mask(irq);
    serial_puts("[E1000] runtime IRQ arm: disable/ACK\n");
    e1000_disable_interrupts(e1000_dev);
    (void)e1000_read_reg(e1000_dev, E1000_ICR);
    serial_puts("[E1000] runtime IRQ arm: enable device\n");
    e1000_enable_interrupts(e1000_dev);
    serial_puts("[E1000] runtime IRQ arm: PIC unmask\n");
    irq_clear_mask(irq);
    serial_puts("[E1000] runtime IRQ arm: PIC unmask returned\n");
    e1000_irq_runtime_enabled = true;
    e1000_rx_irq_pending = true;
    serial_puts("[E1000] PCI RX IRQ active after GUI scheduler handoff\n");
}

/* Cooperative descriptor drain. The routine is bounded by completed RX
 * descriptors, and remains the stability fallback for legacy PIC routing. */
void e1000_poll(void) {
    if (!e1000_dev || !net_iface) return;
    /* Hardware IRQs only signal work. Keep TCP/IP parsing outside the
     * interrupt stack, but skip descriptor reads on idle net_poll() passes. */
    if (!__atomic_exchange_n(&e1000_rx_irq_pending, false, __ATOMIC_ACQ_REL)) return;

    uint64_t frames = 0;
    uint8_t packet[E1000_RX_BUFF_SIZE];
    int len;
    while ((len = e1000_recv_packet(e1000_dev, packet, sizeof(packet))) > 0) {
        /* Process received packet */
        ++frames;
        eth_recv(net_iface, packet, len);
    }
    if (frames != 0u) {
        __atomic_fetch_add(&e1000_deferred_frames, frames, __ATOMIC_RELAXED);
    }
    /* Serial logging for deferred drain completely removed to eliminate UART latency on every packet receive */

    /* The NIC was masked by the hard handler. Drain first, then rearm the
     * receive causes; an arrival between ICR read and IMS write is latched by
     * the device and re-asserts INTx only after this safe point. */
    if (e1000_irq_runtime_enabled) {
        e1000_enable_interrupts(e1000_dev);
        if (e1000_dev->irq < 16u) irq_clear_mask((uint8_t)e1000_dev->irq);
    }
}
