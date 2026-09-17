# C-OS ELF loader and dynamic linker

How to build a program for C-OS, what the loader accepts, what it
refuses and why, and how to test changes to it.

---

## 1. Building a program

```sh
make userland                              # once: builds crt0 + libcos.a
tools/cos-cc -o hello.c-os hello.c         # a program
tools/cos-cc --lib -o mylib.c-osll mylib.c # a shared library
tools/cos-cc --static -o tiny.c-os tiny.c  # a fixed-address ET_EXEC
```

`cos-cc` is a thin wrapper around `gcc`. An external project's build
system usually needs nothing beyond `CC=/path/to/tools/cos-cc`.

A minimal program:

```c
#include "cos.h"

__thread int counter;                       /* thread-local storage works */
static void __attribute__((constructor)) setup(void) { counter = 1; }

int main(int argc, char **argv, char **envp)
{
    cos_printf("hello from %s\n", argv[0]);
    cos_printf("PAGESZ=%lu\n", cos_getauxval(6 /* AT_PAGESZ */));
    return 0;
}
```

`main` now takes three arguments because the initial stack finally
carries an environment. A two-argument `main` still works — the third is
simply ignored, as on any other System V system.

### Flags `cos-cc` passes, and why

| Flag | Reason |
|---|---|
| `-ffreestanding -nostdlib` | there is no host libc |
| `-fno-stack-protector` | the guard needs a libc-provided `__stack_chk_fail` |
| `-fPIE` / `-fPIC` | the loader chooses the load address |
| `-Wl,--no-dynamic-linker` | suppresses `PT_INTERP`; see §4 |
| `-Wl,-z,now` | the kernel binds eagerly; lazy PLT binding does not exist |
| `-Wl,-z,relro` | produces `PT_GNU_RELRO`, which the loader enforces |
| `-Wl,-z,noexecstack` | the loader reads `PT_GNU_STACK` |

`validation/cos_programs/cos-pie.ld` and `cos-lib.ld` exist for anyone who
needs explicit control over layout. They work, but they do **not** produce
`PT_GNU_RELRO` — `ld` only emits that header from its own internal
script's relro bookkeeping, and `DATA_SEGMENT_RELRO_END` in a hand-written
script was not enough (verified directly). Prefer `cos-cc`.

---

## 2. What the loader supports

| Area | Supported |
|---|---|
| Object types | `ET_EXEC` (fixed), `ET_DYN` (PIE executables and shared objects) |
| Segments | `PT_LOAD`, `PT_DYNAMIC`, `PT_TLS`, `PT_INTERP`, `PT_PHDR`, `PT_GNU_STACK`, `PT_GNU_RELRO`, `PT_NOTE` |
| Relocation tables | `DT_RELA`, `DT_REL`, `DT_JMPREL` (either flavour, per `DT_PLTREL`), **`DT_RELR`** |
| Relocation types | `NONE`, `64`, `PC32`, `PC64`, `PLT32`, `32`, `32S`, `16`, `PC16`, `8`, `PC8`, `COPY`, `GLOB_DAT`, `JUMP_SLOT`, `RELATIVE`, `SIZE32`, `SIZE64`, `DTPMOD64`, `DTPOFF64`, `TPOFF64`, `IRELATIVE` |
| Symbol lookup | `DT_GNU_HASH` (Bloom filter + chain), `DT_HASH`, bounded linear fallback |
| Versioning | `DT_VERSYM` / `DT_VERDEF` / `DT_VERNEED`, both directions |
| Weak symbols | resolve to 0 instead of failing the load |
| TLS | full static TLS block, variant II, DTV, `__tls_get_addr` |
| Constructors | `DT_PREINIT_ARRAY`, `DT_INIT`, `DT_INIT_ARRAY`, `DT_FINI`, `DT_FINI_ARRAY` |
| Search paths | `DT_RPATH`, `DT_RUNPATH`, requester's directory, then `/lib:/usr/lib:/` |
| Initial stack | `argv`, `envp` **and** a full auxiliary vector |

### Limits

| | Value |
|---|---|
| Program headers | 128 |
| `PT_LOAD` segments | 16 |
| Per-segment size | 256 MiB |
| Total image | 512 MiB |
| Relocations per table | 2 M |
| Dynamic symbols | 256 K |
| `DT_NEEDED` per object | 32 |
| Objects per process | 24 |
| Dependency depth | 8 |

These are policy, not format limits: they bound how much work a hostile
file can make the kernel do.

---

## 3. What the loader refuses, and what to do instead

