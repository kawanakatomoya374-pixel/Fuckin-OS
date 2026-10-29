local P = {} P.__index = P
local function new(x,y) return setmetatable({x=x,y=y}, P) end
function P:add(o) return new(self.x+o.x, self.y+o.y) end
function P:len2() return self.x*self.x + self.y*self.y end
local p, d, s = new(0,0), new(1,2), 0
for i = 1, 3000000 do p = p:add(d) s = s + p:len2() % 3 end
print(s)
