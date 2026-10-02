trace84 = []

class Scope:
    def __init__(self, name, suppress=False):
        self.name = name
        self.suppress = suppress

    def __enter__(self):
        trace84.append("enter:" + self.name)
        return self.name

    def __exit__(self, exc_type, exc, tb):
        trace84.append("exit:" + self.name)
        return self.suppress

with Scope("outer") as outer, Scope("inner") as inner:
    trace84.append("body:" + outer + ":" + inner)
print("C84", trace84)

trace84 = []
with Scope("outer"), Scope("inner", True):
    trace84.append("body")
    raise ValueError("suppressed")
trace84.append("after")
print("C85", trace84)
