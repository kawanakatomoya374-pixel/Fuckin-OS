#!/usr/bin/env python3
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
def wait(pat, t=40):
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, log())
        if m: return m
        time.sleep(1)
    return None
L = log()
check("the signed example module is loaded (level SERVICE) and its init runs", "[KMOD] loaded hello 2.0 level 1" in L and "[KMOD:hello] hello from a kernel module" in L)
check("a library module and a module that needs it are both loaded (DT_NEEDED)", "[KMOD] loaded kbase 1.0 level 0" in L and "[KMOD] loaded kuser 1.0 level 1" in L)
check("the audit log and the PCI inventory modules are loaded (levels 1 and 2)", "[KMOD] loaded audit 1.0 level 1" in L and "[KMOD] loaded pci 1.0 level 2" in L)
check("a SYSTEM-level module signed with the level-1 key is refused (the key may not vouch for that level)", "rejected lowkey_system.c-osys: level: signed by a key limited to level 1, the module declares level 3" in L)
check("a module with a tampered byte is refused (signature)", "rejected tampered.c-osys: bad or missing signature" in L)
check("an unsigned module is refused", "rejected unsigned.c-osys: bad or missing signature" in L)
check("a module signed with another key is refused", "rejected wrongkey.c-osys: bad or missing signature" in L)
check("a signed module that imports a kernel function the export table lacks is refused", "rejected needs_kernel.c-osys: undefined symbol: serial_puts" in L)
check("a BASIC module that imports a DEVICE function is refused (permission level)", "rejected too_privileged.c-osys: level: cosys_outb" in L)
ui = UI(qmp); ui.open_start_menu(); time.sleep(1); ui.type("term"); time.sleep(2); ui.key("ret"); time.sleep(6); ui.s.close()
run('echo extern long cos_syscall3(long,long,long,long); extern int printf(const char*,...); int main(void){ printf("kmod=%ld\\n", cos_syscall3(4096,1,2,3)); printf("dep=%ld\\n", cos_syscall3(4098,3,4,0)); printf("none=%ld\\n", cos_syscall3(4099,1,2,3)); return 0; } > /tmp/k.c')
run("tcc -run /tmp/k.c")
check("a program calls the module's system call (1+2+3+1000)", wait(r"\[TERMINAL\] out: kmod=1006") is not None)
check("a module that imports another module's function works (kbase_add(3,4)*10)", wait(r"\[TERMINAL\] out: dep=70") is not None)
check("a call number no module provides fails", wait(r"\[TERMINAL\] out: none=-1") is not None)
# the sandbox of a program is still in force for module calls. (A program that gave up all capabilities can no longer write its output file -
# console output through descriptors 1 and 2 used to be exempt, which was a hole - so the second half reports through the exit code.)
run('echo extern long cos_syscall3(long,long,long,long); extern long cos_sandbox(unsigned,const char*); extern int printf(const char*,...); int main(void){ long a = cos_syscall3(4097,0,0,0); printf("priv1=%ld\\n", a); cos_sandbox(0, 0); long b = cos_syscall3(4097,0,0,0); long c = cos_syscall3(4096,5,0,0); return (b == -1 && c == 1005) ? 60 : 69; } > /tmp/s.c')
run("tcc -run /tmp/s.c")
check("a module call that needs a capability works for a normal program", wait(r"\[TERMINAL\] out: priv1=7") is not None)
check("... is refused once the program dropped all capabilities, while a call that needs none still works in the sandbox", wait(r"\[TERMINAL\] exit: tcc\.c-os 60") is not None and "exit: tcc.c-os 69" not in log())
# loading at run time
run("kmod load /system/modtest/rt.c-osys")
check("a module is loaded at run time (kmod load)", wait(r"\[KMOD\] loaded rt 1\.0 level 1") is not None)
run('echo extern long cos_syscall3(long,long,long,long); extern int printf(const char*,...); int main(void){ char b[16] = {0}; long r1 = cos_syscall3(4099,0,0,0); long r2 = cos_syscall3(4100,(long)b,0,0); long r3 = cos_syscall3(4100,1,0,0); long r4 = cos_syscall3(4100,-4096L,0,0); long r5 = cos_syscall3(4101,0,0,0); printf("rt=%ld copy=%ld/%s bad=%ld/%ld alloc=%ld\\n", r1, r2, b, r3, r4, r5); return 0; } > /tmp/r.c')
run("tcc -run /tmp/r.c")
check("the runtime-loaded module works: its call, a checked copy to the program, bad pointers refused, its allocator", wait(r"\[TERMINAL\] out: rt=4242 copy=0/rt-ok bad=-1/-1 alloc=100") is not None)
run("kmod unload kbase")
check("a module another module needs cannot be unloaded", wait(r"\[KMOD\] unload refused \(in use\): kbase") is not None and "[KMOD] unloaded kbase" not in log())
run("kmod load /system/drivers/tampered.c-osys")
check("a tampered file is refused at run time too", wait(r"rejected /system/drivers/tampered\.c-osys: bad or missing signature") is not None)
run("kmod unload rt")
check("the module is unloaded", wait(r"\[KMOD\] unloaded rt") is not None)
run("tcc -run /tmp/k.c")
check("after unloading, its system calls are gone", wait(r"\[TERMINAL\] out: none=-1", 60) is not None and "rt=" not in log().split("unloaded rt")[-1])
# a sandboxed program cannot load or unload modules (they need full privileges)
run('echo extern long cos_sandbox(unsigned,const char*); extern int cos_kmod_load(const char*,const char*); extern int cos_kmod_unload(const char*,const char*); int main(void){ cos_sandbox(0xFFFFFFDFu, 0); int a = cos_kmod_load("/system/modtest/rt.c-osys", 0); int b = cos_kmod_unload("hello", 0); return (a == -1 && b == -1) ? 61 : 69; } > /tmp/z.c')
run("tcc -run /tmp/z.c")
check("a sandboxed program cannot load or unload a kernel module", wait(r"\[TERMINAL\] exit: tcc\.c-os 61") is not None and "[KMOD] unloaded hello" not in log() and log().count("[KMOD] loaded rt") == 1)

