local function make(n) local m = {} for i=0,n-1 do local r = {} for j=0,n-1 do r[#r+1] = (i*j)%7+1 end m[#m+1]=r end return m end
local function mul(a,b,n) local c = {} for i=1,n do local row = {} for j=1,n do local s=0 for k=1,n do s = s + a[i][k]*b[k][j] end row[#row+1]=s end c[#c+1]=row end return c end
local n = 200
local c = mul(make(n), make(n), n)
print(c[11][11], c[200][200])
