# セミコロンで 1 行に複数文を書くと AST_BLOCK が生成される。
# 以前はこの形でコンパイラが二重解放（heap-use-after-free）で落ちていた。
d = {"a": 1, "b": 2}; print(list(d.keys()), list(d.values()))
e = {}; e["x"] = [1, 2]; print(len(e), e["x"][1], list(e))
a = [3, 1, 2]; a.sort(); print(a, a[0], len(a))
s = "a,b,c"; p = s.split(","); print(p, p[1], len(p))
x = 1; y = 2; x, y = y, x; print(x, y)
t = (1, 2); print(t, t[0]); u = (3, 4); print(u[1], t == (1, 2))
f = {"m": 1}; g = {"n": 2}; print(sorted(f.items()), sorted(g.keys()))
i = 0; i += 1; print(i); j = [1]; j.append(2); print(j)
r = range(5); print(list(r), r[2], len(r))
print("x" * 2, "y".upper(), [1, 2] + [3]); print("%d" % 7)
