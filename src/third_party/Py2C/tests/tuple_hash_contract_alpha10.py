d = {(1, "alpha"): "first"}
d[(1, "alpha")] = "second"
print(len(d), d[(1, "alpha")])

s = {(1, "alpha"), (2, "beta")}
print((1, "alpha") in s, (3, "gamma") in s)
print({(1, "alpha"), (2, "beta")} == {(2, "beta"), (1, "alpha")})

mixed = {(1, True, 1.0): "numeric"}
print(len(mixed), mixed[(True, 1.0, 1)])
