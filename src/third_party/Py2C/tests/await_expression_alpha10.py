import asyncio

async def value(n):
    return n

async def main():
    total = 1 + await value(2)
    if await value(1):
        total = total + await value(3)
    print(total, await value(4))

asyncio.run(main())
