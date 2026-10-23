import asyncio

async def value(n):
    return n

async def main():
    same = (await value(3)) == 3
    truth = (await value(1)) and 7
    print(same, truth)

asyncio.run(main())
