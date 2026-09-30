/* cos-lib.ld - linker script for a .c-osll shared library.
 *
 * Identical in shape to cos-pie.ld minus the entry point: a library has
 * none, and the kernel uses exactly that to tell a library from a PIE
 * (both are ET_DYN, so the type alone does not distinguish them - see
 * cos_elf_is_runnable).
 *
 * Usage:
 *   gcc -ffreestanding -fPIC -nostdlib -c lib.c -o lib.o
 *   ld -shared -soname mylib.c-osll -T cos-lib.ld -o mylib.c-osll lib.o
 *
 * -soname matters more than it looks: the linker records it as
 * DT_SONAME, and the loader dedupes libraries by SONAME as well as by
 * path. Without it, the same library reached through two different paths
 * loads twice, at two different addresses, with two divergent copies of
 * its globals.
 */
SECTIONS
{
  . = SIZEOF_HEADERS;

  .dynsym         : { *(.dynsym) }
  .dynstr         : { *(.dynstr) }
  .gnu.hash       : { *(.gnu.hash) }
  .hash           : { *(.hash) }
  .gnu.version    : { *(.gnu.version) }
  .gnu.version_d  : { *(.gnu.version_d) }
  .gnu.version_r  : { *(.gnu.version_r) }
  .rela.dyn       : { *(.rela.init) *(.rela.text*) *(.rela.rodata*)
                      *(.rela.data*) *(.rela.got) *(.rela.bss*)
                      *(.rela.ifunc) }
  .relr.dyn       : { *(.relr.dyn) }
  .rela.plt       : { *(.rela.plt) *(.rela.iplt) }

  . = ALIGN(CONSTANT(MAXPAGESIZE));
  .init           : { KEEP(*(.init)) }
  .plt            : { *(.plt) *(.iplt) }
  .text           : { *(.text .text.*) }
  .fini           : { KEEP(*(.fini)) }

  . = ALIGN(CONSTANT(MAXPAGESIZE));
  .rodata         : { *(.rodata .rodata.*) }

  . = ALIGN(CONSTANT(MAXPAGESIZE));
  .tdata          : { *(.tdata .tdata.*) }
  .tbss           : { *(.tbss .tbss.*) }

  .init_array     : { KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
                      KEEP(*(.init_array)) }
  .fini_array     : { KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
                      KEEP(*(.fini_array)) }
  .dynamic        : { *(.dynamic) }
  .got            : { *(.got) *(.igot) }
  .got.plt        : { *(.got.plt) *(.igot.plt) }
  .data.rel.ro    : { *(.data.rel.ro .data.rel.ro.*) }
  . = DATA_SEGMENT_RELRO_END (0, .);

  .data           : { *(.data .data.*) }
  .bss            : { *(.bss .bss.*) *(COMMON) }
  . = DATA_SEGMENT_END (.);

  /DISCARD/       : { *(.comment) *(.note.GNU-stack) *(.eh_frame*) }
}
