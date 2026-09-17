#!/usr/bin/env python3
"""Validate the NetSurf BGRA -> C-OS XRGB channel contract on real JPEG assets."""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / "abehiroshi_assets"
SAMPLES = {
    "abe-top-20190328-2.jpg": [(175, 190), (180, 330), (150, 90)],
    "abehiroshi_background.jpg": [(85, 48), (200, 48), (10, 10)],
}


def old_xbgr_assumption(raw: int) -> int:
    return ((raw & 0x000000FF) << 16) | (raw & 0x0000FF00) | ((raw & 0x00FF0000) >> 16)


def fixed_bgra_to_xrgb(raw: int) -> int:
    return raw & 0x00FFFFFF


def as_rgb(pixel: int) -> tuple[int, int, int]:
    return ((pixel >> 16) & 0xFF, (pixel >> 8) & 0xFF, pixel & 0xFF)


def main() -> None:
    checked = 0
    visibly_swapped = 0
    for name, positions in SAMPLES.items():
        image = Image.open(ASSETS / name).convert("RGB")
        for x, y in positions:
            red, green, blue = image.getpixel((x, y))
            raw_bgra_le = (0xFF << 24) | (red << 16) | (green << 8) | blue
            fixed = fixed_bgra_to_xrgb(raw_bgra_le)
            old = old_xbgr_assumption(raw_bgra_le)
            assert as_rgb(fixed) == (red, green, blue), (name, x, y, as_rgb(fixed), (red, green, blue))
            if red != blue:
                assert as_rgb(old) == (blue, green, red), (name, x, y, as_rgb(old), (blue, green, red))
                visibly_swapped += 1
            checked += 1
    assert visibly_swapped > 0
    print(f"PASS: {checked} real JPEG samples retain RGB under BGRA->XRGB; old path swaps R/B in {visibly_swapped} samples")


if __name__ == "__main__":
    main()
