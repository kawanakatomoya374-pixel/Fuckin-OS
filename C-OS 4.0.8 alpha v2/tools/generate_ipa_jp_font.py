#!/usr/bin/env python3
"""Generate compact 16x16 1bpp Japanese glyph data for the C-OS kernel.

The generator is a host-build tool.  It renders a trusted, locally installed
IPA Gothic font once; C-OS never parses external TTF data at runtime.
"""
from __future__ import annotations

import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

RANGES = (
    (0x3000, 0x303F),  # CJK symbols and punctuation
    (0x3040, 0x309F),  # Hiragana
    (0x30A0, 0x30FF),  # Katakana
    (0x31F0, 0x31FF),  # Katakana phonetic extensions
    (0x4E00, 0x9FFF),  # CJK unified ideographs
    (0xFF01, 0xFFEF),  # Full-width and half-width forms
)


def render_rows(font: ImageFont.FreeTypeFont, codepoint: int) -> list[int]:
    char = chr(codepoint)
    bbox = font.getbbox(char)
    if not bbox:
        return [0] * 16
    x0, y0, x1, y1 = bbox
    width, height = x1 - x0, y1 - y0
    x = (16 - width) // 2 - x0
    y = (16 - height) // 2 - y0
    image = Image.new("L", (16, 16), 0)
    ImageDraw.Draw(image).text((x, y), char, font=font, fill=255)
    rows: list[int] = []
    for py in range(16):
        row = 0
        for px in range(16):
            if image.getpixel((px, py)) >= 96:
                row |= 1 << (15 - px)
        rows.append(row)
    return rows


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()

    font = ImageFont.truetype(str(args.font), 16)
    glyphs = [(cp, render_rows(font, cp)) for lo, hi in RANGES for cp in range(lo, hi + 1)]

    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.source.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(
        "#ifndef COS_JP_FONT16_H\n"
        "#define COS_JP_FONT16_H\n\n"
        "#include <stdint.h>\n\n"
        "typedef struct {\n"
        "    uint32_t codepoint;\n"
        "    uint16_t rows[16];\n"
        "} cos_jp_font16_glyph_t;\n\n"
        "extern const cos_jp_font16_glyph_t cos_jp_font16[];\n"
        "extern const uint32_t cos_jp_font16_count;\n\n"
        "#endif\n",
        encoding="utf-8",
    )

    lines = [
        "/* Generated from IPA Gothic by tools/generate_ipa_jp_font.py. */",
        "/* Source font license: IPA Font License v1.0 (see LICENSE-IPA-FONT). */",
        "#include \"jp_font16.h\"",
        "",
        "const cos_jp_font16_glyph_t cos_jp_font16[] = {",
    ]
    for cp, rows in glyphs:
        rendered = ", ".join(f"0x{row:04X}" for row in rows)
        lines.append(f"    {{ 0x{cp:04X}, {{ {rendered} }} }},")
    lines.extend(
        [
            "};",
            "",
            "const uint32_t cos_jp_font16_count =",
            "    (uint32_t)(sizeof(cos_jp_font16) / sizeof(cos_jp_font16[0]));",
            "",
        ]
    )
    args.source.write_text("\n".join(lines), encoding="utf-8")
    print(f"generated {len(glyphs)} glyphs")


if __name__ == "__main__":
    main()
