log = []
def t(x):
    log.append(x)
    return x
def f3(a, b, c):
    return (a, b, c)
class C:
    def m(self, a, b, c):
        return (a, b, c)
c = C()
print(f3(t(1), t(2), t(3)), log); log.clear()
print(max(t(4), t(5), t(6)), log); log.clear()
print(c.m(t(1), t(2), t(3)), log); log.clear()
print([t(7), t(8), t(9)], log); log.clear()
print((t(1), t(2), t(3)), log); log.clear()
print({t(1), t(2), t(3)} == {1,2,3}, log); log.clear()
print(f3(c=t(1), b=t(2), a=t(3)), log); log.clear()
print(f3(*[t(1), t(2), t(3)]), log); log.clear()
print(str(t(1)) + str(t(2)), log); log.clear()
print(t(1) * t(2) - t(3), log); log.clear()
print(f3(t(1) + t(2), t(3), t(4)), log); log.clear()

# 辞書の要素は キー → 値 の順に評価する
dlog = []
def dk(x):
    dlog.append(("dk", x))
    return x
def dv(x):
    dlog.append(("dv", x))
    return x
dd = {dk(1): dv(10), dk(2): dv(20)}
print(sorted(dd.items()))
print(dlog)
