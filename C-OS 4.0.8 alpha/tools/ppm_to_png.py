#!/usr/bin/env python3
"""Convert a QEMU PPM screendump to PNG without changing pixels."""
import sys
from pathlib import Path
from PIL import Image

if len(sys.argv) != 3:
    raise SystemExit(f"usage: {Path(sys.argv[0]).name} INPUT.ppm OUTPUT.png")

source = Path(sys.argv[1])
target = Path(sys.argv[2])
with Image.open(source) as image:
    image.save(target, format="PNG")
