# 文字列の % 書式（剰余演算子による書式化）の回帰。
# 以前は左辺が文字列でも数値の剰余として扱われ、"%s(%.2f)" % (...) が
# ZeroDivisionError で落ちていた（Alpha1.0 で修正）。
print("%s(%.2f)" % ("rect", 6.0))
print("%d/%i/%u/%x/%X/%o" % (10, -10, 7, 255, 255, 8))
print("%-6d|%6d|%06d|%+d|% d" % (3, 3, 3, 3, 3))
print("%5.2f|%-6.1f|%.3e|%g" % (3.14159, 2.5, 1234.5, 0.0001))
print("%5s|%-5s|%.2s" % ("ab", "cd", "wxyz"))
print("%r|%a" % ("x", "y"))
print("%c%c%%" % (65, "z"))
print("%(name)s=%(v)d" % {"name": "n", "v": 3})
print("%s" % {"a": 1})
print("%*d|%.*f" % (5, 42, 2, 3.14159))
print("%s" % 6.0, "%d" % 3.7, "%d" % True, "%s" % ((1, 2),))
print("[%s]" % [])
print("%s" % [1, 2, 3], "%s" % ((1, 2),))
print("%s|%r" % (None, None))
print("no spec: %s" % "tail")
value = 7
print("%s=%s" % ("value", value), "%d" % value)
print("%.1f%%" % 33.33)

try:
    "%s %s" % (1,)
except TypeError:
    print("TypeError: not enough arguments")

try:
    "abc" % 5
except TypeError:
    print("TypeError: not all arguments converted")

try:
    "%(missing)s" % {"a": 1}
except KeyError:
    print("KeyError: missing key")

try:
    "%(a)s" % 5
except TypeError:
    print("TypeError: format requires a mapping")
