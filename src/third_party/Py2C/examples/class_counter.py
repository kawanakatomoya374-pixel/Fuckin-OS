class Counter:
    kind = "ctr"
    def __init__(self, start):
        self.value = start
    def inc(self):
        self.value = self.value + 1
        return self.value

c = Counter(4)
print(c.kind)
print(c.inc())
print(c.value)
