def count():
    yield 7
    yield 9

iterator = count()
print(next(iterator))
print(next(iterator))

for item in count():
    print(item)

import asyncio

async def child():
    return 17

async def parent():
    value = await child()
    return value + 5

print(asyncio.run(parent()))
