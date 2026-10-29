local n, parts = 0, {}
for i = 0, 199999 do
  local s = string.format("item-%d-%d", i, i*2)
  n = n + #s
  if s:find("99", 1, true) then parts[#parts+1] = s:upper() end
  local u = s:gsub("item", "x"):sub(1,3)
  n = n + #u
end
print(n, #parts)
