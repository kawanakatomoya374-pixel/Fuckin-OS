d = {b"k": 1, b"a\x00b": 2}
print(d[b"k"], d[b"a\x00b"], b"k" in d, b"zz" in d)
d[b"k"] = 5
print(len(d), d[b"k"])
s = {b"x", b"y", b"x"}
print(len(s), b"y" in s)
print(sorted([b"b", b"a"]) if False else b"ab" < b"b" if False else len({b"q", b"q"}))
