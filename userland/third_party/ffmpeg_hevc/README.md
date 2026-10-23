# ffmpeg_hevc - FFmpeg's HEVC (H.265) decoder, ported to C-OS userland

Source: FFmpeg **n7.0** (`libavcodec/hevc*.c`, `hevcdec.h`, `hevc_ps.*`, `hevc_sei.*`, `hevcdsp*`, `hevcpred*`,
`h2645_parse/vui/sei`, `h26x/*` templates, `cabac*`, `golomb`, `bswapdsp`, plus `libavutil/log2_tab.c`).
FFmpeg is licensed LGPL-2.1-or-later (the files here keep their original headers); this directory
is built into the Media app only.

The decoder source is used as-is. Everything it needs from the rest of libavcodec/libavutil is provided by the
same hand-written compat layer the VP9 port uses (`../ffmpeg_vp9/`, which this port extended: richer
`AVFrame`/`AVCodecContext`/`AVBufferRef`, `refstruct` with destructors, pixel descriptors, `thread.h` hooks), plus:

* `cos_hevc_shim.c` - the functions the HEVC decoder calls that the VP9 shim lacks. Real implementations where the
  decoder needs them (buffer refs, frame move/copy, pixel descriptors, `av_reduce`, "thread" frame hooks, profile
  table); inert stand-ins for features that are deliberately out of scope: Dolby Vision RPUs, film grain, HDR /
  SEI side-data export, MD5 picture-hash verification, SMPTE timecodes.
* `cos_hevc_entry.c` / `cos_hevc.h` - persistent-decoder API for the Media app: `cos_hevc_open(hvcC)`,
  `cos_hevc_decode(sample)`, `cos_hevc_drain()`, `cos_hevc_close()`. Applies the conformance-window crop.

Single-threaded only (no frame/slice threads). Output is 4:2:0 8/10/12-bit (other formats are decoded but
reported as unsupported to the display path).

Validated bit-exact against ffmpeg on the host (Main/Main10; tiles, WPP, multiple slices, lossless,
transform-skip, odd crop, deep B-pyramids, CTU 16/32/64) and on-device in QEMU (see
`VALIDATION_OPEN_HEVC=1` / `VALIDATION_OPEN_HEVC_10BIT=1` with `VALIDATION_FRAME_CRC=1`).
