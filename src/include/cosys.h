/* cosys.h - what a C-OS kernel module (.c-osys) sees of the kernel.
 *
 * A .c-osys is a shared object (ET_DYN) like a .c-osll, built for ring 0 by tools/cos-sys and signed with the module key. The kernel loads it with the
 * same ELF engine as a program or a library (src/kernel/cos_elf.c): segments with W^X, eager relocation, RELRO, GNU hash, DT_NEEDED. What differs is
 * where the undefined symbols come from: only from the kernel's EXPORT TABLE below (src/kernel/cos_kmod.c), each entry with a permission LEVEL, and
 * from other loaded modules that this one lists as DT_NEEDED. A symbol that is in neither, or whose level is above the module's own, makes the load fail.
 *
 *     #include "cosys.h"
 *     static int64_t hello_call(uint64_t a, uint64_t b, uint64_t c) { return (int64_t)(a + b + c); }
 *     static int  hello_init(void) { cosys_log("hello"); return cosys_register_syscall(0x1000, hello_call, 0); }
 *     static void hello_exit(void) { cosys_unregister_syscall(0x1000); }
 *     COSYS_MODULE("hello", "1.0", COSYS_LEVEL_SERVICE, hello_init, hello_exit);
 */
#ifndef COSYS_H
#define COSYS_H

#include <stdint.h>
#include <stddef.h>
#include "cos_kevent.h"

#define COSYS_ABI            2
#define COSYS_SYSCALL_FIRST  0x1000u        /* the system call numbers a module may provide: 0x1000 .. 0x103F */
#define COSYS_SYSCALL_COUNT  64u

/* Permission levels. A module declares its level in its (signed) metadata and may import only exports of that level or lower.
 *   BASIC   log, time, a bounded allocator, string/memory helpers                              - cannot touch hardware or programs
 *   SERVICE + system calls for programs, and checked copies to/from the calling program's memory
 *   DEVICE  + I/O ports (not the interrupt controller, timer, CMOS, ...) and PCI configuration reads, interrupts, device memory, DMA
 *   SYSTEM  + the file system (reading and writing any file, vetoing programs' file operations) and the network (seeing and dropping frames): data of every program
 * Level is a policy and an audit trail, not a sandbox: ring 0 code can do anything the CPU allows. What keeps the kernel safe is that only modules signed
 * by the module key load at all, and that the key holder declares (and so answers for) what the module is for. */
#define COSYS_LEVEL_BASIC    0
#define COSYS_LEVEL_SERVICE  1
#define COSYS_LEVEL_DEVICE   2
#define COSYS_LEVEL_SYSTEM   3
#define COSYS_LEVEL_MAX      3

typedef int64_t (*cosys_syscall_fn)(uint64_t a, uint64_t b, uint64_t c);

/* ---- the kernel's export table (imported by name; the loader resolves them) ---- */
#pragma GCC visibility push(default)
/* BASIC */
void     cosys_log(const char *text);                       /* one line to the kernel log, prefixed with the module's name */
void     cosys_log_hex(const char *text, uint64_t value);
void     cosys_log_dec(const char *text, uint64_t value);
/* A spin lock for data shared between event callbacks, system calls and other CPUs. cosys_lock disables interrupts on this CPU and returns the flags that
 * cosys_unlock restores. Hold it briefly; never call anything that can sleep while holding it, and never copy to or from a program's memory (it can fault). */
typedef struct { volatile int locked; } cosys_lock_t;
uint64_t cosys_lock(cosys_lock_t *l);
void     cosys_unlock(cosys_lock_t *l, uint64_t flags);
uint64_t cosys_time_ms(void);                               /* milliseconds since boot */
void    *cosys_alloc(size_t bytes);                         /* zeroed; at most 256 KiB at a time and 1 MiB per module; freed when the module unloads */
void     cosys_free(void *p);
void    *memcpy(void *d, const void *s, size_t n);
void    *memmove(void *d, const void *s, size_t n);
void    *memset(void *d, int c, size_t n);
int      memcmp(const void *a, const void *b, size_t n);
size_t   strlen(const char *s);
/* SERVICE */
/* Provides system call `nr` (0x1000..0x103F) to programs. `caps` are the COS_CAP_* bits a calling process must hold (0: any process, also a sandboxed
 * one); a process missing one gets -1 and the module is not called. Returns 0, or -1 when `nr` is taken or out of range or `fn` is not this module's code. */
