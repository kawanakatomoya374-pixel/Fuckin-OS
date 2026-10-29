local function run(steps)
  local x, y, vx, vy, dt = 0.0, 1.0, 0.5, 0.0, 0.001
  for i = 1, steps do
    local r2 = x*x + y*y
    local r3 = r2 * math.sqrt(r2)
    vx = vx - x / r3 * dt
    vy = vy - y / r3 * dt
    x = x + vx * dt
    y = y + vy * dt
  end
  return x + y
end
print(run(10000000))
