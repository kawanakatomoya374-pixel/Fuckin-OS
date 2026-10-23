a={1,2,3,4}; b={3,4,5}
print(a<=b, a<b, a>=b, a>b, {3}<=b, {1,2}<=a, a<a, a<=a)
print(frozenset({1})<=frozenset({1,2}), set()<=b, {9}<=b)
