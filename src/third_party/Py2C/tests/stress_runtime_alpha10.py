"""ヒープ/ランタイムのストレス回帰（ASan/UBSan/LSan と併用する）。

辞書・集合の動的リサイズ、文字列の伸縮、例外の反復、GC 圧力、入れ子コンテナ、
ソート、スライス代入を小さなループで大量に回し、通常の回帰では踏みにくい
「確保と解放の繰り返し」経路を叩く。出力は決定的なので CPython と diff できる。
"""

# 1) 辞書の伸縮（追加・削除を交互に大量に行い、リサイズ経路を往復させる）
d = {}
for i in range(600):
    d[i] = i * 3
    if i % 5 == 0 and (i // 5) in d:
        del d[i // 5]
print(len(d), d[599], d.get(4), d.get(999999, -1))

# 2) 集合の伸縮と集合演算
s = set()
for i in range(400):
    s.add(i % 97)
    if i % 11 == 0:
        s.discard(i % 13)
print(len(s), sorted(s)[:5], 42 in s, s.isdisjoint({1000, 2000}))

# 3) 文字列の伸縮（join と連結を繰り返し、部分文字列の解放を促す）
parts = []
for i in range(300):
    parts.append(str(i))
joined = ",".join(parts)
print(len(joined), joined[:7], joined[-5:], joined.count(","))

# 4) リストの伸縮とスライス代入
a = list(range(50))
for i in range(200):
    a.append(i)
    if len(a) > 60:
        del a[0:5]
    if i % 37 == 0:
        a[1:3] = [i, i + 1]
print(len(a), a[0], a[-1], sum(a) % 1000)

# 5) 例外の反復（raise/except のたびに生成・解放される経路）
caught = 0
for i in range(200):
    try:
        if i % 3 == 0:
            raise ValueError("boom")
        caught += 1
    except ValueError as exc:
        caught -= 1
print(caught)

# 6) GC 圧力（短命な入れ子コンテナを大量に作る）
total = 0
for i in range(400):
    nested = {"k": [i, {"n": (i, i + 1)}], "s": {i % 7}}
    total += nested["k"][0]
    total += len(nested["s"])
print(total)

# 7) ジェネレータ式と内包表記の反復
#    （注意: `yield` をループ本体に置く通常のジェネレータ関数は、この版の
#      コード生成では未対応。docs/spec/FEATURE_REFERENCE_ALPHA1.0.md の
#      「意図的な制限」を参照。ここでは対応済みのジェネレータ式を使う。）
print(sum(x * x for x in range(50)), [x for x in range(10) if x % 2 == 0])

# 8) ソートと Unicode 文字列
words = ["banana", "apple", "", "cherry", "date"]
words.sort(key=len)
print(words, sorted(["あ", "い", "う"], reverse=True))
u = "あいうえお"
print(len(u), u[1:4], u[::-1], u.upper(), u.count("い"))

# 9) str.format と % 書式の反復（一時文字列の解放）
for i in range(100):
    text = "{}-{:04d}-{:.2f}".format(i, i, i / 4)
    if i % 25 == 0:
        print(text)
print("%s|%d|%5.2f" % ("x", 7, 3.5))
