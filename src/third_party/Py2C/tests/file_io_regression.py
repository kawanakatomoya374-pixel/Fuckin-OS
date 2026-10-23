f = open("build/p2c_file_io_regression.txt", "w")
f.write("alpha\n")
f.write("beta\n")
f.close()
g = open("build/p2c_file_io_regression.txt")
print(g.read(3))
print(g.readline())
print(g.readlines())
g.close()
with open("build/p2c_file_io_regression.txt") as h:
    print(h.readline().strip())
with open("build/p2c_file_io_regression.txt", "a") as k:
    k.write("gamma\n")
print(open("build/p2c_file_io_regression.txt").read())
