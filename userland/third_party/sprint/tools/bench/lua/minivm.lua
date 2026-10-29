local function run(code, n)
  local stack = {0, 0, 0, 0, 0, 0, 0, 0, 0}
  local sp = 1
  local pc = 1
  local acc = 0
  local steps = 0
  while steps < n do
    local op = code[pc]
    if op == 0 then stack[sp] = code[pc + 1]; sp = sp + 1; pc = pc + 2
    elseif op == 1 then sp = sp - 1; acc = acc + stack[sp]; pc = pc + 1
    elseif op == 2 then stack[sp - 1] = stack[sp - 1] * 3 + 1; pc = pc + 1
    elseif op == 3 then stack[sp] = stack[sp - 1]; sp = sp + 1; pc = pc + 1
    elseif op == 4 then pc = 1
    else pc = pc + 1 end
    steps = steps + 1
  end
  return acc
end
local code = {0, 7, 3, 2, 1, 1, 0, 5, 2, 1, 4}
print(run(code, 6000000))