int      cosys_register_syscall(uint32_t nr, cosys_syscall_fn fn, uint32_t caps);
int      cosys_unregister_syscall(uint32_t nr);
/* Programs' pointers must never be dereferenced directly: these check that the range is user memory of the calling process. 0 on success, -1 otherwise. */
int      cosys_copy_from_user(void *dst, uint64_t user_src, size_t n);
int      cosys_copy_to_user(uint64_t user_dst, const void *src, size_t n);
/* Subscribes to kernel events (a bit mask of 1 << COSYS_EV_*, see cos_kevent.h). One callback per function, up to 16 subscriptions in all; they end when the module
 * unloads. Returns 0, or -1 (bad mask, table full, or `fn` is not this module's code). */
typedef void (*cosys_event_fn)(uint32_t type, uint64_t a, uint64_t b, uint64_t c);
int      cosys_subscribe(uint32_t mask, cosys_event_fn fn);
int      cosys_unsubscribe(cosys_event_fn fn);
void     cosys_yield(void);                                 /* lets other threads run, and interrupts arrive, for a moment (system call context only: never from an event or IRQ callback); wait for a device with a loop around it */
uint64_t cosys_current_pid(void);
uint32_t cosys_current_caps(void);
/* DEVICE: ports 0x20-0x21, 0x40-0x43, 0x60-0x64, 0x70-0x71, 0x92, 0xA0-0xA1, 0xCF8-0xCFF are refused (interrupt controller, timer, keyboard, CMOS, A20, PCI mechanism) */
uint8_t  cosys_inb(uint16_t port);   uint16_t cosys_inw(uint16_t port);   uint32_t cosys_inl(uint16_t port);
int      cosys_outb(uint16_t port, uint8_t v);  int cosys_outw(uint16_t port, uint16_t v);  int cosys_outl(uint16_t port, uint32_t v);
uint32_t cosys_pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset);      /* 0xFFFFFFFF when there is no device */
/* DEVICE (drivers): interrupts, memory-mapped registers, DMA memory, PCI enabling */
/* Calls fn(ctx) when interrupt line `irq` fires. Only a free legacy line (3, 4, 5, 6, 7, 9, 10, 11, 15) that no kernel driver uses can be taken; up to 4 per module.
 * The handler runs in interrupt context: very short, no sleeping, no allocation, lock with cosys_lock. Returns 0 or -1. */
typedef void (*cosys_irq_fn)(void *ctx);
int      cosys_irq_register(int irq, cosys_irq_fn fn, void *ctx);
int      cosys_irq_unregister(int irq, cosys_irq_fn fn);
/* Interrupts through the Local APIC. They are delivered to the boot CPU, in interrupt context like cosys_irq_register's (very short, no sleeping, no allocation), and the kernel sends
 * the end-of-interrupt itself. At most 8 of the two kinds together per module; both end when the module unloads (the device's MSI is switched off / the route masked first).
 *   cosys_msi_register   MSI of PCI function bus:dev.fn: finds its MSI capability, writes the message (address 0xFEE00000 | boot CPU, data = the vector) and enables it, legacy INTx is
 *                        disabled. One message, edge triggered; MSI-X is not supported. Returns the vector (0x40..0x5F), or -1 (no such function, no MSI capability, table full).
 *   cosys_ioapic_route   routes global system interrupt `gsi` of the I/O APIC to the handler. Only GSIs from 16 up (the PCI interrupt inputs); the lower ones are the legacy lines of
 *                        cosys_irq_register. A GSI can be taken by one module. The handler of a level-triggered input must silence the device, or it fires again at once; a PCI line may
 *                        be shared, so check that the device really raised it. Which GSI a function's pin reaches is the firmware's (ACPI _PRT) business; the caller must know it. The kernel's own drivers still
 *                        take their interrupts through the 8259, which a Local APIC vector outranks: do not route a GSI that a device with a kernel driver shares, or that device is starved. Returns the vector, or -1. */
#define COSYS_IOAPIC_LOW    1u      /* active low (PCI INTx) */
#define COSYS_IOAPIC_LEVEL  2u      /* level triggered (PCI INTx) */
int      cosys_msi_register(uint8_t bus, uint8_t dev, uint8_t fn, cosys_irq_fn handler, void *ctx);
int      cosys_msi_unregister(uint8_t bus, uint8_t dev, uint8_t fn);
int      cosys_ioapic_route(uint32_t gsi, uint32_t flags, cosys_irq_fn handler, void *ctx);
int      cosys_ioapic_unroute(uint32_t gsi);
/* Maps `size` (<= 1 MiB) bytes of device memory at physical address `phys` uncached; refused for ranges that overlap RAM. Up to 4 mappings per module. */
void    *cosys_mmio_map(uint64_t phys, size_t size);
void     cosys_mmio_unmap(void *p);
/* Physically contiguous, zeroed, page-aligned memory below 4 GiB for a device to read and write (<= 64 KiB a block, 1 MiB per module, 8 blocks); *phys gets its
 * physical address. Freed when the module unloads - so a driver's exit function must stop the device's DMA first. */
