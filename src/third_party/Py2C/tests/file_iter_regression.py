f = open("build/p2c_file_iter_regression.txt", "w")
f.write("one\ntwo\n")
f.close()
g = open("build/p2c_file_iter_regression.txt")
for line in g:
    print(repr(line))
g.close()
