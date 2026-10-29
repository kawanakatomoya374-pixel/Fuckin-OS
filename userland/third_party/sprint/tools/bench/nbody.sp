import math
func run(steps):
    let x = 0.0
    let y = 1.0
    let vx = 0.5
    let vy = 0.0
    let dt = 0.001
    for i in range(steps):
        let r2 = x * x + y * y
        let r3 = r2 * math.sqrt(r2)
        vx -= x / r3 * dt
        vy -= y / r3 * dt
        x += vx * dt
        y += vy * dt
    return x + y
print(run(10000000))
