p = "build/p2c_open_kw_regression.txt"
f = open(p, mode="w", encoding="utf-8")
f.write("héllo\n")
f.close()
print(open(p, encoding="UTF-8").read().strip())
print(open(p, "r", encoding="utf8").readline().strip())
