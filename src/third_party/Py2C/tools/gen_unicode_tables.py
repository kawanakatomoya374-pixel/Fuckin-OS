#!/usr/bin/env python3
"""Python Code to C の Unicode 表を生成する（決定的な出力、手で編集しない）。

判定と大小変換は、CPython 自身の str メソッドを全コードポイントに適用して作る。
つまり、ここで生成した表は「CPython の str.isalpha() 等と同じ結果」を定義する。

生成物:
  include/common/python_code_to_c_unicode.h
  src/common/python_code_to_c_unicode_tables.c

使い方:
  python3 tools/gen_unicode_tables.py          # 生成
  python3 tools/gen_unicode_tables.py --check  # 既存の生成物と一致するか検査（差があれば 1）

注意:
  - 表の Unicode 版は、この生成に使った Python の版に依存する（CPython 3.12 は Unicode 15.0、
    3.13 は 15.1）。3.13 で生成し直せば、対象の Python 3.13 と完全に一致する。
  - case_ignorable（最終シグマ判定に使う）は CPython が公開しないため、一般カテゴリと
    既知の記号からの近似である。最終シグマの規則だけに影響する。
"""
import os
import sys
import unicodedata

MAX_CP = 0x110000
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
HEADER = os.path.join(ROOT, "include", "common", "python_code_to_c_unicode.h")
SOURCE = os.path.join(ROOT, "src", "common", "python_code_to_c_unicode_tables.c")

# 判定: 名前, CPython の判定（1文字の str に対して）
PREDICATES = [
    ("alpha", lambda c: c.isalpha()),
    ("decimal", lambda c: c.isdecimal()),
    ("digit", lambda c: c.isdigit()),
    ("numeric", lambda c: c.isnumeric()),
    ("space", lambda c: c.isspace()),
    ("printable", lambda c: c.isprintable()),
    ("upper", lambda c: c.isupper()),
    ("lower", lambda c: c.islower()),
    # 1文字の istitle() は大文字でも真になるため、タイトルケース（Lt）だけを取り出す
    ("title", lambda c: c.istitle() and not c.isupper()),
    ("cased", lambda c: c.isupper() or c.islower() or c.istitle()),
    # 識別子: 先頭は XID_Start（'_' は別扱い）、以降は XID_Continue
    ("xid_start", lambda c: c != "_" and c.isidentifier()),
    ("xid_continue", lambda c: ("a" + c).isidentifier()),
    # 近似（上の注意を参照）
    ("case_ignorable", lambda c: unicodedata.category(c) in ("Mn", "Me", "Cf", "Lm", "Sk")
     or c in "'.:^`\u00b7\u2019\u2027"),
]

# 大小変換: 名前, CPython の変換（完全な写像。1文字が複数文字へ展開されうる）
MAPPINGS = [
    ("lower", lambda c: c.lower()),
    ("upper", lambda c: c.upper()),
    ("title", lambda c: c.title()),
    ("fold", lambda c: c.casefold()),
]


def predicate_ranges(pred):
    """pred が真になるコードポイントを [lo, hi] の区間へまとめる。"""
    out = []
    start = None
    for cp in range(MAX_CP):
        ok = pred(chr(cp))
        if ok and start is None:
            start = cp
        elif not ok and start is not None:
            out.append((start, cp - 1))
            start = None
    if start is not None:
        out.append((start, MAX_CP - 1))
    return out


def mapping_rows(fn):
    """変換結果が元と異なるコードポイントを (cp, [変換後のコードポイント...]) で列挙する。"""
    rows = []
    for cp in range(MAX_CP):
        c = chr(cp)
        r = fn(c)
        if r != c:
            seq = [ord(x) for x in r]
            if len(seq) > 3:
                raise SystemExit("mapping longer than 3 code points at U+%04X" % cp)
            rows.append((cp, seq))
    return rows


