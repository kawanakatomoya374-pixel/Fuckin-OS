# typed-array fuzzer: python3 tools/jitfuzz_typed.py SEED COUNT BIN
# Random loops over typed arrays of every element kind (reads, wrapping int stores, f32 rounding, int -> float stores),
# mixed with ordinary arrays and int / float arithmetic. Part way through, the variables are flipped (another kind, a plain
# array, a float into an int array, an out-of-range index) so that guard exits and errors happen. Compares the JIT
# (threshold 1) with --no-jit, including the final contents of every array and the error that stopped the loop.
import random, sys, subprocess, os
sd = int(sys.argv[1]); N = int(sys.argv[2]); B = sys.argv[3]
bad = 0
errs = {}
IK = ['i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64']
FK = ['f32', 'f64']
for t in range(N):
    r = random.Random(sd * 100000 + t)
    kinds = [r.choice(IK) for _ in range(3)] + [r.choice(FK) for _ in range(2)]
    iv = ['I0', 'I1', 'I2']      # int typed arrays
    fv = ['F0', 'F1']            # float typed arrays
    ivars = ['a', 'b', 'c']
    fvars = ['x', 'y']
    def ixe(dep):
        return r.choice(['i % 6', '(i + %d) %% 6' % r.randint(0, 5), 'a % 6', '(b * %d) %% 6' % r.randint(1, 3), str(r.randint(0, 5)), '(i // 60) % 6', 'c % 6'])
    def iex(dep=0):
        k = r.random()
        if dep > 3 or k < 0.2:
            return r.choice(ivars + ['i', '3', '7', '1', 'P[1]', 'P[2]'])
        if k < 0.45: return '%s[%s]' % (r.choice(iv), ixe(dep))
        if k < 0.52: return 'P[%s]' % ixe(dep)
        op = r.choice(['+', '-', '*', '+', '-'])
        return '(%s %s %s)' % (iex(dep + 1), op, iex(dep + 1))
    def fex(dep=0):
        k = r.random()
        if dep > 3 or k < 0.2:
            return r.choice(fvars + ['1.5', '0.25', '2.0', 'Q[0]', 'Q[1]'])
        if k < 0.45: return '%s[%s]' % (r.choice(fv), ixe(dep))
        if k < 0.5: return 'Q[%s]' % ixe(dep)
        if k < 0.58: return '(%s + %s)' % (iex(dep + 1), fex(dep + 1))
        op = r.choice(['+', '-', '*', '+', '*'])
        return '(%s %s %s)' % (fex(dep + 1), op, fex(dep + 1))
    def cond():
        c = r.choice(['<', '<=', '>', '>=', '==', '!='])
        if r.random() < 0.5: return '%s %s %s' % (iex(1), c, iex(1))
        c = r.choice(['<', '<=', '>', '>='])
        return '%s %s %s' % (fex(1), c, fex(1))
    body = ''
    for _ in range(r.randint(4, 9)):
        k = r.random()
        if k < 0.2: body += '        %s = (%s) %% 997\n' % (r.choice(ivars), iex())
        elif k < 0.3: body += '        %s = %s\n' % (r.choice(fvars), fex())
        elif k < 0.45: body += '        %s[%s] = (%s) * %d\n' % (r.choice(iv), ixe(0), iex(), r.choice([1, 3, 1000003, 70001, -5]))
        elif k < 0.55: body += '        %s[%s] = %s\n' % (r.choice(fv), ixe(0), fex())
        elif k < 0.6: body += '        %s[%s] = %s\n' % (r.choice(fv), ixe(0), iex())     # an int into a float array
        elif k < 0.65: body += '        P[%s] = (%s) %% 997\n' % (ixe(0), iex())
        elif k < 0.7: body += '        Q[%s] = %s\n' % (ixe(0), fex())
        elif k < 0.85: body += '        if %s:\n            s += %s\n        else:\n            s -= %s\n' % (cond(), iex(), iex())
        else: body += '        s += %s\n' % iex()
    flips = ['', '        if i == 150:\n            I1 = AL[%d]\n' % r.randint(0, 5),
             '        if i == 150:\n            I0 = P\n', '        if i == 150:\n            F0 = AL[%d]\n' % r.randint(0, 5),
             '        if i == 150:\n            F1 = AL[%d]\n' % r.randint(0, 5), '        if i == 150:\n            I2[1] = 1.5\n',
             '        if i == 150:\n            F0[1] = nil\n', '        if i == 150:\n            a = 9\n',
             '        if i == 150:\n            b = 100\n', '        if i == 150:\n            I0 = AL[%d]\n' % r.randint(0, 5),
             '        if i == 150:\n            x = 2\n', '        if i % 37 == 0:\n            I2 = AL[i % 6]\n']
    flip = r.choice(flips) if r.random() < 0.7 else ''
    decl = ''.join('    let I%d = typed.%s(6)\n' % (j, kinds[j]) for j in range(3)) + ''.join('    let F%d = typed.%s(6)\n' % (j, kinds[3 + j]) for j in range(2))
    prog = ('import typed\n'
            'func body(n, I0, I1, I2, F0, F1, P, Q, R, AL):\n'
            '    let a = 1\n    let b = 2\n    let c = 3\n    let x = 0.5\n    let y = 1.25\n    let s = 0\n'
            '    for i in range(n):\n        R[0] = i\n' + flip + body +
            '        if s > 1000000 or s < -1000000:\n            s = 0\n'
            '        if a > 100000 or a < -100000:\n            a = 1\n'
            '        if b > 100000 or b < -100000:\n            b = 2\n'
            '        if c > 100000 or c < -100000:\n            c = 3\n'
            '        if x > 100000.0 or x < -100000.0:\n            x = 0.5\n'
            '        if y > 100000.0 or y < -100000.0:\n            y = 1.25\n'
            '    R[1] = s\n'
            'func run(n):\n' + decl +
            '    let P = [3, 1, 4, 1, 5, 9]\n    let Q = [0.5, 1.5, 2.5, 0.25, 4.0, 8.0]\n    let R = [0, 0]\n    let AL = [typed.i8(6), typed.u16(6), typed.i64(6), typed.f32(6), typed.u8(6), typed.f64(6)]\n'
            '    for j in range(6):\n        I0[j] = j * 7\n        I1[j] = j - 3\n        I2[j] = 200 - j\n        F0[j] = j * 0.5\n        F1[j] = 1.0 / (j + 1)\n'
            '    let err = ""\n'
            '    try:\n        body(n, I0, I1, I2, F0, F1, P, Q, R, AL)\n    catch e:\n        err = e.kind\n'
            '    let out = [err, R, P, Q]\n'
            '    return out\n'
            'func dump(v):\n    if type(v) == "native":\n        return v.to_array()\n    return v\n'
            'func run2(n):\n'
            + decl +
            '    let P = [3, 1, 4, 1, 5, 9]\n    let Q = [0.5, 1.5, 2.5, 0.25, 4.0, 8.0]\n    let R = [0, 0]\n    let AL = [typed.i8(6), typed.u16(6), typed.i64(6), typed.f32(6), typed.u8(6), typed.f64(6)]\n'
            '    for j in range(6):\n        I0[j] = j * 7\n        I1[j] = j - 3\n        I2[j] = 200 - j\n        F0[j] = j * 0.5\n        F1[j] = 1.0 / (j + 1)\n'
            '    let err = ""\n'
            '    try:\n        body(n, I0, I1, I2, F0, F1, P, Q, R, AL)\n    catch e:\n        err = e.kind\n'
            '    return [err, R, P, Q, I0.to_array(), I1.to_array(), I2.to_array(), F0.to_array(), F1.to_array()]\n'
            'print(run2(300))\n')
    open('pt.sp', 'w').write(prog)
    o1 = subprocess.run([B, 'run', '--no-jit', 'pt.sp'], capture_output=True, text=True)
    env = dict(os.environ, SPRINT_JIT_THRESHOLD='1')
    o2 = subprocess.run([B, 'run', 'pt.sp'], capture_output=True, text=True, env=env)
    ek = o1.stdout[2:o1.stdout.index('"', 2)] if o1.stdout.startswith('["') else 'crash'
    errs[ek] = errs.get(ek, 0) + 1
    if o1.stdout != o2.stdout or o1.returncode != o2.returncode or (o1.returncode != 0 and o1.stderr != o2.stderr):
        bad += 1
        open('badt%d.sp' % t, 'w').write(prog)
        print('MISMATCH', t, o1.stdout[:100], '|', o2.stdout[:100], o2.stderr[:100])
print('done', N, 'bad', bad, 'ends', errs)
