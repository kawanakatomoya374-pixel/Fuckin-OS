"""Kernel-side demo module for the p2c_embed integration test.

Transpiled with:

    python-code-to-c examples/embed/embed_boot.py \
        --embed-entry p2c_embed_program -o build/embed/embed_boot.c

The generated C exposes exactly one kernel-callable symbol
``P2C_Object *p2c_embed_program(void)`` and no ``main``, so a hobby OS can run
it as a task while owning the heap, the stack window, the console/UART and the
clock.  The output below is compared against CPython by
``make test-embed-generated``.
"""

STATE = {}


def checksum(values):
    total = 0
    for value in values:
        total = (total * 31 + value) % 65521
    return total


class Sensor:
    def __init__(self, name, scale):
        self.name = name
        self.scale = scale

    def read(self, raw):
        return raw * self.scale

    def __str__(self):
        return "Sensor(" + self.name + ")"


class Bus:
    """ネストクラスのデモ: Channel は Bus のクラス本体で定義する。

    生成Cでは外側クラス名を前置した C 名（Bus__Channel）になり、
    Bus の __classobj() が属性として登録する。カーネル側のヒープだけで
    動くため、組込み環境でもそのまま動く。
    """

    label = "spi0"

    class Channel:
        def __init__(self, index, scale):
            self.index = index
            self.scale = scale

        def read(self, raw):
            return raw * self.scale

    def channel(self, index, scale):
        return Bus.Channel(index, scale)


def boot():
    samples = [3, 1, 4, 1, 5, 9, 2, 6]
    try:
        checksum([])
        raise ValueError("empty sample set")
    except ValueError as exc:
        print("recovered:", str(exc))
    print("sum", sum(samples), "checksum", checksum(samples))
    odd_squares = [x * x for x in samples if x % 2 == 1]
    print("odd squares", odd_squares)
    sensor = Sensor("temp", 3)
    print(sensor, sensor.read(7))
    bus = Bus()
    channel = bus.channel(2, 4)
    print("bus", Bus.label, channel.index, channel.read(5))
    text = ", ".join(str(x) for x in samples)
    print("text:", text)
    STATE["total"] = sum(samples)
    return STATE["total"]


result = boot()
print("result", result)
for index, value in enumerate([1, 2, 3]):
    if index == 1:
        continue
    print("step", index, value)
print("done")
