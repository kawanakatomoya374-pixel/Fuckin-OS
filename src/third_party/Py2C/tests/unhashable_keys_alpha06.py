d = {}

try:
    d[[1, 2]] = "value"
    print("FAIL dict assignment")
except TypeError:
    print("OK dict assignment")

try:
    d.get([1], "fallback")
    print("FAIL dict get")
except TypeError:
    print("OK dict get")

try:
    d.pop([1])
    print("FAIL dict pop")
except KeyError:
    print("OK dict pop")

if d.pop([1], "fallback") == "fallback":
    print("OK dict pop default")
else:
    print("FAIL dict pop default")

try:
    d.setdefault([1], "value")
    print("FAIL dict setdefault")
except TypeError:
    print("OK dict setdefault")

try:
    [1] in d
    print("FAIL dict contains")
except TypeError:
    print("OK dict contains")

s = set()
try:
    s.add([1, 2])
    print("FAIL set add")
except TypeError:
    print("OK set add")

try:
    s.remove([1])
    print("FAIL set remove")
except TypeError:
    print("OK set remove")

try:
    s.discard([1])
    print("FAIL set discard")
except TypeError:
    print("OK set discard")

try:
    [1] in s
    print("FAIL set contains")
except TypeError:
    print("OK set contains")

try:
    d[([1],)] = "value"
    print("FAIL nested tuple key")
except TypeError:
    print("OK nested tuple key")

try:
    s.add(([1],))
    print("FAIL nested tuple element")
except TypeError:
    print("OK nested tuple element")

