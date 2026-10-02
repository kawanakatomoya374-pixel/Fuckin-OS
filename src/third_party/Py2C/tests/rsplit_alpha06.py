# str.rsplit（右から分割。maxsplit と空白モードの両方）
print("a,b,,c".rsplit(","), "a,b,,c".rsplit(",", 1), "a,b,,c".rsplit(",", 2))
print("a,b,c".rsplit(",", 0), "a,b,c".rsplit(",", 9), ",a,".rsplit(","))
print("  a  b  ".rsplit(), "  a  b  ".rsplit(None, 1), "a b c".rsplit(None, 2))
print("a b c".split(None, 1), "a b c".split(None, 2), "a b c".rsplit(None, 0))
print("xxaxx".rsplit("x"), "xxaxx".rsplit("xx"), "abc".rsplit("z"), "abc".rsplit("z", 1))
s = "one--two--three"
print(s.rsplit("--"), s.rsplit("--", 1), s.partition("--"), s.rpartition("--"))
print("a\u00e9b".rsplit("\u00e9"), "\u3042,\u3044,\u3046".rsplit(",", 1))
print("a,b,c".split(",", 1), "a,b,c".rsplit(",", 1), "a,b,c".split(","))
