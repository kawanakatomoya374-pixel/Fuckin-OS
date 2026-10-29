local function sieve(n)
  local flags = {}
  for i = 0, n do flags[i] = true end
  local count = 0
  local i = 2
  while i <= n do
    if flags[i] then
      count = count + 1
      local j = i * i
      while j <= n do flags[j] = false; j = j + i end
    end
    i = i + 1
  end
  return count
end
print(sieve(5000000))
