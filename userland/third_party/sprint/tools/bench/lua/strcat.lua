local s = ""
for i = 1, 200000 do s = s .. "x" end
local n = 0
for i = 1, #s do if s:byte(i) == 120 then n = n + 1 end end
local parts = {}
for i = 0, 299999 do parts[#parts+1] = i .. ":" .. (i*2) end
print(n, #table.concat(parts, ";"))
