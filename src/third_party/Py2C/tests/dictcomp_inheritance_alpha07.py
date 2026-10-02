squares = {n: n * n for n in range(6) if n % 2 == 0}
print(squares)

class Base:
    def label(self):
        return "base"

class Middle(Base):
    pass

class Child(Middle):
    def label(self):
        return super().label() + "-child"

print(Child().label())
