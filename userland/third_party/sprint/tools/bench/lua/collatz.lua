local function steps(n) local c = 0 while n ~= 1 do if n % 2 == 0 then n = n // 2 else n = 3*n+1 end c = c + 1 end return c end
local best = 0
for i = 1, 599999 do local s = steps(i) if s > best then best = s end end
print(best)
