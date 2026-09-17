# IPA Font notice for C-OS

C-OS includes **generated 16×16 monochrome bitmap glyph data** in
`src/drivers/video/jp_font16.c`. The data is produced by
`tools/generate_ipa_jp_font.py` from **IPA Gothic** (`ipag.ttf`) for use by the
C-OS UTF-8 Japanese UI renderer.

The complete applicable license text is included in
[`LICENSE-IPA-FONT`](LICENSE-IPA-FONT). This generated asset does not embed or
load the original TTF at C-OS runtime. The source font is used only by the
host-side, reproducible generation tool.

| Component | Source | License |
|---|---|---|
| Japanese bitmap glyph data | IPA Gothic | IPA Font License Agreement v1.0 |
| Generator | `tools/generate_ipa_jp_font.py` | C-OS project source |