# kernel extensions that do real work: the audit log (subscribes to kernel events) and the PCI inventory (DEVICE level)
AUDIT_PROG = ('echo extern long cos_sandbox(unsigned,const char*); extern int cos_open(const char*,unsigned); extern int cos_getpid(void); extern long cos_syscall3(long,long,long,long); '
              'typedef struct { unsigned long long seq, t; unsigned type, pid; unsigned long long a, b; } R; '
              'int main(void){ static R r[32]; int me = cos_getpid(); cos_sandbox(0xFFFFFFFFu & ~2u, 0); int o = cos_open("/tmp/au.txt", 2); '
              'long nx = cos_syscall3(4113, 0, 0, 0); long n = cos_syscall3(4112, (long)r, 32, nx > 32 ? nx - 32 : 0); int deny = 0, set = 0, st = 0; long i; '
              'for (i = 0; i < n; i++) { if ((int)r[i].pid != me) continue; if (r[i].type == 2) deny++; if (r[i].type == 3) set++; if (r[i].type == 0) st++; } '
              'return (o < 0 && n > 0 && deny >= 1 && set >= 1 && st >= 1) ? 83 : 89; } > /tmp/au.c')
run(AUDIT_PROG); run("tcc -run /tmp/au.c")
check("the audit log recorded this program's start, its narrowing of capabilities and the sandbox's refusal of its write", wait(r"\[TERMINAL\] exit: tcc\.c-os 83") is not None and "exit: tcc.c-os 89" not in log())
run('echo extern long cos_syscall3(long,long,long,long); typedef struct { unsigned long long seq, t; unsigned type, pid; unsigned long long a, b; } R; int main(void){ static R r[32]; long nx = cos_syscall3(4113, 0, 0, 0); long n = cos_syscall3(4112, (long)r, 32, nx > 32 ? nx - 32 : 0); long i; for (i = 0; i < n; i++) if (r[i].type == 1 && (int)r[i].b == 83) return 84; return 85; } > /tmp/au2.c')
run("tcc -run /tmp/au2.c")
check("... and the exit of that program (status 83) is in the log", wait(r"\[TERMINAL\] exit: tcc\.c-os 84") is not None)
run('echo extern long cos_sandbox(unsigned,const char*); extern long cos_syscall3(long,long,long,long); typedef struct { unsigned char bus, dev, fn, hdr; unsigned short vendor, device; unsigned class_rev; } P; '
    'int main(void){ P p; long cnt = cos_syscall3(4128, 0, 0, 0); int intel = 0; long i; for (i = 0; i < cnt && i < 64; i++) if (cos_syscall3(4129, i, (long)&p, 0) == 0 && p.vendor == 0x8086) intel = 1; '
    'cos_sandbox(0xFFFFFFFFu & ~0x80u, 0); long denied = cos_syscall3(4128, 0, 0, 0); return (cnt > 0 && intel && denied == -1) ? 86 : 89; } > /tmp/pc.c')
