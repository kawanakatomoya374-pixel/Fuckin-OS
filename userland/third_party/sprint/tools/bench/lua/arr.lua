local a = {}
for i = 0, 2999999 do a[#a+1] = i*2 end
local t = 0
for i = 1, #a do t = t + a[i] end
for _, x in ipairs(a) do t = t + x end
print(t)
