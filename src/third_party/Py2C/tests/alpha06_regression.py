# Python Code to C Alpha0.6 regression corpus
class Box:
    def __init__(self, value=0):
        self.value = value
    def __len__(self):
        return self.value
    def __getitem__(self, index):
        return index + self.value

def aggregate(*values):
    return sum(values)

values = [1, 2, 3, 4]
filtered = [x * 2 for x in values if x % 2 == 0]
record = {"total": aggregate(*values), "text": f"{filtered[0]:02d}"}
try:
    assert len(Box(3)) == 3
    print(record["total"], record["text"])
except Exception as exc:
    print("regression failure", exc)
