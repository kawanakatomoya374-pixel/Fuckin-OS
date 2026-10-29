let words = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa", "lambda", "mu"]
let seed = 12345
let parts = []
for i in range(200000):
    seed = (seed * 1103515245 + 12345) % 2147483648
    parts.append(words[(seed // 65536) % 12] .. to_string(seed % 50))
let text = parts.join(" ")
let counts = {}
for r in range(5):
    for w in text.split(" "):
        counts[w] = (counts[w] ?? 0) + 1
let keys = []
for k, v in counts:
    keys.append(k)
keys.sort(func(a, b): counts[a] > counts[b] or (counts[a] == counts[b] and a < b))
print(len(keys), keys[0], counts[keys[0]], keys[len(keys) - 1], counts[keys[len(keys) - 1]])
