"""Alpha1.0 Round-4 で追加・修正した機能の CPython 差分回帰（tests/round4_alpha10.py）。

網羅する内容:
  * リテラル内のイテラブル展開（[*a] / (*a,) / {*a}）と辞書マージ（{**d}）
  * ジェネレータの send()、`x = yield v` の送信値、PEP 380 の return 値
  * ジェネレータ内の assert / 型注釈付き代入、入れ子ループの break と else
  * 入れ子アンパック代入（(a, b), c = ...）
  * ユーザー定義 __lt__ を使う sorted()/min()/max()
  * 組み込み例外の .args、type(x).__name__、super().__init__（組み込み例外の基底）
"""


class Item:
    def __init__(self, name, price):
        self.name = name
        self.price = price

    def __lt__(self, other):
        return self.price < other.price

    def __repr__(self):
        return self.name


# --- リテラル展開 ---
a = [1, 2]
b = (3,)
print([*a, *b, 4])
print((*a, *b))
print(sorted({*a, *b, 9}))
print(sorted({**{"a": 1, "b": 2}, **{"b": 3}}.items()))
print(len((*a,)), [0, *a])

# --- ジェネレータ: send と return 値 ---
def agg(seed):
    total = seed
    while True:
        v = yield total
        if v is None:
            return total
        total += v


it = agg(10)
print(next(it), it.send(1), it.send(2))
try:
    next(it)
except StopIteration as e:
    print("stop args", e.args)

# --- ジェネレータ: 入れ子ループの break と else ---
def walk(rows):
    for row in rows:
        for v in row:
            if v < 0:
                break
            yield v
        else:
            yield -1


print(list(walk([[1, 2], [3, -4], [5]])))

# --- ジェネレータ内の assert と型注釈 ---
def scaled(n):
    factor: int = 2
    for i in range(n):
        assert i < 10, "too big"
        yield i * factor


print(list(scaled(3)))

# --- 入れ子アンパック ---
(p1, p2), p3 = (1, 2), 3
print(p1, p2, p3)
head, *mid, tail = [1, 2, 3, 4, 5]
print(head, mid, tail)

# --- __lt__ によるソート ---
xs = [Item("b", 2), Item("a", 5), Item("c", 1)]
print(sorted(xs), min(xs), max(xs))

# --- 例外の .args と型名 ---
try:
    raise KeyError("k")
except KeyError as e:
    print(e.args, type(e).__name__)


class AppError(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code


try:
    raise AppError(7, "bad")
except AppError as e:
    print(e.code, str(e), e.args)

# --- 書式（入れ子フィールド） ---
name = "ab"
w = 5
print(f"[{name:>{w}}]", "[{:*^7}]".format(name), "{:.{}f}".format(2.71828, 2))
