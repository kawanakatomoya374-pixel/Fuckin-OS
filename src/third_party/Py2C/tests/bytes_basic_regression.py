b = b"abc\x00\xff"
print(len(b), b[0], b[-1], b[2])
print(repr(b))
print(b == b"abc\x00\xff", b == b"x", b == "abc")
for x in b"hi":
    print(x)
print(b"it's", b'say "hi"', b"tab\t")
print(list(b"AB"))
