#!/usr/bin/env python3
"""Convert a QEMU PPM screendump into a PNG for visual regression inspection."""
import sys
from PIL import Image

if len(sys.argv) != 3:
    raise SystemExit(f"Usage: {sys.argv[0]} INPUT.ppm OUTPUT.png")

with Image.open(sys.argv[1]) as image:
    image.convert("RGB").save(sys.argv[2], "PNG", optimize=True)
