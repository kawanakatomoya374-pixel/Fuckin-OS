math.randomseed(7)
local a = {}
for i = 1, 300000 do a[i] = math.random(0, 1000000) end
table.sort(a)
local b = {}
for i = 1, #a do local x = a[i]*2 if x % 3 == 0 then b[#b+1] = x end end
local s = 0
for i = 1, #b do s = s + b[i] end
table.sort(a, function(x,y) return x > y end)
print(a[1], #b, s)
