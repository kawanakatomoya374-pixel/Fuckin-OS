# register-temporary fuzzer: python3 tools/jitfuzz_reg.py SEED COUNT BIN
# Random loops over array elements (flat and nested), instance fields, int / float arithmetic, comparisons and stores;
# types and values are flipped part way through so guard exits happen; compares the JIT (threshold 1) with --no-jit.
import random, sys, subprocess, os
sd = int(sys.argv[1]); N = int(sys.argv[2]); B = sys.argv[3]
bad = 0
for t in range(N):
    r = random.Random(sd * 100000 + t)
    ivars = ['a', 'b', 'c']
    fvars = ['x', 'y']
    def iex(dep=0):
        k = r.random()
        if dep > 3 or k < 0.2:
            return r.choice(ivars + ['i', '3', '7', '1', 'o.p', 'o.q', 'A[1]', 'A[2]'])
        if k < 0.35: return 'A[%s]' % ixe(dep)
        if k < 0.45: return 'M[%s][%s]' % (ixe(dep), ixe(dep))
        if k < 0.55: return 'o.p' if r.random() < 0.5 else 'o.q'
        op = r.choice(['+', '-', '*', '+', '-'])
        return '(%s %s %s)' % (iex(dep + 1), op, iex(dep + 1))
    def ixe(dep):
        return r.choice(['i % 5', '(i + %d) %% 5' % r.randint(0, 4), 'a % 5', 'b % 5', '(c * %d) %% 5' % r.randint(1, 3), str(r.randint(0, 4)), '(i // 60)', '(i // 59)'])
    def fex(dep=0):
        k = r.random()
        if dep > 3 or k < 0.2:
            return r.choice(fvars + ['1.5', '0.25', '2.0', 'o.f', 'F[0]', 'F[1]'])
        if k < 0.3: return 'F[%s]' % ixe(dep)
        if k < 0.38: return 'o.f' if r.random() < 0.5 else 'o.g'
        if k < 0.45: return '(%s + %s)' % (iex(dep + 1), fex(dep + 1))
        op = r.choice(['+', '-', '*', '/', '+', '*'])
        l, rr = fex(dep + 1), fex(dep + 1)
        if op == '/': rr = '(%s * %s + 1.0)' % (rr, rr)
        return '(%s %s %s)' % (l, op, rr)
    def cond():
        c = r.choice(['<', '<=', '>', '>=', '==', '!='])
        if r.random() < 0.5: return '%s %s %s' % (iex(1), c, iex(1))
        c = r.choice(['<', '<=', '>', '>='])
        return '%s %s %s' % (fex(1), c, fex(1))
    body = ''
    for _ in range(r.randint(4, 9)):
        k = r.random()
        if k < 0.25: body += '        %s = (%s) %% 997\n' % (r.choice(ivars), iex())
        elif k < 0.4: body += '        %s = %s\n' % (r.choice(fvars), fex())
        elif k < 0.5: body += '        A[%s] = (%s) %% 997\n' % (ixe(0), iex())
        elif k < 0.58: body += '        F[%s] = %s\n' % (ixe(0), fex())
        elif k < 0.65: body += '        M[%s][%s] = (%s) %% 997\n' % (ixe(0), ixe(0), iex())
        elif k < 0.72: body += '        o.%s = (%s) %% 997\n' % (r.choice(['p', 'q']), iex())
        elif k < 0.78: body += '        o.%s = %s\n' % (r.choice(['f', 'g']), fex())
        elif k < 0.9: body += '        if %s:\n            s += %s\n        else:\n            s -= %s\n' % (cond(), iex(), iex())
        else: body += '        s += %s\n' % iex()
    flip = r.choice(['', '        if i == 150:\n            o.p = 2.5\n', '        if i == 150:\n            A[1] = 1.5\n', '        if i == 150:\n            F[1] = 3\n', '        if i == 150:\n            M[2][2] = 0.5\n', '        if i == 150:\n            o.f = 2\n'])
    prog = ('class O:\n    let p = 1\n    let q = 2\n    let f = 0.5\n    let g = 1.5\n'
            'func run(n):\n    let o = O()\n    let A = [3, 1, 4, 1, 5]\n    let F = [0.5, 1.5, 2.5, 0.25, 4.0]\n'
            '    let M = [[1, 2, 3, 4, 5], [2, 3, 4, 5, 6], [3, 4, 5, 6, 7], [0, 1, 0, 1, 0], [9, 8, 7, 6, 5]]\n'
            '    let a = 1\n    let b = 2\n    let c = 3\n    let x = 0.5\n    let y = 1.25\n    let s = 0\n'
            '    for i in range(n):\n' + flip + body +
            '        if s > 1000000 or s < -1000000:\n            s = 0\n'
            '        if a > 100000 or a < -100000:\n            a = 1\n'
            '        if b > 100000 or b < -100000:\n            b = 2\n'
            '        if c > 100000 or c < -100000:\n            c = 3\n'
            '        if x > 100000.0 or x < -100000.0:\n            x = 0.5\n'
            '        if y > 100000.0 or y < -100000.0:\n            y = 1.25\n'
            '    return [s, a, b, c, x, y, A, F, M, o.p, o.q, o.f, o.g]\n'
            'try:\n    print(run(300))\ncatch e:\n    print("caught", e.kind)\n')
    open('pr.sp', 'w').write(prog)
    o1 = subprocess.run([B, 'run', '--no-jit', 'pr.sp'], capture_output=True, text=True)
    env = dict(os.environ, SPRINT_JIT_THRESHOLD='1')
    o2 = subprocess.run([B, 'run', 'pr.sp'], capture_output=True, text=True, env=env)
    if o1.stdout != o2.stdout or o1.returncode != o2.returncode:
        bad += 1
        open('badr%d.sp' % t, 'w').write(prog)
        print('MISMATCH', t, o1.stdout[:100], '|', o2.stdout[:100], o2.stderr[:100])
print('done', N, 'bad', bad)
