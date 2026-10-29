# constructor / nested-inlining fuzzer: python3 tools/jitfuzz_ctor.py SEED COUNT BIN
# classes with random init bodies, methods and functions that construct (so the constructor is inlined inside an inlined
# body), perturbed with floats, strings, nil and overflowing arguments; JIT (threshold 1/2/20, optionally GC stress) vs --no-jit.
import random,sys,subprocess,os
sd=int(sys.argv[1]); N=int(sys.argv[2]); B=sys.argv[3]
bad=0; nested=0; inl=0
fails={}
for t in range(N):
    r=random.Random(sd*100000+t)
    def ex(vs,dep=0):
        if dep>2 or r.random()<0.3: return r.choice(vs+['1','2','3','0.5'])
        return '(%s %s %s)'%(ex(vs,dep+1),r.choice(['+','-','*','+','-']),ex(vs,dep+1))
    prog=''
    classes=[]
    for ci in range(r.randint(1,3)):
        nf=r.randint(1,3); na=r.randint(1,3)
        fields=['f%d'%i for i in range(nf)]
        args=['a%d'%i for i in range(na)]
        src='class K%d:\n'%ci+''.join('    let %s = %s\n'%(f,r.choice(['0','0.5','7'])) for f in fields)
        src+='    func init(self, %s):\n'%', '.join(args)
        for f in fields: src+='        self.%s = %s\n'%(f,ex(args) if r.random()<0.7 else r.choice(args))
        # methods: read fields, construct a new one (nested), or plain
        src+='    func sum(self):\n        return %s\n'%' + '.join('self.%s'%f for f in fields)
        src+='    func mk(self, %s):\n        return K%d(%s)\n'%(', '.join(args),ci,', '.join('self.%s + %s'%(fields[i%nf],a) if r.random()<0.25 else a for i,a in enumerate(args)))
        src+='    func grow(self, o):\n        return K%d(%s)\n'%(ci,', '.join(('self.%s + o.%s'%(fields[i%nf],fields[i%nf])) if r.random()<0.7 else ex(['self.%s'%fields[0],'o.%s'%fields[0]]) for i,_ in enumerate(args)))
        classes.append((ci,nf,na,src))
        prog+=src
    prog+=''.join('func mkf%d(%s):\n    return K%d(%s)\n'%(ci,', '.join('a%d'%i for i in range(na)),ci,', '.join(ex(['a%d'%i for i in range(na)]) for i in range(na))) for ci,nf,na,_ in classes)
    ci,nf,na,_=classes[0]
    prog+='let o = K%d(%s)\nlet d = K%d(%s)\nlet s = 0\n'%(ci,', '.join(['1']*na),ci,', '.join(['2']*na))
    loop=''
    probes=''
    for _ in range(r.randint(2,5)):
        c2,nf2,na2,_=r.choice(classes)
        pick=r.random()
        argl=lambda n:', '.join(r.choice(['i','i % 5','1','2','i * 2']) for _ in range(n))
        if pick<0.3 and c2==ci: loop+='    o = o.grow(d)\n    s = s + o.sum()\n'
        elif pick<0.55 and c2==ci: loop+='    o = o.mk(%s)\n    s = s + o.sum() %% 7\n'%argl(na)
        elif pick<0.8: loop+='    s = s + mkf%d(%s).sum()\n'%(c2,argl(na2))
        else: loop+='    s = s + K%d(%s).sum()\n'%(c2,argl(na2))
        if r.random()<0.6:      # a pending operand (local, field load, constant, module variable) below an inlined construction, with
                                # arguments that sometimes change type so a guard inside the constructor fails without an error
            c3,nf3,na3,_=r.choice(classes)
            qs=[]
            for qi in range(na3):
                qn='q%d_%d'%(loop.count('let q'),qi)
                loop+='    let %s = %s\n'%(qn,r.choice(['i','i % 5','i + 1']))
                if r.random()<0.6:
                    loop+='    if i %% %d == %d:\n        %s = %s\n'%(r.randint(5,40),r.randint(0,4),qn,r.choice(['0.5','1.5','i + 0.5']))
                qs.append(qn)
            lhs=r.choice(['i','i','3','i * 3','s','(i % 7)','o.f0' if c3==ci else 'i','d.f0' if c3==ci else 's'])
            rhs=r.choice(['K%d(%s).sum()'%(c3,', '.join(qs)),'mkf%d(%s).sum()'%(c3,', '.join(qs))])
            loop+='    s = s %s (%s %s %s)\n'%(r.choice(['+','-']),lhs,r.choice(['+','-','*']),rhs)
        if r.random()<0.35:
            bad_arg=r.choice(['1.5','"q"','nil','4611686018427387904','9223372036854775807'])
            pn='probe%d'%len(probes.split('func ')) if probes else 'probe0'
            pn='probe%d_%d'%(t,probes.count('func '))
            via=r.choice(['mkf%d(%s)'%(c2,', '.join([bad_arg]+['2']*(na2-1))),'o.mk(%s)'%', '.join([bad_arg]+['2']*(na-1)),'o.grow(%s)'%r.choice(['o','d','K%d(%s)'%(ci,', '.join([bad_arg]+['2']*(na-1)))])])
            probes+='func %s():\n    try:\n        return %s.sum()\n    catch e:\n        print(e.kind, e.message)\n        return 0\n'%(pn,via)
            loop+='    if i %% %d == %d:\n        s = s + %s()\n'%(r.randint(20,90),r.randint(0,15),pn)
        if r.random()<0.6:
            fl=r.choice(['1.5','0.25','2.0'])
            loop+='    if i == %d:\n        o = o.mk(%s)\n'%(r.randint(3,120),', '.join([fl]*na))
        if r.random()<0.3:
            loop+='    if i == %d:\n        d = K%d(%s)\n'%(r.randint(3,120),ci,', '.join([r.choice(['1.5','3','0.5'])]*na))
        loop+='    if i % 7 == 0:\n        tr.append(s)\n'
        loop+='    if s > 100000000 or s < -100000000:\n        s = 1\n'
    prog+=probes
    prog+='let tr = []\n'
    prog+='try:\n    for i in range(%d):\n'%r.randint(200,700)+''.join('    '+l+'\n' for l in loop.split('\n') if l)
    prog+='catch e:\n    print(e.kind, e.message)\nprint(tr)\nprint(s, o.sum())\n'
    open('p.sp','w').write(prog)
    o1=subprocess.run([B,'run','--no-jit','p.sp'],capture_output=True,text=True)
    env=dict(os.environ,SPRINT_JIT_THRESHOLD=str(r.choice([1,2,20])))
    if r.random()<0.3: env['SPRINT_GC_STRESS']='1'
    o2=subprocess.run([B,'run','p.sp'],capture_output=True,text=True,env=env)
    o3=subprocess.run([B,'run','p.sp'],capture_output=True,text=True,env=dict(env,SPRINT_JIT_DEBUG='2'))
    o4=subprocess.run([B,'run','p.sp'],capture_output=True,text=True,env=dict(env,SPRINT_JIT_DEBUG='1'))
    for l in o4.stderr.split('\n'):
        if 'inline failed' in l: fails[l]=fails.get(l,0)+1
    nested+=o3.stderr.count('(nested)'); inl+=o3.stderr.count('(inlined callee)')
    if o1.stdout!=o2.stdout or o1.stderr!=o2.stderr or o1.returncode!=o2.returncode:
        bad+=1; open('bad%d.sp'%t,'w').write(prog); print('MISMATCH',t,repr(o1.stdout[-80:]),repr(o2.stdout[-80:]),o1.stderr[:100],o2.stderr[:100])
print('done',N,'bad',bad,'inlined exits',inl,'nested exits',nested)

for k,v in sorted(fails.items(),key=lambda x:-x[1])[:8]: print(v,k)
