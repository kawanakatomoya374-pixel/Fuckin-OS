# ジェネレータ式の複数for節・タプルターゲット・ifフィルタの回帰。
# 以前は1つのfor節と単純名ターゲットしか受け付けなかった。

print(sum(x * y for x in [1, 2, 3] for y in [10, 20]))
print(list(x for x in range(4) if x % 2 == 0))
print(sum(x + y for x, y in [(1, 2), (3, 4)]))
print(list((a, b) for a in [1, 2] for b in "xy" if a == 2))
print(sum(i * j * k for i in range(3) for j in range(3) if j != 1 for k in range(2)))
print(sum(len(s) for s in ["ab", "cde"] if len(s) > 2))
n = 5
print(sum(x + n for x in range(3)))
print(sorted((x, y) for x in [2, 1] for y in [1]))
seen = []
gen = (v * 2 for v in [1, 2, 3])
for v in gen:
    seen.append(v)
print(seen)
print(list(zip([1, 2], [3, 4])))
