
## Window, `.sp` files, JIT
* `cosgui.c` / `cosgui.sp` add the `window` backend `"cos"`: `window.open(w, h, title, "cos")` is a C-OS window, `window.run(app, win, -1, (a, w) => true)` drives a `gui.App` in it.
  See `pack/window_demo.sp`. Install the interpreter from C-OS Apps (`make sprint-pack` builds the pack); double-clicking a `.sp` file runs `sprint run FILE`.
* The loop JIT is on (`SP_JIT=0 build_cos.sh` turns it off; `sprint run --no-jit` skips it at run time). C-OS needs only `mmap`/`mprotect(PROT_EXEC)` for it. The C-OS libc's
  `jmp_buf` is smaller than glibc's, so `VM` fields move: `jit_layout.h` holds the offsets measured with cos-cc (tools/jitlayout.c run in C-OS) and the build reassembles the stencils with them.
  Measured in QEMU: a 3M-iteration arithmetic loop 2.14 s -> 0.17 s; fib, collatz give the same output with and without the JIT.

## Apps written in Sprint (`apps/`, native part `cosapi.c`)
`_cos` is the small set of C functions a Sprint program may ask the system for (network transfer, the catalog's signature check, image decoding, starting a program, SHA-256 of a file).
It needs `--allow-ffi` (never part of `--allow-all`, so a double-clicked `.sp` has none of it); files go through Sprint's own sandboxed `fs` (`--allow-read=DIR`, `--allow-write=DIR`).
`apps/cosapps.sp` is C-OS Apps again, with the screens and the logic in Sprint (search, rows with icons, install progress, the warnings before a removal, tooltips with the download size):

    sprint run --allow-ffi --allow-read=/apps --allow-read=/system --allow-read=/desktop --allow-write=/apps --allow-write=/desktop --allow-write=/system /apps/sprint/cosapps.sp

`fs.exists()` raises when the folder above the path is missing (as on Linux), so the scripts wrap it in `try`.