void    *cosys_dma_alloc(size_t size, uint64_t *phys);
void     cosys_dma_free(void *p);
#define COSYS_PCI_IO         1u
#define COSYS_PCI_MEM        2u
#define COSYS_PCI_BUSMASTER  4u
/* Sets the given bits of the PCI command register of an existing function (they can only be set, never cleared). Returns 0 or -1. */
int      cosys_pci_enable(uint8_t bus, uint8_t dev, uint8_t fn, uint32_t flags);
/* SYSTEM (data): the file system and the network */
/* Module's own file access: not subject to any program's sandbox or jail. Returns the number of bytes (read: at most `cap`, a file up to 1 MiB; write: replaces the file), or -1. */
int64_t  cosys_fs_read(const char *path, void *buf, size_t cap);
int64_t  cosys_fs_write(const char *path, const void *buf, size_t n);
int      cosys_fs_stat(const char *path, uint64_t *size, int *is_dir);          /* 0, or -1 when there is no such path */
/* A filter on programs' file operations (see COSYS_FSOP_* in cos_kevent.h): called in the system call, before the operation. Return 0 to allow, anything else to refuse (the call
 * fails). It also sees every operation, so it works as an audit hook. Must be short and must not sleep. Up to 4 filters in all; all must allow. */
typedef int (*cosys_fs_filter_fn)(uint32_t op, uint64_t pid, const char *path, const char *path2);
int      cosys_fs_filter_register(cosys_fs_filter_fn fn);
int      cosys_fs_filter_unregister(cosys_fs_filter_fn fn);
/* A filter on Ethernet frames (COSYS_NET_RX / COSYS_NET_TX): read-only view of the whole frame; return 0 to pass, anything else to drop it. Called from the network code on whatever
 * thread handles the frame: short, no sleeping. Up to 4 in all. */
typedef int (*cosys_net_filter_fn)(uint32_t dir, const uint8_t *frame, size_t len);
int      cosys_net_filter_register(cosys_net_filter_fn fn);
int      cosys_net_filter_unregister(cosys_net_filter_fn fn);
/* A rewriter of Ethernet frames: may change the frame in place before the stack sees it (RX) or the driver sends it (TX). `frame` holds `len` bytes in a buffer of `cap`
 * (len + 256 at least), so it may also grow it. Returns the new length (>= 14, <= cap; `len` when nothing changed) or -1 to drop the frame. Read-only filters see the result.
 * After editing IPv4/TCP/UDP fields call cosys_net_fix_checksums, or the stack (and the peer) will discard the frame. Short, no sleeping. Up to 4 in all, run in registration order. */
typedef int64_t (*cosys_net_rewrite_fn)(uint32_t dir, uint8_t *frame, size_t len, size_t cap);
int      cosys_net_rewrite_register(cosys_net_rewrite_fn fn);
int      cosys_net_rewrite_unregister(cosys_net_rewrite_fn fn);
int      cosys_net_fix_checksums(uint8_t *frame, size_t len);       /* IPv4 header + TCP/UDP checksum of an unfragmented frame; 0, or -1 when it is not IPv4 */
#pragma GCC visibility pop

/* A module's own functions that other modules may import (DT_NEEDED) are marked with this; everything else is hidden by the build flags. */
#define COSYS_EXPORT __attribute__((visibility("default")))

struct cosys_module {
    uint32_t magic;                                         /* COSYS_MAGIC */
    uint32_t abi;                                           /* COSYS_ABI the module was built for */
    uint32_t level;                                         /* COSYS_LEVEL_* */
    uint32_t reserved;
    const char *name, *version;
    int  (*init)(void);                                     /* 0: loaded; anything else: the module is unloaded again */
    void (*exit)(void);                                     /* called before the module's memory is released */
};
#define COSYS_MAGIC 0x53595343u                             /* "CSYS" */
#define COSYS_MODULE(modname, modversion, modlevel, initfn, exitfn) \
    COSYS_EXPORT const struct cosys_module cosys_module_info = { COSYS_MAGIC, COSYS_ABI, modlevel, 0, modname, modversion, initfn, exitfn }

/* signature trailer appended to the object by tools/cos-sys (tools/kmod_sign.py): "COSYSIG2", a 32-bit length (65), the key id (1 byte) and the raw ECDSA P-256 signature
 * (r||s) over the SHA-256 of the object bytes that come before the trailer. The key id is the highest module level that key may sign (1, 2 or 3): a module that declares
 * a level above its key's is refused, and a kernel built without a key (gen_kmod_keys.py TRUSTED_LEVEL) refuses everything signed with it. */
#define COSYS_SIG_MAGIC "COSYSIG2"
#define COSYS_SIG_LEN   64u

#endif
