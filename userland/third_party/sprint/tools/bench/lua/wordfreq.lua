local words = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa", "lambda", "mu"}
local seed = 12345
local parts = {}
for i = 1, 200000 do
  seed = (seed * 1103515245 + 12345) % 2147483648
  parts[#parts + 1] = words[(seed // 65536) % 12 + 1] .. tostring(seed % 50)
end
local text = table.concat(parts, " ")
local counts = {}
for r = 1, 5 do
  for w in text:gmatch("[^ ]+") do
    counts[w] = (counts[w] or 0) + 1
  end
end
local keys = {}
for k, v in pairs(counts) do keys[#keys + 1] = k end
table.sort(keys, function(a, b) if counts[a] ~= counts[b] then return counts[a] > counts[b] end return a < b end)
print(#keys, keys[1], counts[keys[1]], keys[#keys], counts[keys[#keys]])
