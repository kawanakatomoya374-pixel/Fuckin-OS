func is_digit(c):
    return c >= 48 and c <= 57
func is_alpha(c):
    return (c >= 97 and c <= 122) or (c >= 65 and c <= 90) or c == 95
func tokenize(s):
    let n = len(s)
    let i = 0
    let toks = 0
    let nums = 0
    let ids = 0
    let syms = 0
    while i < n:
        let c = s[i]
        if c == 32 or c == 10:
            i += 1
        elif is_digit(c):
            let j = i
            while j < n and is_digit(s[j]):
                j += 1
            nums += 1
            toks += 1
            i = j
        elif is_alpha(c):
            let j = i
            while j < n and (is_alpha(s[j]) or is_digit(s[j])):
                j += 1
            ids += 1
            toks += 1
            i = j
        else:
            syms += 1
            toks += 1
            i += 1
    return [toks, nums, ids, syms]
let unit = "let x1 = foo(42, bar_baz) + 3 * (y - 17);\nif x1 >= 100 { print(x1) }\n"
let src = unit.repeat(20000)
let r = [0, 0, 0, 0]
for k in range(5):
    r = tokenize(src)
print(r[0], r[1], r[2], r[3])
