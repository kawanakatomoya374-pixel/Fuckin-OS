# C-OS browser / GUI / storage regression

## Completed in this pass

The host-side storage toolchain now includes `tools/pack_storage.py`, `tools/inject_storage.py`, `tools/validate_storage_image.py`, and `tools/inject_mp3_test.sh`. The tools implement the C-OS persistent catalog format used by `src/drivers/disk/storage.c`; they create parent directories, allocate aligned sectors, write file bytes, update both primary and backup catalogs, and verify CRCs.

`tools/run_qemu_c-os.sh` now attaches an AC97 device with a host-independent QEMU audio backend. The launcher continues to use Secure Boot UEFI, q35, TCG multi-threading, SMP 1..8, EHCI USB keyboard/mouse, and E1000.

The Makefile now uses the Python storage packer when the optional Go toolchain is absent, and links `cos_netsurf_render.o` into the final kernel. The QuickJS bridge documentation was updated to reflect the existing real DOM, selectors, classList/style, Event, fetch, XMLHttpRequest, localStorage, and sessionStorage paths. An executable browser smoke-test page was added at `validation/browser/quickjs_dom_storage_selftest.html`.

## Verification

- Python syntax checks: passed.
- New 512 MiB storage image: created successfully.
- MP3 injection: `src/assets/sample_beep.mp3` -> `/music/sample_beep.mp3`, 8482 bytes, 17 sectors; catalog verification passed.
- Nested browser test injection: `validation/browser/quickjs_dom_storage_selftest.html` -> `/browser/quickjs_dom_storage_selftest.html`; catalog verification passed.
- Differential kernel build: passed.
- Secure Boot UEFI ISO rebuild: passed.
- Strict QEMU regression: SMP8 booted with `Online CPUs=8`, AC97 initialized and PCM DMA started, EHCI/TinyUSB initialized, E1000 initialized, MP3 backend initialized, and GUI reached `Boot complete - drawing desktop`. No PANIC, page fault, general protection fault, triple fault, or OOM marker was observed. QEMU was stopped by the external timeout because the launcher intentionally uses `-no-shutdown`.

## Known limits

The browser smoke-test HTML is an asset and is injected correctly, but this pass did not automate clicking through the C-OS GUI to open the injected file and capture a visual `PASS` result. The full CSS box-model and pixel-accurate browser pipeline remains upstream-heavy; linking the existing real DOM line renderer removes the Makefile omission, but does not claim complete standards-compliant layout. The C-OS storage image is raw, so an ISO itself is not modified in-place; MP3/browser files are placed in the companion `.img` used by QEMU.
