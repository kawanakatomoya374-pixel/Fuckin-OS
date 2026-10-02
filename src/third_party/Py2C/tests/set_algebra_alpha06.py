left = {1, 2, 3}
right = {3, 4}
print(sorted(left | right))
print(sorted(left & right))
print(sorted(left - right))
print(sorted(left ^ right))
print(left == {3, 2, 1})
unioned = {1, 2}
unioned |= {2, 3}
print(sorted(unioned))
intersection = {1, 2, 3}
intersection &= {2, 3, 4}
print(sorted(intersection))
difference = {1, 2, 3}
difference -= {2}
print(sorted(difference))
symmetric = {1, 2}
symmetric ^= {2, 3}
print(sorted(symmetric))
