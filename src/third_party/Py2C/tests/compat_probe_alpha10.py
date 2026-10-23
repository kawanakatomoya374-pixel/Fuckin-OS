# CPython互換性プローブ（Alpha1.0）
# ここに並べた式の結果を1行ずつ出力し、CPython と文字単位で比較する。
# 例外は捕捉して「タグのみ」を出し、メッセージ差では差分にならないようにする。
# 実行: sh tests/quick_diff_alpha10.sh tests/compat_probe_alpha10.py


def show(tag, fn):
    try:
        print(tag, fn())
    except TypeError:
        print(tag, "<TypeError>")
    except ValueError:
        print(tag, "<ValueError>")
    except KeyError:
        print(tag, "<KeyError>")
    except IndexError:
        print(tag, "<IndexError>")
    except ZeroDivisionError:
        print(tag, "<ZeroDivisionError>")
    except AttributeError:
        print(tag, "<AttributeError>")
    except OverflowError:
        print(tag, "<OverflowError>")
    except StopIteration:
        print(tag, "<StopIteration>")


# ── 数値 ──
show("P01", lambda: 7 // 2)
show("P02", lambda: -7 // 2)
show("P03", lambda: 7 % 3)
show("P04", lambda: -7 % 3)
show("P05", lambda: 7 % -3)
show("P06", lambda: divmod(-7, 3))
show("P07", lambda: divmod(7.5, 2))
show("P08", lambda: 0.1 + 0.2)
show("P09", lambda: 1 / 3)
show("P10", lambda: round(2.5))
show("P11", lambda: round(3.5))
show("P12", lambda: round(-0.5))
show("P13", lambda: round(2.675, 2))
show("P14", lambda: float("inf") > 10 ** 9)
show("P15", lambda: int(-3.7))
show("P16", lambda: abs(-3.5))
show("P17", lambda: pow(2, 10))
show("P18", lambda: pow(2, 10, 7))
show("P19", lambda: 2 ** 100 > 10 ** 29)
show("P20", lambda: 10 ** 30)
show("P21", lambda: (2 ** 64) // 3)
show("P22", lambda: 5 // 0 if False else "skip")
show("P23", lambda: float(2) ** 0.5)
show("P24", lambda: 10 % 0 if False else "skip")
show("P25", lambda: True + True)
show("P26", lambda: int("0x1f", 16))
show("P27", lambda: int(" 42 "))
show("P28", lambda: int("-0"))
show("P29", lambda: float("1e3"))
show("P30", lambda: 3 == 3.0)

# ── 文字列 ──
show("P31", lambda: "abc"[::-1])
show("P32", lambda: "abcdef"[1:5:2])
show("P33", lambda: "abcdef"[-3:])
show("P34", lambda: "a,b,,c".split(","))
show("P35", lambda: "  x  ".strip())
show("P36", lambda: "aaa".replace("a", "b", 2))
show("P37", lambda: "abc".find("z"))
show("P38", lambda: "abc".startswith(("x", "ab")))
show("P39", lambda: "-".join(["a", "b"]))
show("P40", lambda: "ab" * 3)
show("P41", lambda: "Hello".lower().title())
show("P42", lambda: "abc".upper().isupper())
show("P43", lambda: "1,2".partition(","))
show("P44", lambda: "%s|%d|%.1f" % ("a", 3, 2.25))
show("P45", lambda: "%05.2f" % 3.14159)
show("P46", lambda: "{}-{}".format(1, "x"))
show("P47", lambda: f"{3.14159:.3f}|{42:>5}|{7:05d}")
show("P48", lambda: repr("a'b"))
show("P49", lambda: str([1, "a", (2,)]))
show("P50", lambda: str({"a": [1, 2]}))

# ── コンテナ ──
show("P51", lambda: [1, 2] + [3])
show("P52", lambda: sorted([3, 1, 2], reverse=True))
show("P53", lambda: sorted(["bb", "a", "ccc"], key=len))
show("P54", lambda: sorted([(2, "b"), (1, "a")]))
show("P55", lambda: list(zip([1, 2, 3], "ab")))
show("P56", lambda: [x * 2 for x in range(3) if x])
show("P57", lambda: {k: v for k, v in [("a", 1)]})
show("P58", lambda: sorted({3, 1, 2}))
show("P59", lambda: len({1: "a", 2: "b"}))
show("P60", lambda: list({"a": 1, "b": 2}.keys()))
show("P61", lambda: {"a": 1}.get("z", "d"))
show("P62", lambda: [1, 2, 3][-1])
show("P63", lambda: (1, 2) < (1, 3))
show("P64", lambda: [1, [2]] == [1, [2]])
show("P65", lambda: min([3, 1, 2], key=lambda v: -v))
show("P66", lambda: max("a", "b"))
show("P67", lambda: sum([1, 2], 10))
show("P68", lambda: list(enumerate("ab", 1)))
show("P69", lambda: list(reversed([1, 2, 3])))
show("P70", lambda: any([0, "", 3]))
show("P71", lambda: all([1, "a"]))

# ── 制御と関数 ──
def add_default(a, b=2):
    return a + b


def sum_all(*a):
    return sum(a)


def sorted_keys(**k):
    return sorted(k)


def pos_only(a, /, b, *, c):
    return a + b + c


def pair(n):
    return [n, n]


odd_squares = [x for x in range(5) if x % 2]
gen_total = sum(y for y in range(4))
show("P72", lambda: odd_squares[1])
show("P73", lambda: add_default(1))
show("P74", lambda: sum_all(1, 2, 3))
show("P75", lambda: sorted_keys(b=1, a=2))
show("P76", lambda: pos_only(1, 2, c=3))
show("P77", lambda: gen_total)
show("P78", lambda: pair(5))
show("P79", lambda: [list(p) for p in [(1, 2)]])
show("P80", lambda: isinstance(True, int))
