# タプル/リストの辞書式比較とsorted()の安定性の回帰。
# 以前はタプルを整数0として比較しており、sorted()が黙って未ソートの結果を返していた。

print(sorted([(2, 1), (1, 1)]))
print(sorted([(2, "a"), (1, "b"), (1, "a")]))
print(sorted([[2, 1], [1, 9]]))
print(min([(3, 0), (1, 5)]), max([(3, 0), (1, 5)]))
print(sorted([3, 1, 2], reverse=True))
print(sorted(["b", "a", "c"]))
print(sorted([1.5, 0.5, 2.5]))
pairs = [(1, "b"), (1, "a"), (0, "z")]
print(sorted(pairs))
print(sorted(pairs, key=lambda p: p[1]))
print(sorted([(2, 2), (2, 1), (1, 9)], key=lambda p: p[0]))
