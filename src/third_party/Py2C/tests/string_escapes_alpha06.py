r"""Regression: string escape decoding (\x / \u / \U / octal / unknown / continuation).

Previously the lexer dropped the backslash for \u and any unknown escape, so
"caf\u00e9" silently became "cafu00e9". Escapes are now decoded like CPython:
codepoints become UTF-8 bytes, unknown escapes keep their backslash, octal and
hex escapes produce bytes, and a backslash-newline continuation produces
nothing.

Known boundary: string length and indexing count UTF-8 *bytes* (see
SYNTAX_AND_PORTABILITY.md), so len() of a non-ASCII string differs from CPython
and is therefore not asserted here.
"""

print("tab:\t|nl:\n|")
print("hex:" + "\x41\x7a" + "|" + "hexlen:" + str(len("\x41\x7a")))
print("octal:" + "\101\102\103" + "|" + "nulleq:" + str("\0" == "\x00"))
print("unicode:" + "caf\u00e9" + "|" + "\u3042\u3044" + "|")
print("astral:" + "\U0001F600" + "|" + "\U0001F1EF\U0001F1F5" + "|")
print("unknown:" + "\d\w" + "|" + "\q" + "|")
print("continuation:" + "abcd" + "|")
print("raw:" + r"\u00e9\n" + "|")
name = "world"
print(f"f-string: {name}\u0021 {1 + 1}\t|")
print(f"f-unicode: caf\u00e9 {{literal}}")
print("backslash:" + "a\b" + "|")
print("quote:" + "\"'" + "|")
print("length:", len("cafe"), len("\d\w"), len("\x41\x42"))
print("isascii:", "caf\u00e9".isascii(), "cafe".isascii())
print("compare:", "caf\u00e9" == "caf" + "\u00e9", "caf\u00e9" != "cafe")
print("contains:", "\u00e9" in "caf\u00e9", "z" in "caf\u00e9")
