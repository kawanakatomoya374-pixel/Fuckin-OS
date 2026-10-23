print("€".encode("cp1252"), "é".encode("windows-1252"), "ÿ".encode("cp1252"))
print(b"\x80\x93x\x94".decode("cp1252"), b"\xe9".decode("cp1252"))
try:
    b"\x81".decode("cp1252")
except UnicodeDecodeError:
    print("decode error")
try:
    "中".encode("cp1252")
except UnicodeEncodeError:
    print("encode error")
f = open("build/p2c_cp1252_regression.txt", "w", encoding="cp1252")
f.write("€ é\n")
f.close()
print(open("build/p2c_cp1252_regression.txt", encoding="cp1252").read().strip())
