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

for channel in [Bus().channel(0, 2), Bus().channel(1, 3)]:
    print(Bus.label, channel.index, channel.read(4))
