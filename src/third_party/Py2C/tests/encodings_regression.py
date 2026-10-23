print("é".encode("latin-1"), "abc".encode("ascii"))
print(b"\xe9".decode("latin-1"), b"abc".decode("ascii"))
try:
    "é".encode("ascii")
except UnicodeEncodeError:
    print("encode error")
try:
    b"\xff".decode("ascii")
except UnicodeDecodeError:
    print("decode error")
print(bytes("é", "latin1"), bytes("abc", "US-ASCII"), "ok".encode("LATIN_1"))
try:
    "x".encode("nosuchcodec")
except LookupError:
    print("lookup")
f = open("build/p2c_enc_regression.txt", "w", encoding="latin-1")
f.write("é\n")
f.close()
print(open("build/p2c_enc_regression.txt", encoding="latin-1").read().strip())
print(open("build/p2c_enc_regression.txt", encoding="latin-1").readline().strip())
