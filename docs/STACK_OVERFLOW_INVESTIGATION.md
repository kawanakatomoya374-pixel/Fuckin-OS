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

## 5. Interrupt Stack Table: implemented and verified

The guard page (§4, now implemented) converts a kernel-thread stack
overflow into a page fault instead of silent corruption - but only if
the CPU can actually deliver that fault. The automatic exception-frame
push that happens on ANY fault normally lands on whatever stack was
already active, which for an overflow is the very stack that just ran
out - itself about to write into the (unmapped) guard page. That second
write is a second page fault while the first is still being handled: a
double fault (#DF). If #DF's own frame push ALSO lands on the same
unusable stack, there is nothing left to catch it: a triple fault, which
resets the CPU with no diagnostic output at all. The guard page fix
could therefore have turned a diagnosable (if silent-corruption-causing)
bug into a completely silent reboot, without this.

x86-64's Interrupt Stack Table exists for exactly this. A TSS carries
seven fixed stack addresses (`ist[0..6]`); an IDT gate can name one, and
the CPU switches to it BEFORE pushing anything, unconditionally,
regardless of what RSP held at the fault. The hardware structure
(`tss_t.ist[7]`) already existed in this codebase's `gdt.h` - it had
simply never been populated or referenced by any gate.

**Implemented:** a dedicated 16 KiB emergency stack
(`ist1_emergency_stack`, `src/kernel/arch/gdt.c`), wired into
`TSS.ist[0]` in `tss_init()`, and armed on the four vectors a corrupted
stack can plausibly raise - `#UD`(6), `#DF`(8), `#GP`(13), `#PF`(14) -
via a new `idt_set_gate_ist()` (`src/kernel/idt.c`).

**Verified**, not merely argued to be correct, with a purpose-built
deterministic test (`COS_VALIDATION_GUARD_PAGE_TEST`, kept in the tree
disabled by default): a kernel thread given a deliberately tiny 16 KiB
stack, forced into genuine unbounded recursion. Getting a compiler to
actually produce that took three attempts - GCC defeated the first two
by proving the recursion had a closed-form arithmetic equivalent (see
the code comments for exactly what it did each time) - the working
version uses `__builtin_alloca()` with a runtime-only size, which cannot
be reasoned about at compile time. Live GDB inspection of the running
system confirmed IST working exactly as designed:

```
rsp   0x3184110  <ist1_emergency_stack+16176>     <- CPU switched stacks
#0 page_fault_handler (...) at src/kernel/page_fault.c:162
```

and the flushed serial log showed the complete, correct diagnostic:

```
[PF] Page fault analysis:
  Address: 0x000000002A550FF0
  Error code: 0x0000000000000002
  Cause: Page not present
  Cause: Write operation
  Cause: Kernel mode access
[TASK] page fault @ 0x000000002A550FF0 pid=3
[PF] Unhandled kernel page fault - halting
```

No double fault, no triple fault, no silent reset - a clean, attributed,
logged halt. (One debugging note for whoever touches this test next:
while chasing what looked like a hang, the same log appeared completely
empty until QEMU was cleanly terminated - `-serial file:` output can sit
in a host-side buffer that is not flushed until the process exits. Not a
kernel bug; a instrumentation gotcha worth knowing about in advance.)

Also fixed as part of building this: `gui_main` is now skipped (the same
technique `COS_VALIDATION_HTML_STRESS` already used) when
`COS_VALIDATION_GUARD_PAGE_TEST` is set, because the first version of
this test raced a busy desktop for CPU time and did not complete in over
a minute of wall-clock time despite needing only a few hundred trivial
loop iterations.

**Not SMP-safe, matching an existing limitation stated plainly rather
than silently inherited:** `tss_entry` was already a single global
instance shared by every CPU before this change, not one per CPU - on
real multi-CPU hardware, loading the same TSS selector on a second CPU
while the first still holds it busy is undefined per the architecture
manuals. IST1 inherits that same constraint: two CPUs faulting into it
simultaneously would corrupt each other's handler state. Every
configuration this investigation tested used one CPU (no `-smp` flag).
Making both the TSS and IST1 properly per-CPU is one piece of follow-up
work, not two.

## 6. Kernel-thread crash *isolation* — investigated, correctly NOT attempted

The natural next step after "halt safely" is "recover fully": catch the
guard-page fault, terminate just the offending kernel thread, and let
everything else keep running - exactly what this kernel's existing CPL
check already does for a crashing ring3 *process*.

This was investigated and deliberately NOT implemented, for a reason
specific to this codebase rather than a generic caution: **this kernel's
critical sections are protected by disabling interrupts
(`sync_irq_save()`/`sync_irq_restore()` in `src/kernel/sync.c`), not by
spinlocks with any ownership tracking.** If a kernel thread calls
`sync_irq_save()`, stashes the returned flags on its OWN stack, and then
overflows before calling `sync_irq_restore()`, that saved flag value is
gone the moment the thread is torn down. Simply killing the thread and
resuming the scheduler could leave interrupts **permanently disabled
system-wide** - freezing the timer tick that drives the scheduler
itself, which is a worse and far harder to diagnose failure than the
clean, loud halt this session already achieved.

A ring3 process crash does not have this problem: user code is never
trusted to hold a kernel critical section across a fault boundary in the
first place, so killing it cannot leave the kernel's own interrupt state
inconsistent. A ring0 kernel thread has no such guarantee.

Doing this correctly would need the fault handler to know, at the moment
of the fault, whether the crashing thread currently holds any critical
section - which this codebase has no mechanism to record. That is
meaningfully more infrastructure than a page-fault handler change, and
shipping a naive version would trade a diagnosable, controlled halt for
an occasionally-worse, harder-to-diagnose freeze - the wrong direction
for a crash-safety feature to move in. Recorded here as the specific,
concrete reason, not a vague "needs more work".

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

---

## 7. "[GUIPERF] slow frame" during ordinary mouse use — three redundant triggers found and fixed

A separate but related report: even with every fix above in place, the
GUI logged frequent `[GUIPERF] slow frame ms=34` during ordinary use.
Investigated and fixed as its own, self-contained piece of work.

### Root cause

`gui_lifecycle.c`'s per-frame update treated ANY mouse position change
as equivalent to a real desktop mutation, taking the same full
composition path (wallpaper/icon layer blit, window loop, chrome, cursor
- the ~14,500+ kcycle cost already measured and commented on elsewhere in
this file) for plain cursor movement as for an actual window drag or
click. During ordinary use - moving the mouse - this is the overwhelming
majority of frames.

Fixing this took finding not one cause but **three independent places**
that each, separately, unconditionally called `gui_request_redraw()` for
a plain mouse position change - fixing any one alone left the others
fully able to defeat the fix:

1. **`src/drivers/input/mouse.c`** - the PS/2 mouse interrupt handler
   called `gui_request_redraw()` on every packet, including pure
   position-only ones. Fixed to fire only on an actual button-state
   transition (press/release), which is the event this call's original
   purpose - not losing a click - actually needs it for.
2. **`src/netsurf/cos_gui_table.c`** (`cos_netsurf_schedule_pump()`) -
   requested a redraw whenever ANY NetSurf work was still scheduled,
   which is nearly always true (`html_css_process_modified_styles` in
   upstream `content/handlers/html/css.c` reschedules itself every
   1000ms until every stylesheet finishes, and this codebase's NetSurf
   integration keeps something scheduled almost continuously during
   ordinary page activity). Throttled to once per second - long enough
   to stop dominating frame cost, short enough that a background
   completion still appears promptly. `cos_netsurf_browser_poll()`
   already runs every frame unconditionally regardless of this, so the
   underlying scheduled work is serviced at full rate either way; only
   the forced full redraw was throttled.
3. **`src/gui/core/input/gui_input.c`** (`gui_handle_input()`) - kept its
   OWN, entirely separate position tracking (`last_mouse_x/y`,
   independent of `gui_lifecycle.c`'s own) and requested a redraw for any
   change in it too. Measured as the dominant remaining source once (1)
   and (2) were fixed - 55-77 of the redraw requests during a sustained
   mouse-movement test, versus low single digits from any other source.
   Fixed the same way as (1): split out interaction events (button
   press/release/click, keyboard) from plain position change, keeping
   the unconditional immediate call only for the former.

### The actual fix: a cursor-only fast path

Rather than only removing redundant triggers, `gui_lifecycle.c` and
`gui_render_loop.c` gained a proper software-cursor fast path
(`cos_cursor_try_fast_move()`, declared in `gui.h`): a small backing
buffer holds whatever is currently under the cursor; a frame where the
mouse moved and nothing else needs redrawing restores the old position
from it, captures and draws the cursor at the new position, and flips
only those two small rects - independent of how expensive the rest of
the scene would be to rebuild, because it is never touched. Falls back
to the existing full-redraw path (which reseeds the backing buffer)
whenever the buffer cannot be proven current - the first frame, or
right after any real redraw - so it can never draw something wrong.
Scoped to when the multi-cursor feature is off, to avoid a second
sprite's backing rect needing to be reasoned about too.

### Verified

Real relative mouse-movement events injected via QEMU's QMP
(`input-send-event`, matching the emulated PS/2 mouse's actual protocol
- an earlier attempt using absolute-axis events silently went nowhere,
since this device is relative-only), then confirmed the caller of every
observed `gui_request_redraw()` by capturing `__builtin_return_address()`
and resolving it against `nm build/kernel.elf` - the specific three
sources above, not a guess:

```
[GFXPERF] cursor fast-path hits=1
[GFXPERF] cursor fast-path hits=200
[GFXPERF] cursor fast-path hits=400
[GFXPERF] cursor fast-path hits=600
[GFXPERF] cursor fast-path hits=800
```

800 fast-path hits during one sustained-movement test, versus a single
digit hit count before source (3) was found and fixed - and zero before
any of the three were addressed. Zero exceptions throughout. All
temporary diagnostic logging (`[REDRAWDBG]`, `[CURSORDBG]`) added during
this investigation was removed before the final build; `[GFXPERF] cursor
fast-path hits=` was kept as a permanent, cheap, genuinely useful counter
for the same reason the rest of this file's own GFXPERF instrumentation
was kept.

### Not chased further

A small remaining source (`gui_main_thread`, ~77 occurrences against 800
fast-path hits in the same test) was identified but not investigated -
an order of magnitude smaller than what was fixed, and the point of
diminishing returns for this specific investigation.

---


## 9. Dynamic heap sizing — implemented as a safe, additive extension

The gap identified in §3c (`g_runtime_heap_bytes` computed from detected
RAM but never actually consulted by the allocator) is now closed, without
touching the existing, proven main-heap logic at all.

### Why not a full rewrite

A truly general fix would back the entire heap with dynamically mapped
pages sized from detected RAM, replacing `kernel_heap[HEAP_SIZE]`
entirely. That was deliberately not attempted: `kmalloc`/`kfree` are the
single most pervasively-relied-upon subsystem in the kernel, and a subtle
bug in a full rewrite could cause corruption anywhere, at any time,
attributable to nothing in particular - a materially worse failure mode
than "some allocations fail" (today's behaviour) or even the stack
overflows this investigation spent most of its time on.

### What was built instead

`kernel_heap[]` is completely unchanged - identical code path, identical
behaviour, for every machine, at every RAM size. On top of it,
`kmalloc_flags()`'s existing first-fit walk, if it fails, now tries a new
extension allocator (`cos_ext_alloc()`, `src/kernel/memory.c`) before
reporting "Out of memory": ONE dedicated page range is mapped for that
SPECIFIC request via `paging_alloc_pages()` - the same primitive this
session's guard-page work already exercised extensively - used, and
unmapped as a single unit via `paging_free_pages()` on free. No
free-list, no coalescing, no splitting: deliberately a narrower contract
than the main heap's own, which is why it is called an extension and not
a second heap.

Two functions every existing allocation already trusts completely for
corruption detection - `validate_block()` and `block_is_in_heap()` - are
**not modified** to recognise these blocks. Widening what they accept is
exactly the kind of change most likely to silently break something that
already works. An extension block instead gets its own, parallel
validation (`cos_ext_find_block()`, checking a small tracked slot list
rather than an address-range formula), and `kfree()`/`krealloc()` branch
to it via a new `FLAG_EXTENSION` bit, checked *before* either function's
existing logic runs at all.

### Ceiling

Enabled at all only when detected RAM is at least 4x `HEAP_SIZE` (leaving
substantial headroom for kernel code/stacks/framebuffers regardless of
how much this allocates), and even then capped at the smaller of 25% of
available RAM or 2 GiB additional. This project's own stated "strict
QEMU validation" baseline of 2 GiB RAM does not qualify at all under this
formula - it gets exactly today's 512 MiB, unchanged. This is
specifically for machines with meaningfully more RAM than that baseline.

### Verified, not merely argued to be correct

A dedicated, deterministic, synchronous test
(`COS_VALIDATION_HEAP_EXTENSION_TEST`, boot-thread only - no dependency
on the threaded NetSurf test path, whose own unrelated regression is
recorded in §8) allocates 600 x 1 MiB blocks (600 MiB, deliberately more
than the whole 512 MiB main heap by itself), writes a distinct,
position-encoded byte pattern into each, verifies every pattern is intact
*before* freeing anything, frees all 600 in a deliberately mixed order
(every third block first, then the remainder - not simple LIFO/FIFO), and
finally confirms an ordinary small allocation still succeeds and holds
correct data afterward. Run with 3 GiB of guest RAM (comfortably clearing
the 2048 MiB/4x threshold):

```
[HEAPTEST] starting: exhaust the main heap, then verify the extension allocator
[HEAPTEST] allocated 600 blocks (600 MiB total)
[HEAPTEST] extension_hits=87 corruption_failures=0 post_free_sanity_alloc=OK
[HEAPTEST] PASSED
```

All 600 requested blocks succeeded (the main heap alone would have failed
well before block 512 given real per-allocation overhead); 87 of them
were served entirely by the new extension mechanism; zero corruption
across every sampled byte in every block, including the 87 extension
ones; the main heap's own free-list/coalescing state was confirmed intact
afterward via a fresh allocation. Zero exceptions.

Re-verified normal 2 GiB boot is completely unaffected (the extension
correctly never engages: `heap=512 MiB` reported exactly as before, zero
`Out of memory` events, zero exceptions), and that the cursor fast-path
work from §7 still functions correctly alongside this change
(`cursor fast-path hits=800` on the same mouse-injection test, unchanged).

### Known limitations, stated plainly

* Not SMP-aware in any special way beyond what the main heap already
  is - `sync_irq_save()`/`sync_irq_restore()` around the whole operation,
  same as every other allocation, which is this codebase's existing
  concurrency model throughout, not a new constraint introduced here.
* The tracked-slot table caps at 128 concurrent extension allocations
  (`COS_EXT_MAX_ALLOCATIONS`) - a 129th concurrent large allocation while
  128 are already outstanding falls through to "Out of memory" rather
  than growing the table. Given this is meant for occasional overflow
  capacity rather than sustained heavy use, this was judged an acceptable
  bound rather than something worth adding dynamic growth for.
* Each extension allocation rounds up to a whole number of pages (4 KiB),
  so it is relatively wasteful for many small allocations - by design,
  this path is intended for the large allocations most likely to exhaust
  a fragmented main heap, not as a general replacement for it.

---

## 10. IPC exposed to userspace — and three real kernel bugs it uncovered

### The gap

`src/kernel/ipc.c` had a complete implementation - message send/receive/
respond/notify, per-process mailboxes, semaphore-backed blocking receive,
shared memory create/attach - and **not one syscall reaching any of it**.
Every caller was in-kernel. From userspace the subsystem may as well not
have existed: a userspace file server or GUI server had no way to receive
a request and no way to answer one.

That is the single thing blocking the worthwhile part of a microkernel
design (isolating what benefits from isolation) without the part that
isn't worth it here (moving the scheduler, paging, interrupt handling or
the graphics hot path out of the kernel, which costs real performance for
little gain on a single-user desktop).

### What was added

Six syscalls (`src/kernel/syscall.c`): `SYS_IPC_SEND`, `SYS_IPC_RECV`,
`SYS_IPC_RESPOND`, `SYS_IPC_NOTIFY`, `SYS_SHM_CREATE`, `SYS_SHM_ATTACH`,
plus matching userland wrappers and `cos_ipc_msg_t` in
`userland/include/cos.h`.

Every one validates user pointers with `paging_user_range_ok()` before
dereferencing - messages cross a trust boundary, and a server a malformed
client request can crash is not isolation, just indirection.
`SYS_IPC_RECV` returns the payload length through a caller-supplied
header rather than through `rax`, keeping `rax` free for the `IPC_*`
status so "received an empty message" stays distinguishable from "timed
out" - collapsing those is what makes a server loop spin.

### Three bugs found by actually running it

A deterministic in-kernel check (`COS_VALIDATION_IPC_TEST`, off by
default) exercised the layer and found real defects, each hidden behind
the last:

**1. `SHM_VA_BASE` collided with the kernel identity map.** It was
`0x20000000` (512 MiB) - fine when the kernel image plus heap ended below
that. It no longer did: raising `HEAP_SIZE` to 512 MiB (§3c) pushed
`_kernel_end` to ~612 MiB, putting the shared-memory window *inside* the
kernel's own identity-mapped memory. `shm_range_is_clear()` correctly saw
those pages present and refused **every** request with
`PERMISSION_DENIED`. Shared memory was 100% broken and would have stayed
undetected, because nothing called it until these syscalls made it
reachable. Moved to PML4 slot 6 (`0x30000000000`), clear of the kernel
identity map however far it grows and of every existing userspace region
(PIE=1, stack=2, heap=3, libs=4, mmap=5).

**2. `paging_is_present()` conflated "maps to physical 0" with "not
mapped".** It was `paging_virt_to_phys(v) != 0`. Mapping to physical
frame 0 is perfectly legal and `paging_alloc_page()` can hand out frame
0, so a mapped page was reported absent. `leaf_entry()` already walked to
the leaf, so the correct present-bit test was one line away. This was a
latent hole for *every* caller asking "is anything mapped here?", not
just shared memory.

**3. `paging_alloc_page()` returned a spurious out-of-memory for any
frame inside the identity-map slack.** It identity-maps each frame it
allocates (`paging_map_page(phys, phys, ...)`), and `map_page` refuses to
overwrite a present leaf - by design. But the kernel identity-maps its
image plus `KERNEL_LOW_IDENTITY_DYNAMIC_SLACK` (64 MiB) above
`_kernel_end`, and `paging_alloc_physical()` hands out frames from
exactly that region. So every such frame hit the refusal, freed the frame
it had just successfully allocated, and returned 0 as though memory were
exhausted. Diagnosed by printing the actual faulting address (`0x2644F000`
- just past `_kernel_end`, not in the SHM range at all, which is what
ruled out the obvious suspect). Fixed by treating an existing *correct*
identity mapping as success, while still failing on a genuinely
conflicting one. Nothing about this was SHM-specific: any caller
allocating a frame in the slack got a bogus out-of-memory.

### Verified

```
[IPCTEST] roundtrip OK
[IPCTEST] empty-queue timeout OK
[IPCTEST] shm create/attach/rw OK
[IPCTEST] PASSED
```

Message round trip (send, blocking receive, sender/type/length/payload
all verified), empty-queue timeout distinct from success, and shared
memory created, attached, written and read back across 4 KiB. Zero
exceptions.

Because `paging_alloc_page()` is used kernel-wide, the other subsystem
tests were re-run against the change rather than assumed unaffected:
heap extension (`[HEAPTEST] PASSED`, 600 blocks, 87 extension hits, zero
corruption - identical to before), guard page + IST (still produces a
clean attributed `[PF]` halt), full host suite (3,226 checks, 0
failures), and the GUI cursor fast path (`cursor fast-path hits=800`,
zero exceptions).

### Not done

The syscalls are the gate, not the servers. An actual userspace file
server or GUI server, and a userspace client exercising `SYS_SHM_*`
through the real syscall boundary (the in-kernel test validates the layer
beneath the syscalls, not the marshalling), remain to be built.

---

## 11. Preemptive multitasking - confirmed genuinely active, and made a hard boot requirement

### The question

Whether `scheduler_set_preemption(1)` (called once per boot outcome from
three places in `kernel.c`) results in real preemption, or merely sets a
flag nothing acts on, is a chain of several things all needing to hold at
once - `config.enable_preemption` set and never reset, `scheduler_running`
set before anything depends on it, the PIT firing at its configured rate,
IRQ0 dispatching through `irq_install_handler()` -> `register_interrupt_handler()`
(an indirection worth naming, since grepping for the literal vector
number 32 misses it - it is computed as `32 + irq`) to `timer_tick()` ->
`scheduler_tick()`, and interrupts genuinely unmasked at the relevant
point. Reading each link confirms the chain is wired correctly; it does
not by itself prove preemption fires in practice, since a mistake in any
one link would not necessarily show up as a compile or link error.

### Empirical proof, not code-reading

A pre-existing, already-well-designed probe (`COS_SCHED_PREEMPTION_PROBE`,
present in the tree but never wired into the Makefile or run before this)
settles it directly: a spinner thread busy-loops for 200 ticks calling
nothing that voluntarily yields (`pause` is a CPU hint, not a yield), and
an observer thread does nothing but record the tick at which it first
runs. Under cooperative-only scheduling the observer could never run
until the spinner finished, since nothing would ever take the CPU back
from it. Wired into the Makefile (`SCHED_PREEMPTION_PROBE=1`) and run for
real:

```
[SCHED] PREEMPTION-PROBE PASS: observer ran during CPU-bound no-yield worker
[SCHED] PREEMPTION-PROBE ticks start/observer/end=4861/4871/5061
```

The observer recorded tick 4871 - inside the spinner's 4861-5061 window,
while the spinner was still running and had not yielded. This is direct,
empirical proof preemption is genuinely active, not merely configured.

### Made a hard requirement, not just verified once

The empirical probe above is deliberately kept opt-in (it costs ~700ms of
real boot time, a cost not worth paying on every boot for a property
that essentially never regresses silently once verified). A cheap,
always-on counterpart was added instead: `kernel_require_preemption()`,
called immediately after each of the three `scheduler_set_preemption(1)`
sites. It checks `scheduler_get_preemption()` actually reflects the
request (not just that the call returned), retries once, and calls the
existing `kernel_fatal()` panic-and-halt path if it still hasn't -
turning "silently boot into an unsupported cooperative-only mode because
some future change accidentally skipped or broke the enable call" into
a loud, immediate, unmissable failure instead. This cannot prove
`scheduler_tick()`'s internal timeslice logic is itself correct (only
the empirical probe does that); it guarantees the one link most likely
to regress silently - the enable call being present and effective - is
never silently absent.

Verified this adds no false positives: a full clean production boot
(no validation flags) shows zero `FATAL` and zero `EXCEPTION` lines, and
the GUI cursor fast-path (§7) and full host suite (3,226 checks) remain
unaffected.

---

## 12. `Server/` — the first real userspace server, proven end to end

Section 10 exposed the SYS_IPC_* syscalls; this section proves what they
were for. `Server/fileserver/fileserver.c` is a genuine, standalone
userspace process - not a kernel thread standing in for one - that
answers file-read requests from `Server/testclients/fileclient.c`, a
completely separate userspace process, entirely through the syscall
boundary added in section 10.

Full design rationale, protocol, and scope (deliberately narrow - see
`Server/README.md` for why FatFs itself staying in the kernel is a
considered decision, not a shortcut, given the bootstrap-ordering hazard
of getting that migration wrong) live in `Server/README.md` rather than
duplicated here.

### Verified booting, spawned as two real scheduled processes

```
[USERSPACE] launched fileserver.c-os pid=2 entry=0x00000080000011C0
[fileclient] read-roundtrip OK
[fileclient] missing-file NOT_FOUND OK
[fileclient] PASSED
[fileserver] shutdown requested, exiting
```

Zero exceptions. Re-confirmed the full host suite (3,226 checks), a
clean default production boot (no validation flags: zero `FATAL`, zero
`EXCEPTION`), and the GUI cursor fast-path (`cursor fast-path hits=800`)
are all unaffected by this addition.

### A Makefile gotcha worth stating plainly (not a source bug)

While re-verifying `Server/`'s test from a fresh zip extraction: running a
plain `make` first and *then* `VALIDATION_FILESERVER_TEST=1 make` on the
same tree silently reused stale `kernel.o`/`userspace_demo.o` built
without the flag - the same `-D...` value-change-not-triggering-a-rebuild
gap this session already hit for several other `COS_VALIDATION_*` flags.
A genuinely fresh checkout with the flag set on the *first* `make` call
is unaffected (confirmed directly) - this only bites when switching
flags on an already-built tree. `make clean` before changing any
`COS_VALIDATION_*`/`VALIDATION_*` flag is the reliable workaround; making
the Makefile track these as real dependencies (e.g. a sentinel file
recording the last-used flag values) would fix this properly and is
worth doing given how many of this session's own flags it has now
affected, but was not attempted here to avoid a late, broad Makefile
change this close to delivery.

---

## 13. W^X / NX / mprotect - a real, confirmed exploit-mitigation gap, found and fixed

Prompted by a broad security/ABI design document covering the kind of
userspace maturity a Linux-like OS has (PT_INTERP, TLSDESC, CET, vDSO,
namespaces, and dozens more). That full scope is genuinely multiple
sessions of work; this entry covers the single highest-value, most
foundational piece actually completed: the CPU's own execute-disable
hardware was not being engaged for any of a process's writable memory.

### The gap

`cos_elf.c`'s ELF loader already correctly set `PAGE_NX` on a program's
own non-executable `PT_LOAD` segments, gated on `cos_elf_nx_available()`.
But `task_handle_page_fault()` (task.c) - which demand-pages a process's
heap, stack, and `mmap()` regions - built its page flags as
`PAGE_PRESENT | PAGE_RW | PAGE_USER` with **no NX bit at all**, on any of
the three. Every byte of writable memory a process has after its static
image loads was executable, on any CPU that supports NX, because the bit
was simply never set for it. This is exactly the condition a classic
stack- or heap-based shellcode exploit needs.

### What was fixed

* `task_handle_page_fault()`: heap and stack pages now always carry
  `PAGE_NX` (gated on `cos_elf_nx_available()`, matching the ELF loader's
  own existing pattern - never set on hardware that does not support it,
  since that bit is reserved there and setting it faults).
* `cos_mmap_region_t` gained an `executable` field, false by default and
  **explicitly** cleared on every new region `cos_mmap_alloc()` hands
  out - not merely assumed zero. A real, separate bug was found and
  fixed here in the same pass: a reused region-table slot (after an
  earlier `mmap()`+`mprotect(PROT_EXEC)`+`munmap()`) would otherwise
  silently inherit `executable=true` from its prior occupant, making a
  brand new mapping executable without anyone asking for that - exactly
  the kind of residual-permission bug W^X exists to prevent.
* A new `SYS_MPROTECT` syscall (`cos_mprotect()` in userland), the
  correct, standard way to get executable memory at all now that
  `mmap()` itself cannot create one: **W^X is enforced at the syscall
  itself** - a request for `PROT_WRITE|PROT_EXEC` together is refused
  outright, not silently resolved one way. The supported JIT pattern is
  `mmap(RW)` → write code → `mprotect(RX)`. Already-resident pages in the
  mprotect'd range are updated in place via the existing
  `paging_protect_page()` (preserves the physical frame and its
  contents, replaces only the flags, flushes that one TLB entry) -
  necessary because a JIT's whole point is protecting pages it already
  wrote to, not ones it hasn't touched yet.
* **The actual root cause of why NX did nothing despite all of the
  above being correct**: `cos_elf_nx_enable()` - which probes
  `CPUID.80000001h:EDX[20]` and sets `EFER.NXE` - was fully implemented
  and correctly exposed via `cos_elf_nx_available()`, but was **never
  called from anywhere**. `g_nx_available` stayed false forever, so
  every `PAGE_NX`-setting call site (all correctly gated on that flag)
  silently never set the bit. Found only by writing and running the
  test below - reading the code gave no reason to suspect this, since
  every individual piece was implemented correctly. Fixed by calling it
  once from `kernel.c` (right after `paging_init()`, before `task_init()`
  creates the first process) and once from `smp_ap_entry()` (EFER is a
  per-CPU MSR - the BSP enabling it has no effect on an AP). This
  kernel's APs do not yet run general per-process scheduling (see this
  file's own SMP notes), so nothing today actually exercises an AP
  touching an NX-bit page table - enabled anyway, on the principle that
  a correctness property this cheap to establish is worth establishing
  before something depends on it.

### Verified with a real exploit attempt, not just "nothing broke"

`Server/testclients/nx_probe.c` (`COS_VALIDATION_NX_PROBE`): a real
userspace process `mmap()`s memory, writes a single `ret` instruction
(`0xC3`) into it, and calls it as a function. Before the
`cos_elf_nx_enable()` fix:

```
[nx_probe] about to call into heap memory - if NX is working this line is the last one this process ever logs
[nx_probe] UNEXPECTED: call into heap memory succeeded - NX is NOT being enforced
```

The call returned normally - full proof code execution from writable
memory was possible. After the fix, same test:

```
[ELF] NX enabled: non-executable pages will be enforced by hardware
[nx_probe] about to call into heap memory - if NX is working this line is the last one this process ever logs
[PF] RIP: 0x0000028000000000
[PF] Page fault occurred
[PF] Unhandled user page fault - terminating process
```

The fault address (`0x28000000000`) is exactly `COS_MMAP_BASE` - the
CPU refused the instruction fetch. The process's own "UNEXPECTED" line
never appears. This codebase's existing ring3 crash-isolation
(`error_code & PF_USER` → `process_exit()`, already used by every other
per-process fault) tears down only this one process; the kernel logged
zero exceptions and continued normal operation immediately afterward
(NetSurf/GUI activity resumed in the same log).

Re-verified unaffected by this change: the full host suite (3,226
checks), the real userspace file-server/client round trip (§12, still
`PASSED` - ordinary program execution from properly-marked-executable
code segments is untouched by any of this), the heap-extension allocator
test (§9, identical `87 extension_hits, 0 corruption_failures`), and a
clean default production boot with the GUI cursor fast-path (§7, still
`cursor fast-path hits=800`) - zero exceptions, zero FATAL in every case.

### Explicitly not done in this pass

The design document this responds to covers a full Linux-like userspace
maturity level: PT_INTERP/ld.so/libc/CRT hardening, TLSDESC, `RTLD_NEXT`,
real `dlclose()` unloading, ASLR entropy quality, vDSO, `auxv` alignment
with the Linux ABI, CET/shadow stack, IBT, namespaces, and more. That is
realistically several further sessions of work, each needing the same
verify-before-implementing discipline this entry followed - stated here
plainly rather than implied, so "the design document" is not mistaken
for "done."

---

## 14. `Server/apps/hello_app` — the standing "will right-click still work?" question, answered

Every time this conversation touched converting a built-in app
(Calculator, About, ...) to a real separate process, the same concern
came back: would opening it from the desktop still work? This is the
answer, proven rather than argued: a real, standalone `.c-os` GUI
application, with its own window, drawing, and live keyboard input, opened
through **exactly** the call a user double-clicking it in the file
manager already triggers.

### What it uses (already existed; nothing had ever exercised it end to end)

* `cos_win_create`/`fill`/`text`/`poll_key` (`userland/include/cos.h`) -
  wrappers over five `SYS_WIN_*` syscalls letting a ring3 process own a
  `WIN_COS_APP` window that the desktop's own compositor paints every
  frame from queued draw commands, using its own font renderer - the app
  never touches a pixel buffer directly.
* `gui_open_file_in_app()` → `cos_launch_elf_file()`
  (`gui_apps_common.c`) - the real double-click path, already correctly
  wired to FatFs (a prior session had already found and fixed two real
  bugs here: a storage.c/FatFs backend mismatch, and full-path vs
  bare-filename matching in `fs_find()`).

`hello_app.c-os` is seeded onto the real FAT32 volume at boot
(`spawn_cos_hello_app_test()`) and opened through this same path -
deliberately not the embedded-byte-array spawn every other test program
in this investigation uses, since the whole point is proving the
on-disk, double-click-shaped path works for a real GUI app.

An earlier attempt to get the app onto disk via `tools/inject_storage.py`
was reverted after actually checking what that tool's target format is:
`src/drivers/disk/storage.c` (password/settings records, its own
catalog) is a **completely separate storage subsystem from FatFs** (the
one `cos_read_file()`/`cos_spawn_elf_path_args()` actually read) - the
injection would have succeeded on its own terms while making the file
invisible to the mechanism that needed to find it. Caught by checking
`storage.c`'s actual functions before trusting the docstring's
implication, not by running it and being confused by a failure.

### Verified visually

Booted with QMP attached, the seed-and-open sequence run, and the
framebuffer captured directly with `screendump` - not inferred from
serial log absence-of-crash, since this app has no serial output at all:

- A window titled "Hello App (.c-os)", composited normally alongside the
  desktop's own icons and taskbar, indistinguishable in presentation
  from any built-in app's window.
- After sending real keypresses through QMP: the typed character
  displayed and a keypress counter correctly incremented - keyboard
  input reaching a real, separate ring3 process, and that process
  redrawing in response. Both screenshots are checked into
  `Server/apps/screenshots/`.

Zero exceptions in either capture. Re-confirmed unaffected: the full
host suite (3,226 checks), the file-server round trip (§12, still
`PASSED`), and a clean default production boot with the GUI cursor fast
path (still `cursor fast-path hits=800`).

### Where this leaves "convert the standard apps"

This is the proof of pattern, not the conversion itself - Calculator,
About, and the rest of the built-in apps still run exactly as they did
before (in-kernel functions dispatched by window kind). What this
establishes is that the replacement pattern is real and costs nothing
extra to the desktop experience: a converted app is still opened by
clicking something, still shows up as an ordinary window, still takes
keyboard input normally - it is simply, underneath, a separate process
instead of a function call. Converting an actual built-in app is future
work that can now follow this exact template rather than needing its own
end-to-end proof first.