| Refused | Why | Fix |
|---|---|---|
| `PT_INTERP` present | the program wants an `ld.so` this system does not have; starting it anyway jumps into a program whose GOT was never populated | link with `--no-dynamic-linker` |
| `R_X86_64_TLSDESC` | a TLS descriptor's first word is a resolver called at **ring3** on every access; writing it as a plain offset would be silently wrong | build without `-mtls-dialect=gnu2` |
| `GOTPCREL`, `TLSGD`, `GOTTPOFF`, … | link-time-only types; seeing one means a partial link | do not pass `ld -r` output to the loader |
| `ET_REL` / `ET_CORE` | an object file is not a program | link it |
| Overlapping `PT_LOAD` **byte** ranges | two segments claiming the same bytes with different `p_flags` is a W^X bypass | — (crafted input) |
| `ET_DYN` with `e_entry == 0` offered as a program | that is a library | load it as `.c-osll` |
| Executable with `DT_NEEDED` passed to `cos_elf_load_program()` | its imports would be unbound | go through `cos_link_load_executable()` |
| `dlopen` of a library with its own `PT_TLS`, after startup | the static TLS block is fixed once the process starts | link the library in at build time |

Segments sharing a **page** (as opposed to a byte range) are normal linker
output and are accepted; the page gets the union of both segments'
permissions, which is what Linux does.

---

## 4. Architecture

Loading is four phases, because a dynamic linker cannot do them in one
pass:

```
inspect   read deps / SONAME / search paths / size / TLS from the FILE,
          before anything is mapped
   |
map       map every PT_LOAD at the chosen bias, parse PT_DYNAMIC.
          Pages are mapped WRITABLE regardless of declared permissions,
          because relocations must still be written into them (including
          into .text, for DT_TEXTREL). Nothing runs in this address
          space yet, so nothing can reach that writability.
   |
relocate  apply every relocation table, resolving external symbols
          through the caller's resolver
   |
protect   apply the FINAL permissions: W^X per segment, NX where the CPU
          allows it, PT_GNU_RELRO dropped to read-only
```

| File | Role |
|---|---|
| `src/kernel/cos_elf.c/.h` | maps and relocates **one** object. No filesystem, no policy about where dependencies come from. |
| `src/kernel/cos_elf_link.c/.h` | the dynamic linker: per-process namespaces, dependency graph, global symbol scope, search paths, `dlopen`/`dlsym`/`dlclose`, TLS layout, link map |
| `userland/lib/cos_crt0.S` | captures RSP at entry, hands it to the C runtime |
| `userland/lib/cos_rtld.c` | ring3 half: walks auxv, applies ifuncs, runs constructors, `__tls_get_addr`, per-thread TLS |
| `tools/cos-cc` | the documented build path |
| `validation/elfloader/` | host conformance tests |

The split matters: it is what lets the loader be tested on its own
against a simulated MMU, with no filesystem and no scheduler.

### What the kernel deliberately does *not* do

`DT_INIT`, `DT_INIT_ARRAY`, `DT_PREINIT_ARRAY` and `R_X86_64_IRELATIVE`
resolvers are all **ring3 code chosen by the file being loaded**. Calling
any of them from the kernel would execute user code at ring0 — the same
shape as the privilege-escalation bug this tree already found and fixed
once in its signal delivery path.

Instead the kernel publishes a **link map**: a read-only page in the
process's own address space, advertised through a private auxiliary-vector
tag (`AT_COS_LINKMAP`, `0x434F5300`). `cos_rtld.c` reads it, applies the
ifuncs, then runs the constructors — dependencies first.

A program that ignores the link map still runs. It just gets no
constructors and no ifuncs, which is exactly what the loader did before
any of this existed.

### Thread-local storage

x86-64 variant II. The thread pointer (`%fs` base) is at the **top** of
the block and every module sits at a negative offset from it:

```
  low
  +------------------+ <- block base
  | module N data    |
  | ...              |
  | module 1 data    |
  +------------------+ <- tp     *(void**)tp == tp  (self-pointer)
  | TCB              |
  +------------------+
  | DTV[0..modules]  |
  +------------------+
  high
```

The self-pointer is not decorative: `mov %fs:0, %rax` is how a program
materialises the thread pointer as an ordinary address.

`%fs.base` lives in an MSR and is **per-CPU**, so it is saved and restored
on every context switch alongside the FPU state (`thread_t::fs_base`,
`scheduler_set_thread_tls()`). Without that, every thread would read TLS
through whichever base ran last — which is why `__thread` could not work
here before, even though building the block was never the hard part.

`cos_thread_create()` gives each new thread its own block automatically.

---

## 5. Address space layout

| Region | Base | Purpose |
|---|---|---|
| PML4[1] | `0x00000080_00000000` | program image (PIE default base) |
| PML4[2] | `0x00000100_00000000` | user stack |
| PML4[3] | `0x00000180_00000000` | heap (`sbrk`) |
| PML4[4] | `0x00000200_00000000` | libraries (bump allocator, 2 MiB aligned) |
| ” | `0x00000278_00000000` | static TLS block |
| ” | `0x0000027C_00000000` | link map (read-only) |
| PML4[5] | `0x00000280_00000000` | `mmap` |

Library bases come from a bump allocator sized by each object's actual
span. The previous scheme assigned them as *slot index × 256 MiB*, which
silently overlapped for any library larger than the stride.

---

## 6. Testing

```sh
make check-elfloader          # or: sh validation/elfloader/run.sh
COS_ELF_VERBOSE=1 sh validation/elfloader/run.sh   # with loader logs
```

