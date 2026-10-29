local byte = string.byte
local function is_digit(c) return c >= 48 and c <= 57 end
local function is_alpha(c) return (c >= 97 and c <= 122) or (c >= 65 and c <= 90) or c == 95 end
local function tokenize(s)
  local n = #s
  local i = 1
  local toks, nums, ids, syms = 0, 0, 0, 0
  while i <= n do
    local c = byte(s, i)
    if c == 32 or c == 10 then i = i + 1
    elseif is_digit(c) then
      local j = i
      while j <= n and is_digit(byte(s, j)) do j = j + 1 end
      nums = nums + 1; toks = toks + 1; i = j
    elseif is_alpha(c) then
      local j = i
      while j <= n and (is_alpha(byte(s, j)) or is_digit(byte(s, j))) do j = j + 1 end
      ids = ids + 1; toks = toks + 1; i = j
    else syms = syms + 1; toks = toks + 1; i = i + 1 end
  end
  return toks, nums, ids, syms
end
local unit = "let x1 = foo(42, bar_baz) + 3 * (y - 17);\nif x1 >= 100 { print(x1) }\n"
local src = string.rep(unit, 20000)
local a, b, c, d
for k = 1, 5 do a, b, c, d = tokenize(src) end
print(a, b, c, d)
