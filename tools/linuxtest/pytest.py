import sys
steps = [
 "print('A', sys.version.split()[0], sys.platform, sys.executable)",
 "import math,struct; print('B', sum(range(1000)), math.sqrt(2), struct.pack('<I', 7))",
 "import json; print('C', json.dumps({'a': [1, 2, 3], 'b': 'c-os'}))",
 "import threading,os\nr=[]\nt=threading.Thread(target=lambda: r.append(os.getpid()>0)); t.start(); t.join(); print('D thread', r)",
 "import os; print('E cwd', os.getcwd(), sorted(os.listdir('/linux'))[:3])",
 "import hashlib; print('F sha', hashlib.sha256(b'c-os').hexdigest()[:16])",
 "import os\npid=os.fork()\nif pid==0: os._exit(5)\nprint('G fork', os.waitpid(pid,0)[1]>>8)",
]
for i, s in enumerate(steps):
    print("step", i, flush=True)
    exec(s)
    sys.stdout.flush()
print("PYOK", flush=True)