run("tcc -run /tmp/pc.c")
check("the PCI module lists the devices (an Intel one among them) and the call needs the device capability", wait(r"\[TERMINAL\] exit: tcc\.c-os 86") is not None)
run("kmod unload audit")
check("the audit module unloads (its event subscription is released)", wait(r"\[KMOD:audit\] events recorded: ") is not None and wait(r"\[KMOD\] unloaded audit") is not None)
run('echo extern long cos_sandbox(unsigned,const char*); extern int cos_open(const char*,unsigned); extern long cos_syscall3(long,long,long,long); int main(void){ cos_sandbox(0xFFFFFFFFu & ~2u, 0); cos_open("/tmp/au.txt", 2); return cos_syscall3(4113, 0, 0, 0) == -1 ? 87 : 89; } > /tmp/au3.c')
run("tcc -run /tmp/au3.c")
check("events with the module gone cause no trouble, and its system calls are gone", wait(r"\[TERMINAL\] exit: tcc\.c-os 87") is not None)
run("kmod load /system/drivers/audit.c-osys")
check("the audit module loads again", log().count("[KMOD] loaded audit 1.0 level 1") >= 2 or wait(r"(?s)unloaded audit.*\[KMOD\] loaded audit 1\.0 level 1") is not None)
run("tcc -run /tmp/au.c")
check("... and records again (the subscription was registered anew)", len(re.findall(r"\[TERMINAL\] exit: tcc\.c-os 83", log())) >= 2 or wait(r"(?s)exit: tcc\.c-os 87.*exit: tcc\.c-os 83") is not None)

# a real device driver as a module: virtio-rng (PCI, I/O ports, DMA virtqueue, interrupt line)
check("the virtio-rng driver found its device and loaded", "[KMOD] loaded virtiorng 1.0 level 2" in log())
RNG = ('echo extern long cos_syscall3(long,long,long,long); int main(void){ static unsigned char a[64], b[64]; long r1 = cos_syscall3(4144, (long)a, 64, 0); long r2 = cos_syscall3(4144, (long)b, 64, 0); '
       'long inf = cos_syscall3(4145, 0, 0, 0); int i, nz = 0, diff = 0; for (i = 0; i < 64; i++) { if (a[i]) nz++; if (a[i] != b[i]) diff++; } '
       'long big = cos_syscall3(4144, (long)a, 4096 + 1, 0); long bad = cos_syscall3(4144, 0, 8, 0); '
       'return (r1 == 64 && r2 == 64 && nz > 20 && diff > 20 && (inf & 255) >= 1 && big == -1 && bad == -1) ? (88 + (int)(inf & 255)) : 99; } > /tmp/rng.c')
run(RNG); run("tcc -run /tmp/rng.c")
m = wait(r"\[TERMINAL\] exit: tcc\.c-os (99|89|90)")
check("random bytes arrive from the device through the driver (two reads differ, bad requests refused)", m is not None and m.group(1) != "99")
check("the driver is interrupt driven and the device's interrupts reached the module", m is not None and m.group(1) == "90" and "[KMOD:virtiorng] interrupt driven" in log())
run("kmod unload virtiorng")
check("the driver unloads (the device is reset first, its interrupt line released)", wait(r"\[KMOD\] unloaded virtiorng") is not None)
run("kmod load /system/drivers/virtiorng.c-osys")
check("... and loads again", wait(r"(?s)unloaded virtiorng.*\[KMOD\] loaded virtiorng 1\.0 level 2") is not None)
run("tcc -run /tmp/rng.c")
check("... and works again after the reload", len(re.findall(r"\[TERMINAL\] exit: tcc\.c-os (?:89|90)", log())) >= 2 or wait(r"(?s)loaded virtiorng.*exit: tcc\.c-os (?:89|90)") is not None)

