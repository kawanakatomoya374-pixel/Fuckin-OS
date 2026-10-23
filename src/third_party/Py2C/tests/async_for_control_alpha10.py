import asyncio

class Counter:
    def __init__(self, limit):
        self.value = 0
        self.limit = limit

    def __aiter__(self):
        return self

    async def __anext__(self):
        if self.value >= self.limit:
            raise StopAsyncIteration()
        result = self.value
        self.value = self.value + 1
        return result

async def main():
    total = 0
    async for item in Counter(6):
        if item == 1:
            continue
        total += item
        if item == 3:
            break
    else:
        total += 100
    print(total)

asyncio.run(main())
