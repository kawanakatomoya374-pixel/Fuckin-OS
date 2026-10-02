class PairScope:
    def __enter__(self):
        return (4, 9)
    def __exit__(self, exc_type, exc, tb):
        return False

with PairScope() as (left, right):
    print(left + right)

with PairScope() as [first, second]:
    print(second - first)
