class Collector:
    def pack(self, first, *items, flag=False, **options):
        return (first, items, flag, options)

obj = Collector()
print(obj.pack(1, 2, 3))
print(obj.pack(1, 2, flag=True, color="blue", size=4))

options = {"flag": True, "source": "mapping"}
print(obj.pack(7, 8, **options))
