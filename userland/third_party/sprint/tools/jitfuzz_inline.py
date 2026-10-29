# inlining fuzzer: python3 tools/jitfuzz_inline.py SEED COUNT BIN -- leaf calls with type/overflow perturbations; JIT (threshold 1/2/20) vs --no-jit
import random,sys,subprocess,os
sd=int(sys.argv[1]); N=int(sys.argv[2]); B=sys.argv[3]
bad=0
inl_exits=0
inl_sites=0
for t in range(N):
    r=random.Random(sd*100000+t)
    def ex(vs,dep=0):
        k=r.random()
        if dep>2 or k<0.3: return r.choice(vs+['1','2','3','0.5'])
        op=r.choice(['+','-','*','+','-'])
        return '(%s %s %s)'%(ex(vs,dep+1),op,ex(vs,dep+1))
    funcs=[]
    # leaf functions
    for i in range(r.randint(2,4)):
        kind=r.choice(['arith','neg','inst','set','mix'])
        if kind=='arith': funcs.append(('f%d'%i,2,'func f%d(a, b):\n    return %s\n'%(i,ex(['a','b']))))
        elif kind=='neg': funcs.append(('f%d'%i,2,'func f%d(a, b):\n    let t = -a\n    t = t + b\n    return t * 2\n'%i))
        elif kind=='inst': funcs.append(('f%d'%i,3,'func f%d(p, b):\n    return p.x + p.y * b - b\n'%i))
        elif kind=='set': funcs.append(('f%d'%i,3,'func f%d(p, b):\n    p.x = p.y + b\n    p.y = p.y + 1\n    return p.x\n'%i))
        else: funcs.append(('f%d'%i,2,'func f%d(a, b):\n    let u = a\n    let w = b\n    u = u + w\n    w = u * 2\n    return w - a\n'%i))
    prog='class P:\n    let x = 1\n    let y = 2\n    func init(self, x, y):\n        self.x = x\n        self.y = y\nlet live = P(3, 4)\n'
    for n,a,src in funcs: prog+=src
    prog+='func mk():\n    let n = 0\n    func inc(d):\n        n += d\n        return n\n    return inc\nlet inc = mk()\n'
    names=[n for n,a,s in funcs]+['inc1']
    inst=set(n for n,a,s in funcs if a==3)
    probes=[]
    loop=''

    for _ in range(r.randint(2,5)):
        fn=r.choice(names)
        arg=r.choice(['i','i % 5','i * 2','1','2'])
        pert=r.choice(['i','i % 3','2'])
        if fn=='inc1': call='inc(%s)'%arg
        elif fn in inst: call='%s(live, %s)'%(fn,pert)
        else: call='%s(%s, %s)'%(fn,arg,pert)
        if r.random()<0.12:  # perturbation: a float or string argument at some iteration
            bad_arg=r.choice(['1.5','"q"','nil','4611686018427387904'])
            if fn=='inc1': call2='inc(%s)'%bad_arg
            elif fn in inst: call2='%s(%s, 2)'%(fn,r.choice([bad_arg,'live']))
            else: call2='%s(%s, 2)'%(fn,bad_arg)
            pn='probe%d'%len(probes)
            probes.append('func %s():\n    try:\n        return %s\n    catch e:\n        print(e.kind, e.message)\n        return 0\n'%(pn,call2))
            loop+='    if i == %d:\n        s = s + %s()\n'%(r.randint(50,250),pn)
        if fn!='inc1' and fn not in inst and r.random()<0.5:
            loop+='    if i == %d:\n        s = s + %s(%s, %s)\n'%(r.randint(5,200),fn,r.choice(['i * 4611686018427387904','1.5','i * 0.25','9223372036854775807']),r.choice(['3','i','2.0']))
        if fn=='inc1' and r.random()<0.4:
            loop+='    if i == %d:\n        s = s + inc(%s)\n'%(r.randint(5,200),r.choice(['0.5','9223372036854775807','i * 0.5']))
        loop+='    s = s + %s\n'%call
        loop+='    if s > 100000000 or s < -100000000:\n        s = 1\n'
    prog+=''.join(probes)+'let s = 0\nfor i in range(%d):\n'%r.randint(300,900)+loop
    prog+='print(s, live.x, live.y, inc(0))\n'
    open('p.sp','w').write(prog)
    o1=subprocess.run([B,'run','--no-jit','p.sp'],capture_output=True,text=True)
    env=dict(os.environ,SPRINT_JIT_THRESHOLD=str(r.choice([1,2,20])))
    o2=subprocess.run([B,'run','p.sp'],capture_output=True,text=True,env=env)
    o3=subprocess.run([B,'run','p.sp'],capture_output=True,text=True,env=dict(env,SPRINT_JIT_DEBUG='2'))
    inl_exits+=o3.stderr.count('(inlined callee)')
    inl_sites+=o3.stderr.count('cand=1')
    if o1.stdout!=o2.stdout or o1.stderr!=o2.stderr or o1.returncode!=o2.returncode:
        bad+=1; open('bad%d.sp'%t,'w').write(prog); print('MISMATCH',t,repr(o1.stdout[-80:]),repr(o2.stdout[-80:]),o1.stderr[:100],o2.stderr[:100])
print('done',N,'bad',bad,'inline sites',inl_sites,'exits in inlined code',inl_exits)
