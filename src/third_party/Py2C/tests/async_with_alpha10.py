import asyncio

class Guard:
    def __init__(self, events):
        self.events = events

    async def __aenter__(self):
        self.events.append("enter")
        return 7

    async def __aexit__(self, exc_type, exc, tb):
        self.events.append("exit:" + str(exc))
        return False

async def main():
    events = []
    async with Guard(events) as value:
        events.append("body:" + str(value))
    print(events)

class SuppressingGuard:
    def __init__(self, events):
        self.events = events

    async def __aenter__(self):
        self.events.append("s-enter")

    async def __aexit__(self, exc_type, exc, tb):
        self.events.append("s-exit")
        return True

async def suppressed_main():
    events = []
    async with SuppressingGuard(events):
        events.append("s-body")
        raise ValueError("hidden")
    events.append("s-after")
    print(events)

asyncio.run(main())
asyncio.run(suppressed_main())

failing_events = []

async def failing_main():
    async with Guard(failing_events):
        failing_events.append("f-body")
        raise ValueError("visible")

try:
    asyncio.run(failing_main())
except ValueError as exc:
    print(failing_events)
    print("caught:" + str(exc))

async def note(events, text):
    events.append(text)

class AwaitingGuard:
    def __init__(self, events):
        self.events = events

    async def __aenter__(self):
        await note(self.events, "w-enter")
        return 11

    async def __aexit__(self, exc_type, exc, tb):
        await note(self.events, "w-exit")
        return False

async def awaiting_main():
    events = []
    async with AwaitingGuard(events) as value:
        events.append("w-body:" + str(value))
    print(events)

asyncio.run(awaiting_main())
