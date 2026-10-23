"""Alpha1.0 Round-6 の回帰（tests/round6_alpha10.py）。

対象:
  * `for (a, b) in ...` / `for (x) in ...` / 入れ子 `for (a, (b, c)) in ...` /
    先頭 `*` 付きの for ターゲット
  * 再帰呼び出しを含む二項演算式（`1 + f(n-1)`）が深い再帰でも動作すること
    （式評価スタックを動的化する前は 64 段で誤って RuntimeError になっていた）
  * 型変換・除算・剰余の符号、round の偶捨五入
"""


def countdown(n):
    if n <= 0:
        return 0
    return 1 + countdown(n - 1)


print(countdown(140))


def deep(n):
    if n == 0:
        return "bottom"
    return deep(n - 1)


print(deep(200))

pairs = [(1, 2), (3, 4)]
for (a, b) in pairs:
    print(a + b)

for (only) in [7]:
    print(only)

for (x, (y, z)) in [(1, (2, 3))]:
    print(x, y, z)

for (head, *rest) in [(1, 2, 3, 4)]:
    print(head, rest)

print(int(-3.9), int(3.9), -7 // 2, -7 % 3, divmod(-7, 2))
print(round(0.5), round(1.5), round(2.5), round(-0.5), round(-1.5))
print(7.0 // 2, -7.5 // 2, 7.5 % 2, -7.5 % 2)