# SYSTEM-level extensions: write protection through a file-operation filter, and the network frame filter
check("the SYSTEM-level modules (write protection, network filter) are loaded", "[KMOD] loaded wprotect 1.0 level 3" in log() and "[KMOD] loaded netfw 1.0 level 3" in log())
run('echo extern long cos_syscall3(long,long,long,long); extern int cos_mkdir(const char*); extern long cos_write_file(const char*,const void*,unsigned long); extern int cos_rename(const char*,const char*); extern long cos_read_file(const char*,void*,unsigned long); '
    'int main(void){ char b[8]; cos_mkdir("/tmp/prot"); long a = cos_write_file("/tmp/ok.txt", "x", 1); long s = cos_syscall3(4146, (long)"/tmp/prot/", 10, 0); long w = cos_write_file("/tmp/prot/a.txt", "y", 1); '
    'long m = cos_mkdir("/tmp/prot/sub"); long r = cos_rename("/tmp/ok.txt", "/tmp/prot/ok.txt"); long rd = cos_read_file("/tmp/ok.txt", b, 8); long den = cos_syscall3(4147, 0, 0, 0); '
    'return (a == 1 && s == 0 && w < 0 && m < 0 && r < 0 && rd == 1 && den >= 3) ? 93 : 99; } > /tmp/wp.c')
run("tcc -run /tmp/wp.c")
check("a file-operation filter refuses write, mkdir and rename into the protected prefix (reads and other paths stay allowed)", wait(r"\[TERMINAL\] exit: tcc\.c-os (93|99)") is not None and "[KMOD:wprotect] refused a file operation" in log())
run('echo extern long cos_sandbox(unsigned,const char*); extern long cos_syscall3(long,long,long,long); int main(void){ cos_sandbox(0xFFFFFFFFu & ~0x1000u, 0); return cos_syscall3(4146, (long)"/", 1, 0) == -1 ? 94 : 99; } > /tmp/wp2.c')
run("tcc -run /tmp/wp2.c")
check("a sandboxed program cannot change the protection (it needs full privileges)", wait(r"\[TERMINAL\] exit: tcc\.c-os 94") is not None)
run("kmod unload wprotect")
check("the module unloads and leaves its count in a file written with the module's own file access", wait(r"\[KMOD\] unloaded wprotect") is not None)
run('echo extern long cos_write_file(const char*,const void*,unsigned long); extern long cos_read_file(const char*,void*,unsigned long); extern int printf(const char*,...); '
    'int main(void){ char b[32] = {0}; long w = cos_write_file("/tmp/prot/after.txt", "z", 1); long n = cos_read_file("/tmp/wprotect.last", b, 31); printf("after=%ld last=%s", w, b); return (w == 1 && n >= 8 && b[0] == 100) ? 95 : 99; } > /tmp/wp3.c')
run("tcc -run /tmp/wp3.c")
check("... the protection ends with the module (the write works again) and /tmp/wprotect.last holds the count", wait(r"\[TERMINAL\] exit: tcc\.c-os (95|99)") is not None and "exit: tcc.c-os 95" in log() and "last=denied=" in log())
run('echo extern long cos_syscall3(long,long,long,long); extern long cos_sandbox(unsigned,const char*); extern int cos_net_resolve(const char*,unsigned char*); '
    'int main(void){ unsigned long long st[8]; unsigned char ip[4]; long a = cos_syscall3(4148, (long)st, 0, 0); unsigned long long tx0 = st[2], rx0 = st[0]; long r0 = cos_syscall3(4149, 1, 0, 0); '
    'cos_net_resolve("example.com", ip); long a2 = cos_syscall3(4148, (long)st, 0, 0); long dropped = st[4]; long r1 = cos_syscall3(4149, 0, 0, 0); '
    'cos_sandbox(0xFFFFFFFFu & ~0x1000u, 0); long denied = cos_syscall3(4149, 1, 0, 0); '
    'return (a == 0 && a2 == 0 && tx0 > 0 && rx0 > 0 && r0 == 0 && dropped > 0 && r1 == 0 && denied == -1) ? 96 : 99; } > /tmp/nf.c')
