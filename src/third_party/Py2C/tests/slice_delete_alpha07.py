first = [0, 1, 2, 3, 4, 5]
del first[1:4]
print(first)

second = [0, 1, 2, 3, 4, 5, 6]
del second[::2]
print(second)

third = [0, 1, 2, 3, 4, 5]
del third[5:0:-2]
print(third)
