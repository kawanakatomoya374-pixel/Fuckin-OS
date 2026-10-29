# boolean-value fuzzer: python3 tools/jitfuzz_bool.py SEED COUNT BIN
# Random functions whose result is a comparison, a negation or an and / or chain used as a VALUE (return, let, argument,
# stored into an array), over ints, floats, bools, nil and strings; the arguments change type part way through the loop so
# that guards fail and mixed-type comparisons raise. Compares the JIT (threshold 1) with --no-jit, results and errors.
import random, sys, subprocess, os
sd = int(sys.argv[1]); N = int(sys.argv[2]); B = sys.argv[3]
bad = 0
ends = {}
for t in range(N):
    r = random.Random(sd * 100000 + t)
    def atom():
        return r.choice(['a', 'b', 'i', 'c', '3', '7', '0', 'x', 'y', 'm', 'k', 'g', '2.5', '0.5', 'p', 'q', 'true', 'false', 'nil', 'A[%d]' % r.randint(0, 3), 'F[%d]' % r.randint(0, 2)])
    def num():
        return r.choice(['a', 'b', 'i', 'c', '3', '7', '0', 'x', 'y', 'm', 'k', 'g', 'm', 'k', 'g', '2.5', '0.5', 'A[%d]' % r.randint(0, 3), 'F[%d]' % r.randint(0, 2)])
    def ex(dep=0, nt=False):
        k = r.random()
        if r.random() < 0.3:
            pool = r.choice([['m', 'k', '3', '7', '0'], ['g', '2.5', '0.5', '1.0']])
            e = '(%s %s %s)' % (r.choice(pool), r.choice(['<', '<=', '>', '>=', '==', '!=']) if pool[0] == 'm' else r.choice(['<', '<=', '>', '>=']), r.choice(pool))
            if nt and r.random() < 0.5: e = '(not %s)' % e
            return '(%s %s %s)' % (e, r.choice(['and', 'or']), ex(dep + 1, nt)) if dep < 2 and r.random() < 0.6 else e
        if r.random() < 0.15:
            X = r.choice(['a', 'b', 'p', 'q', 'x', 'm', 'g', 'nil', 'A[%d]' % r.randint(0, 3), 'F[%d]' % r.randint(0, 2), 'D[%d]' % r.randint(0, 3), 'c'])
            f = r.random()
            if f < 0.4: return '(%s ?? %s)' % (X, ex(dep + 1, nt) if dep < 2 else r.choice(['3', '0.5', 'k']))
            if f < 0.7: return '(%s == nil)' % X
            return '(%s != nil)' % X
        if dep > 3 or k < 0.18:
            return atom()
        if k < 0.5:
            return '(%s %s %s)' % (num(), r.choice(['<', '<=', '>', '>=', '==', '!=']), num())
        if k < 0.58 and nt:
            return '(not %s)' % ex(dep + 1, nt)
        if k < 0.65:
            return '(%s + %s)' % (num(), num())
        return '(%s %s %s)' % (ex(dep + 1, nt), r.choice(['and', 'or']), ex(dep + 1, nt))
    n = r.randint(1, 4)
    fns = ''
    calls = ''
    for j in range(n):
        form = r.random()
        if form < 0.4:
            fns += 'func f%d(a, b, c, x, y, p, q, i, A, F, D):\n    let m = 5\n    let k = m * 3 - 4\n    let g = 1.5 + 1.0\n    return %s\n' % (j, ex())
        elif form < 0.7:
            fns += 'func f%d(a, b, c, x, y, p, q, i, A, F, D):\n    let m = 5\n    let k = m * 3 - 4\n    let g = 1.5 + 1.0\n    let v = %s\n    if %s:\n        v = %s\n    return v\n' % (j, ex(), ex(), ex())
        else:
            fns += 'func f%d(a, b, c, x, y, p, q, i, A, F, D):\n    let m = 5\n    let k = m * 3 - 4\n    let g = 1.5 + 1.0\n    let v = %s\n    let w = %s\n    if v:\n        return w\n    return v\n' % (j, ex(), ex())
        calls += '        out.append(f%d(a, b, c, x, y, p, q, i, A, F, D))\n' % j
        calls += '        if %s:\n            h += %d\n' % (ex(0, True), j + 1)
    flip = r.choice(['', '        if i == 40:\n            D = [1, 2, 3, 4]\n', '        if i == 40:\n            a = nil\n', '        if i == 40:\n            c = nil\n', '        if i == 40:\n            a = 2.5\n', '        if i == 40:\n            p = nil\n', '        if i == 40:\n            b = "s"\n',
                     '        if i == 40:\n            x = 3\n', '        if i == 40:\n            q = 1\n', '        if i == 40:\n            c = true\n'])
    # the loop is written without try inside (try-in-loop blocks compilation); errors surface from run() itself
    loop = ('func run(n):\n    let out = []\n    let h = 0\n    let a = 1\n    let b = 2\n    let c = 3\n    let x = 0.5\n    let y = 1.5\n    let m = 5\n    let k = m * 3 - 4\n    let g = 1.5 + 1.0\n'
            '    let p = true\n    let q = false\n    let A = [3, 1, 4, 1]\n    let F = [0.5, 2.5, 3.0]\n    let D = [nil, 7, nil, false]\n'
            '    for i in range(n):\n        a = (i * 7) % 11\n        b = (i * 5) % 13\n        x = (i % 9) * 0.5\n        p = true\n        q = false\n        if i % 3 != 0:\n            p = false\n        if i % 4 == 1:\n            q = true\n'
            + flip + calls +
            '    return [h, out]\n'
            'let res = nil\ntry:\n    res = run(120)\ncatch e:\n    res = e.kind\nprint(res)\n')
    prog = fns + loop
    open('pb.sp', 'w').write(prog)
    o1 = subprocess.run([B, 'run', '--no-jit', 'pb.sp'], capture_output=True, text=True)
    env = dict(os.environ, SPRINT_JIT_THRESHOLD='1')
    o2 = subprocess.run([B, 'run', 'pb.sp'], capture_output=True, text=True, env=env)
    ends[('err' if not o1.stdout.startswith('[') else 'ok')] = ends.get('err' if not o1.stdout.startswith('[') else 'ok', 0) + 1
    if t % 10 == 0:
        o3 = subprocess.run([B, 'run', 'pb.sp'], capture_output=True, text=True, env=dict(env, SPRINT_JIT_DEBUG='1'))
        for ln in o3.stderr.splitlines():
            if 'failed' in ln and ('function f' in ln or 'function run' not in ln and 'loop run' in ln or 'function run' in ln):
                ends['nc'] = ends.get('nc', 0) + 1
                ends[ln.split('failed: ')[-1]] = ends.get(ln.split('failed: ')[-1], 0) + 1
    if o1.stdout != o2.stdout or o1.returncode != o2.returncode or (o1.returncode != 0 and o1.stderr != o2.stderr):
        bad += 1
        open('badb%d.sp' % t, 'w').write(prog)
        print('MISMATCH', t, o1.stdout[:100], '|', o2.stdout[:100], o2.stderr[:100])
print('done', N, 'bad', bad, 'ends', ends)