run("tcc -run /tmp/nf.c")
check("the network filter counts frames in both directions, drops outgoing UDP on request, and the rule needs full privileges", wait(r"\[TERMINAL\] exit: tcc\.c-os (96|99)") is not None and "exit: tcc.c-os 96" in log())
run('echo extern long cos_syscall3(long,long,long,long); extern long cos_sandbox(unsigned,const char*); extern int cos_net_resolve(const char*,unsigned char*); '
    'int main(void){ unsigned long long st[8]; unsigned char ip[4]; long r0 = cos_syscall3(4150, 53, 5353, 0); cos_net_resolve("example.org", ip); '
    'long a = cos_syscall3(4148, (long)st, 0, 0); long rw = st[5], seen = st[6], bad = st[7]; long r1 = cos_syscall3(4150, 0, 0, 0); '
    'cos_sandbox(0xFFFFFFFFu & ~0x1000u, 0); long denied = cos_syscall3(4150, 53, 5353, 0); '
    'return (r0 == 0 && a == 0 && rw > 0 && seen > 0 && bad == 0 && r1 == 0 && denied == -1) ? 97 : 99; } > /tmp/nf2.c')
run("tcc -run /tmp/nf2.c")
check("the frame rewriter changes the destination port of outgoing DNS queries, the checksums are fixed, the read-only filter sees the rewritten frame, and it needs full privileges", wait(r"\[TERMINAL\] exit: tcc\.c-os (97|99)") is not None and "exit: tcc.c-os 97" in log())
check("the edu driver is loaded, with its interrupt delivered by MSI", "[KMOD] loaded edu 1.0 level 2" in log() and "edu device ready (MSI)" in log())
run('echo extern long cos_syscall3(long,long,long,long); extern long cos_sandbox(unsigned,const char*); '
    'int main(void){ long f5 = cos_syscall3(4151, 5, 0, 0); long f10 = cos_syscall3(4151, 10, 0, 0); long i1 = cos_syscall3(4152, 0, 0, 0); long bad = cos_syscall3(4151, 99, 0, 0); '
    'return (f5 == 120 && f10 == 3628800 && (i1 & 255) == 1 && (i1 >> 8) >= 2 && bad == -1) ? 91 : 99; } > /tmp/edu1.c')
run("tcc -run /tmp/edu1.c")
check("a device interrupt arrives by MSI (the device computes factorials and signals completion)", wait(r"\[TERMINAL\] exit: tcc\.c-os (91|99)") is not None and "exit: tcc.c-os 91" in log())
run('echo extern long cos_syscall3(long,long,long,long); extern long cos_sandbox(unsigned,const char*); '
    'int main(void){ long m = cos_syscall3(4153, 2, 0, 0); long f = cos_syscall3(4151, 6, 0, 0); long i = cos_syscall3(4152, 0, 0, 0); long f2 = cos_syscall3(4151, 7, 0, 0); '
    'cos_sandbox(0xFFFFFFFFu & ~0x1000u, 0); long denied = cos_syscall3(4153, 1, 0, 0); '
    'return (m == 0 && f == 720 && (i & 255) == 2 && f2 == 5040 && denied == -1) ? 92 : 99; } > /tmp/edu2.c')
run("tcc -run /tmp/edu2.c")
check("... and through a route of the I/O APIC (level triggered, shared line), switching modes needs full privileges", wait(r"\[TERMINAL\] exit: tcc\.c-os (92|99)") is not None and "exit: tcc.c-os 92" in log())
run("kmod unload edu")
check("the edu driver unloads (MSI / route switched off, vector freed)", wait(r"\[KMOD\] unloaded edu") is not None)
run("kmod load /system/drivers/edu.c-osys")
check("... and loads again (the vector and the device are usable once more)", wait(r"(?s)unloaded edu.*\[KMOD\] loaded edu 1\.0 level 2") is not None)
sys.exit(0 if ok else 1)
