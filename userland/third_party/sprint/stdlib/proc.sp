# proc: a tiny cooperative operating-system kernel written in Sprint. Processes are functions
# `main(sys, args)` that run as coroutines; they call `sys.sleep(ms)`, `sys.yield_now()`, `sys.recv()`,
# `sys.send(pid, msg)`, `sys.exit(code)` and use `sys.fs`. The kernel schedules round-robin on a virtual
# clock (`run_for(ms)` / `step(ms)`), so a run is deterministic. A Shell with built-in commands sits on
# top of the kernel and the file system.
import coroutine
import vfs

# what a process sees of the kernel
export class Sys:
    let pid = 0
    let kernel = nil
    let fs = nil
    let out = []
    func init(self, k, pid):
        self.kernel = k
        self.pid = pid
        self.fs = k.fs
        self.out = []
    func now(self):
        return self.kernel.now
    func print(self, s):
        self.kernel.stdout.append(to_string(s))
        self.out.append(to_string(s))
    func sleep(self, ms):
        yield ["sleep", ms]
    func yield_now(self):
        yield ["yield", 0]
    # next message sent to this process (blocks until there is one)
    func recv(self):
        yield ["recv", 0]
        return self.kernel.procs[self.pid].inbox.remove_at(0)
    func send(self, pid, msg):
        return self.kernel.send(pid, msg)
    func spawn(self, name, main, args = []):
        return self.kernel.spawn(name, main, args, self.pid)
    func kill(self, pid):
        return self.kernel.kill(pid)
    func exit(self, code = 0):
        yield ["exit", code]

export class Kernel:
    let fs = nil
    let procs = {}
    let order = []
    let next_pid = 1
    let now = 0
    let stdout = []
    let ticks = 0
    func init(self, fs = nil):
        self.fs = fs if fs != nil else vfs.FS()
        self.procs = {}
        self.order = []
        self.stdout = []
    # start a process; returns its pid
    func spawn(self, name, main, args = [], parent = 0):
        let pid = self.next_pid
        self.next_pid += 1
        let sys = Sys(self, pid)
        let co = coroutine.create(() => main(sys, args))
        self.procs[pid] = {pid: pid, name: name, co: co, state: "ready", wake: 0, inbox: [], code: nil,
                           parent: parent, sys: sys, cpu: 0}
        self.order.append(pid)
        return pid
    func send(self, pid, msg):
        let p = self.procs[pid]
        if p == nil or p.state == "dead":
            return false
        p.inbox.append(msg)
        if p.state == "wait_msg":
            p.state = "ready"
        return true
    func kill(self, pid):
        let p = self.procs[pid]
        if p == nil or p.state == "dead":
            return false
        coroutine.close(p.co)
        p.state = "dead"
        p.code = -9
        return true
    func alive(self, pid):
        let p = self.procs[pid]
        return p != nil and p.state != "dead"
    func exit_code(self, pid):
        return self.procs[pid].code if pid in self.procs else nil
    # run one process for one slice
    func slice(self, p):
        let r = coroutine.resume(p.co)
        p.cpu += 1
        if coroutine.status(p.co) == "dead":
            p.state = "dead"
            p.code = 0 if r == nil else r
            return nil
        if r == nil:
            return nil
        if r[0] == "sleep":
            p.state = "sleep"
            p.wake = self.now + r[1]
        elif r[0] == "recv":
            if len(p.inbox) == 0:
                p.state = "wait_msg"
        elif r[0] == "exit":
            coroutine.close(p.co)
            p.state = "dead"
            p.code = r[1]
    # advance the clock by `ms` and give every runnable process one slice per tick (1 ms)
    func step(self, ms = 1):
        for i in range(ms):
            self.now += 1
            self.ticks += 1
            for pid in self.order.copy():
                let p = self.procs[pid]
                if p.state == "sleep" and p.wake <= self.now:
                    p.state = "ready"
                if p.state == "ready":
                    self.slice(p)
            self.reap()
    # drop dead processes from the run list (their exit codes stay readable with exit_code)
    func reap(self):
        let keep = []
        for pid in self.order:
            if self.procs[pid].state == "dead":
                self.procs[pid].reaped = true
            else:
                keep.append(pid)
        self.order = keep
    func run_for(self, ms):
        self.step(ms)
    # run until no process is alive or `limit` ms passed; returns elapsed ms
    func run_all(self, limit = 100000):
        let t0 = self.now
        while len(self.order) > 0 and self.now - t0 < limit:
            self.step(1)
        return self.now - t0
    func ps(self):
        let out = []
        for pid in self.order:
            let p = self.procs[pid]
            if p.state != "dead":
                out.append([p.pid, p.name, p.state])
        return out

# ---- shell ----------------------------------------------------------------------------------
export class Shell:
    let kernel = nil
    let fs = nil
    let programs = {}
    func init(self, kernel):
        self.kernel = kernel
        self.fs = kernel.fs
        self.programs = {}
    # install a program callable from the shell: main(sys, args)
    func install(self, name, main):
        self.programs[name] = main
        self.fs.mkdir("/bin", true)
        self.fs.write("/bin/" .. name, "#!sprint " .. name)
    func prompt(self):
        return self.fs.cwd .. " $ "
    # run one command line; returns the output text (without trailing newline)
    func run(self, line):
        let words = line.trim().split(" ").filter(w => w != "")
        if len(words) == 0:
            return ""
        let cmd = words[0]
        let a = words.slice(1, len(words))
        let fs = self.fs
        try:
            if cmd == "ls":
                return fs.ls(a[0] if len(a) > 0 else ".").join("  ")
            if cmd == "cd":
                fs.chdir(a[0] if len(a) > 0 else "/")
                return ""
            if cmd == "pwd":
                return fs.cwd
            if cmd == "cat":
                return fs.read(a[0])
            if cmd == "echo":
                return a.join(" ")
            if cmd == "write":
                fs.write(a[0], a.slice(1, len(a)).join(" "))
                return ""
            if cmd == "mkdir":
                fs.mkdir(a[0], true)
                return ""
            if cmd == "rm":
                fs.rm(a[len(a) - 1], a[0] == "-r")
                return ""
            if cmd == "cp":
                fs.cp(a[0], a[1])
                return ""
            if cmd == "mv":
                fs.mv(a[0], a[1])
                return ""
            if cmd == "ps":
                return self.kernel.ps().map(r => to_string(r[0]) .. " " .. r[1] .. " " .. r[2]).join("\n")
            if cmd == "kill":
                return "" if self.kernel.kill(to_int(a[0])) else "kill: no such process"
            if cmd == "tree":
                return fs.walk(a[0] if len(a) > 0 else ".").join("\n")
            if cmd == "help":
                let names = ["ls", "cd", "pwd", "cat", "echo", "write", "mkdir", "rm", "cp", "mv", "ps", "kill", "tree"]
                for k, v in self.programs:
                    names.append(k)
                return names.join(" ")
            if cmd in self.programs:
                let before = len(self.kernel.stdout)
                let pid = self.kernel.spawn(cmd, self.programs[cmd], a)
                let guard = 0
                while self.kernel.alive(pid) and guard < 10000:
                    self.kernel.step(1)
                    guard += 1
                return self.kernel.stdout.slice(before, len(self.kernel.stdout)).join("\n")
            return cmd .. ": command not found"
        catch e:
            return cmd .. ": " .. to_string(e)
