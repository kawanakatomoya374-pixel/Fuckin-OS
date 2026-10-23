#!/usr/bin/env python3
"""コンテナ・数値の差分ファザ（Alpha1.0）。

シード付き乱数で list/dict/set と整数・浮動小数の書式を組み合わせた
スニペットを生成し、CPython とトランスパイル済み C の出力を比較する。

  使い方: python3 tests/container_fuzz_diff_alpha10.py [cases]
  環境変数: P2C_FUZZ_SEED / P2C_COMPILER
"""
import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P2C = os.environ.get("P2C_COMPILER", os.path.join(ROOT, "build/python-code-to-c"))
OUT = os.path.join(ROOT, "build/fuzz10c")
CASES = int(sys.argv[1]) if len(sys.argv) > 1 else int(os.environ.get("P2C_FUZZ_CASES", "120"))
SEED = int(os.environ.get("P2C_FUZZ_SEED", "20261004"))

SRCS = [
    "src/runtime/python_code_to_c_runtime.c",
    "src/common/python_code_to_c_common.c",
    "src/platform/python_code_to_c_platform.c",
    "src/platform/python_code_to_c_platform_hosted.c",
    "src/platform/python_code_to_c_gui.c",
    "src/modules/python_code_to_c_pygame.c",
]
SPECS = ['"{:>6d}"', '"{:<6}"', '"{:^7}"', '"{:06d}"', '"{:+d}"', '"{: d}"',
         '"{:.3f}"', '"{:9.2f}"', '"{:e}"', '"{:x}"', '"{:X}"', '"{:o}"', '"{:b}"', '"{:,}"']
PERCENT_PAIRS = [
    ('"%s"', "('x',)"),
    ('"%r"', "(3.5,)"),
    ('"%5s"', "('x',)"),
    ('"%-5s|"', "('x',)"),
    ('"%05d"', "(7,)"),
    ('"%+d"', "(7,)"),
    ('"%x"', "(255,)"),
    ('"%o"', "(8,)"),
    ('"%e"', "(12345.678,)"),
    ('"%.3f"', "(3.14159,)"),
    ('"%8.2f"', "(-2.5,)"),
    ('"%%d=%d"', "(3,)"),
    ('"%*d|"', "(5, 42)"),
    ('"%s=%d"', "('a', 1)"),
]


def gen_case(rng):
    lines = []
    nums = [rng.randint(-40, 40) for _ in range(rng.randint(3, 7))]
    lines.append("a = %s" % nums)
    kind = rng.randrange(9)
    if kind == 0:
        lines.append("a.sort()")
        lines.append("print(a, a[::-1], a[1:3], len(a))")
    elif kind == 1:
        lines.append("a.sort(reverse=%s, key=%s)" % (rng.choice(["True", "False"]),
                                                     rng.choice(["abs", "str", "None"])))
        lines.append("print(a)")
    elif kind == 2:
        lines.append("a[1:2] = %s" % [rng.randint(0, 9) for _ in range(rng.randint(0, 2))])
        lines.append("del a[0:1]")
        lines.append("print(a, a.index(%d) if %d in a else -1)" % (nums[0], nums[0]))
    elif kind == 3:
        lines.append("print(sum(a), min(a), max(a), sorted(a, key=abs), a.count(%d))" % nums[1])
    elif kind == 4:
        lines.append("d = {}")
        for k in range(rng.randint(3, 6)):
            lines.append("d[%d] = %d" % (rng.randint(-3, 3), rng.randint(0, 9)))
        lines.append("print(sorted(d.items()), len(d), d.get(99, -1))")
    elif kind == 5:
        lines.append("s = set(%s)" % [rng.randint(0, 9) for _ in range(rng.randint(3, 8))])
        lines.append("print(sorted(s), len(s), sorted(s | {%d}), sorted(s & {%d, %d}))"
                     % (rng.randint(0, 9), rng.randint(0, 9), rng.randint(0, 9)))
    elif kind == 6:
        n = rng.randint(-9, 30)
        lines.append("n = %d" % n)
        lines.append("print(n, -n, n // 3, n %% 3, -n // 3, -n %% 3, divmod(n, 3), n ** 2)")
    elif kind == 7:
        v = rng.choice(["3.14159", "-2.5", "0.0", "12345.6789", "1e3"])
        lines.append("x = %s" % v)
        lines.append("print(%s, %s, %s)" % (rng.choice(SPECS), rng.choice(SPECS), rng.choice(SPECS)))
    else:
        fmt, args = rng.choice(PERCENT_PAIRS)
        lines.append("print(%s %% %s)" % (fmt, args))
    return "\n".join(lines) + "\n"


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, timeout=120)


def main():
    if not os.path.exists(P2C):
        print("compiler not built: %s" % P2C)
        return 2
    os.makedirs(OUT, exist_ok=True)
    rng = random.Random(SEED)
    diffs = bad = 0
    for i in range(CASES):
        src = gen_case(rng)
        pyfile = os.path.join(OUT, "case_%03d.py" % i)
        cfile = os.path.join(OUT, "case_%03d.c" % i)
        exe = os.path.join(OUT, "case_%03d" % i)
        with open(pyfile, "w", encoding="utf-8") as fh:
            fh.write(src)
        py = run([sys.executable, pyfile])
        if py.returncode != 0:
            continue  # 生成側のミスはケースごと捨てる
        conv = run([P2C, pyfile, "-o", cfile])
        if conv.returncode != 0:
            sys.stderr.write("TRANSPILE-FAIL case_%03d: %s\n" % (i, conv.stderr.strip().splitlines()[:1]))
            bad += 1
            continue
        cc = run(["cc", "-I./include", "-std=gnu11", "-O1", cfile] + SRCS + ["-lm", "-o", exe])
        if cc.returncode != 0:
            sys.stderr.write("BUILD-FAIL case_%03d\n" % i)
            bad += 1
            continue
        got = run([exe])
        if got.stdout != py.stdout:
            diffs += 1
            print("DIFF case_%03d" % i)
            print("  src   : %s" % src.strip().replace("\n", "; ")[:150])
            print("  python: %r" % py.stdout.strip()[:160])
            print("  c     : %r" % got.stdout.strip()[:160])
    print("container_fuzz: %d cases, %d mismatches, %d failures (seed=%d)" % (CASES, diffs, bad, SEED))
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
