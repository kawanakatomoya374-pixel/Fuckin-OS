# range のスライスは range を返す（CPython 準拠・要素を作らないので O(1)）。
r10 = range(10)
print(repr(r10[2:5]), repr(r10[:]), repr(r10[::-1]), repr(r10[8:2:-2]))
print(repr(r10[5:5]), repr(r10[100:200]), repr(r10[-3:]), repr(r10[::-2]))
print(repr(r10[2:8:-1]), repr(r10[3:1]), repr(r10[5::-1]), repr(r10[-100:100]))
print(repr(range(0, 20, 3)[1:3]), repr(range(0, 20, 3)[::-1]))
print(repr(range(5, 50, 5)[10:20]), repr(range(5, 50, 5)[2:4]))

# スライス結果も本物の range（len/添字/in/list が使える）
s = range(0, 10, 2)
print(repr(s[1:4]), len(s[1:4]), list(s[1:4]))
print(4 in s[1:4], 8 in s[1:4], s[1:4][0])
rbig = range(10 ** 12)
print(repr(rbig[100:200]), len(rbig[100:200]), min(rbig[100:200]), max(rbig[100:200]))
print(len(rbig[::1000000]), repr(rbig[::1000000]))
print(rbig[100], rbig[-1], list(r10[1:8:3]))

# 辞書のキー / 集合の要素として使える（等しい range は同じキー）
d = {}
d[range(3)] = "a"
d[range(0, 3, 1)] = "b"
print(len(d), d[range(3)])
d[range(0)] = "empty"
d[range(5, 5)] = "empty2"
print(len(d), d[range(1, 1, 7)])
print(range(0, 4, 2) in d, range(0, 5, 2) in d)
s2 = set()
s2.add(range(10))
s2.add(range(0, 10))
print(len(s2), range(1, 11) in s2, range(10) in s2)
