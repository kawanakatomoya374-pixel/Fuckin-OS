"""Alpha1.0 Round-5 の回帰（tests/round5_alpha10.py）。

対象:
  * `assert cond, expr` のメッセージ式を評価して AssertionError.args に載せる
  * ジェネレータ内 assert（メッセージ式つき）
  * dict の `|` / `|=` と `str.removeprefix` / `str.removesuffix`
  * `zip(*m)`（引数列の展開）と enumerate(start=)
  * タプル値代入 `x = 1, 2` と連鎖代入 `a, b = c, d = 5, 6`
"""


def check(x):
    assert x > 0, "bad input: " + str(x)
    return x


print(check(5))
try:
    check(-3)
except AssertionError as e:
    print("assert args", e.args)


def gen(n):
    for i in range(n):
        assert i < 3, "too big: {}".format(i)
        yield i


print(list(gen(3)))
try:
    list(gen(5))
except AssertionError as e:
    print("gen assert", e.args)

d1 = {"a": 1, "b": 2}
d2 = {"b": 9, "c": 3}
print(dict(sorted((d1 | d2).items())), dict(sorted(d1.items())))
d1 |= d2
print(dict(sorted(d1.items())))

print("prefix_x".removeprefix("prefix_"), "x_suffix".removesuffix("_suffix"))
print("nosuffix".removeprefix("nope"), "none".removesuffix("x"))

m = [[1, 2, 3], [4, 5, 6]]
print(list(zip(*m)))
print(list(enumerate("ab", start=1)), list(enumerate(["z"], 7)))

x = 1, 2
a, b = c, d = 5, 6
print(x, a, b, c, d)
