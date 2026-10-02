# range() は要素を作らない遅延オブジェクト（CPython と同じ）。
# 巨大な range でもメモリを使わず、len/添字/in/repr が算術で答えること。
big = range(10 ** 12)
print(len(big))
print(big[0], big[999999999999], big[-1])
print(500000000000 in big, 1000000000000 in big, (-1) in big)
print(repr(big))
print(repr(range(5)), repr(range(1, 5)), repr(range(1, 9, 2)))
print(str(range(3, 0, -1)))

# 負のステップ
print(len(range(10, 0, -3)), list(range(10, 0, -3)))
print(range(10, 0, -3)[1], range(10, 0, -3)[-1])
print(9 in range(10, 0, -2), 8 in range(10, 0, -2), 0 in range(10, 0, -2))

# 空の range / 端の場合
print(len(range(0)), len(range(5, 5)), len(range(5, 1)), bool(range(0)), bool(range(1)))
print(list(range(0)), list(range(3, 3)), list(range(-2, 2)))

# for 文・合計・ソート
print(sum(range(101)))
print(sum(range(50, 0, -7)))
print(sorted(range(5, 0, -2)))
print(max(range(3, 9, 4)), min(range(30, 0, -11)))
for i in range(3, 0, -1):
    print(i)
for i in range(0):
    print("never")

# 等値比較: CPython は (長さ, start, step) で比較する
print(range(0, 3, 2) == range(0, 4, 2), range(3) == range(3), range(0) == range(1, 1))
print(range(3) == [0, 1, 2], range(0, 3) != range(0, 4))

# in と float
print(1.0 in range(3), 1.5 in range(3), 2.0 in range(1, 3))

# 引数の誤り（CPython と同じ例外）
try:
    range(0, 5, 0)
except ValueError as e:
    print("ValueError", str(e))
try:
    range(1.5)
except TypeError as e:
    print("TypeError", str(e))
