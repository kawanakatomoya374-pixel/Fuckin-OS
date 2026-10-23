#!/usr/bin/env python3
"""8x16 の仮フォント（ASCII 0x20〜0x7E）を生成する。

字形の元: TeX Gyre Cursor（GUST Font License。再配布と埋め込みを許す。著作権表示を残すこと）。
1 行 1 バイト、左端の画素が最上位ビット。仮仕様の段階の字形で、後で差し替え可能。
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FONT = '/usr/share/texmf/fonts/opentype/public/tex-gyre/texgyrecursor-regular.otf'
SCALE = 8          # 8x16 を SCALE 倍で描いてから縮小する（細い線を残すため）
THRESHOLD = 40     # 縮小後の画素の白黒のしきい値（0〜255）
FONT_SIZE = 160

def glyph_rows(font, ch):
    W, H = 8 * SCALE, 16 * SCALE
    im = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(im)
    bbox = d.textbbox((0, 0), ch, font=font)
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = (W - w) // 2 - bbox[0]
    y = (H - h) // 2 - bbox[1]
    d.text((x, y), ch, font=font, fill=255)
    small = im.resize((8, 16), Image.BOX).load()
    rows = []
    for yy in range(16):
        b = 0
        for xx in range(8):
            if small[xx, yy] >= THRESHOLD:
                b |= 0x80 >> xx
        rows.append(b)
    return rows

def main(out_path):
    font = ImageFont.truetype(FONT, FONT_SIZE)
    lines = []
    lines.append('/* 自動生成: tools/gen_gui_font8x16.py（編集しないこと）。')
    lines.append(' * 字形の元: TeX Gyre Cursor（GUST Font License。再配布・埋め込みを許す。著作権表示を残す）。')
    lines.append(' * 8x16、ASCII 0x20〜0x7E。1 行 1 バイト、左端の画素が最上位ビット。 */')
    lines.append('#ifndef PYTHON_CODE_TO_C_GUI_FONT8X16_H')
    lines.append('#define PYTHON_CODE_TO_C_GUI_FONT8X16_H')
    lines.append('#include <stdint.h>')
    lines.append('#define P2C_GUI_FONT_FIRST 0x20')
    lines.append('#define P2C_GUI_FONT_COUNT 95')
    lines.append('static const uint8_t p2c_gui_font8x16[95][16] = {')
    for code in range(0x20, 0x7F):
        ch = chr(code)
        rows = [0] * 16 if ch == ' ' else glyph_rows(font, ch)
        body = ', '.join('0x%02X' % r for r in rows)
        lines.append('    { %s }, /* %s */' % (body, ch.replace('*', '+') if ch != '\\' else 'backslash'))
    lines.append('};')
    lines.append('#endif')
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'include/gui/python_code_to_c_gui_font8x16.h')
