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
    .text ALIGN(4K) :
    {
        *(.text*)
    }

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
