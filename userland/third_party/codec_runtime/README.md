# C-OS codec runtime

This directory is the freestanding portability boundary for imported video decoders.

The runtime deliberately maps allocation, memory, sorting, logging, abort, and the single-thread pthread surface to C-OS userland APIs. It does not provide a fake decoder and it does not link host `.so` files.

## Current state

- `libvpx_vp9`: integrated and QEMU-validated.
- `dav1d_src`: vendored from the upstream source and configured for an 8-bit/no-assembly probe. The complete decoder still requires generated bitdepth sources and additional ABI adaptation.
- `libde265_src`: vendored. It is C++ and needs a C++ freestanding runtime before it can be linked.
- `openh264_src`: vendored. It is C++ and needs the same C++ runtime problem solved before integration.

The `pthread.h` and `cos_pthread_compat.c` pair is intentionally single-threaded. It is sufficient for deterministic bring-up only; it is not a claim of multithreaded decoder performance.
