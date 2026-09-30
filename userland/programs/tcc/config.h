/* config.h - build configuration for tcc.c-os, the on-device TinyCC.
 * (Upstream generates this with ./configure; the C-OS build does not run
 * configure, so it is written by hand.) */
#define TCC_VERSION "0.9.28rc"
#define TCC_TARGET_X86_64 1
/* the compiler runs on C-OS itself: no cross prefixes, no triplets, nothing
 * that assumes a host libc directory layout */
#define CONFIG_TCCDIR "/system/sdk"
#define CONFIG_TCC_PREDEFS 1
#define CONFIG_TCC_STATIC 1
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0
/* tccrun.c's -run memory layout: 0 (its default off Apple) puts .text and everything
 * else in ONE combined RWX region - simplest, but exactly the write+execute-together
 * grant the kernel's SYS_MPROTECT refuses (W^X, real - see docs/ELF_LOADER.md and
 * src/kernel/syscall.c's SYS_MPROTECT comment). 1 is the same layout already used for
 * Apple's hardened runtime: .text rx, .rodata ro, .data/.bss rw, none of them ever both
 * writable and executable at once, so every mprotect -run makes is one this kernel
 * actually allows. */
#define CONFIG_RUNMEM_RO 1
/* tccrun.c's rt_mem() otherwise reaches for tcc_malloc() (plain heap memory) on any
 * platform that isn't Windows, macOS, or built with CONFIG_SELINUX - which the kernel's
 * SYS_MPROTECT cannot act on at all, only on a region it tracked from an actual cos_mmap()
 * call (see task.h's design note on cos_mmap_region_t: mprotect here is scoped to real
 * mappings, not "any page-aligned address," the way a real OS's is). Forcing tccrun.c's
 * alternate VirtualAlloc()-based path - normally Windows-only - repoints -run's one
 * allocation through tcc_shim.c's VirtualAlloc, which calls real cos_mmap(); everything
 * downstream (protect_pages) already calls plain mprotect() on any non-_WIN32 build
 * regardless of this setting, so this changes ONLY where the memory comes from, not how
 * it is later protected. */
#define CONFIG_RUNMEM_VIRTUALALLOC 1
#define ONE_SOURCE 1
