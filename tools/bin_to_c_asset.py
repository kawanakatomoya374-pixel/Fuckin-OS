#!/usr/bin/env python3
"""Convert a small binary test asset into a freestanding C source array."""
from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: bin_to_c_asset.py INPUT OUTPUT SYMBOL", file=sys.stderr)
        return 2
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    symbol = sys.argv[3]
    data = src.read_bytes()
    lines = [
        f"/* Auto-generated from {src.name}; do not edit by hand. */",
        f"const unsigned char {symbol}[] = {{",
    ]
    for offset in range(0, len(data), 12):
        chunk = ", ".join(f"0x{byte:02X}" for byte in data[offset:offset + 12])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append(f"const unsigned int {symbol}_len = {len(data)}U;")
    lines.append("")
    dst.write_text("\n".join(lines), encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
