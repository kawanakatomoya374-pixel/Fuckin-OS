local parts = {}
for i = 0, 499999 do parts[#parts+1] = "item" .. i end
local s = table.concat(parts, ",")
local c = 0
for w in s:gmatch("[^,]+") do if w:sub(1,5) == "item1" then c = c + 1 end end
print(#s, c)
