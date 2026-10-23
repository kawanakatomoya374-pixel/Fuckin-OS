b = b"hi"
print(bytes(3), bytes([65, 66]), bytes(b), bytes("é", "utf-8"))
print(b + b"!!", len(b + b"!!"))
print("héllo".encode("utf-8"), "ab".encode())
print(b"h\xc3\xa9".decode("utf-8"), b"ok".decode())
print(isinstance(b, bytes), isinstance(b, str), isinstance("x", bytes))
try:
    b"\xff".decode()
except UnicodeDecodeError:
    print("decode error")
