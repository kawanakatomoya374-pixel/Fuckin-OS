# 空文字列・空白のみの分割（CPython は空白モードで空リストを返す）
print(repr("".rsplit()), repr("   ".rsplit()), repr("\t ".rsplit()))
print(repr("".rsplit(None, 0)), repr("  a  ".rsplit(None, 0)), repr("  a  b  ".rsplit(None, 1)))
print(repr("".split()), repr("   ".split()), repr("\t".split()), repr("".split(None, 0)))
print(repr("  a  ".split(None, 0)), repr("  a  b  ".split(None, 1)))
print(repr("".rsplit(",")), repr("".rsplit(",", 0)), repr("".split(",")), repr("".partition(",")))
try:
    print(repr("".rsplit("", 1)))
except ValueError as exc:
    print("ValueError", str(exc))
try:
    print(repr("abc".rsplit("", 1)))
except ValueError as exc:
    print("ValueError", str(exc))
