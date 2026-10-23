class Gate:
    def __init__(self, name):
        self.name = name

    async def __aenter__(self):
        print("enter", self.name)
        return self.name

    async def __aexit__(self, typ, exc, tb):
        print("exit", self.name, typ, exc)
        return self.name == "A"

async def main():
    async with Gate("A") as first, Gate("B") as second:
        print("body", first, second)
        raise ValueError("boom")

import asyncio
asyncio.run(main())
print("after")
