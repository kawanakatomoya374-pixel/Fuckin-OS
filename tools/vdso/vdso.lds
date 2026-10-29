/* the layout of the vDSO image: one page (ELF header, hash, symbols, code, dynamic section) and, 0x1000 further, the data page the kernel updates (not part of the file; inside the one PT_LOAD, because glibc takes the load address from the first PT_LOAD whose address is not 0) */
SECTIONS
{
    . = SIZEOF_HEADERS;
    .hash     : { *(.hash) }     :text
    .gnu.hash : { *(.gnu.hash) } :text
    .dynsym   : { *(.dynsym) }   :text
    .dynstr   : { *(.dynstr) }   :text
    .text     : { *(.text*) *(.rodata*) } :text
    .dynamic  : { *(.dynamic) }  :text :dynamic
    .vvar 0x1000 (NOLOAD) : { vvar_data = .; . += 0x1000; } :text
    /DISCARD/ : { *(.note*) *(.eh_frame*) *(.comment) *(.data*) *(.bss*) *(.got*) *(.plt*) *(.rela*) *(.gnu.version*) }
}
PHDRS
{
    text    PT_LOAD FILEHDR PHDRS FLAGS(5);
    dynamic PT_DYNAMIC;
}
