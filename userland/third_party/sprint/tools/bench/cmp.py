#!/usr/bin/env python3
"""usage: cmp.py OLD NEW [RUNS]  -- min user-CPU seconds over RUNS runs per benchmark (noise-resistant)."""
import glob, os, resource, subprocess, sys
old, new = sys.argv[1], sys.argv[2]
runs = int(sys.argv[3]) if len(sys.argv) > 3 else 5
here = os.path.dirname(os.path.abspath(__file__))

def best(binary, f):
    m = 1e9
    for _ in range(runs):
        r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
        if subprocess.run([binary, "run", f], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
            return None  # the binary cannot run this benchmark (e.g. newer syntax)
        r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
        m = min(m, r1.ru_utime - r0.ru_utime)
    return m

print(f"{'bench':10} {'old':>7} {'new':>7} {'delta':>7}")
tot_o = tot_n = 0
for f in sorted(glob.glob(os.path.join(here, "*.sp"))):
    o, n = best(old, f), best(new, f)
    if o is None or n is None:
        print(f"{os.path.basename(f)[:-3]:10} {'n/a' if o is None else format(o, '7.2f'):>7} {'n/a' if n is None else format(n, '7.2f'):>7}")
        continue
    tot_o += o; tot_n += n
    print(f"{os.path.basename(f)[:-3]:10} {o:7.2f} {n:7.2f} {100*(n-o)/o:+6.0f}%")
print(f"{'total':10} {tot_o:7.2f} {tot_n:7.2f} {100*(tot_n-tot_o)/tot_o:+6.0f}%")
