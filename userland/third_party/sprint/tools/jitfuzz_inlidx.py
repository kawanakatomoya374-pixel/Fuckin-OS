# index-in-inlined-method fuzzer: python3 tools/jitfuzz_inlidx.py SEED COUNT BIN
# Methods that read and write array / typed-array elements (through a field or an argument) are called from a hot loop with
# operands (locals, constants, field reads, earlier element reads) still pending below the call. Part way through the
# container is replaced (another typed kind, an ordinary array, a float into an int array, an out-of-range index) so guards
# fail inside the inlined body. Compares the JIT (threshold 1) with --no-jit, including the final state and the error.
import random, sys, subprocess, os
sd = int(sys.argv[1]); N = int(sys.argv[2]); B = sys.argv[3]
bad = 0
ends = {}
KINDS = ['i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'f32', 'f64']
for t in range(N):
    r = random.Random(sd * 100000 + t)
    k0 = r.choice(KINDS)
    isf0 = k0 in ('f32', 'f64')
    def ixe():
        return r.choice(['i % 6', '(i + %d) %% 6' % r.randint(0, 5), 'a % 6', str(r.randint(0, 5)), '(i // 50) % 6'])
    def pend(dep=0):
        k = r.random()
        if dep > 2 or k < 0.3: return r.choice(['a', 'b', 'i', '3', '7', 'o.n', 'P[1]'])
        if k < 0.5: return 'o.get(%s)' % ixe()
        if k < 0.6: return 'G.get(%s)' % ixe()
        return '(%s %s %s)' % (pend(dep + 1), r.choice(['+', '-', '*']), pend(dep + 1))
    body = ''
    for _ in range(r.randint(3, 7)):
        k = r.random()
        if k < 0.25: body += '        s += %s + %s\n' % (pend(), pend())
        elif k < 0.4: body += '        s += %s - o.get(%s)\n' % (pend(), ixe())
        elif k < 0.55: body += '        o.put(%s, (%s) %% 1000)\n' % (ixe(), pend())
        elif k < 0.65: body += '        s += %s + o.addat(%s, %s)\n' % (pend(), ixe(), r.choice(['1', 'a', '2.5' if isf0 else '3']))
        elif k < 0.75: body += '        s += %s + put2(P, %s, %s)\n' % (pend(), ixe(), pend())
        elif k < 0.85: body += '        a = (%s + o.get(%s)) %% 997\n' % (pend(), ixe())
        elif k < 0.92: body += '        G.put(%s, (%s) %% 100)\n' % (ixe(), pend())
        else: body += '        b = %s %% 13\n' % pend()
    flips = ['', '        if i == 120:\n            o.d = ALT[%d]\n' % r.randint(0, 5), '        if i == 120:\n            o.d = P\n',
             '        if i == 120:\n            o.put(1, 1.5)\n', '        if i == 120:\n            o.put(9, 1)\n',
             '        if i == 120:\n            G = ALT[%d]\n' % r.randint(0, 5), '        if i == 120:\n            a = 4.5\n',
             '        if i == 120:\n            o.n = 2.5\n', '        if i % 41 == 0:\n            o.d = ALT[i % 6]\n']
    flip = r.choice(flips) if r.random() < 0.75 else ''
    prog = ('import typed\n'
            'class Box:\n    let d = nil\n    let n = 1\n'
            '    func init(self, d):\n        self.d = d\n'
            '    func get(self, i):\n        return self.d[i]\n'
            '    func put(self, i, v):\n        self.d[i] = v\n'
            '    func addat(self, i, v):\n        self.d[i] = self.d[i] + v\n        return self.d[i]\n'
            'func put2(arr, i, v):\n    arr[i] = v\n    return arr[i]\n'
            'class Gx:\n    let d = nil\n    func init(self, d):\n        self.d = d\n'
            '    func get(self, i):\n        return self.d[i]\n    func put(self, i, v):\n        self.d[i] = v\n'
            'func body(n, o, G, P, ALT, R):\n'
            '    let a = 1\n    let b = 2\n    let s = 0\n'
            '    for i in range(n):\n        R[0] = i\n' + flip + body +
            '        if s > 100000 or s < -100000:\n            s = 0\n'
            '        if a > 1000 or a < -1000:\n            a = 1\n'
            '        if b > 1000 or b < -1000:\n            b = 2\n'
            '    R[1] = s\n'
            'func run(n):\n'
            '    let P = [3, 1, 4, 1, 5, 9]\n    let R = [0, 0]\n'
            '    let ALT = [typed.i8(6), typed.u16(6), typed.i64(6), typed.f32(6), typed.u8(6), typed.f64(6)]\n'
            '    let D = typed.@K0@(6)\n    let E = typed.i32(6)\n'
            '    for j in range(6):\n        D[j] = j * 3\n        E[j] = j - 2\n'
            '    let o = Box(D)\n    let G = Gx(E)\n'
            '    let err = ""\n'
            '    try:\n        body(n, o, G, P, ALT, R)\n    catch e:\n        err = e.kind\n'
            '    let dd = o.d\n    let ee = G.d\n'
            '    if type(dd) == "native":\n        dd = dd.to_array()\n'
            '    if type(ee) == "native":\n        ee = ee.to_array()\n'
            '    return [err, R, P, dd, ee, o.n]\n'
            'print(run(300))\n').replace('@K0@', k0)
    open('pi.sp', 'w').write(prog)
    o1 = subprocess.run([B, 'run', '--no-jit', 'pi.sp'], capture_output=True, text=True)
    env = dict(os.environ, SPRINT_JIT_THRESHOLD='1')
    o2 = subprocess.run([B, 'run', 'pi.sp'], capture_output=True, text=True, env=env)
    ek = o1.stdout[2:o1.stdout.index('"', 2)] if o1.stdout.startswith('["') else 'crash'
    ends[ek] = ends.get(ek, 0) + 1
    if ek == 'NameError': open('name%d.sp' % t, 'w').write(prog)
    if o1.stdout != o2.stdout or o1.returncode != o2.returncode or (o1.returncode != 0 and o1.stderr != o2.stderr):
        bad += 1
        open('badi%d.sp' % t, 'w').write(prog)
        print('MISMATCH', t, o1.stdout[:100], '|', o2.stdout[:100], o2.stderr[:100])
print('done', N, 'bad', bad, 'ends', ends)
