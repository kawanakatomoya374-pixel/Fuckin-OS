events = []

def record(label):
    events.append("eval:" + label)
    def decorate(fn):
        events.append("apply:" + label)
        return fn
    return decorate

@record("outer")
@record("inner")
def add(value, extra=2):
    return value + extra

print(events)
print(add(3))
print(add(3, 4))

def identity(fn):
    return fn

@identity
def triple(value):
    return value * 3

print(triple(4))
print(identity(add)(2))

import asyncio

@identity
async def answer():
    return 9

print(asyncio.run(answer()))

@identity
class Box:
    def __init__(self, value):
        self.value = value

    def double(self):
        return self.value * 2

print(Box(5).double())
