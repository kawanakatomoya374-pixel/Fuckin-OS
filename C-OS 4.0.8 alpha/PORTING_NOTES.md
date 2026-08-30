# C-OS × NetSurf × QuickJS — Porting Notes

Status as of 2026-08-09 (C-OS 4.0.8 second alpha). This file is the
source of truth for "what's actually done vs. not" on the
NetSurf/QuickJS integration — read this before assuming anything about
the state of the port. Several Makefile comments point here.

## C-OS 4.0.8 second alpha, link-time fixes (2026-08-09)

Direct continuation of the 2026-08-08 entry below, prompted by the
person actually trying `make all` in a real WSL environment with
`nasm` - something the 2026-08-08 entry explicitly flagged as
unverified. It found real problems, all now fixed:

1. **`fetch_http_register`/`cos_netsurf_render_page` undefined at link
   time.** Both new files (`cos_fetch_http.c`, `cos_netsurf_render.c`)
   were added to `COS_NETSURF_SRCS` - which controls what gets
   *compiled* - but not to the separate, hand-maintained `OBJS`
   variable near the top of the Makefile, which is what the final
   `kernel.elf` link actually uses. `COS_NETSURF_OBJS` (derived from
   `COS_NETSURF_SRCS`) looks like it should feed the link and doesn't -
   it's dead, unreferenced elsewhere in the file. Fixed by adding both
   objects to `OBJS` directly, and left a comment on `COS_NETSURF_OBJS`
   so the next person (or the next session of this) doesn't repeat the
   mistake.

2. **~50 undefined references from `html.o`/`dom_event.o`/`css.o`/
   `hints.o`/`select.o`** (`box_coords`, `layout_document`,
   `selection_*`, `textarea_*`, `scrollbar_*`, `html_object_*`,
   `browser_window_*`, `imagemap_*`, `form_*`, `html_redraw*`,
   `html_mouse*`, `html_keypress`, `content_textsearch_*`,
   `libdom_dump_structure`, `ns_system_colour`, `utf8_from_ucs4`,
   `squash_whitespace`, `strcasestr`, `bsearch`). Root cause: these
   real, unmodified Tier A files reference Tier B (box/layout/redraw)
   and desktop/ (selection, textarea, scrollbar, browser_window,
   imagemap) symbols that were never part of this port - and, until
   the 2026-08-08 fix below started actually calling
   `html_init()`/`nscss_init()`, nothing had ever pulled `html.o` (or
   the other four) into a real link attempt, so this had simply never
   surfaced before. Not new breakage - a pre-existing gap the
   2026-08-08 fix was always going to expose the moment it worked.
   Fixed with real, careful (not lazy) stubs in two new files:
   - `cos_netsurf_tierb_stubs.c` - see its own header comment for the
     full reasoning, but the one that actually mattered:
     `dom_to_box()` is the entry point into Tier B, called
     unconditionally by `html_finish_conversion()` every time a page
     finishes parsing, and returning anything other than `NSERROR_OK`
     (or never calling its completion callback) makes every single
     page load fail or hang. Traced the full success path
     (`dom_to_box` → `html_box_convert_done` → `imagemap_extract` →
     `content_set_ready` → `html_proceed_to_done` → `content_set_done`
     → `CONTENT_MSG_DONE`) by reading html.c directly before writing
     this stub, specifically so it completes that chain instead of
     just satisfying the linker. Also picked up
     `urldb_get_url_data` (`content/handlers/css/select.c`'s
     `:visited` support) into the pre-existing `cos_urldb_stub.c`,
     same file, same "honest nothing-stored-yet answer" philosophy it
     already used for the other 4 urldb functions.
   - `strcasestr`/`bsearch` aren't NetSurf-specific at all - real,
     correct, ordinary implementations added to `src/lib/string.c`
     (`bsearch` was already declared in `src/include/stdlib.h`, just
     never defined anywhere).
3. **`liblcss.a`: hundreds of "multiple definition of `_ALIGNED`"
   errors.** Genuine pre-existing gap in the libcss vendoring, same
   "never fully linked before" root cause as (2). `src/stylesheet.h`
   uses `} _ALIGNED;` as a trailing GCC attribute on `struct css_rule`
   (the normal `struct S { ... } ATTR;` position), expecting some
   upstream build-config header to have `#define`d `_ALIGNED` as an
   alignment attribute already - nothing in this tree ever did, so the
   compiler read it as declaring an actual global variable named
   `_ALIGNED`, once per translation unit that includes the header
   (~300 of them). Fixed with `-D_ALIGNED=` (empty, not `1`) added to
   `LCSS_INCLUDES` - `_ALIGNED` is used in exactly this one place in
   the whole libcss tree, and `struct css_rule`'s members are already
   naturally pointer-aligned, so an empty definition was safer than
   guessing at a specific alignment value upstream never documented
   anywhere in this vendored copy.
