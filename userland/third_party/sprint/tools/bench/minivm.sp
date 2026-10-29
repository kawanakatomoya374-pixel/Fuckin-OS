# a tiny stack machine: dispatch by if-chain over an opcode array, the shape of many interpreters written in scripting languages
func run(code, n):
    let stack = [0, 0, 0, 0, 0, 0, 0, 0]
    let sp = 0
    let pc = 0
    let acc = 0
    let steps = 0
    while steps < n:
        let op = code[pc]
        if op == 0:
            stack[sp] = code[pc + 1]
            sp += 1
            pc += 2
        elif op == 1:
            sp -= 1
            acc += stack[sp]
            pc += 1
        elif op == 2:
            stack[sp - 1] = stack[sp - 1] * 3 + 1
            pc += 1
        elif op == 3:
            stack[sp] = stack[sp - 1]
            sp += 1
            pc += 1
        elif op == 4:
            pc = 0
        else:
            pc += 1
        steps += 1
    return acc
let code = [0, 7, 3, 2, 1, 1, 0, 5, 2, 1, 4]
print(run(code, 6000000))
