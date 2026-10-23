#!/usr/bin/env python3
"""文字列/コンテナ操作の差分ファザ（Alpha1.0）。

シード付き乱数で Python スニペットを生成し、CPython の出力と
トランスパイル済み C の出力を比較する。差分が出たケースは
build/fuzz10/ に残して最小再現に使えるようにする。

  使い方: python3 tests/string_fuzz_diff_alpha10.py [cases]
  環境変数: P2C_FUZZ_SEED（既定 20261003）/ P2C_COMPILER
"""
import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P2C = os.environ.get("P2C_COMPILER", os.path.join(ROOT, "build/python-code-to-c"))
OUT = os.path.join(ROOT, "build/fuzz10")
CASES = int(sys.argv[1]) if len(sys.argv) > 1 else int(os.environ.get("P2C_FUZZ_CASES", "150"))
SEED = int(os.environ.get("P2C_FUZZ_SEED", "20261003"))

SRCS = [
    "src/runtime/python_code_to_c_runtime.c",
    "src/common/python_code_to_c_common.c",
    "src/platform/python_code_to_c_platform.c",
    "src/platform/python_code_to_c_platform_hosted.c",
    "src/platform/python_code_to_c_gui.c",
    "src/modules/python_code_to_c_pygame.c",
]

LITERALS = ['""', '"a"', '"abc"', '"a,b,,c"', '"  pad  "', '"xxaxx"', '"one--two"',
            '"\\u3042\\u3044\\u3046"', '"caf\\u00e9"', '"MiXeD"', '",,"', '"ab" * 3']
SEPS = ['","', '"--"', '"x"', 'None', '" "']
CHARS = ['""', '"a"', '"ab"', '" \\t"']
INTS = ["0", "1", "2", "-1", "3", "5", "-3"]
SLICES = ["[0]", "[1]", "[-1]", "[1:3]", "[:2]", "[2:]", "[::-1]", "[::2]", "[-3:-1]"]


def gen_case(rng):
    """1 ケース分の Python ソースを生成する（決定的な出力のみ）。"""
    lines = []
    s = rng.choice(LITERALS)
    lines.append("s = %s" % s)
    kind = rng.randrange(10)
    if kind == 0:
        lines.append("print(repr(s.split(%s, %s)))" % (rng.choice(SEPS), rng.choice(INTS)))
    elif kind == 1:
        lines.append("print(repr(s.rsplit(%s, %s)))" % (rng.choice(SEPS), rng.choice(INTS)))
    elif kind == 2:
        lines.append("print(repr(s.partition(%s)), repr(s.rpartition(%s)))"
                     % (rng.choice(['","', '"--"', '"x"']), rng.choice(['","', '"--"', '"x"'])))
    elif kind == 3:
        method = rng.choice(["strip", "lstrip", "rstrip"])
        lines.append("print(repr(s.%s(%s)))" % (method, rng.choice(CHARS)))
    elif kind == 4:
        lines.append("print(repr(s.replace(%s, %s, %s)))"
                     % (rng.choice(['"a"', '"x"', '","', '""']), rng.choice(['"Z"', '""', '"ab"']),
                        rng.choice(["0", "1", "2", "-1"])))
    elif kind == 5:
        method = rng.choice(["find", "rfind", "index", "rindex", "count", "startswith", "endswith"])
        lines.append("print(s.%s(%s), s.%s(%s))" % (method, rng.choice(['"a"', '"x"', '","', '"ab"']),
                                                    method, rng.choice(['""', '"a"', '"--"'])))
    elif kind == 6:
        method = rng.choice(["upper", "lower", "casefold", "title", "capitalize", "swapcase"])
        lines.append("print(repr(s.%s()))" % method)
    elif kind == 7:
        method = rng.choice(["ljust", "rjust", "center", "zfill"])
        lines.append("print(repr(s.%s(%s)))" % (method, rng.choice(["0", "1", "4", "7", "10"])))
    elif kind == 8:
        lines.append("print(repr(s%s), len(s), s in s, repr(s + s))" % rng.choice(SLICES))
    else:
        lines.append("print(repr(s.splitlines()), repr(\"|\".join([s, s])), s * 2)")
    return "\n".join(lines) + "\n"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, timeout=120, **kw)


def main():
    if not os.path.exists(P2C):
        print("compiler not built: %s" % P2C)
        return 2
    os.makedirs(OUT, exist_ok=True)
    rng = random.Random(SEED)
    diffs = 0
    build_fail = 0
    for i in range(CASES):
        src = gen_case(rng)
        pyfile = os.path.join(OUT, "case_%03d.py" % i)
        cfile = os.path.join(OUT, "case_%03d.c" % i)
        exe = os.path.join(OUT, "case_%03d" % i)
        with open(pyfile, "w", encoding="utf-8") as fh:
            fh.write(src)
        py = run([sys.executable, pyfile])
        if py.returncode != 0:
            continue  # CPython 側が例外を出したケースは差分比較の対象外
        want = py.stdout
        conv = run([P2C, pyfile, "-o", cfile])
        if conv.returncode != 0:
            sys.stderr.write("TRANSPILE-FAIL case_%03d: %s\n" % (i, conv.stderr.strip().splitlines()[:1]))
            build_fail += 1
            continue
        cc = run(["cc", "-I./include", "-std=gnu11", "-O1", cfile] + SRCS + ["-lm", "-o", exe])
        if cc.returncode != 0:
            sys.stderr.write("BUILD-FAIL case_%03d\n" % i)
            build_fail += 1
            continue
        got = run([exe]).stdout
        if got != want:
            diffs += 1
            print("DIFF case_%03d (%s)" % (i, src.strip().splitlines()[0]))
            print("  python: %r" % want.strip()[:160])
            print("  c     : %r" % got.strip()[:160])
    print("string_fuzz: %d cases, %d mismatches, %d build/transpile failures (seed=%d)"
          % (CASES, diffs, build_fail, SEED))
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
