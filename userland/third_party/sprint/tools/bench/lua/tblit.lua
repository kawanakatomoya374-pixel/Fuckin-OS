local t = {}
for i = 0, 199999 do t["key" .. i] = i end
local s = 0
for r = 1, 10 do
  for k, v in pairs(t) do s = s + v end
  for k in pairs(t) do s = s + #k end
end
print(s)
