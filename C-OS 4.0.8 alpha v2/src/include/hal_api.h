/**
 * hal_api.h - Hardware Abstraction Layer API
 * C-OS 4.0.8 alpha HAL Interface
 * 
 * Provides unified HAL interface that abstracts x86_64 hardware.
 * Kernel and drivers use this API instead of direct hardware access.
 */

#ifndef HAL_API_H
#define HAL_API_H

#include "types.h"
#include <stdint.h>

struct regs;

/* HAL Error Codes */
#define HAL_ERROR_OK 0
#define HAL_ERROR_INVALID_PARAM -1
#define HAL_ERROR_NOT_SUPPORTED -2
#define HAL_ERROR_HARDWARE -3
#define HAL_ERROR_TIMEOUT -4

/* CPU Abstraction */
void hal_cpu_init(void);
void hal_cpu_halt(void);
void hal_cpu_enable_interrupts(void);
void hal_cpu_disable_interrupts(void);
uint64_t hal_cpu_get_features(void);
void hal_cpu_get_cpuid(uint64_t leaf, uint64_t* eax, uint64_t* ebx, 
                       uint64_t* ecx, uint64_t* edx);

/* Interrupt Abstraction */
void hal_interrupt_init(void);
void hal_interrupt_enable(uint8_t irq);
void hal_interrupt_disable(uint8_t irq);
void hal_interrupt_send_eoi(uint8_t irq);
int hal_interrupt_register_handler(uint8_t vector, void (*handler)(struct regs*));
int hal_interrupt_unregister_handler(uint8_t vector);

/* Timer Abstraction */
void hal_timer_init(void);
uint64_t hal_timer_get_ticks(void);
uint64_t hal_timer_get_ms(void);
uint64_t hal_timer_get_seconds(void);
int hal_timer_set_frequency(uint64_t frequency);
void hal_timer_delay_ms(uint64_t ms);
void hal_timer_delay_us(uint64_t us);
int hal_timer_register_callback(void (*callback)(uint64_t), uint64_t interval_ms, bool one_shot);
int hal_timer_unregister_callback(int callback_id);

/* Paging Abstraction */
void hal_paging_init(void);
void hal_paging_set_cr3(uint64_t cr3_value);
uint64_t hal_paging_get_cr3(void);
void hal_paging_invalidate_tlb(void);
void hal_paging_invalidate_tlb_entry(virt_addr_t vaddr);
int hal_paging_map_page(virt_addr_t vaddr, phys_addr_t paddr, uint64_t flags);
int hal_paging_unmap_page(virt_addr_t vaddr);
int hal_paging_protect_page(virt_addr_t vaddr, uint64_t flags);
bool hal_paging_is_mapped(virt_addr_t vaddr);
phys_addr_t hal_paging_get_phys_addr(virt_addr_t vaddr);

/* I/O Abstraction */
void hal_io_outb(uint16_t port, uint8_t value);
void hal_io_outw(uint16_t port, uint16_t value);
void hal_io_outd(uint16_t port, uint32_t value);
uint8_t hal_io_inb(uint16_t port);
uint16_t hal_io_inw(uint16_t port);
uint32_t hal_io_ind(uint16_t port);
void hal_io_insb(uint16_t port, uint8_t* buffer, size_t count);
void hal_io_insw(uint16_t port, uint16_t* buffer, size_t count);
void hal_io_insl(uint16_t port, uint32_t* buffer, size_t count);
void hal_io_outsb(uint16_t port, const uint8_t* buffer, size_t count);
void hal_io_outsw(uint16_t port, const uint16_t* buffer, size_t count);
void hal_io_outsl(uint16_t port, const uint32_t* buffer, size_t count);

/* Memory-Mapped I/O */
virt_addr_t hal_io_map_mmio(phys_addr_t phys_addr, size_t size, const char* name);
int hal_io_unmap_mmio(virt_addr_t virt_addr);
uint8_t hal_io_mmio_read8(virt_addr_t addr);
uint16_t hal_io_mmio_read16(virt_addr_t addr);
uint32_t hal_io_mmio_read32(virt_addr_t addr);
uint64_t hal_io_mmio_read64(virt_addr_t addr);
void hal_io_mmio_write8(virt_addr_t addr, uint8_t value);
void hal_io_mmio_write16(virt_addr_t addr, uint16_t value);
void hal_io_mmio_write32(virt_addr_t addr, uint32_t value);
void hal_io_mmio_write64(virt_addr_t addr, uint64_t value);

/* Memory Management */
void hal_mem_barrier(void);
void hal_mem_invalidate_cache(void);
void hal_mem_flush_cache(void);

/* Context Management
 *
 * MUST be byte-for-byte identical to the anonymous `context` struct
 * embedded in thread_t (see task.h): scheduler.c passes
 * `&thread->context` around cast to `hal_context_t*`, and
 * context_switch.asm's hal_context_switch() reads/writes this exact
 * layout by hardcoded byte offset. Previously this struct was 144
 * bytes (18 fields, missing rdx/rcx entirely) while thread_t.context
 * was 160 bytes (20 fields) in a different field order, and the asm
 * used yet a *third*, different order/offsets of its own - three
 * mutually incompatible layouts for what has to be one shared
 * structure. Every real context switch (starting with the very first
 * one, into gui_main_thread) would read/write registers at the wrong
 * offsets - e.g. the asm's "rip" slot lined up with what task.h calls
 * `cs`, and its "rsp" slot lined up with what task.h calls `rip` - so
 * restoring a thread would load RSP with an instruction-pointer value
 * and jump to a raw CS selector value instead, producing exactly the
 * kind of wild/non-canonical access that raises #GP (vector 13) on
 * every retry, forever.
 */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} hal_context_t;

void hal_context_save(hal_context_t* context);
void hal_context_restore(const hal_context_t* context);
void hal_context_switch(hal_context_t* old_context, const hal_context_t* new_context);

/* HAL Statistics */
typedef struct {
    uint64_t interrupt_count;
    uint64_t timer_interrupts;
    uint64_t page_faults;
    uint64_t context_switches;
    uint64_t hal_calls;
} hal_stats_t;

hal_stats_t* hal_get_stats(void);

/* HAL Initialization and Cleanup */
int hal_init(void);
int hal_cleanup(void);

#endif /* HAL_API_H */
