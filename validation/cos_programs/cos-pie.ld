/* cos-pie.ld - linker script for a position-independent .c-os program.
 *
 * WHY A SECOND SCRIPT
 * -------------------
 * cos.ld places the program at a fixed 0x8000000000 and produces an
 * ET_EXEC. That is still supported and still the right choice for a
 * hand-written assembly test. It is the wrong choice for anything built
 * from C by a normal toolchain, which defaults to -fPIE and expects to
 * be placed by the loader.
 *
 * This script produces an ET_DYN with a base of 0, which the kernel
 * loads at whatever bias it chooses (COS_ELF_PIE_BASE by default). Note
 * what is NOT here: no fixed load address, and no PHDRS block pinning
 * segments to hard-coded permissions. The relocation and RELRO sections
 * have to land where the linker wants them, and over-specifying the
 * layout is how a linker script silently breaks .rela.dyn placement.
 *
 * Usage:
 *   gcc -ffreestanding -fPIE -nostdlib -c prog.c -o prog.o
 *   ld -pie --no-dynamic-linker -T cos-pie.ld -o prog.c-os \
 *      cos_crt0.o prog.o libcos.a
 *
 * --no-dynamic-linker is not optional: it suppresses PT_INTERP. A
 * program carrying PT_INTERP is asking to be started by an ld.so, and
 * the loader refuses it rather than jumping into a program whose GOT was
 * never populated.
 */
ENTRY(_start)

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

  /* Thread-local storage. .tdata carries initial values and .tbss does
   * not occupy file space - the loader copies p_filesz bytes and zeroes
   * the rest, so the two must stay adjacent and in this order. */
  . = ALIGN(CONSTANT(MAXPAGESIZE));
  .tdata          : { *(.tdata .tdata.*) }
  .tbss           : { *(.tbss .tbss.*) }

  /* Everything the dynamic linker writes and nothing else should: this
   * is the RELRO region, made read-only once relocation is done.
   *
   * DATA_SEGMENT_ALIGN / DATA_SEGMENT_RELRO_END are what actually make
   * `ld -z relro` emit a PT_GNU_RELRO header. Laying the sections out in
   * the right order is NOT enough on its own - verified by linking this
   * script without them and getting no PT_GNU_RELRO at all, so the
   * kernel had nothing to write-protect. */
  . = DATA_SEGMENT_ALIGN (CONSTANT (MAXPAGESIZE), CONSTANT (COMMONPAGESIZE));
  .init_array     : { KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
                      KEEP(*(.init_array)) }
  .fini_array     : { KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
                      KEEP(*(.fini_array)) }
  .preinit_array  : { KEEP(*(.preinit_array)) }
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
