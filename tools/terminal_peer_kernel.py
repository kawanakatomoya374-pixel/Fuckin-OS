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
    from qmp_ui import type_command
    type_command(logp, qmp, TYPE, text)
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
run("echo int main(void){ return 0; } > /tmp/h.c")
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
# 4. a confined program cannot have the file manager launch an executable with full privileges (cos_open_path), nor create unbounded threads
run('echo extern long cos_sandbox(unsigned,const char*); extern int cos_open_path(const char*); static void f(void){ for(;;); } extern long cos_thread_create(void(*)(void*),void*); int main(void){ cos_sandbox(0xFFFFFFDFu, 0); int a = cos_open_path("/bin/tcc.c-os"); int b = cos_open_path("/lib/X.C-OSLL"); return (a < 0 && b < 0) ? 74 : 79; } > /tmp/op.c')
run("tcc -run /tmp/op.c")
check("a sandboxed program cannot launch executables through cos_open_path", wait(r"\[TERMINAL\] exit: tcc\.c-os 74") is not None and "exit: tcc.c-os 79" not in log())
# 5. a confined program cannot start more than 8 children, nor open more than 4 windows
run('echo extern long cos_sandbox(unsigned,const char*); extern int cos_spawn(const char*); int main(void){ int ok = 0, bad = 0; cos_sandbox(0xFFFFFFFFu & ~0x10u, 0); for (int i = 0; i < 12; i++) { if (cos_spawn("/bin/tcc.c-os") > 0) ok++; else bad++; } return (ok == 8 && bad == 4) ? 75 : 79; } > /tmp/kids.c')
run("tcc -run /tmp/kids.c")
check("a confined program is limited to 8 children", wait(r"\[TERMINAL\] exit: tcc\.c-os 75") is not None and "exit: tcc.c-os 79" not in log())
# 6. wrong passwords are locked out: after 5 failures even the right old password is refused
run('echo extern int cos_password_change(const char*,const char*); int main(void){ int i; cos_password_change("x", "newpw1"); for (i = 0; i < 5; i++) cos_password_change("wrong", "zz"); return cos_password_change("newpw1", "newpw2") < 0 ? 76 : 79; } > /tmp/pw.c')
run("tcc -run /tmp/pw.c")
check("password changes are locked out after repeated failures", wait(r"\[TERMINAL\] exit: tcc\.c-os 76") is not None and "exit: tcc.c-os 79" not in log())
# 7. a confined program can message itself but not an unrelated process, and cannot attach shared memory it does not belong to
run('echo extern long cos_sandbox(unsigned,const char*); extern int cos_ipc_send(unsigned long,unsigned long,const void*,unsigned long); extern int cos_getpid(void); extern int cos_shm_attach(unsigned long,void**); int main(void){ void *p; cos_sandbox(0xFFFFFFDFu, 0); int a = cos_ipc_send(9999, 1, "x", 1); int b = cos_ipc_send(cos_getpid(), 1, "x", 1); int c = cos_shm_attach(999, &p); return (a != 0 && b == 0 && c != 0) ? 77 : 80 + (a == 0) + 2 * (b != 0) + 4 * (c == 0); } > /tmp/ipc.c')
run("tcc -run /tmp/ipc.c")
check("a confined program cannot message a process outside its family", wait(r"\[TERMINAL\] exit: tcc\.c-os 77") is not None and "exit: tcc.c-os 79" not in log())
# 8. power and the account are separate capabilities: a program that kept everything except them can read/change settings but cannot restart the machine or change the password
run('echo extern long cos_sandbox(unsigned,const char*); extern int cos_settings_action(int,int); extern int cos_password_change(const char*,const char*); extern int cos_settings_get(void*); int main(void){ char buf[4096]; cos_sandbox(0xFFFFFFFFu & ~0x1000u & ~0x2000u, 0); int a = cos_settings_action(2, 0); int b = cos_settings_action(3, 0); int c = cos_settings_action(1, 0); int d = cos_password_change("x", "y"); int e = cos_settings_get(buf); return (a == -1 && b == -1 && c == -1 && d == -1 && e == 0) ? 78 : 79; } > /tmp/pwr.c')
run("tcc -run /tmp/pwr.c")
check("without the power and account capabilities, restart/shutdown/sleep and a password change are refused (settings still readable)", wait(r"\[TERMINAL\] exit: tcc\.c-os 78") is not None and "exit: tcc.c-os 79" not in log())
# 9. spawn arguments are data, not paths (a jailed program may pass an argument outside its jail), and the argument storage is not shared between calls
run('echo extern long cos_sandbox(unsigned,const char*); extern long cos_spawn_argv(const char*,const char*const*,const char*const*); int main(void){ const char *av[] = { "tcc", "-v", "/etc/os-release", "x/../y", 0 }; cos_sandbox(0xFFFFFFFFu, "/bin"); long pid = cos_spawn_argv("/bin/tcc.c-os", av, 0); return pid > 0 ? 88 : 79; } > /tmp/sp.c')
run("tcc -run /tmp/sp.c")
check("a jailed program can pass arguments that are not inside its jail (arguments are not paths)", wait(r"\[TERMINAL\] exit: tcc\.c-os 88") is not None and "exit: tcc.c-os 79" not in log())
# 10. shared-memory ids are random 64-bit handles, not a counter
run('echo extern int cos_shm_create(unsigned long,unsigned long*); extern int cos_shm_attach(unsigned long,void**); int main(void){ unsigned long a = 0, b = 0, c = 0; void *p = 0; int ra = cos_shm_create(4096, &a), rb = cos_shm_create(4096, &b), rc = cos_shm_create(4096, &c); '
    'int at = cos_shm_attach(a, &p); int wild = cos_shm_attach(1, &p); return (ra == 0 && rb == 0 && rc == 0 && a > 0xFFFFFFFFul && b > 0xFFFFFFFFul && c > 0xFFFFFFFFul && a != b && b != c && b != a + 1 && c != b + 1 && at == 0 && wild != 0) ? 97 : 99; } > /tmp/shm.c')
run("tcc -run /tmp/shm.c")
check("shared-memory ids are random 64-bit numbers (not 1, 2, 3 ...), and a guessed small id attaches nothing", wait(r"\[TERMINAL\] exit: tcc\.c-os (97|99)") is not None and "exit: tcc.c-os 97" in log())
sys.exit(0 if ok else 1)
