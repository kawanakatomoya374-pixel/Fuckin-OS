class Gate:
    def __init__(self, name):
        self.name = name

    async def __aenter__(self):
        print("enter", self.name)
        return self.name

    async def __aexit__(self, typ, exc, tb):
        print("exit", self.name)
        return False

async def main():
    async with Gate("A") as first, Gate("B") as second:
        print("body", first, second)

import asyncio
asyncio.run(main())
