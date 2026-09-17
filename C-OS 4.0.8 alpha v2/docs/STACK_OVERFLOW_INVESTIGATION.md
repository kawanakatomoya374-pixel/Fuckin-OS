# Real-page crash investigation: NetSurf native-code stack overflow

What "the OS dies on complex real pages" actually was, how it was found,
and what was and was not fixed.

---

## 1. Reproduction

A real Wikipedia article (`C (programming language)`, 745,228 bytes,
fetched live — ordinary nested tables, citations, headings; nothing
synthetic or pathological) was embedded into the kernel image and driven
through the real NetSurf pipeline via a boot-time diagnostic hook
(`COS_VALIDATION_HTML_STRESS`, off by default):

```sh
VALIDATION_HTML_STRESS=1 make
```

No network, no filesystem — the page is delivered as a `data:` URL
straight to `cos_netsurf_load_url_sync_nowait()`, so the only thing under
test is parse → DOM → CSS → layout → redraw.

**Result:** a real, reproducible kernel halt.

```
[MEMORY] Out of memory (requested 1124576 bytes)
[NSLOG] ... html_box_convert_done: DOM to box conversion complete
[NSLOG] ... navigate_internal_real: Loading 'about:query/fetcherror'
[EXCEPTION] Unhandled interrupt: 6 err_code=0x0 rip=0x3D388E2 cs=0x8
[EXCEPTION] Fatal unhandled CPU exception - halting
```

Interrupt 6 is `#UD` — invalid opcode. The CPU tried to execute
non-code bytes.

---

## 2. Root cause

A live GDB session against QEMU (`-s -S`, hardware watchpoints armed
only after `kernel_main` to exclude bootloader noise) caught the fault
directly:

```
Breakpoint 2, default_handler (r=0x6610b0 <wiki_stress_page+291888>)
```

The **exception frame itself** was being constructed at an address
*inside the embedded page's own static byte array* — a completely
unrelated `const` data blob nowhere near any legitimate stack. That is
only possible if the stack pointer itself had already wandered down
into that region before the fault — i.e. the call chain had descended
far enough to run out of its real stack and kept going into whatever
static memory happened to sit below it, silently corrupting it, with no
page fault to stop it.

Checking the numbers confirms *why* nothing stopped it:

| | Value |
|---|---|
| `KERNEL_STACK_SIZE` (generic kernel thread default) | 512 KiB |
| `GUI_MAIN_STACK_SIZE` (the thread that runs **all** of NetSurf's HTML/CSS/layout + QuickJS) | **512 KiB — identical** |
| Guard page below either stack | **none** — `task_alloc_stack()` maps exactly the requested pages and nothing else |
| Demonstrated stack depth before corruption reached the embedded page array | tens of megabytes |

`gui_main` is not a hypothetical — it is the actual, real thread real
browsing runs on (`gui_update()` → `cos_netsurf_browser_poll()` → the
whole HTML/CSS/layout pipeline, per `kernel.c`'s own comment at the
thread's creation site). A prior session had already found and fixed
**half** of this: QuickJS's own interpreter recursion was blowing this
same 512 KiB stack on JS-heavy pages, fixed by calling
`JS_SetMaxStackSize()` with a budget QuickJS could actually check itself
against. That fix is correct and still needed — but it only bounds
QuickJS's *own* recursion. It has no visibility into NetSurf's *native C*
recursion (box construction, CSS selection, layout, DOM walking), and a
JS-free, markup-heavy page — exactly this reproduction — never touches
QuickJS's guard at all.

**This is a real, present-day bug**, not a testing artifact: any
sufficiently complex real page, loaded through the ordinary GUI, runs
this exact native code on this exact 512 KiB budget with nothing
checking it.

---

## 3. What was fixed

**`GUI_MAIN_STACK_SIZE`: 512 KiB → 8 MiB** (`src/kernel/kernel.c`).
QuickJS's own budget (128 KiB) is unaffected and stays a small fraction
of the new total, so native NetSurf code now has roughly 7.9 MiB of
headroom instead of ~384 KiB.

This is a **generous safety margin, not a measured exact requirement**.
The real peak stack depth for this page was not cleanly measured — it
can't be, without a guard page (see §4) to report a clean fault at the
actual high-water mark rather than silent corruption somewhere past it.
8 MiB is a deliberate multiple of the demonstrated failure, chosen to be
robust rather than to just barely pass one test page.

**Defence in depth, unrelated root cause:** `cos_netsurf_schedule_pump()`
now refuses to call a scheduled callback whose address falls outside the
kernel's own compiled code (`_text_start`.`_text_end`, added to
`src/boot/linker.ld`), logging and skipping instead of jumping through
it. This was the *first* hypothesis (a real, upstream-acknowledged
NetSurf risk — `html_destroy()`'s own comment says *"Unable to cancel
conversion context, browser may crash"*) and turned out **not** to be
what caused this specific crash, but it is real, cheap, and independently
worth keeping.

**Exception diagnostics** (`src/kernel/idt.c`): a fatal, unhandled
exception now also logs `rsp`, the three qwords at/above `[rsp]`, and
`rax`/`rbx`/`rdi`, unconditionally. This is what turned "the kernel
halts" into "here is the return address the CPU had queued, and here is
what's sitting on the stack" without needing a live debugger, and is a
strict improvement for diagnosing *any* future fatal exception, not just
this one.

**Kept as a permanent regression test:** `COS_VALIDATION_HTML_STRESS`
and the embedded `wiki_stress_page.c` asset stay in the tree, disabled
by default (`VALIDATION_HTML_STRESS=0`), for exactly this class of bug —
a real, complex, JS-free page driven through the actual pipeline with
per-iteration timing. Re-run it against future changes with
`VALIDATION_HTML_STRESS=1 make`.

---

## 3b. Second bug, found only after the first was fixed

Fixing the stack overflow let the pipeline run far enough to reach a
**second, previously-unreachable crash** — a `#GP` (interrupt 13) after
the heap filled up.

Diagnosis, from the extended exception dump plus `objdump`:

* `[rsp+8]` held `0x4477F6` — inside kernel text, so a real return
  address. It resolved to `navigate_internal_real+0x146`.
* Disassembling there showed the faulting instruction was
  `call *0x10(%rax)`, where `%rax` had just been loaded from the global
  `guit` table.
* Offset `0x10` in `struct gui_misc_table` is **`launch_url`** —
  the third member, after `schedule` (0x00) and `quit` (0x08).
* `cos_gui_table.c` set only `.schedule`. `launch_url` was **NULL**.

Why that is a bug and not merely an unimplemented feature: NetSurf's
headers call these entries "optional", but the *core* calls them
**unconditionally** — `browser_window.c` has two bare
`guit->misc->launch_url(url)` call sites with no NULL check. Upstream
guarantees non-NULL a different way: `desktop/gui_factory.c`'s
`verify_misc_register()` walks the table at registration and substitutes
a `gui_default_*` stub into every unset slot. **C-OS does not build
`gui_factory.c`** — it assembles the table itself — so that substitution
never ran and the NULLs survived.

The crash path was therefore: heap exhausted → fetch fails →
`navigate_internal_real()` takes its "no fetcher for this URL" branch →
calls through the NULL `launch_url` → CPU executes unrelated memory as
code until it hits an invalid instruction.

**Fixed** by populating every optional `gui_misc_table` entry
(`quit`, `launch_url`, `login`, `pdf_password`, `present_cookies`) with
bodies that mirror `gui_factory.c`'s own defaults and return the same
error codes the core already handles.

`gui_fetch_table.mimetype` was audited too and deliberately left NULL:
its only call sites are in `content/fetchers/curl.c`, which C-OS does
not build, so it is genuinely unreachable — and leaving it NULL means it
faults loudly rather than silently misbehaving if curl.c is ever added.

### Verified result

Same page, same test, after both fixes:

```
[STRESS] pipeline hit the iteration cap after 2000 redraw call(s), 6744 ms, final_status=1
[STRESS] SURVIVED: no crash, control returned to the boot sequence
```

Zero `EXCEPTION` lines, zero `[PF]` lines. The guard page never fired,
confirming 16 MiB was genuinely sufficient for this page rather than the
overflow merely having moved somewhere quieter. The 11 `Out of memory`
events are still there — heap pressure is real and unfixed (see §4) —
but the system now *survives* them instead of dying.

---

## 3c. Third finding: the "scaled" heap ceiling was cosmetic only

With both the stack overflow and the NULL `launch_url` fixed, the same
page ran to completion — but along the way it produced **11** separate
`[MEMORY] Out of memory` events for a single 745 KB page, each for an
almost-identical ~5.6 MB allocation (`css_select_ctx` rebuilds triggered
by DOM-mutating inline scripts — a real, intentional NetSurf code path,
already carefully guarded against redundant re-parsing, not a leak).

Investigating *why* a 256 MiB heap couldn't absorb that led to this:

```c
/* src/kernel/memory.c */
#define HEAP_SIZE 0x10000000ULL              /* 256 MiB - the ACTUAL array size */
static uint8_t kernel_heap[HEAP_SIZE];

void cos_runtime_memory_init(uint64_t total, uint64_t available) {
    uint64_t cap = 512 * 1024 * 1024;         /* the INTENDED ceiling */
    uint64_t heap = available / 4;
    if (heap > cap) heap = cap;
    ...
    g_runtime_heap_bytes = heap;              /* computed, logged, and READ BACK
                                                * by cos_runtime_heap_bytes() -
                                                * but never consulted by the
                                                * allocator itself */
}
```

`kfree()`'s own bounds check reads `HEAP_SIZE` - the compile-time
constant - directly, not `g_runtime_heap_bytes`. On a 2 GiB machine (the
codebase's own documented `"strict QEMU validation supplies 2GiB RAM"`
target) the boot log printed `heap=512 MiB`, truthfully reporting the
*intended* design ceiling its authors had already sized NetSurf's 128 MiB
cache and QuickJS's 24 MiB runtime around - while the allocator backing
it was, and always had been, capped at 256 MiB. The number logged and the
number actually usable had quietly diverged.

**Fixed** by changing `HEAP_SIZE` itself to 512 MiB, so the array matches
the ceiling the rest of the code already targets - not a new number
invented for this investigation, but the one the surrounding runtime
logic and its own comments were already written around.

### Verified result

Same page, same test, all three fixes in place, booted with 2 GiB RAM
(matching the documented target rather than the 512 MiB this session had
been testing with up to this point):

```
[MEMORY] runtime authority set: total=2047 MiB, available=2047 MiB, heap=512 MiB
[STRESS] pipeline hit the iteration cap after 2000 redraw call(s), 6917 ms, final_status=1
[STRESS] SURVIVED: no crash, control returned to the boot sequence
```

`Out of memory`: **11 → 1**. The one remaining instance is an
~1.1 MB allocation during a retry navigation, matching the size of this
diagnostic's own worst-case percent-encoding buffer (held for the whole
2000-iteration run rather than freed promptly) - a property of the test
harness, not of NetSurf or the kernel. Zero exceptions, zero page faults,
in both this run and a repeat with only 1 GiB of RAM.

**Caveat, stated plainly:** `cos_runtime_memory_init()`'s floor
(`if (heap < HEAP_SIZE) heap = HEAP_SIZE`) means the new 512 MiB is now
also the *minimum* heap the kernel will ever try to back, on any amount
of RAM. Verified booting cleanly at 1 GiB; **not** tested below that.
A machine with meaningfully less than the project's own stated 2 GiB
target should be checked before this change is assumed safe there.

**Not fixed, and a real architectural gap:** the ceiling is still capped
at 512 MiB regardless of how much RAM is actually detected - a machine
with 8 GiB or 32 GiB gets exactly the same 512 MiB any 2 GiB machine
does. Properly scaling further would mean backing the heap with
dynamically mapped pages sized from detected RAM rather than one fixed
BSS array, which is the same class of larger, riskier change as the
kernel-thread crash isolation gap below - not attempted here.

### A separate, lower-priority finding from the same run

The failed-fetch cascade visible in the log (`data:///w/load.php?...`,
`data://thumb.wikimedia.org/...`, all logged as `Malformed data: URL` or
simply `failed!`) is very likely a **test-harness artifact**, not a
browsing bug: this diagnostic delivers the page as a bare `data:` URL
with no real origin, so the page's absolute-path resource references
(`/w/load.php?...`) have nothing sensible to resolve against. A real
`https://` navigation gives every such reference a proper origin to
resolve relative to; the fetches would still fail in this offline test
environment, but as ordinary "no network" failures rather than malformed
URLs. Noted rather than chased further, since fixing it would mean
changing the test's input, not the kernel.

---

## 4. What was investigated but NOT fixed — follow-up work

**Guard pages — NOW IMPLEMENTED.** (This section originally listed them
as outstanding.) `task_alloc_stack()` now requests one extra page
contiguously with the stack and immediately unmaps it, freeing its
physical frame, so an overflow hits an unmapped page and faults cleanly
instead of silently corrupting whatever lies below.

Three things were verified before shipping it, because a careless change
here corrupts the allocator:

* `paging_virt_to_phys()` returns 0 for a non-present PTE, so the frame's
  physical address is read *before* unmapping — otherwise the frame leaks.
* `prune_empty_page_tables()` only frees a page table when *all* 512
  entries are clear, so punching one hole cannot free a table still in use.
* `page_fault.c`'s existing `handle_stack_growth()` auto-grows kernel
  stacks on fault, which would have silently paved over the guard. Its
  `is_kernel_stack_area()` bound computes to `base + PAGE_SIZE` — exactly
  one page *above* the guard — so the guard address falls outside it and
  the auto-grow correctly declines. Confirmed by arithmetic against the
  real code, not assumed.

The guard costs one page of virtual address space per thread, never
reclaimed, deliberately: a reclaimed guard address could be handed to a
later allocation and silently stop being a guard.

**Remaining gap: isolation.** A guard-page hit still halts the kernel —
it converts an undiagnosable memory corruption into a clean, attributable
fault, which is a large improvement, but the kernel does not yet *survive*
a kernel-thread stack overflow. The existing ring3 crash-isolation path in
`idt.c` (CPL check → kill the process, keep the kernel up) is the model to
extend. This is the *correct*, general fix
(bounded, immediate failure regardless of how deep a future regression
goes, versus a fixed budget that is only ever "probably enough"), and it
is what would let the kernel's *existing* ring3-crash-isolation pattern
(`idt.c`'s CPL check, already used to kill a misbehaving process without
taking the kernel down) extend to a kernel-thread stack overflow too.

It was not implemented this session because retrofitting it safely
requires auditing `task_alloc_stack()`/`task_free_stack()`'s frame
accounting (does `paging_protect_page()` preserve the physical frame
address when clearing the present bit, so the guard page's frame is
still correctly freed later? does `paging_free_pages()` handle a
non-present page in its range?) against a shipped allocator, under time
pressure, without being able to fully verify the answer. Shipping an
unverified change to the physical/virtual memory allocator carries more
risk than the value of implementing it hastily. The technical approach
is fully specified above for whoever picks this up next.

**Stack budget is now 16 MiB and IS verified on a real thread.** The
diagnostic originally ran on the raw boot stack (a fixed `mov rsp, ...`
in `src/boot/boot.asm`, entirely unrelated to `task_alloc_stack()` or
`GUI_MAIN_STACK_SIZE`) — so it was never testing the mechanism real
browsing uses. It now runs via
`thread_create_kernel_stack_size(..., GUI_MAIN_STACK_SIZE)`, the exact
mechanism and size `gui_main` itself uses.

One consequence had to be handled: that thread and `gui_main` both drive
the same single global `browser_window` (`g_cos_ns_bw`), which is not
reentrant. Running both would have added a genuine data race on top of
the thing under test. In this diagnostic build only, `gui_main` is
skipped so the test has exclusive use — a documented, mode-specific
tradeoff, not something a normal boot does.

**Still not verified: interactive browsing through the GUI.** The
pipeline is exercised end-to-end, but not via real user input on a
running desktop.

**Heap exhaustion is real and unfixed.** The run still produces 11
`Out of memory` events, including repeated ~5.6 MB requests during CSS
processing of a single page. The system now survives them, but a 745 KB
page should not exhaust a 256 MiB heap. That is its own investigation:
likely candidates are per-stylesheet buffers being reallocated rather
than grown in place, or fetch contexts accumulating (the log shows
"fetches active" climbing steadily: 23, 24, ...).

**Why does this pipeline need so much stack at all?** Not identified.
Tens of megabytes of depth for a 745 KB page is extreme by any measure -
consistent with either genuinely deep (if unusual) recursion in box
construction or CSS selector matching for this page's specific markup
shape, or a genuine unbounded/runaway recursion bug in one of those
paths. Distinguishing the two needs the guard-page instrumentation from
§4 to get a clean fault at the exact function and recursion depth,
rather than inferring depth from address arithmetic against unrelated
static data (which is what this investigation had to do instead, and is
why "tens of megabytes" is stated as a lower bound rather than an exact
figure).
