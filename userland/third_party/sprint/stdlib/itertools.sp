# itertools: eager helpers over arrays (written in Sprint, bundled with the interpreter).
# Every function returns a new array/table and never modifies its arguments.

export func enumerate(a, start = 0):
    return [[start + i, a[i]] for i in range(len(a))]

export func zip(a, b, ...more):
    let n = len(a)
    if len(b) < n:
        n = len(b)
    for m in more:
        if len(m) < n:
            n = len(m)
    let out = []
    for i in range(n):
        let row = [a[i], b[i]]
        for m in more:
            row.append(m[i])
        out.append(row)
    return out

export func take(a, n):
    return a.slice(0, n if n < len(a) else len(a))

export func drop(a, n):
    return a.slice(n if n < len(a) else len(a))

export func take_while(a, f):
    let out = []
    for x in a:
        if not f(x):
            break
        out.append(x)
    return out

export func drop_while(a, f):
    let i = 0
    while i < len(a) and f(a[i]):
        i += 1
    return a.slice(i)

export func chunks(a, n):
    if n <= 0:
        throw error("ValueError", "chunks: size must be positive")
    let out = []
    let i = 0
    while i < len(a):
        out.append(a.slice(i, i + n if i + n < len(a) else len(a)))
        i += n
    return out

export func windows(a, n):
    if n <= 0:
        throw error("ValueError", "windows: size must be positive")
    return [a.slice(i, i + n) for i in range(len(a) - n + 1)]

export func pairwise(a):
    return windows(a, 2)

export func flatten(a):
    let out = []
    for x in a:
        out.extend(x)
    return out

export func flat_map(a, f):
    let out = []
    for x in a:
        out.extend(f(x))
    return out

export func chain(...arrays):
    let out = []
    for a in arrays:
        out.extend(a)
    return out

export func unique(a, key = nil):
    let seen = {}
    let out = []
    for x in a:
        let k = x if key == nil else key(x)
        if k not in seen:
            seen[k] = true
            out.append(x)
    return out

export func group_by(a, f):
    let out = {}
    for x in a:
        let k = f(x)
        if k in out:
            out[k].append(x)
        else:
            out[k] = [x]
    return out

export func count_by(a, f):
    let out = {}
    for x in a:
        let k = f(x)
        out[k] = (out[k] ?? 0) + 1
    return out

export func partition(a, f):
    let yes = []
    let no = []
    for x in a:
        if f(x):
            yes.append(x)
        else:
            no.append(x)
    return [yes, no]

export func sum(a, init = 0):
    let s = init
    for x in a:
        s += x
    return s

export func product(a, init = 1):
    let s = init
    for x in a:
        s *= x
    return s

export func min_by(a, f):
    let best = nil
    let bk = nil
    let first = true
    for x in a:
        let k = f(x)
        if first or k < bk:
            best = x
            bk = k
            first = false
    return best

export func max_by(a, f):
    let best = nil
    let bk = nil
    let first = true
    for x in a:
        let k = f(x)
        if first or k > bk:
            best = x
            bk = k
            first = false
    return best

export func sorted(a, cmp = nil):
    let c = a.copy()
    c.sort(cmp)
    return c

export func sort_by(a, f):
    let keyed = [[f(x), x] for x in a]
    keyed.sort((p, q) => p[0] < q[0])
    return keyed.map(p => p[1])

export func first(a, default = nil):
    return a[0] if len(a) > 0 else default

export func last(a, default = nil):
    return a[len(a) - 1] if len(a) > 0 else default

export func repeat(v, n):
    return [v for i in range(n)]

export func reversed(a):
    let c = a.copy()
    c.reverse()
    return c

export func index_by(a, f):
    let out = {}
    for x in a:
        out[f(x)] = x
    return out

export func range_list(a, b = nil, step = 1):
    return [x for x in (range(a) if b == nil else range(a, b, step))]
