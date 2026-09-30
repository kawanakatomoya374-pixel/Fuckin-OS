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

/* ---- IST1: a dedicated emergency stack for exceptions that stack
 * corruption itself could cause. ----------------------------------
 *
 * Every IDT gate normally runs its handler on WHATEVER stack was
 * active at the moment of the fault - if that stack is the very thing
 * that just overflowed (a kernel thread walking off the bottom of its
 * guard-paged stack; see task_alloc_stack() in task.c), the CPU's own
 * automatic push of the exception frame lands on the SAME bad stack.
 * For an ordinary corrupted-but-still-mapped stack that merely
 * SUCCEEDS by luck (as this investigation's first reproduction did:
 * RSP wandered into unrelated static data that happened to be mapped
 * and writable). For a stack that has walked into this kernel's
 * now-UNMAPPED guard page specifically, that automatic push instead
 * takes a SECOND page fault while still handling the first - a double
 * fault (#DF, vector 8) - and if #DF's OWN frame push ALSO lands on
 * that same unusable stack, a THIRD fault occurs with no handler left
 * to take it, which is a triple fault: the CPU resets. On real
 * hardware and in QEMU alike, that is a silent, undiagnosable reboot -
 * strictly worse than the invalid-opcode halts this investigation had
 * been getting, because it leaves no serial output at all.
 *
 * x86-64's answer to exactly this is the Interrupt Stack Table: a gate
 * can name one of seven fixed stacks (TSS.ist[0..6]) that the CPU
 * switches to BEFORE pushing anything, unconditionally, regardless of
 * what RSP held beforehand. idt.c arms IST1 for #UD/#DF/#GP/#PF - the
 * four vectors stack corruption can plausibly raise - so each of them
 * is guaranteed a valid stack to run its handler on, however badly the
 * faulting context's own stack was corrupted.
 *
 * SIZE: generous, and containing only C code that must never itself
 * recurse deeply or hold large locals - default_handler() and
 * page_fault_handler() do a bounded handful of serial_puts() calls and
 * either halt or (for a page fault correctly identified as ordinary
 * stack growth) return. 16 KiB is far more than either needs and cheap
 * to reserve permanently.
 *
 * NOT SMP-SAFE, MATCHING AN EXISTING LIMITATION: `tss_entry` above is
 * already a single global instance shared by every CPU, not one per
 * CPU - on real multi-CPU hardware, loading the same TSS selector
 * (`ltr`) on a second CPU while the first still holds it busy is
 * undefined per the architecture manuals. This emergency stack
 * inherits that same constraint: two CPUs faulting into it at once
 * would corrupt each other's handler state. This is acceptable for
 * every configuration this investigation tested (QEMU with no `-smp`
 * flag, i.e. one CPU) and is not a new limitation introduced here -
 * making both the TSS and this stack properly per-CPU is a single,
 * larger piece of follow-up work, not two. */
#define IST1_STACK_SIZE (16u * 1024u)
static uint8_t ist1_emergency_stack[IST1_STACK_SIZE] __attribute__((aligned(16)));

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

    /* IST1 = the emergency stack declared above. Stacks grow down on
     * x86-64, so this is the address ONE PAST THE END of the array -
     * the same convention task_alloc_stack() uses for an ordinary
     * thread stack's "top". Must be set before idt_init() arms any
     * gate's IST field, since a fault arriving before this line would
     * switch to IST1 = 0, which the CPU treats as "do not switch
     * stacks" only when the GATE's ist field is also 0 - a gate armed
     * with ist=1 but a zero TSS.ist[0] would instead switch to linear
     * address 0 and fault again immediately. gdt_init() (which calls
     * this) already runs before idt_init() in kernel.c, so this
     * ordering already holds; stated here so it stays true if that
     * call order ever moves. */
    tss_entry.ist[0] = (uint64_t)(uintptr_t)(ist1_emergency_stack + IST1_STACK_SIZE);
}

void tss_set_kernel_stack(uint64_t rsp0) {
    tss_entry.esp0 = rsp0;
}

void tss_flush(void) {
    __asm__ volatile("ltr %w0" : : "r"((uint16_t)GDT_TSS));
}
