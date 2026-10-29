# vfs: an in-memory hierarchical file system for programs that play operating system. Paths are
# "/"-separated; relative paths resolve against `cwd`. Directories hold their entries in a table,
# listings are sorted, so behaviour is deterministic. Errors are thrown as error("OSError", ...).
import json

func fail(msg):
    throw error("OSError", msg)

# normalise a path against a base directory: removes ".", "..", empty parts; the result starts with "/"
export func normalize(path, base = "/"):
    let parts = []
    let full = path if path.starts_with("/") else base .. "/" .. path
    for p in full.split("/"):
        if p == "" or p == ".":
            continue
        if p == "..":
            if len(parts) > 0:
                parts.pop()
            continue
        parts.append(p)
    return "/" .. parts.join("/")

func last_slash(n):
    let i = len(n) - 1
    while i > 0 and n[i] != 47:
        i -= 1
    return i

export func basename(path):
    let n = normalize(path)
    if n == "/":
        return "/"
    return n.slice(last_slash(n) + 1, len(n))

export func dirname(path):
    let n = normalize(path)
    let i = last_slash(n)
    return "/" if i <= 0 else n.slice(0, i)

export func join(a, b):
    return normalize(b, a)

export class FS:
    let root = {kind: "dir", entries: {}}
    let cwd = "/"
    let mtime = 0
    func init(self):
        self.root = {kind: "dir", entries: {}}
    func tick(self):
        self.mtime += 1
        return self.mtime
    func abs(self, path):
        return normalize(path, self.cwd)
    # node at path or nil
    func node(self, path):
        let n = self.root
        for p in self.abs(path).split("/"):
            if p == "":
                continue
            if n.kind != "dir" or not (p in n.entries):
                return nil
            n = n.entries[p]
        return n
    func exists(self, path):
        return self.node(path) != nil
    func is_dir(self, path):
        let n = self.node(path)
        return n != nil and n.kind == "dir"
    func is_file(self, path):
        let n = self.node(path)
        return n != nil and n.kind == "file"
    func parent_of(self, path):
        let p = self.node(dirname(self.abs(path)))
        if p == nil or p.kind != "dir":
            fail("no such directory: " .. dirname(self.abs(path)))
        return p
    func mkdir(self, path, parents = false):
        let a = self.abs(path)
        if a == "/":
            return nil
        if parents:
            let cur = "/"
            for p in a.split("/"):
                if p == "":
                    continue
                cur = join(cur, p)
                let n = self.node(cur)
                if n == nil:
                    self.parent_of(cur).entries[p] = {kind: "dir", entries: {}, mtime: self.tick()}
                elif n.kind != "dir":
                    fail("not a directory: " .. cur)
            return nil
        if self.exists(a):
            fail("already exists: " .. a)
        self.parent_of(a).entries[basename(a)] = {kind: "dir", entries: {}, mtime: self.tick()}
    func write(self, path, data):
        let a = self.abs(path)
        let n = self.node(a)
        if n != nil and n.kind == "dir":
            fail("is a directory: " .. a)
        if n == nil:
            self.parent_of(a).entries[basename(a)] = {kind: "file", data: data, mtime: self.tick()}
        else:
            n.data = data
            n.mtime = self.tick()
    func append(self, path, data):
        if self.exists(path):
            self.write(path, self.read(path) .. data)
        else:
            self.write(path, data)
    func read(self, path):
        let n = self.node(path)
        if n == nil:
            fail("no such file: " .. self.abs(path))
        if n.kind != "file":
            fail("is a directory: " .. self.abs(path))
        return n.data
    # sorted names in a directory
    func ls(self, path = "."):
        let n = self.node(path)
        if n == nil:
            fail("no such directory: " .. self.abs(path))
        if n.kind != "dir":
            return [basename(path)]
        let out = []
        for k, v in n.entries:
            out.append(k)
        out.sort()
        return out
    func stat(self, path):
        let n = self.node(path)
        if n == nil:
            fail("no such file or directory: " .. self.abs(path))
        return {kind: n.kind, size: len(n.data) if n.kind == "file" else len(n.entries), mtime: n.mtime if "mtime" in n else 0}
    func rm(self, path, recursive = false):
        let a = self.abs(path)
        if a == "/":
            fail("cannot remove /")
        let n = self.node(a)
        if n == nil:
            fail("no such file or directory: " .. a)
        if n.kind == "dir" and len(n.entries) > 0 and not recursive:
            fail("directory not empty: " .. a)
        self.parent_of(a).entries[basename(a)] = nil
    func chdir(self, path):
        let a = self.abs(path)
        if not self.is_dir(a):
            fail("not a directory: " .. a)
        self.cwd = a
    func clone_node(self, n):
        if n.kind == "file":
            return {kind: "file", data: n.data, mtime: self.tick()}
        let e = {}
        for k, v in n.entries:
            e[k] = self.clone_node(v)
        return {kind: "dir", entries: e, mtime: self.tick()}
    func cp(self, src, dst):
        let n = self.node(src)
        if n == nil:
            fail("no such file or directory: " .. self.abs(src))
        let d = self.abs(dst)
        if self.is_dir(d):
            d = join(d, basename(self.abs(src)))
        if d == self.abs(src) or d.starts_with(self.abs(src) .. "/"):
            fail("cannot copy into itself")
        self.parent_of(d).entries[basename(d)] = self.clone_node(n)
    func mv(self, src, dst):
        let a = self.abs(src)
        self.cp(a, dst)
        let d = self.abs(dst)
        if self.is_dir(d):
            d = join(d, basename(a))
        if d != a:
            self.rm(a, true)
    # every path below `path` (files and directories), sorted depth first
    func walk(self, path = "/"):
        let out = []
        let base = self.abs(path)
        func rec(p):
            for name in self.ls(p):
                let q = join(p, name)
                out.append(q)
                if self.is_dir(q):
                    rec(q)
        rec(base)
        return out
    # a flat JSON image of the whole tree, and its inverse
    func dump(self):
        return json.stringify(self.root)
    func load(self, text):
        self.root = json.parse(text)
        self.cwd = "/"
