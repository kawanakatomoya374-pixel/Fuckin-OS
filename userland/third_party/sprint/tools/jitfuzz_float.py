# float-expression fuzzer: python3 tools/jitfuzz_float.py SEED COUNT BIN -- compares the JIT (threshold 1) with --no-jit
import random,sys,subprocess
sd=int(sys.argv[1]); N=int(sys.argv[2]); B=sys.argv[3]
bad=0
for t in range(N):
    r=random.Random(sd*100000+t)
    vs=['a','b','c','d']
    def ex(dep=0):
        k=r.random()
        if dep>3 or k<0.25: return r.choice(vs+['1.5','2.0','0.25','3'])
        op=r.choice(['+','-','*','/','+','*'])
        l,rr=ex(dep+1),ex(dep+1)
        if op=='/': rr='(%s + 1.0 + %s * %s)'%(rr,rr,rr)
        if r.random()<0.15: return 'math.sqrt(%s * %s + 1.0)'%(l,l)
        return '(%s %s %s)'%(l,op,rr)
    body=''
    for _ in range(r.randint(3,7)):
        body+='        %s = %s\n'%(r.choice(vs),ex())
    prog='import math\nfunc f(n):\n    let a = 0.5\n    let b = 1.25\n    let c = 2.0\n    let d = 0.75\n    for i in range(n):\n'+body+'        if a > 1000000.0 or a < -1000000.0:\n            a = 0.5\n        if b > 1000000.0 or b < -1000000.0:\n            b = 1.25\n        if c > 1000000.0 or c < -1000000.0:\n            c = 2.0\n        if d > 1000000.0 or d < -1000000.0:\n            d = 0.75\n    return [a,b,c,d]\nprint(f(300))\n'
    open('p.sp','w').write(prog)
    o1=subprocess.run([B,'run','--no-jit','p.sp'],capture_output=True,text=True)
    import os
    env=dict(os.environ,SPRINT_JIT_THRESHOLD='1')
    o2=subprocess.run([B,'run','p.sp'],capture_output=True,text=True,env=env)
    if o1.stdout!=o2.stdout or o1.returncode!=o2.returncode:
        bad+=1; open('bad%d.sp'%t,'w').write(prog); print('MISMATCH',t,o1.stdout[:80],o2.stdout[:80],o2.stderr[:100])
print('done',N,'bad',bad)
