/**
 * gdt.c - Global Descriptor Table Implementation
 * C-OS 4.0.7 - 64-bit Long Mode
 */

#include "gdt.h"
#include "serial.h"
#include "memory.h"
#include "types.h"

void* memset(void* ptr, int value, size_t n);

/* Real hardware GDT array (8-byte entries) */
static gdt_entry_t gdt_entries[GDT_ENTRIES];
static gdt_ptr_t gdt_ptr;
static tss_t tss_entry;

/**
 * GDT entry setup (8 bytes)
 */
void gdt_set_entry(int index, uint64_t base, uint64_t limit, 
                   uint8_t access, uint8_t granularity) {
    if (index < 0 || index >= GDT_ENTRIES) return;
    
    gdt_entry_t* entry = &gdt_entries[index];
    
    entry->limit_low     = (limit & 0xFFFF);
    entry->base_low      = (base & 0xFFFF);
    entry->base_middle   = (base >> 16) & 0xFF;
    entry->access        = access;
    entry->granularity   = ((limit >> 16) & 0x0F) | (granularity & 0xF0);
    entry->base_high     = (base >> 24) & 0xFF;
}

/**
 * Set GDT entry for TSS (64-bit, 16 bytes)
 */
void gdt_set_tss(uint64_t base) {
    uint64_t limit = sizeof(tss_t) - 1;
    
    /* TSS Descriptor is 16 bytes in 64-bit mode.
     * It occupies two consecutive GDT slots. */
    
    /* Lower 8 bytes */
    gdt_set_entry(8, base & 0xFFFFFFFF, limit & 0xFFFF,
                  SEG_ACCESS_PRESENT | SEG_ACCESS_PRIV_RING0 | 0x09, 0);
    
    /* Upper 8 bytes */
    uint64_t* upper = (uint64_t*)&gdt_entries[9];
    *upper = (base >> 32);
}

/**
 * GDT initialization for x86-64 Long Mode
 */
void gdt_init(void) {
    serial_puts("[GDT] Initializing GDT...\n");
    
    /* Clear GDT entries */
    memset(gdt_entries, 0, sizeof(gdt_entries));
    
    /* Entry 0: Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);
    
    /* Entry 1: Kernel code segment (ring 0) */
    gdt_set_entry(1, 0, 0xFFFFFFFF,
                  SEG_ACCESS_PRESENT | SEG_ACCESS_PRIV_RING0 |
                  SEG_ACCESS_DESCRIPTOR | SEG_ACCESS_EXECUTABLE | SEG_ACCESS_READ_WRITE,
                  SEG_FLAG_LONG_MODE | SEG_FLAG_GRAN_4K);

    /* Entry 2: Kernel data segment (ring 0) */
    gdt_set_entry(2, 0, 0xFFFFFFFF,
                  SEG_ACCESS_PRESENT | SEG_ACCESS_PRIV_RING0 |
                  SEG_ACCESS_DESCRIPTOR | SEG_ACCESS_READ_WRITE,
                  SEG_FLAG_GRAN_4K);
    
    /* Entry 3: User code segment (ring 3) */
    gdt_set_entry(3, 0, 0xFFFFFFFF,
                  SEG_ACCESS_PRESENT | SEG_ACCESS_PRIV_RING3 |
                  SEG_ACCESS_DESCRIPTOR | SEG_ACCESS_EXECUTABLE | SEG_ACCESS_READ_WRITE,
                  SEG_FLAG_LONG_MODE | SEG_FLAG_GRAN_4K);
    
    /* Entry 4: User data segment (ring 3) */
    gdt_set_entry(4, 0, 0xFFFFFFFF,
                  SEG_ACCESS_PRESENT | SEG_ACCESS_PRIV_RING3 |
                  SEG_ACCESS_DESCRIPTOR | SEG_ACCESS_READ_WRITE,
                  SEG_FLAG_GRAN_4K);
    
    /* Entry 7: temporary 32-bit code segment used only while APs
     * transition from real mode through protected mode to long mode. */
    gdt_set_entry(7, 0, 0xFFFFFFFF,
                  SEG_ACCESS_PRESENT | SEG_ACCESS_PRIV_RING0 |
                  SEG_ACCESS_DESCRIPTOR | SEG_ACCESS_EXECUTABLE |
                  SEG_ACCESS_READ_WRITE,
                  SEG_FLAG_32BIT | SEG_FLAG_GRAN_4K);

    /* Entries 8 & 9: TSS (16-byte descriptor). */
    gdt_set_tss((uint64_t)&tss_entry);
    
    /* Set up GDT pointer */
    gdt_ptr.limit = (uint16_t)((GDT_ENTRIES * 8) - 1);
    gdt_ptr.base = (uint64_t)&gdt_entries;
    
    /* Install GDT */
    gdt_install();
    
    /* Reload segments to use new GDT selectors */
    gdt_reload_cs();
    set_kernel_segments();
    
    /* Initialize and load TSS */
    tss_init();
    tss_flush();
    
    serial_puts("[GDT] GDT and TSS installed successfully\n");
}

void gdt_install(void) {
    __asm__ volatile("lgdt %0" : : "m"(gdt_ptr));
}

void tss_init(void) {
    memset(&tss_entry, 0, sizeof(tss_t));
    tss_entry.iomap_base = sizeof(tss_t);
}

void tss_set_kernel_stack(uint64_t rsp0) {
    tss_entry.esp0 = rsp0;
}

void tss_flush(void) {
    __asm__ volatile("ltr %w0" : : "r"((uint16_t)GDT_TSS));
}
