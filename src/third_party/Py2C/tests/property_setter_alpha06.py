# @property の setter（@x.setter）の回帰。
class Temp:
    def __init__(self, celsius):
        self._celsius = celsius
        self.trace = []

    @property
    def celsius(self):
        return self._celsius

    @celsius.setter
    def celsius(self, value):
        self.trace.append(value)
        self._celsius = value

    @property
    def fahrenheit(self):
        return self._celsius * 9 / 5 + 32


t = Temp(0)
print(t.celsius)
t.celsius = 25
print(t.celsius, t.fahrenheit, t.trace)


class ReadOnly:
    def __init__(self):
        self._v = 1

    @property
    def v(self):
        return self._v


ro = ReadOnly()
print(ro.v)
try:
    ro.v = 5
    print("assigned")
except AttributeError:
    print("AttributeError")
print(ro.v)


class Base:
    def __init__(self):
        self._v = 1

    @property
    def v(self):
        return self._v

    @v.setter
    def v(self, value):
        self._v = value * 2


class Derived(Base):
    pass


d = Derived()
d.v = 21
print(d.v)
