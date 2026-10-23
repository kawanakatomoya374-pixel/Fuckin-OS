import asyncio

def pulses():
    yield 7
    yield 9

async def sensor_read():
    return 17

async def boot_task():
    value = await sensor_read()
    return value + 5

class Bus:
    label = "spi0"

    class Channel:
        def __init__(self, index, scale):
            self.index = index
            self.scale = scale

        def read(self, raw):
            return raw * self.scale

    def channel(self, index, scale):
        return Bus.Channel(index, scale)

for pulse in pulses():
    print(pulse)

print(asyncio.run(boot_task()))

first = Bus().channel(0, 2)
second = Bus().channel(1, 3)
for channel in [first, second]:
    # 1行に副作用のある式を複数置くと、ISO C11 の厳格モード（--c11）では評価順を固定できないため、
    # 順に変数へ取り出す（評価順は左から右のまま）。
    label = Bus.label
    index = channel.index
    reading = channel.read(4)
    print(label, index, reading)
