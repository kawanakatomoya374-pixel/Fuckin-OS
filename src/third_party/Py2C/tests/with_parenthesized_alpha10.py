events = []

class Scope:
    def __init__(self, name):
        self.name = name
    def __enter__(self):
        events.append("enter:" + self.name)
        return self.name
    def __exit__(self, exc_type, exc, tb):
        events.append("exit:" + self.name)
        return False

with (
    Scope("left") as left,
    Scope("right") as right,
):
    events.append(left + ":" + right)

print(events)
