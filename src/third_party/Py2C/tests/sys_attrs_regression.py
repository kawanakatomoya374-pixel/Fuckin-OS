import sys
print(sys.version_info[0] >= 3, sys.version_info[0])
print(sys.maxsize > 10 ** 9)
print(sys.byteorder)
print(isinstance(sys.path, list))
print(sys.platform in ("linux", "darwin", "win32", "unknown"))
