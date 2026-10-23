events = []

def source():
    events.append("source")
    return [1, 2, 3, 4]

x = 99
g = (x * 10 for x in source() if x % 2 == 0)
print(events)
print(next(g))
print(events)
print(list(g))
print(x)
