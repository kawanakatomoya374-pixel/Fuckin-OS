local function counter() local n = 0 return function() n = n + 1 return n end end
local f = counter()
local r = 0
for i = 1, 20000000 do r = f() end
print(r)