This compiles `cos_elf.c` and `cos_elf_link.c` **unmodified** against a
simulated MMU (`sim_mmu.c`) and runs them over binaries built by the host
`gcc`. The thing under test is the shipping source, and the inputs are
real toolchain output — not hand-written fixtures that only exercise what
the loader already does.

Current: **184 checks, 0 failures.**

| Fixture | Covers |
|---|---|
| `static_exec.elf` | `ET_EXEC`, `.bss` zeroing, W^X |
| `pie_exec.elf` | PIE, `R_X86_64_RELATIVE`, RELRO |
| `pie_relr.elf` | **`DT_RELR` with `RELASZ == 0`** |
| `tls_lib.so` | `PT_TLS`, `DTPMOD64`/`DTPOFF64`, TCB, DTV, ifunc |
| `base_lib.so` + `dep_lib.so` | cross-object `JUMP_SLOT`, weak imports, load order |
| `ver_lib.so` | two versions of one name, default-version selection |
| `dyn_exec.elf` | `DT_NEEDED`×2 + `R_X86_64_COPY` + `PT_TLS` + constructor |
| `cosapp.c-os` | the documented `tools/cos-cc` path, end to end |

The simulator poisons fresh frames with `0xAA` rather than zeroing them,
so a failure to zero a page shows up as a test failure instead of passing
by luck. It also keeps a **separate page table per address space**, which
is what lets a test assert that a program was mapped into the *process*
and not into whoever happened to be running.

`pie_relr.elf` deserves a note: it has `DT_RELASZ == 0` and only
`DT_RELR`. A loader that reads `DT_RELA` alone applies **zero**
relocations to it and reports success. The test checks the relocated
pointer value, which is what catches that.

---

## 7. Bugs found while doing this

Three in the new code, caught by the harness before any of it reached the
kernel:

1. **`cursor_push_str` did not reserve the NUL terminator**, so every
   string's terminator landed on the first byte of the previous string.
   Invisible if you only check `argv[0]` — nothing overwrites that one.
2. **RELRO rounded its end address up**, write-protecting `.data` and
   `.bss`, which share the last RELRO page in real `ld` output. glibc
   aligns both ends *down*; so does this now.
3. **Unversioned lookups matched hidden versions.** `name@OLD` exists so
   old binaries keep reaching the old implementation; an unversioned
   reference must bind to `name@@CURRENT`.

Four pre-existing, in code this replaces or touches:

4. **`cos_launch_elf_image()` never called `paging_switch_directory()`.**
   The loader resolves every address against the *active* tables, and
   `paging_create_directory()` zeroes PML4[1..255] on a new directory, so
   a freshly created process cannot already be active. `cos_elf.h`
   documented the requirement and every other cross-address-space path in
   the tree honours it; this one did not. The harness now has a test that
   fails if a program leaks into the caller's address space.
5. **`g_lib_slots[COS_LIB_MAX_PER_PROC]` was a single global array**
   shared by every process, so two programs each loading two libraries
   exhausted it. Library bases were also assigned by slot index × fixed
   stride.
6. **Leaked processes on every rejected image.** `cos_elf_load()`'s
   contract says the caller must discard the address space on failure;
   the launcher and `spawn_cos_elf_process()` both just returned.
7. **`cos_crt0.S` was NASM source in a `.S` file.** Compiler drivers hand
   `.S` to GAS, so it could only ever be built by invoking `nasm` by hand
   — which nothing in the Makefile did. It is GAS syntax now.

Also cleaned up: `syscall.c` and `cos_elf.c` declared `paging_*` functions
by hand as `unsigned long`/`int` where the real signatures are
`uint64_t`/`bool`. Call-compatible on x86-64 by luck, not contract; they
now use the header.

---

## 8. Known gaps

- **No `ld.so` support.** Mapping an external interpreter and giving it
  control is a coherent next step; refusing `PT_INTERP` is the honest
  interim behaviour.
- **No lazy PLT binding.** Binding is always eager. This is what makes
  RELRO possible at all — a lazily-bound GOT must stay writable for the
  life of the process.
- **`dlclose()` retires the handle but keeps the mapping.** Unmapping
  would require finding and undoing every relocation pointing *into* the
  object from anywhere else, and there is no reverse index. Dangling
  function pointers in another library's GOT would be far worse than
  holding address space until exit.
- **No ASLR.** The bias is fixed. The loader takes a caller-chosen bias,
  so adding it is a change in one place.
- **`AT_RANDOM` is a weak seed**, derived from the pid and a pointer
  value. Labelled as such rather than presented as entropy — a libc that
  believes it has an unpredictable stack guard when it does not is worse
  off than one that knows it does not.
- **NX is off by default.** `cos_elf_nx_enable()` checks
  `CPUID.80000001h:EDX[20]` and sets `EFER.NXE`, but `EFER` is per-CPU:
  every AP must call it before any page table carries an NX bit,
  otherwise bit 63 is a reserved-bit violation on that CPU. Until the SMP
  bring-up path calls it, W^X means "not writable" rather than "not
  executable".
- **No `dlopen` of TLS-using libraries after startup.** Refused
  explicitly rather than left with a module id pointing at storage that
  does not exist.