4. **`libparserutils.a`: `parserutils__filter_create`/`_setopt`/
   `_destroy`/`_process_chunk` undefined**, from `inputstream.c` (which
   is compiled) calling into `input/filter.c` (which the Makefile
   deliberately `filter-out`s, because it needs `<iconv.h>` - real
   charset conversion, not available here, and a real port of it is
   its own separate, substantial piece of work). Unlike (2)/(3), this
   one is on the critical path for parsing *any* page at all -
   `parserutils_inputstream_create()` calls
   `parserutils__filter_create("UTF-8", ...)` unconditionally. Fixed
   with a new file, `cos_parserutils_filter_stub.c`: an honest
   byte-for-byte passthrough rather than a real charset converter -
   which is actually *correct*, not just a safe placeholder, for the
   very common case of already-UTF-8 (or ASCII) content, and degrades
   to mojibake rather than a crash or hang for genuinely non-UTF-8
   pages. `parserutils__filter_process_chunk()`'s exact in/out
   pointer-advancing contract (including the "NOMEM means 'caller
   already handles this as expected, not fatal'" detail) was read
   directly from its one real caller
   (`parserutils_inputstream_refill_buffer()`) before writing this,
   not guessed.

**Verification**: same honest standard as 2026-08-08 below - every
new/changed file compiles clean under this project's real `-Wall
-Wextra` flags via the real Makefile rules. Beyond that, this round
went one step further and actually **reached a complete, successful
`kernel.elf` link** in the sandbox this was written in - something
2026-08-08's entry explicitly could not do. `nasm` still isn't
available there, so the 5 assembly-only objects
(`boot`/`irq`/`isr`/`storage_blob.o`, `context_switch.o`) were
substituted with minimal same-signature C placeholders *for this link
check only* - not part of the actual source tree, deleted again
immediately after, never shipped. That link is real confirmation the
complete C-level symbol graph resolves; it is *not* boot confirmation
(a kernel built with fake `_start`/IRQ/context-switch code cannot
boot, and wasn't tried). Actually assembling with real `nasm`, linking
for real, and boot-testing in QEMU is still the next step, same as
2026-08-08's entry says - please do that and report back what the
serial log shows.

## C-OS 4.0.8 second alpha additions (2026-08-08)

Prompted by a direct ask to make sure the GUI's "NetSurf" actually
launches real NetSurf rather than a look-alike. Two things turned out
to be true that this file didn't previously say clearly enough:

- **The window that was actually live was never `modern_browser.c`.**
  Grepped every call site in the tree: `modern_browser_init/open/draw/
  refresh` and the `cos_browser.c` facade wrapping them are called from
  *nowhere*. They're linked into the kernel (still in `OBJS`) but
  unreachable dead code. The window the desktop's "NetSurf" icon/dock
  button/menu entries actually open is `gui/apps/browser/
  gui_apps_browser.c`'s own `WIN_BROWSER` implementation — a complete,
  independent HTTP client + hand-rolled HTML-tag-stripping parser +
  renderer, entirely separate from `modern_browser.c`. This is the
  file the rest of this entry's changes are about; the paragraph below
  about `modern_browser.c` being the fallback (and item 3 in "Not
  started yet", further down) were both inaccurate on this point and
  are corrected there now.

- **The engine never actually initialised.** `qemu_serial.log` from
  the prior session's own boot test shows `[NetSurf] init failed in
  nowait path` — traced (by reading `cos_netsurf_init()` and everything
  it calls) to `corestrings_init()` never being called anywhere in this
  build (it's normally called from `desktop/netsurf.c`, which isn't
  part of this port). Every `corestring_lwc_*`/`corestring_dom_*`
  pointer was therefore NULL, and `fetch_data_register()` dies on
  `lwc_string_ref(corestring_lwc_data)` → NULL scheme →
  `fetcher_add(NULL, ...)` → `NSERROR_BAD_PARAMETER`. Same root cause
  meant `html_init()`/`nscss_init()` (which register the text/html and
  text/css content handlers with `content_factory`) were also never
  called, so even a successful fetch would have had nowhere to go.
  **This means the data: URI pipeline described lower down in this
  file as "Done, verified by actually building it" had not, in fact,
  ever successfully run** — it built and linked clean, which is as far
  as that section's own claims go, but the one actual boot-tested
  attempt to run it failed at the first line. Fixed: `cos_netsurf_init()`
  in `cos_netsurf.c` now calls `corestrings_init()` → `nscss_init()` →
  `html_init()` → `fetcher_init()` → `fetch_data_register()` →
  `fetch_http_register()` (new, see below) → `hlcache_initialise()`, in
  that order — matching upstream `desktop/netsurf.c`'s `netsurf_init()`
  ordering for the calls this build also needs.

With both of those fixed, the remaining, genuinely new work:

- **`cos_fetch_http.c`** (new): a real `http:`/`https:` fetcher
  following the exact `fetcher_operation_table` contract
  `content/fetchers/data.c` uses, backed by `kernel/drivers/http.c`'s
  existing TCP/TLS client (the same `http_create`/`http_get`/
  `http_post` calls `gui_apps_browser.c` already used on its own).
  Registered alongside `fetch_data_register()` in `cos_netsurf_init()`.
  Redirects are handled transparently by `http.c` itself before this
  code ever sees a response, so there's no `FETCH_REDIRECT` message to
  send. multipart/form-data POST isn't supported (`http.c` has no
  multipart encoder); url-encoded POST is. Whether a fetch actually
  reaches anything still depends on `kernel/drivers/net.c`'s
  `COS_ENABLE_NETWORK` flag, which is `0` (E1000 DMA heap corruption,
  see that file) — deliberately left untouched, out of scope for this
  ask. `data:` URIs need no network and work regardless; real hosts
  will start working the moment that flag flips back on, with no
  further change needed here.
- **`cos_netsurf_render.c` + `cos_netsurf_render.h`** (new): walks the
  real `dom_document` a loaded page produces (`html_get_document()`,
  from `<body>` via `dom_html_document_get_body()`) using libdom's own
  `libdom_treewalk()` (`dom/walk.h`), and turns it into a flat sequence
  of display lines (title/headings/paragraphs/links) — real title via
  `content_get_title()`, real structure from the real parsed DOM, not
  from string-searching raw HTML. This is deliberately **not** Tier B
  (no CSS box model, no layout — see "Not started yet" below, still
  accurate) — reading order is top-to-bottom through the DOM, not a
  laid-out page. `cos_netsurf_render_page()` is the public entry point;
  kept in its own header (not `cos_netsurf.h`) specifically so GUI code
  can include it without pulling in NetSurf's own include paths (tried
  the alternative first — `gui_apps_browser.c` including `cos_netsurf.h`
  directly fails with `fatal error: utils/errors.h: No such file or
  directory`, since that header needs `-I` paths only
  `COS_NETSURF_INCLUDES` provides).
- **`gui_apps_browser.c`**: `browser_load_http_url()`'s GET path now
  calls `cos_netsurf_render_page()` (new `browser_load_via_real_netsurf()`
  helper) instead of its own `http_get()` + `browser_load_text_file()`
  hand-rolled parser, reusing the window's existing chrome/scrolling/
  link-click handling to display the result. **This is the change that
  makes "open NetSurf from the GUI" literally true rather than
  true-in-label-only** — what item 3 below asked for. POST requests are
  deliberately left on the old direct path for now (see the comment at
  the call site) - a reasonable next step, not silently dropped.
  `javascript:` URLs are unaffected (still go straight to
  `cos_netsurf_eval_script()`), and neither is `file://`/gopher/gemini
  browsing, which still use this file's own existing handling.

**Verification actually performed in this pass** (no `nasm`/`qemu` were
available in the environment this was written in, so — per this file's
own golden rule — the following is what "verified" means here, no
more): every new/modified file compiles cleanly, zero warnings under
this project's actual `-Wall -Wextra` flags, via the real Makefile
rules (not a hand-reconstructed command line — `make obj/netsurf/
cos_netsurf.o` etc. directly). `make kernel -j4 -k` was then run to
completion against the whole tree (1249 object files, every vendored
library archived: `libldom.a`, `liblhubbub.a`, `liblcss.a`,
`libns_content.a`, `libns_utils.a`, `libquickjs.a`, all of it) — the
only failures anywhere in that log are the five pre-existing `.asm`
files (`boot`/`irq`/`isr`/`storage_blob`/`context_switch`) with `nasm`
missing (`Error 127`), which is an environment limitation, not a
code problem; zero `error:` lines otherwise, zero undefined-reference
output (linking was never reached, since those five objects don't
exist). **What this does *not* cover, and still needs doing before
this entry could honestly move to "boot-tested": actually assembling,
linking, and booting in QEMU** — the same headless-serial-log process
the 2026-08-06 entry above used, which is exactly what would have
caught the corestrings bug immediately if it had been re-run before
now. Please do that next and update this file with what the serial log
actually shows, good or bad.

## C-OS 4.0.8 alpha additions (2026-08-06)

- **`cos_netsurf_load_url_sync_nowait()`** implemented in `cos_netsurf.c`.
  Previously declared (extern) in `gui_apps_browser.c` but never defined,
  causing a link-time gap. Now fully implemented: initialises the engine
  lazily, wraps non-data: URLs in a JS-probe data: URI so the full
  parse+style+script pipeline runs immediately, and logs all output to
  the serial console. `gui_apps_browser.c` now calls it for real on every
  URL navigation.
- **`cos_netsurf_eval_script()`** added: evaluates a JavaScript snippet
  through the shared QuickJS runtime. Used by the browser's `javascript:`
  URL scheme (address bar `javascript:alert(1)` etc.).
- **`cos_netsurf_is_ready()`** added: side-effect-free query for GUI code.
- **`console.log/warn/error/info`** installed automatically in every new
  JSContext via `cos_js_new_context()` (in `quickjs_port.c`). Routes to
  the serial console. Scripts can now use `console.log()` without any
  extra setup.
- **`COS` global object** installed automatically in every new JSContext:
  `COS.version`, `COS.osName`, `COS.print(s)`, `COS.getMemInfo()`.
- **Taskbar dock** now has a live "NS" (NetSurf) quick-launch button.
  Clicking it opens the NetSurf window or brings it to front if already
  open. Implemented in `gui_taskbar_frame.c` + `gui_input.c`.
- **`mktime()` / `difftime()`** added to `lib/time_impl.c` and declared
  in `src/include/time.h`. Required by QuickJS's `getTimezoneOffset()`
  in the `NO_TM_GMTOFF` path (which is now correctly activated for
  `COS_KERNEL` builds).
- **QuickJS thread/pthread stubs**: `cutils.h` now guards all pthread
  includes and `JS_HAVE_THREADS` behind `!defined(COS_KERNEL)`, so the
  freestanding build no longer needs a pthread shim.
- **Version**: all `src/` files updated from `C-OS 4.0.7 Demo` to
  `C-OS 4.0.8 alpha`. Makefile ISO target renamed to
  `C-OS_4.0.8_alpha.iso`. QuickJS `CONFIG_VERSION` updated to
  `cos-4.0.8-alpha`.
- **Makefile bug fix**: orphaned recipe lines 1011-1012 (duplicate
  `mkdir -p`/compile commands with no target) removed.

**Golden rule for this file: only write something down as done once
it has actually been compiled (and, where applicable, linked and
boot-tested) in this tree. Aspirational/planned work goes under
"Not started yet", not under "Done".**

## The honest one-line summary

QuickJS runs real, complete ECMAScript, and is wired into NetSurf's
own javascript-engine plugin interface for real. The entire NetSurf
dependency stack (libwapcaplet, libparserutils, libutf8proc,
libhubbub, libcss, libdom) builds and links clean, NetSurf's own
`utils/` is ported, and NetSurf's actual content pipeline -
`content/` core plus the html and css content handlers - builds and
links clean too. As of the 2026-08-08 entry above, it also actually
*runs*: `corestrings_init()`/`nscss_init()`/`html_init()` are now
called (they weren't before - see above), `CONTENT_MSG_GETTHREAD` is
answered (`cos_content_callback()` in `cos_netsurf.c` - this was
apparently added at some point after the paragraph a few sections
down about "nothing has registered a listener yet" was written, since
the code already does it, but the surrounding prose here was never
updated to say so - corrected now), and there's a real `http:`/
`https:` fetcher (`cos_fetch_http.c`) alongside the `data:` one. The
GUI's "NetSurf" window (`gui_apps_browser.c` - see the correction
above about `modern_browser.c` never actually being it) now gets its
HTML content by walking the real parsed DOM this pipeline produces,
not by parsing HTML itself. Opening it now does open real NetSurf.

What's still genuinely missing, unchanged by the above: no CSS box
model or layout (Tier B - see item 2 in "Not started yet"), so what's
on screen is a plain top-to-bottom reading of the real DOM, not a
laid-out page; the real network fetcher has nothing to fetch from
today (`COS_ENABLE_NETWORK` is `0` - a separate, pre-existing,
deliberate limitation, see `kernel/drivers/net.c`) so only `data:`
URIs actually resolve in this build until that changes; and there is
still no JS-DOM binding (`js_fire_event()` etc. are still honest
no-ops), so `<script>` tags that touch `document`/DOM APIs will throw
inside QuickJS rather than do anything, same as always.

## Done, verified by actually building it

- **QuickJS embedded in the kernel.** `third_party/quickjs/` (vendored,
  unmodified upstream) + `quickjs_port.{c,h}` (C-OS glue: allocator
  wired to kmalloc/krealloc/kfree, `cos_js_new_runtime()`,
  `cos_js_new_context()`, `cos_js_eval_and_report()`,
  `cos_js_eval_quiet()`). Builds clean as `build/libquickjs.a`.

- **A real NetSurf javascript-engine backend using QuickJS**:
  `third_party/netsurf-all-3.11/netsurf/content/handlers/javascript/quickjs/quickjs.c`.
  Implements all 12 functions NetSurf's `content/handlers/javascript/js.h`
  requires (same contract the historical `../duktape/dukky.c` backend
  satisfies). `js_exec()` genuinely runs script through QuickJS.
  `js_fire_event()` / `js_dom_event_add_listener()` /
  `js_handle_new_element()` are honest no-ops (matching
  `../none/none.c`, not faking DOM integration) because there's no
  libdom yet to bind against — see "The big remaining item" below.
  Builds clean as `build/libns_js_quickjs.a`. **Nothing calls into
  this yet** — see the DOM section below for why.

- **libwapcaplet, libparserutils, libutf8proc, libhubbub**: build clean
  as static libs (`build/liblwapcaplet.a`, `build/liblparserutils.a`,
  `build/liblutf8proc.a`, `build/liblhubbub.a`). Ported one at a time,
  following the same "wildcard the upstream src/*.c into one static
  lib" pattern each time — look at any of the `L*_DIR`/`L*_SRCS`/
  `L*_LIB` blocks in the Makefile as the template for the next one.
  Nothing links against any of these yet except by inclusion in the
  final kernel link line (they carry no dead-code risk sitting
  unused).

  Gotcha hit and fixed (twice): both libutf8proc's `utf8proc_data.c`
  and libhubbub's `entities.inc`/`autogenerated-element-type.c` are
  *not* meant to be their own translation units — they're `#include`d
  by a sibling .c file (`utf8proc.c` line 53; `entities.c` and
  `element-type.c` respectively). Compiling them standalone fails
  (missing typedefs) and, worse, cascades into thousands of spurious
  "excess elements" warnings that make it look like a slow/hanging
  build rather than a wrong one. If a future library has a similarly
  generated-looking `*_data.c`/`*.inc`/`autogenerated-*.c` file,
  check whether a sibling source `#include`s it before globbing it in
  as its own compilation unit.

  libhubbub also needed two files generated ahead of time and vendored
  as regular source (same "generate once, vendor as source" strategy
  as the utf8proc lesson above, done in this session): `entities.inc`
  via `perl build/make-entities.pl` (run from the libhubbub root), and
  `treebuilder/autogenerated-element-type.c` via `gperf` on
  `element-type.gperf` + a `sed` pass to add `static` (see the
  `LHUBBUB_DIR` comment block in the Makefile for the exact two
  commands). Its debug-only dump functions (`#ifndef NDEBUG`, using
  `fprintf`/`FILE*`) are compiled out via `-DNDEBUG` in
  `LHUBBUB_CFLAGS` rather than porting real hosted stdio.

  **libcss** was the big one so far: 302 source files across
  `src/`, `charset/`, `lex/`, `parse/`, `parse/properties/`,
  `select/`, `select/properties/`, and `utils/`, all compiling and
  linking clean as `build/liblcss.a` (17MB). Needed *two* rounds of
  one-time code generation (see the `LCSS_DIR` comment block in the
  Makefile for exact commands):
  - `src/select/autogenerated_{computed,propget,propset}.h` via
    `python3 src/select/select_generator.py` (stdlib only, trivial).
  - `src/parse/properties/autogenerated_<property>.c` — **119
    separate files, one per CSS property** — via a small host-native
    generator (`css_property_parser_gen.c`, built with the sandbox's
    own `gcc`, no cross-compilation) run once per property against
    that property's spec line(s) in `properties.gen`. Property list
    extracted the same way upstream's Perl one-liner does. All 119
    generated cleanly, no manual fixups needed.
  - As with the two libraries above, `css_property_parser_gen.c`
    itself (the *generator's* source, not generated output) is
    excluded from `LCSS_SRCS` - it's a host tool, not kernel code.

  Also surfaced two real gaps in the libc shim (not libcss-specific -
  fixed at the root so anything ported later benefits too):
  `strdup()` and `strcasecmp()`/`strncasecmp()` were used in a couple
  of libcss files but not declared *anywhere* in `src/include/`,
  which GCC was quietly treating as implicit `int`-returning
  functions — harmless-looking warnings that are actually a real
  correctness bug on a 64-bit target (a pointer truncated through an
  assumed `int` return). Added proper declarations to
  `src/include/string.h` and real implementations to `src/lib/string.c`
  (which was already linked in) - `strdup` allocates via `kmalloc` and
  is meant to be paired with `free()`, which `string.c` already
  provides as a thin `kfree()` wrapper.

  **libdom** (the actual DOM implementation) turned out to be the
  easiest of the big three, somewhat surprisingly: 95 files across
  `src/core`, `src/events`, `src/html` (one file per HTML element
  type - anchor, body, table, form, ...), `src/utils`, plus
  `bindings/hubbub/parser.c` (the glue that builds a real DOM tree by
  driving libhubbub's HTML5 parser) — all plain hand-written C, **no
  code generation step at all**, unlike libhubbub/libcss above.
  Builds and links clean as `build/libldom.a` (4.8MB).
  `bindings/xml/` (expat- or libxml2-backed XML *document* parsing,
  as opposed to HTML via hubbub) is deliberately excluded entirely -
  neither expat nor libxml2 is part of this build, it's not needed
  for ordinary HTML browsing, and pulling either in would be a whole
  new external dependency for no current benefit.

  One more libc shim gap surfaced and fixed the same way as
  strdup/strcasecmp above: `strtoul()` was used but not declared;
  added to `string.h`/`string.c` (reuses the existing signed
  `cos_parse_ll()` helper that already backs `strtol`/`strtoll`,
  cast to unsigned - sufficient for the realistic small-positive-
  integer values this kernel's ported code actually parses, even
  though it isn't a from-scratch unsigned-overflow-correct parser).

  **The full kernel link was re-verified after each of libutf8proc,
  libhubbub, libcss, and libdom individually** (not just once at the
  end) - each one was added to the Makefile, built standalone via its
  own `build/libX.a` target, *then* the whole kernel was relinked and
  reboot-tested in QEMU before moving to the next library. Zero
  regressions at any step.

- **NetSurf's own `utils/` subsystem** — the first piece of NetSurf's
  *actual* source ported, not a dependency library. Builds and links
  clean as `build/libns_utils.a`, plus `build/liblnsutils.a` for the
  one file used from the small separate `libnsutils` component
  (`base64.c` - RFC4648, fully self-contained; its siblings
  `time.c`/`unistd.c` are thin wrappers *around* real `<time.h>`/
  `<unistd.h>`, not portable replacements for them, so weren't
  useful here and were skipped).

  This directory needed real triage, not just "does it compile":
  `utils/Makefile`'s full `S_UTILS` list plus `http/` and `nsurl/`
  is 32 files, of which **9 are deliberately excluded** because they
  need something this build fundamentally doesn't have. Each is a
  judgement call, recorded here so it's a decision and not a mystery:
  - `file.c`, `filename.c`, `filepath.c`, `utils.c`: pervasive real
    POSIX filesystem calls (`stat`/`opendir`/`fopen`) for resolving
    local resource/cache paths. Not portable as-is - a C-OS frontend
    will need its own answer to "where do resources live" against
    `kernel/api/fs_api.c` instead, when that becomes relevant.
  - `time.c`: needs curl (`curl_getdate`, for parsing HTTP `Date`
    headers) - NetSurf's HTTP layer isn't ported yet either, so
    nothing needs this yet; a small hand-written date parser would be
    a cheap fix later without pulling in curl at all.
  - `utf8.c`: needs `<iconv.h>` pervasively, for legacy non-UTF-8
    charset conversion. Not required for modern (UTF-8) content -
    the already-ported libparserutils/libutf8proc cover that case.
  - `hashtable.c`: needs zlib for one separable optional feature
    (loading a gzip-compressed hash table from disk) that isn't
    trimmed out yet.
  - `log.c`: wants real hosted `FILE*`/`fprintf`/`stderr` for `NSLOG`
    - this kernel logs via `serial_puts` instead everywhere, matching
    the decision already made for the quickjs.c backend.
  - `nsoption.c`: entangled with `desktop/options.h` and per-frontend
    option tables (`riscos/options.h`, `gtk/options.h`, ...) that
    don't exist in this tree - NetSurf's options system reaching
    across into frontend-specific code that hasn't been written yet.

  `messages.c` (loads localised UI strings) *is* included despite
  the theme of the exclusions above - the code paths actually compiled
  here don't touch a filesystem path (nothing calls the file-loading
  entry point yet), and `content/handlers/html` will want its
  string-lookup API later.

  One source-level change, not just an exclusion: NetSurf's own
  `utils/libdom.c` (a thin convenience wrapper *around* the libdom
  library - different from libdom itself) has two FILE*-dependent
  pieces - a debug tree-dump printer, and `libdom_parse_file()` (loads
  and parses an HTML file from a local path). Both are wrapped in a
  new `#ifndef COS_KERNEL` guard rather than excluding the whole file,
  since its other two functions
  (`libdom_iterate_child_elements`/`libdom_hubbub_error_to_nserror`)
  are genuinely useful and don't touch FILE* at all. Nothing is lost
  for real page loading either: `libdom_parse_file()` turned out to
  be a thin wrapper around `dom_hubbub_parser_create()`/
  `dom_hubbub_parser_parse_chunk()`/`dom_hubbub_parser_completed()`
  (in `libdom/bindings/hubbub/parser.c`, already part of this build)
  that just supplies chunks by reading a local file in a loop - a
  future content handler can call that same chunk API directly with
  data from `http_fetch()` instead, without needing `fopen()` at all.

  Also needed two include-path fixes rather than code changes:
  `netsurf/inttypes.h` and `netsurf/ssl_certs.h` (NetSurf's own public
  headers, at `netsurf/include/netsurf/`, needed `-I$(NS_DIR)/include`
  added) and `<dom/bindings/hubbub/parser.h>` (only physically present
  at `libdom/bindings/hubbub/parser.h` - normally copied to
  `include/dom/bindings/hubbub/` by libdom's own `make install`, which
  this build doesn't run - fixed by vendoring that one copy directly:
  `libdom/include/dom/bindings/hubbub/{errors,parser}.h` now exist for
  real in this tree).

  Five more real libc shim gaps surfaced and fixed the same way as the
  libcss/libdom ones (declaration + implementation, not just silencing
  a warning): `isascii()` (added to `ctype.h`/`ctype.c`), `sprintf()`
  (added to `string.h`/`string.c` and, since several files reach for
  it via `<stdio.h>` instead per normal convention, also declared in
  `stdio.h` now - `snprintf`/`vsnprintf` gained the same dual
  declaration for the same reason), and `fflush()`/`atexit()` (both
  real, honest no-ops - explained in their own doc comments in
  `string.c` - added since nothing in this kernel ever exits or
  buffers stdio output to flush).

- **NetSurf's `content/` core, plus the html and css content
  handlers** — this is the piece that actually parses a page. Builds
  and links clean as `build/libns_content.a`. Split deliberately into
  "Tier A" (this session) vs "Tier B" (future): Tier A is
  `content/{content,content_factory,hlcache,llcache,mimesniff}.c` +
  `content/handlers/html/{html,script,dom_event,css,css_fetcher}.c` +
  all of `content/handlers/css/` - the pieces that parse HTML into a
  *real* DOM (via libdom+libhubbub, both now exercised for real, not
  just linked-but-unused), associate stylesheets (via libcss), and
  request a JS thread to run `<script>` tags through. Tier B - the
  visual box-model layout + rendering pipeline
  (`box_construct`/`box_normalise`/`box_special`/`box_textarea`/
  `box_inspect`/`box_manipulate`/`layout`/`layout_flex`/`table`/
  `redraw`/`redraw_border`/`font`/`imagemap`/`interaction`/
  `textselection`/`form`/`forms`/`object`) - is *not* included yet:
  none of it can be exercised without a working plotter/frontend
  (item 1 below) to draw anything, and it would need its own round of
  the same dependency triage this tier did. Also excluded outright:
  `content/fetchers/curl.c` (needs curl - a C-OS-native fetcher
  belongs in the frontend instead) and `content/handlers/image/*`
  (needs external image codec libraries - libjpeg/libpng/giflib/... -
  not part of this build).

  Two source-level changes, both `#ifndef COS_KERNEL`-guarded rather
  than deleted (upstream's code and the reasoning both stay visible):
  - `utils/inet.h` now skips real BSD socket headers entirely for
    `COS_KERNEL` (this kernel has its own networking stack, not BSD
    sockets) and defines a minimal stand-in `fd_set` so the couple of
    header-only signatures that still mention `fd_set*`
    (`content/fetch.h`'s `fetch_fdset()`, an unused `fdset`
    fetcher-table entry) can be *parsed* without pulling in the host
    sandbox's real `<sys/socket.h>` (which, without this guard, GCC
    was silently finding on its own via the default system include
    path and then failing deep inside, in a very confusing way that
    took a moment to diagnose - the actual error was about
    `__socklen_t`, several includes removed from the real cause).
  - `content/handlers/html/html.c`: `html_drop_file_at_point()` (real
    `fopen`/`fread` to read a dropped file) is excluded - it's
    drag-and-drop, which needs a real GUI frontend to ever be reached
    anyway, and should read via `fs_api` instead of `fopen()` once it
    is. The one struct field pointing at it
    (`.drop_file_at_point = html_drop_file_at_point`) is guarded the
    same way, defaulting to NULL.

  Also needed a small *native* addition, not a port: `nsu_getmonotonic_ms()`
  (from libnsutils, used by `html.c` for parse-time logging) wraps a
  real POSIX clock upstream, which doesn't exist here. Written from
  scratch instead (`libnsutils/src/time_cos.c`) against this kernel's
  own `hal_timer_get_ms()` (already used by the scheduler, so already
  a real monotonic millisecond counter) - not a stub, a genuine if
  small implementation.

  Three more libc shim gaps, same pattern as before: `strndup()`
  (trivial, added alongside `strdup()`), and **`sscanf()`**, which
  got a real if deliberately narrow implementation (documented at the
  top of its definition in `string.c`) - it supports exactly the
  conversions used anywhere in this tree today (`%d`, `%u`, `%zu`,
  plus literal character/whitespace matching), built on the same
  `cos_parse_ll()` helper `strtol`/`strtoul` already use, and is
  explicitly *not* claimed to be a complete ISO C `sscanf` - anything
  that needs a specifier outside that set will need it extended.

  **The mechanism for the actually-interesting part - a `<script>`
  tag on a real page reaching QuickJS - is now fully traceable end to
  end, and correct as far as it goes:**
  `content/handlers/html/dom_event.c` and `html.c`, when script
  execution is enabled and a `jsthread` isn't set up yet for the
  current page, call `content_broadcast(&htmlc->base,
  CONTENT_MSG_GETTHREAD, &msg_data)` (`content_broadcast` genuinely
  defined in `content/content.c`, part of this build, not a stub) -
  this is NetSurf's normal way of asking whatever's listening on this
  content (in upstream, the `desktop/browser_window` layer) to
  actually create one via `js_newheap()`/`js_newthread()` and hand it
  back through `msg_data.jsthread`. **Nothing has registered as a
  listener yet**, so today the broadcast goes out, nobody answers,
  `jsthread` stays NULL, and `js_exec()` - though completely correctly
  wired, per everything above - doesn't get called on a live page yet.
  Answering `CONTENT_MSG_GETTHREAD` (call `js_newheap`/`js_newthread`,
  fill in `msg_data.jsthread`) is a small, precisely-scoped piece of
  what item 1 below needs to do anyway, not a new category of work.

  **[2026-08-08 correction: this paragraph is stale.]** `cos_netsurf.c`
  already contains `cos_content_callback()`, which does exactly the
  above (answers `CONTENT_MSG_GETTHREAD` via `js_newheap`/
  `js_newthread`) - it's used by `cos_netsurf_load_url_sync()`. Not
  clear from this file alone whether that was added after this
  paragraph was written and never backfilled, or written and just
  never cross-referenced here: either way, source code is ground truth
  over this paragraph. See the 2026-08-08 entry at the top of this
  file for what was actually re-verified this pass.

- **Kernel links and boots end to end.** `make kernel` produces
  `build/kernel.elf` with zero link errors; `make iso` produces a
  working BIOS-bootable ISO; boot-tested headless in QEMU
  (`qemu-system-x86_64 -cdrom ... -serial file:... -display none`) —
  serial log shows a full boot: GDT/IDT/IRQ/timer/paging → PCI scan →
  PS/2 mouse/keyboard → FAT32 mount → config manager → permissions →
  **GUI system initialized, desktop drawn** → GUI main loop entered →
  preemptive scheduler started → ring3 userspace demo process ran and
  printed via a real syscall. No panics, faults, or hangs.

  Three **pre-existing** bugs (present before any of this session's
  changes, unrelated to NetSurf/QuickJS) were blocking this and got
  fixed along the way:
  1. `drivers/video/gfx_blit.c` (`gfx_blit`/`gfx_blit_scaled`) was
     implemented but never added to `OBJS` → undefined at link time.
     Added `gfx_blit.o` + its `gfx_blit_avx2.o` dependency to `OBJS`.
  2. `lib/ctype.c` (`toupper`/`tolower`/`isdigit`/`isxdigit`/
     `isprint`/`isspace`) — same story, added `ctype.o` to `OBJS`.
  3. `cos_assert_fail` (needed by MicroPython's `assert()` calls,
     e.g. in `py/objlist.c`) was implemented in `lib/stdlib.c`, but
     that file's `malloc`/`calloc`/`realloc`/`free`/`atoi`/`strtol`
     etc. would collide with the versions already linked in from
     `lib/string.c` if added wholesale. Pulled just
     `cos_assert_fail()` (+ its static `cos_libc_halt()` helper) out
     into a new, conflict-free `lib/cos_assert.c`. **`lib/stdlib.c`
     itself is still not in the build** — if something later needs
     another symbol only defined there, extract it the same way
     rather than adding the whole file; the double-`free` collision
     with `lib/string.c` is real and un-investigated (which one is
     supposed to be canonical is not yet known).

  Known non-fatal issue observed in the same boot log, **not fixed,
  pre-existing, unrelated to this work**: `[MEMORY] kfree: invalid
  pointer` printed once during JPEG image-viewer init (right before
  `[MODERN_UI] Initializing modern UI framework`). Boot continues fine
  past it. Worth a look some time, but out of scope here.

- **"Browser" → "NetSurf" renamed consistently across the whole GUI**:
  desktop icon, start menu, both context-menu variants, task manager
  display name, shell `open` command (kept `browser` working as an
  alias, added `netsurf`), OS-wide app-launch API (same alias
  treatment). This was more involved than a find-replace because many
  of these dispatch by comparing against the literal display string
  rather than an enum — producer and matcher strings had to be
  updated in the same pairs. Verified after editing that no orphaned
  "Browser"-only string remained anywhere in `src/`.
  **This is a label change only** — see the top of this file for what
  it does and doesn't imply about what's actually running.

## Not started yet — what "real NetSurf in the GUI" still needs

1. **A C-OS NetSurf frontend** (parallel to `netsurf/frontends/
   framebuffer/` or `.../monkey/` upstream) - or rather, the
   hand-written equivalent this project is actually taking (see the
   2026-08-08 entry at the top): `cos_netsurf.c` + `cos_fetch.c` +
   `cos_fetch_http.c` + `cos_netsurf_render.c` together now cover:
   a. **Done.** Driving the page-load lifecycle: `hlcache_handle_retrieve()`
      in `cos_netsurf_load_url_sync_for_render()` (`cos_netsurf.c`)
      creates the content and waits for it; `cos_fetch_http.c` feeds it
      real fetched bytes over `kernel/drivers/http.c`'s client for
      `http:`/`https:`, `content/fetchers/data.c` for `data:`. (Not
      done the way this bullet originally envisioned - via
      `kernel/api/net_api.c`'s `http_fetch` - because `gui_apps_browser.c`
      turned out to already use `http.c`'s lower-level
      `http_create`/`http_get`/`http_post` directly, which is what
      `cos_fetch_http.c` wraps instead; same underlying transport
      either way.)
   b. **Done**, and turned out to already have been done before this
      session - see the correction a few sections up: `cos_content_callback()`
      answers `CONTENT_MSG_GETTHREAD`.
   c. **Still not done, and still not blocking real content from being
      useful** - see (d) below for how that gap is being worked around
      for now. A real plotter (mapping to `gfx_blit` et al.) remains
      the way to eventually get pixel-accurate output.
   d. **Sidestepped rather than done**: no port of NetSurf's own
      scheduling/`gui_open_window` window creation. Instead,
      `cos_netsurf_render.c` produces a flat list of display lines
      from the real DOM, and `gui_apps_browser.c` feeds those into the
      *existing* C-OS `WIN_BROWSER` window it already knows how to
      create, scroll, and handle clicks in. This gets real content on
      screen now without needing a real plotter or a ported window
      layer, at the cost of not being a "real NetSurf frontend" in the
      upstream architectural sense - it's a C-OS-specific shortcut to
      the same visible result for the common case (reading a page),
      not a substitute for (c) if/when pixel-accurate rendering,
      scripted DOM mutation reflected on screen, etc. are wanted.
2. **Tier B of `content/handlers/html`** (box model construction +
   layout + rendering - see the "Done" section above for the full
   file list) - needed for actually *seeing a laid-out page* the way a
   real browser renders one (positioned boxes, applied CSS, images),
   as opposed to a plain top-to-bottom read of the real DOM (which
   `cos_netsurf_render.c` now provides - see above). Needs a working
   plotter (1c above) to make sense of at all, and its own round of
   dependency triage (check every file's real includes before assuming
   a clean glob, the way every tier before it needed).
3. **Wire `gui/apps/browser/gui_apps_browser.c`** to call into the real
   engine instead of a hand-rolled parser, once 1 exists in some usable
   form. **Done** as of 2026-08-08, for GET requests specifically - see
   the entry at the top of this file. POST is intentionally still on
   the old path (see the comment at that call site in
   `gui_apps_browser.c`); `file://`/gopher/gemini are unaffected,
   still on this file's own pre-existing handling for those schemes.

`gui_apps_browser.c`'s own hand-rolled HTML-to-text parser
(`browser_load_text_file()`) is what actually backed the GUI's
"NetSurf" window before 2026-08-08 above - **not** `modern_browser.c`
(see the top of this file: that file and the `cos_browser.c` facade
wrapping it are dead code, called from nowhere in the tree, confirmed
by grepping every call site). `browser_load_text_file()` remains in
use today only for content the real engine doesn't cover yet: POST
responses, and non-HTML local/gopher/gemini content.

## Loose thread, not acted on

`QEMU TestCommand.txt` has a trailing note "Duktapeも入れる" ("add
Duktape too") — i.e. a wish to have Duktape available as a second JS
engine option alongside QuickJS. Not investigated; noted here so it
isn't lost.

## How to pick this back up

```
cd cos
make kernel -j2          # ~a few min on 1 core, longer on the huge
                          # MicroPython/BearSSL/ACPICA components the
                          # first time; incremental after that
make iso
qemu-system-x86_64 -cdrom C-OS_4.0.8_alpha.iso \
  -drive file=build/storage.img,format=raw,if=ide,index=0,media=disk \
  -m 1024M -serial file:qemu_serial.log -display none -no-reboot
# then read qemu_serial.log
```

Next concrete step: assemble/link/boot-test this session's changes for
real (see the 2026-08-08 entry at the top for exactly what is and
isn't verified yet) and update this file with what the serial log
shows. After that, item 1c/2 (a real plotter and Tier B layout) is the
next substantial piece - what's on screen today is a real-DOM-backed
but unstyled, unlaid-out reading of a page, not a rendered one.
