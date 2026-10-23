# Browser / QuickJS / GUI audit

## Confirmed existing paths

- QuickJS has real libdom element wrappers, wrapper caching, live `textContent`, bounded `innerHTML` fragment parsing, `classList`, style accessors, `createElement`, `createTextNode`, selector support, timers, Event/CustomEvent constructors, and a bound DOM event dispatcher.
- `document.addEventListener` and `window.addEventListener` are installed by a JavaScript bridge after initial function registration. Global dispatch is still compatibility-script based.
- `document.querySelectorAll` and `getElementsByTagName` return snapshot arrays, not live NodeLists.
- `cos_web_new_element()` remains an inert fallback when no libdom document is bound, with no-op mutation methods.
- NetSurf fetch is registered through `cos_fetch.c`; network fetchers are polled cooperatively. The browser path contains explicit nonblocking/nowait work from earlier changes.
- GUI has double buffers, dirty rectangle fields, a parallel fill foundation, and an idle-frame redraw skip. `gui_update()` owns NetSurf/QuickJS pumps and frame composition.
- MP3 backend is linked and the GUI update loop now calls `mk_mp3_update()`.

## Priority gaps to address

1. Add a real bounded Storage object (`localStorage` and `sessionStorage`) with get/set/remove/clear/key/length semantics and per-context cleanup.
2. Replace inert fallback element mutations with a lightweight detached-node model or explicit DocumentFragment support; keep real libdom path unchanged.
3. Improve selector parsing for descendant and compound selectors where safe, while preserving bounds.
4. Ensure DOM mutations call GUI invalidation and page redraw scheduling.
5. Audit layout path: `dom_to_box()` and plotter activation must be verified against the current linked objects before changing upstream layout code.
6. Add browser regression scripts that inject test HTML/JS into the custom storage image and verify serial markers.

## Build baseline

- `make -j2` succeeds after recent MP3/GUI changes.
- Secure Boot ISO target succeeds with the development signing key.
- Strict QEMU SMP8 boot reaches AC97, EHCI/TinyUSB, E1000, MP3 init, and GUI boot markers without fault markers.
