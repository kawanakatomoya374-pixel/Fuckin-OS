import typed
func sieve(n):
    let flags = typed.u8(n + 1)
    let count = 0
    let i = 2
    while i <= n:
        if flags[i] == 0:
            count += 1
            let j = i * i
            while j <= n:
                flags[j] = 1
                j += i
        i += 1
    return count
print(sieve(5000000))
