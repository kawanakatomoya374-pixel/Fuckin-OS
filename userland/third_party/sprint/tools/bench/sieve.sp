func sieve(n):
    let flags = []
    for i in range(n + 1):
        flags.append(true)
    let count = 0
    let i = 2
    while i <= n:
        if flags[i]:
            count += 1
            let j = i * i
            while j <= n:
                flags[j] = false
                j += i
        i += 1
    return count
print(sieve(5000000))
