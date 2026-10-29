local t = {}
for i = 0, 999999 do t["k" .. (i % 5000)] = i end
local n = 0
for i = 0, 1999999 do n = n + t["k" .. (i % 5000)] end
print(n)
