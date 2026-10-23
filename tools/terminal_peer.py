#!/usr/bin/env python3
"""Drives the C-OS Terminal in the guest (see run_terminal_test.sh). Evidence comes from the "[TERMINAL] ..." lines the program writes to the serial log."""
import os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qmp_ui import UI
logp, qmp = sys.argv[1], sys.argv[2]
TYPE = os.path.join(os.path.dirname(__file__), "qmp_type_text.py")
ok = True
def check(n, c):
    global ok; print(("PASS " if c else "FAIL ") + n, flush=True); ok &= bool(c)
def log(): return open(logp, errors="replace").read()
def run(text, enter=True):
    if enter:
        from qmp_ui import type_command
        type_command(logp, qmp, TYPE, text); return
    subprocess.run(["python3", TYPE, text, "--no-enter"], stdout=subprocess.DEVNULL, check=False, env=dict(os.environ, COS_QMP=qmp))
def key(q, n=1):
    ui = UI(qmp)
    try:
        for _ in range(n): ui.key(q)
    finally: ui.s.close()
def wait(pat, t=40):
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, log())
        if m: return m
        time.sleep(1)
    return None
def shot(path):
    ui = UI(qmp)
    try: ui.shot(path)
    finally: ui.s.close()

ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()
shot("/tmp/term_0.ppm")
# Enter submits; Backspace edits the line; Up recalls the history
run("echo abc", enter=False); key("backspace"); run("d", enter=False); key("ret")
check("Enter submits a command and Backspace edits it", wait(r"\[TERMINAL\] cmd: echo abd") is not None)
key("up"); key("ret")
check("Up recalls the previous command", len(re.findall(r"\[TERMINAL\] cmd: echo abd", log())) == 2)
# a program with arguments, its output shown as it is produced
run("tcc -v")
m = wait(r"\[TERMINAL\] out: (tcc version[^\n]*)")
check("a program is started with its arguments and its output arrives (tcc -v)", m is not None)
shot("/tmp/term_1.ppm")
# write a C file with echo > and compile and run it with tcc
run('echo int main(void){ extern int puts(const char*); puts("hello from tcc"); return 3; } > /tmp/h.c')
run("tcc -run /tmp/h.c")
check("tcc -run compiles and runs a program, its output arrives", wait(r"\[TERMINAL\] out: hello from tcc") is not None)
check("the program's exit code is reported", wait(r"\[TERMINAL\] exit: tcc\.c-os 3") is not None)
run("tcc -o /tmp/h.c-os /tmp/h.c")
run("/tmp/h.c-os")
check("a program built by tcc can be started from the terminal and ends with its exit code", wait(r"\[TERMINAL\] exit: h\.c-os 3", 40) is not None and len(re.findall(r"out: hello from tcc", log())) >= 2)
shot("/tmp/term_2.ppm")
# the sandbox: a program that gave up capabilities may signal only its own children (a sleeper started from the terminal is not its child)
run('echo extern void cos_sleep_ms(unsigned long long); int main(void){ for(;;) cos_sleep_ms(500); } > /tmp/sl.c')
run("tcc -run /tmp/sl.c")
time.sleep(8)
pids = re.findall(r"\[TERMINAL\] started: /bin/tcc\.c-os pid=(\d+)", log()); sl = pids[-1]
run('echo extern long cos_kill(int,int); extern long cos_sandbox(unsigned,const char*); extern int atoi(const char*); extern int printf(const char*,...); int main(int c,char**v){ int p=atoi(v[1]); cos_sandbox(11,0); printf("kill1=%ld\\n", cos_kill(p,15)); return 0; } > /tmp/kl.c')
run("tcc -run /tmp/kl.c " + sl)
check("a sandboxed program cannot signal a process that is not its own child", wait(r"\[TERMINAL\] out: kill1=-1") is not None)
check("the kernel says why", wait(r"\[SANDBOX\] kill denied for pid \d+: not its own child", 10) is not None)
run('echo extern long cos_kill(int,int); extern int atoi(const char*); extern int printf(const char*,...); int main(int c,char**v){ printf("kill2=%ld\\n", cos_kill(atoi(v[1]),15)); return 0; } > /tmp/kl2.c')
run("tcc -run /tmp/kl2.c " + sl)
check("an ordinary program still can (single-user system)", wait(r"\[TERMINAL\] out: kill2=0") is not None)
# heap integrity (libcos): a double free and an overflow into the next block's header stop the program with a message
run('echo extern void free(void*); extern void *malloc(unsigned long); extern int printf(const char*,...); int main(void){ void *p = malloc(32); free(p); printf("before\\n"); free(p); printf("not reached\\n"); return 0; } > /tmp/df.c')
run("tcc -run /tmp/df.c")
check("a double free is caught", wait(r"\[TERMINAL\] out: \[HEAP\] double free") is not None and "out: not reached" not in log())
check("... and the program ends with the abort code", wait(r"\[TERMINAL\] exit: tcc\.c-os 134") is not None)
run('echo extern void free(void*); extern void *malloc(unsigned long); extern void *memset(void*,int,unsigned long); extern int printf(const char*,...); int main(void){ char *a = malloc(32); char *b = malloc(32); memset(a, 65, 80); free(b); printf("not reached\\n"); return 0; } > /tmp/ov.c')
run("tcc -run /tmp/ov.c")
check("an overflow into the next block's header is caught at free()", wait(r"\[TERMINAL\] out: \[HEAP\] a heap block header is damaged") is not None)
# stack protector: a program built by cos-cc with a buffer overflow is stopped by the canary
run("ssptest")
check("a stack overflow is caught by the stack protector", wait(r"\[TERMINAL\] out: \[STACK\] stack smashing detected") is not None and "out: not reached" not in log())
check("... and the program ends with the abort code", wait(r"\[TERMINAL\] exit: ssptest\.c-os 134") is not None)
# kernel hardening (the review of the system call layer): each program reports through its exit code, because a program that gave up capabilities
# can no longer write its output file
# 1. opening a file for writing needs the file-write capability (it used to need only read; descriptors 1 and 2 were then writable without it)
run('echo extern long cos_sandbox(unsigned,const char*); extern int cos_open(const char*,unsigned); int main(void){ cos_sandbox(1,0); return cos_open("/tmp/fdcap.txt",2) < 0 ? 71 : 79; } > /tmp/fdcap.c')
run("tcc -run /tmp/fdcap.c")
check("a program with only the read capability cannot open a file for writing", wait(r"\[TERMINAL\] exit: tcc\.c-os 71") is not None and "exit: tcc.c-os 79" not in log())
# 2. the jail: backslashes, dot-space components and .. cannot leave it, an ordinary path inside still works
run('echo extern long cos_sandbox(unsigned,const char*); extern int cos_open(const char*,unsigned); int main(void){ char a[] = "/tmp?..?bin?tcc.c-os"; a[4] = 92; a[7] = 92; a[11] = 92; cos_sandbox(0xFFFFFFFFu, "/tmp"); int ok = cos_open("/tmp/h.c", 1); int x = cos_open(a, 1); int y = cos_open("/tmp/.. /bin/tcc.c-os", 1); int z = cos_open("/tmp/../bin/tcc.c-os", 1); return (ok >= 0 && x < 0 && y < 0 && z < 0) ? 72 : 78; } > /tmp/jail.c')
run("tcc -run /tmp/jail.c")
check("a jailed program cannot leave its jail by backslash, dot-space or .. (and can open files inside)", wait(r"\[TERMINAL\] exit: tcc\.c-os 72") is not None and "exit: tcc.c-os 78" not in log())
# 3. a signal still reaches its handler and returns (the frame is now checked)
run('echo extern int cos_signal(int, void (*)(int)); extern int cos_kill(int,int); extern int cos_getpid(void); static volatile int hit; static void h(int s){ hit = s; } int main(void){ cos_signal(10, h); cos_kill(cos_getpid(), 10); cos_getpid(); return hit == 10 ? 73 : 79; } > /tmp/sig.c')
run("tcc -run /tmp/sig.c")
check("a signal is delivered to its handler and the program continues (checked signal frame)", wait(r"\[TERMINAL\] exit: tcc\.c-os 73") is not None and "exit: tcc.c-os 79" not in log())
sys.exit(0 if ok else 1)
