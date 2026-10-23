/* edu.c - a driver for QEMU's "edu" teaching device (PCI 1234:11e8), written to exercise both ways a kernel module can take a device interrupt that is not on a legacy line:
 * MSI (cosys_msi_register) and an I/O APIC route of the function's INTx input (cosys_ioapic_route).
 *   0x1037  edu_factorial(n)   the device computes n! (n <= 12) and raises an interrupt when done; the call waits for it. Returns the result, or -1
 *   0x1038  edu_info()         (interrupts seen << 8) | mode   (mode 1 = MSI, 2 = I/O APIC, 0 = none)
 *   0x1039  edu_mode(m)        switch to MSI (1) or the I/O APIC (2). 0, or -1. Fully privileged callers only */
#include "cosys.h"

#define R_ID 0x00
#define R_FACT 0x08
#define R_STATUS 0x20
#define R_IRQ_STATUS 0x24
#define R_IRQ_RAISE 0x60
#define R_IRQ_ACK 0x64

static volatile uint32_t *g_regs;
static uint8_t g_bus, g_dev, g_fn;
static int g_mode, g_gsi = -1;
static volatile uint32_t g_irqs;

static void on_irq(void *ctx) {
    (void)ctx;
    uint32_t st = g_regs[R_IRQ_STATUS / 4];
    if (!st) return;                                       /* a shared line: someone else's interrupt */
    g_regs[R_IRQ_ACK / 4] = st;                            /* silences a level-triggered input; harmless for MSI */
    g_irqs++;
}

static void stop(void) {
    if (g_mode == 1) cosys_msi_unregister(g_bus, g_dev, g_fn);
    if (g_mode == 2 && g_gsi >= 0) cosys_ioapic_unroute((uint32_t)g_gsi);
    g_mode = 0;
}
static int start(int m) {
    stop();
    if (m == 1) {
        int v = cosys_msi_register(g_bus, g_dev, g_fn, on_irq, 0);
        if (v < 0) return -1;
        g_mode = 1;
        cosys_log_hex("MSI vector ", (uint64_t)v);
        return 0;
    }
    if (m == 2) {
        /* Which global system interrupt a function's INTx pin reaches is described by the firmware (ACPI _PRT). QEMU's q35 board wires pin A of device d on the root bus to
         * PIRQ (d + pin - 1) mod 8, i.e. GSI 16 + that; this driver, written for that board, uses the rule instead of interpreting AML. */
        uint32_t pin = (cosys_pci_read32(g_bus, g_dev, g_fn, 0x3C) >> 8) & 0xFF;
        if (pin == 0) return -1;
        int gsi = 16 + (int)((g_dev + pin - 1) & 7);
        int v = cosys_ioapic_route((uint32_t)gsi, COSYS_IOAPIC_LOW | COSYS_IOAPIC_LEVEL, on_irq, 0);
        if (v < 0) return -1;
        g_gsi = gsi; g_mode = 2;
        cosys_log_hex("I/O APIC GSI ", (uint64_t)gsi);
        cosys_log_hex("I/O APIC vector ", (uint64_t)v);
        return 0;
    }
    return -1;
}

static int64_t edu_factorial(uint64_t n, uint64_t b, uint64_t c) {
    (void)b; (void)c;
    if (!g_regs || !g_mode || n > 12) return -1;
    uint32_t before = g_irqs;
    g_regs[R_STATUS / 4] = 0x80;                           /* raise an interrupt when the computation is finished */
    g_regs[R_FACT / 4] = (uint32_t)n;
    uint64_t t0 = cosys_time_ms();
    while (g_irqs == before) {
        if (cosys_time_ms() - t0 > 1000) { cosys_log_hex("timeout, irq status ", g_regs[R_IRQ_STATUS / 4]); return -1; }
        cosys_yield();
    }
    return (int64_t)g_regs[R_FACT / 4];
}
static int64_t edu_info(uint64_t a, uint64_t b, uint64_t c) { (void)a; (void)b; (void)c; return (int64_t)((uint64_t)g_irqs << 8 | (uint32_t)g_mode); }
static int64_t edu_mode(uint64_t m, uint64_t b, uint64_t c) {
    (void)b; (void)c;
    if (cosys_current_caps() != 0xFFFFFFFFu) return -1;
    return start((int)m);
}

static int edu_init(void) {
    int found = 0;
    for (uint32_t d = 0; d < 32 && !found; d++) {
        uint32_t id = cosys_pci_read32(0, (uint8_t)d, 0, 0);
        if ((id & 0xFFFFu) == 0x1234u && (id >> 16) == 0x11E8u) { g_bus = 0; g_dev = (uint8_t)d; g_fn = 0; found = 1; }
    }
    if (!found) { cosys_log("no edu device"); return -1; }
    uint32_t bar0 = cosys_pci_read32(g_bus, g_dev, g_fn, 0x10);
    if (bar0 & 1u) return -1;
    if (cosys_pci_enable(g_bus, g_dev, g_fn, COSYS_PCI_MEM | COSYS_PCI_BUSMASTER) != 0) return -1;
    g_regs = (volatile uint32_t *)cosys_mmio_map(bar0 & ~0xFull, 0x1000);
    if (!g_regs) return -1;
    if (g_regs[R_ID / 4] != 0x010000EDu) { cosys_mmio_unmap((void *)g_regs); return -1; }
    if (start(1) != 0) { cosys_log("MSI not available"); cosys_mmio_unmap((void *)g_regs); return -1; }
    if (cosys_register_syscall(0x1037, edu_factorial, 0) != 0 || cosys_register_syscall(0x1038, edu_info, 0) != 0 || cosys_register_syscall(0x1039, edu_mode, 0) != 0) {
        stop(); cosys_unregister_syscall(0x1037); cosys_unregister_syscall(0x1038); return -1;
    }
    cosys_log("edu device ready (MSI)");
    return 0;
}
static void edu_exit(void) {
    cosys_unregister_syscall(0x1037); cosys_unregister_syscall(0x1038); cosys_unregister_syscall(0x1039);
    stop();                                                 /* the interrupt is switched off before the registers go away */
    g_regs[R_IRQ_ACK / 4] = 0xFFFFFFFFu;
}
COSYS_MODULE("edu", "1.0", COSYS_LEVEL_DEVICE, edu_init, edu_exit);