def emit(out):
    w = out.append
    w("/* 自動生成ファイル。手で編集しないこと。")
    w(" * 生成: tools/gen_unicode_tables.py（Python %s, Unicode %s）" % (
        sys.version.split()[0], unicodedata.unidata_version))
    w(" * 判定と大小変換は CPython の str メソッドの結果そのもの。 */")
    w('#include "common/python_code_to_c_unicode.h"')
    w("")
    for name, fn in PREDICATES:
        ranges = predicate_ranges(fn)
        w("static const uint32_t p2c_uni_%s_tab[][2] = {" % name)
        line = "   "
        for lo, hi in ranges:
            item = " {0x%X,0x%X}," % (lo, hi)
            if len(line) + len(item) > 100:
                w(line)
                line = "   "
            line += item
        if line.strip():
            w(line)
        w("};")
        w("")
    for name, fn in MAPPINGS:
        rows = mapping_rows(fn)
        w("static const uint32_t p2c_uni_map_%s_tab[][5] = {" % name)
        for cp, seq in rows:
            vals = seq + [0] * (3 - len(seq))
            w(" {0x%X,%d,0x%X,0x%X,0x%X}," % (cp, len(seq), vals[0], vals[1], vals[2]))
        w("};")
        w("")
    w("#define P2C_UNI_COUNT(t) (sizeof(t) / sizeof((t)[0]))")
    w("")
    w("static bool p2c_uni_in_ranges(const uint32_t (*tab)[2], size_t n, uint32_t cp) {")
    w("    size_t lo = 0, hi = n;")
    w("    while (lo < hi) {")
    w("        size_t mid = lo + (hi - lo) / 2;")
    w("        if (cp < tab[mid][0]) hi = mid;")
    w("        else if (cp > tab[mid][1]) lo = mid + 1;")
    w("        else return true;")
    w("    }")
    w("    return false;")
    w("}")
    w("")
    for name, _ in PREDICATES:
        w("bool p2c_uni_is_%s(uint32_t cp) {" % name)
        w("    return p2c_uni_in_ranges(p2c_uni_%s_tab, P2C_UNI_COUNT(p2c_uni_%s_tab), cp);" % (name, name))
        w("}")
        w("")
    w("int p2c_uni_case_map(uint32_t cp, int mode, uint32_t out[3]) {")
    w("    const uint32_t (*tab)[5] = NULL;")
    w("    size_t n = 0;")
    w("    switch (mode) {")
    for mode, (name, _) in enumerate(MAPPINGS):
        w("    case %d: tab = p2c_uni_map_%s_tab; n = P2C_UNI_COUNT(p2c_uni_map_%s_tab); break;" % (mode, name, name))
    w("    default: break;")
    w("    }")
    w("    out[0] = cp; out[1] = 0; out[2] = 0;")
    w("    if (!tab) return 1;")
    w("    size_t lo = 0, hi = n;")
    w("    while (lo < hi) {")
    w("        size_t mid = lo + (hi - lo) / 2;")
    w("        if (tab[mid][0] < cp) lo = mid + 1;")
    w("        else if (tab[mid][0] > cp) hi = mid;")
    w("        else {")
    w("            int count = (int)tab[mid][1];")
    w("            out[0] = tab[mid][2]; out[1] = tab[mid][3]; out[2] = tab[mid][4];")
    w("            return count;")
    w("        }")
    w("    }")
    w("    return 1;")
    w("}")
    return out


HEADER_TEXT = """/* 自動生成ファイル（tools/gen_unicode_tables.py）。手で編集しないこと。
 * Unicode の文字判定と大小変換。判定は CPython の str.isalpha() 等と同じ結果を返す。 */
#ifndef PYTHON_CODE_TO_C_UNICODE_H
#define PYTHON_CODE_TO_C_UNICODE_H

#include "common/python_code_to_c_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 判定（CPython の str.is* と同じ定義を1文字に対して適用した結果） */
"""


def header_text():
    lines = [HEADER_TEXT.rstrip("\n")]
    for name, _ in PREDICATES:
        lines.append("bool p2c_uni_is_%s(uint32_t cp);" % name)
    lines.append("")
    lines.append("/* 大小変換の種類 */")
    lines.append("enum {")
    for mode, (name, _) in enumerate(MAPPINGS):
        lines.append("    P2C_UNI_CASE_%s = %d," % (name.upper(), mode))
    lines.append("};")
    lines.append("")
    lines.append("/* cp の変換結果（完全な写像）を out へ書き、コードポイント数（1〜3）を返す。")
    lines.append(" * 変換しない文字は out[0] = cp と 1 を返す。 */")
    lines.append("int p2c_uni_case_map(uint32_t cp, int mode, uint32_t out[3]);")
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append("}")
    lines.append("#endif")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    return "\n".join(lines)


def build():
    src = "\n".join(emit([])) + "\n"
    return header_text(), src


def main(argv):
    header, source = build()
    if "--check" in argv:
        ok = True
        for path, text in ((HEADER, header), (SOURCE, source)):
            try:
                with open(path, encoding="utf-8") as f:
                    current = f.read()
            except OSError:
                current = None
            if current != text:
                print("stale: %s" % os.path.relpath(path, ROOT))
                ok = False
        if ok:
            print("unicode_tables_fresh_ok")
            return 0
        return 1
    with open(HEADER, "w", encoding="utf-8") as f:
        f.write(header)
    with open(SOURCE, "w", encoding="utf-8") as f:
        f.write(source)
    print("wrote %s and %s" % (os.path.relpath(HEADER, ROOT), os.path.relpath(SOURCE, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
