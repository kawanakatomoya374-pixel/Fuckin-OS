#!/usr/bin/env python3
"""lib/gui.py を C の文字列にした include/gui/python_code_to_c_gui_prelude.h を生成する（編集しないこと）。"""
import sys

def c_escape(line):
    out = []
    for ch in line:
        o = ord(ch)
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif 32 <= o < 127:
            out.append(ch)
        else:
            out.append('\\x%02x' % (o & 0xFF) if o < 256 else '?')
    return ''.join(out)

def main(src, dst):
    text = open(src, encoding='utf-8').read()
    lines = text.split('\n')
    out = ['/* 自動生成: tools/gen_gui_prelude.py（編集しないこと）。lib/gui.py の内容。 */',
           '#ifndef PYTHON_CODE_TO_C_GUI_PRELUDE_H',
           '#define PYTHON_CODE_TO_C_GUI_PRELUDE_H',
           'static const char p2c_gui_prelude_src[] =']
    for line in lines:
        out.append('    "%s\\n"' % c_escape(line))
    out.append('    ;')
    out.append('#endif')
    open(dst, 'w', encoding='utf-8').write('\n'.join(out) + '\n')

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'lib/gui.py',
         sys.argv[2] if len(sys.argv) > 2 else 'include/gui/python_code_to_c_gui_prelude.h')
