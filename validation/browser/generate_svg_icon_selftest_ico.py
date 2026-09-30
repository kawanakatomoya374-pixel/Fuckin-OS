#!/usr/bin/env python3
"""Generate a deterministic 16x16 32bpp ICO for the C-OS NetSurf regression."""
from __future__ import annotations

import struct
from pathlib import Path

OUT = Path(__file__).with_name("svg_icon_selftest.ico")
W = H = 16


def pixel(x: int, y: int) -> tuple[int, int, int, int]:
    # X-shaped white highlight on a blue/green tile; ICO stores BGRA.
    border = x in (0, W - 1) or y in (0, H - 1)
    diagonal = abs(x - y) <= 1 or abs((W - 1 - x) - y) <= 1
    if border:
        return (34, 48, 66, 255)
    if diagonal:
        return (255, 255, 255, 255)
    if x < W // 2:
        return (64, 151, 220, 255)
    return (53, 180, 119, 255)


def main() -> None:
    # BITMAPINFOHEADER with doubled height because ICO XOR and AND planes share
    # one DIB. A 32-bit alpha-bearing XOR bitmap needs a zeroed 1bpp AND plane.
    dib_header = struct.pack(
        "<IIIHHIIIIII",
        40, W, H * 2, 1, 32, 0, W * H * 4, 0, 0, 0, 0,
    )
    xor = bytearray()
    for y in range(H - 1, -1, -1):
        for x in range(W):
            r, g, b, a = pixel(x, y)
            xor.extend((b, g, r, a))
    and_mask = bytes(((W + 31) // 32 * 4) * H)
    image = dib_header + xor + and_mask
    icon_header = struct.pack("<HHH", 0, 1, 1)
    directory = struct.pack("<BBBBHHII", W, H, 0, 0, 1, 32, len(image), 6 + 16)
    OUT.write_bytes(icon_header + directory + image)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
