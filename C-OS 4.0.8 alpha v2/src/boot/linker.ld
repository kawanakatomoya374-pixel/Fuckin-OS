ENTRY(_start)

SECTIONS
{
    /* Physical load start */
    . = 0x100000;

    /* Boot header and trampoline must stay first */
    .multiboot ALIGN(4K) :
    {
        KEEP(*(.multiboot))
        KEEP(*(.multiboot.*))
        KEEP(*(.multiboot2))
        KEEP(*(.multiboot2.*))
    }

    .boot.text ALIGN(4K) :
    {
        KEEP(*(.boot.text))
        KEEP(*(.boot.text.*))
    }

    .boot.data ALIGN(4K) :
    {
        KEEP(*(.boot.data))
        KEEP(*(.boot.data.*))
    }

    .boot.bss ALIGN(4K) :
    {
        KEEP(*(.boot.bss))
        KEEP(*(.boot.bss.*))
    }

    /* Kernel proper */
    /* PROVIDE(_text_start/_text_end): bounds of executable kernel code,
     * INCLUDING .boot.text - a wild function pointer landing in
     * .boot.text is exactly as much "not real code to jump through
     * right now" as one landing in the heap, since that trampoline is
     * done running by the time any of this is live.
     *
     * Added so a caller through a function pointer it does not fully
     * trust (see cos_netsurf_schedule_pump()'s callback-address check
     * in cos_gui_table.c) can verify the target is actually inside
     * compiled code before jumping to it, rather than discovering the
     * answer via a CPU #UD fault after control has already left. */
    PROVIDE(_text_start = ADDR(.boot.text));
    .text ALIGN(4K) :
    {
        *(.text*)
    }
    PROVIDE(_text_end = .);

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
    }

    .data ALIGN(4K) :
    {
        *(.data*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss*)
    }

    PROVIDE(_kernel_end = .);

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.note*)
        *(.comment)
    }
}
