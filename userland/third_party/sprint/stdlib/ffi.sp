# ffi: call C (and C++ exported with extern "C") from Sprint, LuaJIT style.
#
#   import ffi
#   ffi.cdef("double sqrt(double); int printf(const char *fmt, ...);")
#   let c = ffi.open(nil)              # the running program (libc, libm, ...); or ffi.open("libz.so.1")
#   let sqrt = c.fn("sqrt")
#   print(sqrt(2.0))
#
# Needs `--allow-ffi` (never available with --deterministic). It is unsafe by nature: a wrong
# declaration or address crashes the process. Calls support up to 6 integer/pointer and 8
# floating-point arguments, no structs by value, callbacks from a pool of 16 per return kind.
import _ffi

export const NULL = 0
export const consts = {}

const SIZE = {i8: 1, u8: 1, i16: 2, u16: 2, i32: 4, u32: 4, i64: 8, u64: 8, f32: 4, f64: 8, p: 8, s: 8}
const WORD = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
const QUALS = ["const", "volatile", "extern", "static", "inline", "register", "restrict", "__restrict", "__restrict__", "__inline"]

# builtin fixed-width and size typedefs
const BASE_TYPEDEFS = {
    int8_t: "i8", uint8_t: "u8", int16_t: "i16", uint16_t: "u16", int32_t: "i32", uint32_t: "u32",
    int64_t: "i64", uint64_t: "u64", size_t: "u64", ssize_t: "i64", ptrdiff_t: "i64",
    intptr_t: "i64", uintptr_t: "u64", off_t: "i64", time_t: "i64", wchar_t: "i32"
}

let typedefs = {}
let structs = {}
let protos = {}
let live = {}

func fail(msg):
    throw error("ValueError", "ffi.cdef: " .. msg)

func align_up(n, a):
    return (n + a - 1) // a * a

# ---- tokenizer ---------------------------------------------------------------------
func tokenize(src):
    let toks = []
    let i = 0
    let n = len(src)
    while i < n:
        let c = src.slice(i, i + 1)
        if c == " " or c == "\n" or c == "\t" or c == "\r":
            i += 1
        elif c == "/" and src.slice(i + 1, i + 2) == "/":
            while i < n and src.slice(i, i + 1) != "\n":
                i += 1
        elif c == "/" and src.slice(i + 1, i + 2) == "*":
            let rest = src.slice(i + 2, n)
            let k = rest.find("*/")
            if k < 0:
                fail("unterminated comment")
            i = i + 2 + k + 2
        elif WORD.contains(c):
            let j = i
            while j < n and WORD.contains(src.slice(j, j + 1)):
                j += 1
            toks.append(src.slice(i, j))
            i = j
        elif c == "." and src.slice(i, i + 3) == "...":
            toks.append("...")
            i += 3
        else:
            toks.append(c)
            i += 1
    return toks

func is_word(t):
    return t != nil and WORD.contains(t.slice(0, 1))

func parse_number(t):
    if t.starts_with("0x") or t.starts_with("0X"):
        let v = 0
        for ch in t.slice(2, len(t)).split(""):
            v = v * 16 + "0123456789abcdef".find(ch.lower())
        return v
    return to_int(t)

class Parser:
    let toks = []
    let pos = 0
    func init(self, toks):
        self.toks = toks
    func peek(self, k = 0):
        if self.pos + k >= len(self.toks):
            return nil
        return self.toks[self.pos + k]
    func next(self):
        let t = self.peek()
        if t == nil:
            fail("unexpected end of declaration")
        self.pos += 1
        return t
    func accept(self, t):
        if self.peek() == t:
            self.pos += 1
            return true
        return false
    func expect(self, t):
        let g = self.next()
        if g != t:
            fail("expected '" .. t .. "', found '" .. g .. "'")
        return g

# ---- types -------------------------------------------------------------------------
# A type description: {code, ptr, st, plain_char, fnp}
#   code  scalar code (i8..f64, v) of the base; st  struct layout; ptr  pointer depth;
#   fnp   function prototype when this is a pointer to function

func scalar(code):
    return {code: code, ptr: 0, st: nil, plain_char: false, fnp: nil}

# System V x86-64 classification of a struct of at most 16 bytes: one class per eightbyte,
# "D" when every scalar in it is a float/double, otherwise "I"
func classify(st):
    let cls = ["", ""]
    collect_classes(st, 0, cls)
    let out = ""
    for k in range((st.size + 7) // 8):
        out = out .. ("D" if cls[k] == "D" else "I")
    return out

func collect_classes(st, base, cls):
    for name in st.order:
        let f = st.fields[name]
        let count = f.n if f.n > 0 else 1
        for i in range(count):
            let off = base + f.off + i * f.esize
            if f.st != nil:
                collect_classes(f.st, off, cls)
            else:
                let k = off // 8
                let c = "D" if (f.code == "f32" or f.code == "f64") else "I"
                if cls[k] == "" or c == "I":
                    cls[k] = c

func wire(d, what):
    if d.ptr > 0:
        if d.ptr == 1 and d.code == "i8" and d.plain_char and d.st == nil:
            return "s"
        return "p"
    if d.st != nil:
        if d.st.size > 16:
            fail("a struct larger than 16 bytes cannot be passed by value (" .. what .. "); pass a pointer")
        return "S" .. to_string(d.st.size) .. classify(d.st)
    return d.code

func parse_base(p):
    let uns = false
    let longs = 0
    let short = false
    let word = nil
    let td = nil
    let st = nil
    let any = false
    while true:
        let t = p.peek()
        if t == nil:
            break
        if QUALS.contains(t):
            p.next()
        elif t == "unsigned":
            p.next()
            uns = true
            any = true
        elif t == "signed":
            p.next()
            any = true
        elif t == "long":
            p.next()
            longs += 1
            any = true
        elif t == "short":
            p.next()
            short = true
            any = true
        elif t == "char" or t == "int" or t == "float" or t == "double" or t == "void" or t == "_Bool" or t == "bool":
            p.next()
            word = t
            any = true
        elif t == "union":
            fail("unions are not supported")
        elif t == "struct":
            p.next()
            let tag = nil
            if is_word(p.peek()):
                tag = p.next()
            if p.peek() == "{":
                st = parse_struct_body(p, tag)
            else:
                if tag == nil:
                    fail("expected a struct name")
                st = structs[tag] if has(structs, tag) else nil
                if st == nil:
                    st = {name: tag, size: 0, align: 1, fields: {}, order: [], incomplete: true}
                    structs[tag] = st
            any = true
        elif t == "enum":
            p.next()
            if is_word(p.peek()) and p.peek() != "{":
                p.next()
            if p.peek() == "{":
                parse_enum_body(p)
            word = "int"
            any = true
        elif not any and word == nil and has(typedefs, t):
            p.next()
            td = typedefs[t]
            any = true
        elif not any and word == nil and has(structs, t):
            p.next()
            st = structs[t]
            any = true
        elif not any and word == nil and has(BASE_TYPEDEFS, t):
            p.next()
            td = scalar(BASE_TYPEDEFS[t])
            any = true
        else:
            break
    if not any:
        fail("expected a type, found '" .. to_string(p.peek()) .. "'")
    if td != nil:
        return {code: td.code, ptr: td.ptr, st: td.st, plain_char: td.plain_char, fnp: td.fnp}
    if st != nil:
        return {code: "p", ptr: 0, st: st, plain_char: false, fnp: nil}
    let code = "i32"
    let plain = false
    if word == "void":
        code = "v"
    elif word == "float":
        code = "f32"
    elif word == "double":
        code = "f64"
    elif word == "_Bool" or word == "bool":
        code = "u8"
    elif word == "char":
        code = "u8" if uns else "i8"
        plain = not uns
    elif short:
        code = "u16" if uns else "i16"
    elif longs >= 1:
        code = "u64" if uns else "i64"
    else:
        code = "u32" if uns else "i32"
    return {code: code, ptr: 0, st: nil, plain_char: plain, fnp: nil}

func parse_enum_body(p):
    p.expect("{")
    let v = 0
    while p.peek() != "}":
        let name = p.next()
        if p.accept("="):
            let neg = p.accept("-")
            v = parse_number(p.next())
            if neg:
                v = -v
        consts[name] = v
        v += 1
        if not p.accept(","):
            break
    p.expect("}")

# a declarator: stars, a name (optional), array dims or a parameter list or "(*name)(params)"
func parse_declarator(p, base):
    let d = {code: base.code, ptr: base.ptr, st: base.st, plain_char: base.plain_char, fnp: base.fnp}
    while p.peek() == "*":
        p.next()
        d.ptr += 1
        while p.peek() != nil and QUALS.contains(p.peek()):
            p.next()
    let name = nil
    let arr = 0
    let params = nil
    if p.peek() == "(" and p.peek(1) == "*":
        p.next()
        p.next()
        if is_word(p.peek()):
            name = p.next()
        p.expect(")")
        let ps = parse_params(p)
        let ret = {code: d.code, ptr: d.ptr, st: d.st, plain_char: d.plain_char, fnp: nil}
        d = {code: "p", ptr: 1, st: nil, plain_char: false, fnp: make_proto(ret, ps, "callback")}
    else:
        if is_word(p.peek()):
            name = p.next()
        if p.peek() == "(":
            params = parse_params(p)
        else:
            while p.accept("["):
                if p.peek() == "]":
                    arr = -1
                else:
                    let n = parse_number(p.next())
                    arr = n if arr <= 0 else arr * n
                p.expect("]")
    return {name: name, d: d, arr: arr, params: params}

# parameters: returns {types: [desc], variadic}
func parse_params(p):
    p.expect("(")
    let types = []
    let variadic = false
    if p.peek() == "void" and p.peek(1) == ")":
        p.next()
    while p.peek() != ")":
        if p.accept("..."):
            variadic = true
            break
        let base = parse_base(p)
        let dc = parse_declarator(p, base)
        let d = dc.d
        if dc.arr != 0:
            d.ptr += 1
        types.append(d)
        if not p.accept(","):
            break
    p.expect(")")
    return {types: types, variadic: variadic}

func make_proto(ret, ps, name):
    let args = []
    let kinds = []
    let byval = []
    for t in ps.types:
        args.append(wire(t, name))
        kinds.append(t.fnp)
        byval.append(t.st != nil and t.ptr == 0)
    # a struct returned by value: <= 16 bytes in registers, larger through a hidden pointer
    let ret_val = nil
    let sret = false
    let r = ""
    if ret.st != nil and ret.ptr == 0:
        ret_val = ret.st
        if ret.st.size > 16:
            sret = true
            r = "p"
        else:
            r = wire(ret, name)
    else:
        r = wire(ret, name)
    let wargs = args
    if sret:
        wargs = args.copy()
        wargs.insert(0, "p")
    let sig = r .. ":" .. wargs.join(",")
    if ps.variadic:
        sig = sig .. ("..." if len(wargs) == 0 else ",...")
    let ret_st = ret.st if ret.ptr == 1 else nil
    return {name: name, sig: sig, ret: r, args: args, fnp: kinds, variadic: ps.variadic, ret_st: ret_st, ret_val: ret_val, sret: sret, byval: byval}

func scalar_size(d):
    if d.ptr > 0:
        return 8
    if d.st != nil:
        return d.st.size
    return SIZE[d.code]

func scalar_align(d):
    if d.ptr > 0:
        return 8
    if d.st != nil:
        return d.st.align
    return SIZE[d.code]

func parse_struct_body(p, tag):
    p.expect("{")
    let lay = {name: tag, size: 0, align: 1, fields: {}, order: [], incomplete: false}
    if tag != nil:
        structs[tag] = lay
    let off = 0
    while p.peek() != "}":
        let base = parse_base(p)
        while true:
            let dc = parse_declarator(p, base)
            if dc.name == nil:
                fail("unnamed struct member")
            if dc.params != nil:
                fail("function members are not supported")
            let d = dc.d
            if d.st != nil and d.ptr == 0 and d.st.incomplete:
                fail("member '" .. dc.name .. "' has an incomplete struct type")
            if d.code == "v" and d.ptr == 0:
                fail("member '" .. dc.name .. "' has type void")
            let esize = scalar_size(d)
            let al = scalar_align(d)
            let n = dc.arr if dc.arr > 0 else 0
            off = align_up(off, al)
            let w = "p" if d.ptr > 0 else d.code
            if d.ptr == 1 and d.code == "i8" and d.plain_char and n == 0:
                w = "s"
            lay.fields[dc.name] = {off: off, code: w, esize: esize, n: n, st: d.st if d.ptr == 0 else nil, pst: d.st if d.ptr == 1 else nil}
            lay.order.append(dc.name)
            off += esize * (n if n > 0 else 1)
            if al > lay.align:
                lay.align = al
            if not p.accept(","):
                break
        p.expect(";")
    p.expect("}")
    lay.size = align_up(off, lay.align)
    return lay

# ---- cdef --------------------------------------------------------------------------
# Declare C types and functions. Supported: function prototypes (incl. `...`), struct, enum,
# typedef (incl. function pointers), pointers, fixed arrays in structs. Not supported:
# unions, bit fields, structs by value, `long double`, global variables.
export func cdef(src):
    let p = Parser(tokenize(src))
    while p.peek() != nil:
        if p.accept(";"):
            continue
        if p.peek() == "typedef":
            p.next()
            let base = parse_base(p)
            while true:
                let dc = parse_declarator(p, base)
                if dc.name == nil:
                    fail("typedef without a name")
                let d = dc.d
                if dc.arr != 0:
                    fail("array typedefs are not supported")
                if d.st != nil and d.st.name == nil:
                    d.st.name = dc.name
                    structs[dc.name] = d.st
                typedefs[dc.name] = d
                if not p.accept(","):
                    break
            p.expect(";")
            continue
        let base = parse_base(p)
        if p.peek() == ";":
            p.next()
            continue
        let dc = parse_declarator(p, base)
        if dc.params == nil:
            fail("global variables are not supported ('" .. to_string(dc.name) .. "')")
        if dc.name == nil:
            fail("function without a name")
        protos[dc.name] = make_proto(dc.d, dc.params, dc.name)
        p.expect(";")

# prototype from a standalone declaration like "double hyp(double, double)" or "int (*)(int)"
func proto_from(decl):
    let p = Parser(tokenize(decl))
    let base = parse_base(p)
    let dc = parse_declarator(p, base)
    if dc.fnp != nil:
        return dc.fnp
    if dc.params == nil:
        fail("not a function declaration: " .. decl)
    return make_proto(dc.d, dc.params, to_string(dc.name))

# ---- addresses, buffers, views -------------------------------------------------------
# address of an int, a view (Arr / Struct), an ffi buffer or a string's bytes
export func addr(x):
    if type(x) == "int":
        return x
    if type(x) == "nil":
        return 0
    if type(x) == "instance":
        return x.addr()
    if type(x) == "native":
        return x.ptr()
    throw error("TypeError", "cannot take the address of a " .. type(x))

export class Arr:
    let base = 0
    let code = "i32"
    let esize = 4
    let n = 0
    let keep = nil
    func init(self, base, code, n, keep = nil):
        self.base = base
        self.code = code
        self.esize = SIZE[code]
        self.n = n
        self.keep = keep
    func addr(self):
        return self.base
    func count(self):
        return self.n
    func at(self, i):
        if type(i) != "int" or i < 0 or i >= self.n:
            throw error("IndexError", "ffi array index " .. to_string(i) .. " out of range 0.." .. to_string(self.n - 1))
        return self.base + i * self.esize
    func get(self, i = 0):
        return _ffi.peek(self.at(i), self.code)
    func set(self, i, v):
        if self.code == "p":
            v = addr(v)
        _ffi.poke(self.at(i), self.code, v)
    func to_array(self):
        return [_ffi.peek(self.base + i * self.esize, self.code) for i in range(self.n)]
    func fill_from(self, xs):
        if len(xs) > self.n:
            throw error("IndexError", "too many initial values for the ffi array")
        for i in range(len(xs)):
            self.set(i, xs[i])
    # the bytes up to the first NUL (for char arrays); `n` limits the read
    func str(self, limit = nil):
        let m = self.n * self.esize
        if limit != nil and limit < m:
            m = limit
        let s = _ffi.string_at(self.base, m)
        let z = s.find("\0")
        return s if z < 0 else s.slice(0, z)

export class Struct:
    let layout = nil
    let base = 0
    let keep = nil
    let owned = []
    func init(self, layout, base, keep = nil):
        self.layout = layout
        self.base = base
        self.keep = keep
    func addr(self):
        return self.base
    func size(self):
        return self.layout.size
    func field(self, name):
        if not has(self.layout.fields, name):
            throw error("NameError", "struct " .. to_string(self.layout.name) .. " has no field '" .. to_string(name) .. "'")
        return self.layout.fields[name]
    # value of a field (arrays take an index; nested structs return a view; pointers return an int)
    func get(self, name, i = nil):
        let f = self.field(name)
        let a = self.base + f.off
        if f.n > 0:
            if i == nil:
                if f.st != nil:
                    throw error("TypeError", "field '" .. name .. "' is an array of structs; pass an index")
                return Arr(a, f.code, f.n, self.keep)
            if type(i) != "int" or i < 0 or i >= f.n:
                throw error("IndexError", "index " .. to_string(i) .. " out of range for field '" .. name .. "'")
            a += i * f.esize
        if f.st != nil:
            return Struct(f.st, a, self.keep)
        if f.code == "s":
            let ptr = _ffi.peek(a, "i64")
            return nil if ptr == 0 else _ffi.string_at(ptr)
        return _ffi.peek(a, "i64" if f.code == "p" else f.code)
    func set(self, name, v, i = nil):
        let f = self.field(name)
        let a = self.base + f.off
        if f.n > 0:
            if i == nil or type(i) != "int" or i < 0 or i >= f.n:
                throw error("IndexError", "field '" .. name .. "' is an array; pass an index in 0.." .. to_string(f.n - 1))
            a += i * f.esize
        if f.st != nil:
            throw error("TypeError", "cannot assign a struct field; set its members")
        if f.code == "s" or f.code == "p":
            if type(v) == "string":
                let b = cstr(v)
                self.owned.append(b)
                v = b.addr()
            else:
                v = addr(v)
            _ffi.poke(a, "i64", v)
        else:
            _ffi.poke(a, f.code, v)
    # a char-array field as a string
    func str(self, name):
        let v = self.get(name)
        return v.str() if type(v) == "instance" else v
    func to_table(self):
        let t = {}
        for name in self.layout.order:
            let f = self.layout.fields[name]
            if f.st == nil and f.n == 0:
                t[name] = self.get(name)
            elif f.st == nil:
                t[name] = self.get(name).to_array()
        return t

func alloc_view(nbytes):
    return _ffi.alloc(nbytes)

# a NUL-terminated copy of a string, as a char array
export func cstr(s):
    let b = alloc_view(len(s) + 1)
    b.put(0, s)
    return Arr(b.ptr(), "i8", len(s) + 1, b)

# typed array: ffi.array("i32", 4, [1, 2, 3, 4])
export func array(code, n, init = nil):
    if not has(SIZE, code) or code == "s":
        throw error("ValueError", "unknown array element type '" .. to_string(code) .. "'")
    let b = alloc_view(n * SIZE[code])
    let a = Arr(b.ptr(), code, n, b)
    if init != nil:
        a.fill_from(init)
    return a

# view over foreign memory (no bounds beyond n, nothing is freed)
export func view(address, code, n):
    return Arr(address, code, n, nil)

func type_of(text):
    let p = Parser(tokenize(text))
    let base = parse_base(p)
    let dc = parse_declarator(p, base)
    return dc

# ffi.new("struct Point"), ffi.new("Point", {x: 1, y: 2}), ffi.new("int[8]", [1, 2]), ffi.new("double")
export func new(text, init = nil):
    let dc = type_of(text)
    let d = dc.d
    if d.ptr > 0 or dc.params != nil:
        let b = alloc_view(8)
        return Arr(b.ptr(), "p", 1, b)
    if dc.arr > 0:
        if d.st != nil:
            let b = alloc_view(d.st.size * dc.arr)
            return Arr(b.ptr(), "p", dc.arr, b)
        return array(d.code, dc.arr, init)
    if d.st != nil:
        let b = alloc_view(d.st.size)
        let s = Struct(d.st, b.ptr(), b)
        if init != nil:
            for k, v in init:
                s.set(k, v)
        return s
    let b = alloc_view(SIZE[d.code])
    let a = Arr(b.ptr(), d.code, 1, b)
    if init != nil:
        a.set(0, init)
    return a

# a view of a struct at an address: ffi.cast("struct Point", p)
export func cast(text, address):
    let dc = type_of(text)
    if dc.d.st == nil:
        throw error("TypeError", "ffi.cast needs a struct type")
    return Struct(dc.d.st, addr(address), nil)

export func sizeof(text):
    let dc = type_of(text)
    let d = dc.d
    if dc.arr > 0:
        return scalar_size(d) * dc.arr
    return scalar_size(d)

export func offsetof(text, field):
    let dc = type_of(text)
    if dc.d.st == nil or not has(dc.d.st.fields, field):
        throw error("NameError", "no such struct field")
    return dc.d.st.fields[field].off

# string from an address or char array (n bytes, or up to the NUL)
export func string(x, n = nil):
    let a = addr(x)
    if a == 0:
        return nil
    return _ffi.string_at(a) if n == nil else _ffi.string_at(a, n)

export func abi():
    return _ffi.abi()

# ---- calling -------------------------------------------------------------------------
export class Callback:
    let a = 0
    func init(self, a):
        self.a = a
    func addr(self):
        return self.a
    func release(self):
        if self.a != 0:
            _ffi.release_callback(self.a)
            live[to_string(self.a)] = nil
            self.a = 0

# Sprint function -> C function pointer. ffi.callback("int(const void*, const void*)", fn)
export func callback(decl, fn):
    let pr = proto_from(decl)
    let a = _ffi.callback(fn, pr.sig)
    live[to_string(a)] = fn
    return Callback(a)

func make_fn(address, pr):
    let sig = pr.sig
    let name = pr.name
    let fnp = pr.fnp
    let ret_st = pr.ret_st
    let ret_val = pr.ret_val
    let sret = pr.sret
    let byval = pr.byval
    let nfixed = len(pr.args)
    func call(...args):
        let conv = []
        let temps = []
        for i in range(len(args)):
            let a = args[i]
            let t = type(a)
            if i < nfixed and byval[i] and t != "instance":
                throw error("TypeError", "argument " .. to_string(i + 1) .. " of " .. name .. " is a struct passed by value: pass an ffi.new(...) struct")
            if t == "instance" or t == "native":
                a = addr(a)
            elif t == "function" and i < nfixed and fnp[i] != nil:
                let cb = _ffi.callback(a, fnp[i].sig)
                temps.append(cb)
                a = cb
            conv.append(a)
        defer release_all(temps)
        if ret_val != nil:
            let b = alloc_view(ret_val.size)
            if sret:
                conv.insert(0, b.ptr())
                _ffi.call(address, sig, conv)
            else:
                _ffi.copy_in(b.ptr(), _ffi.call(address, sig, conv))
            return Struct(ret_val, b.ptr(), b)
        let r = _ffi.call(address, sig, conv)
        if ret_st != nil:
            return nil if r == 0 else Struct(ret_st, r, nil)
        return r
    return call

func release_all(temps):
    for a in temps:
        _ffi.release_callback(a)

# function from a raw address: ffi.fn_at(p, "int(int, int)")
export func fn_at(address, decl):
    return make_fn(addr(address), proto_from(decl))

export class Lib:
    let h = nil
    func init(self, h):
        self.h = h
    func addr(self, name):
        return _ffi.sym(self.h, name)
    # function `name`: its prototype comes from ffi.cdef, or from `decl` ("double sqrt(double)")
    func fn(self, name, decl = nil):
        let pr = nil
        if decl != nil:
            pr = proto_from(decl)
        elif has(protos, name):
            pr = protos[name]
        else:
            throw error("NameError", "no prototype for '" .. name .. "': call ffi.cdef first or pass a declaration")
        return make_fn(_ffi.sym(self.h, name), pr)
    # table of functions: lib.bind(["sqrt", "pow"]).sqrt(2.0)
    func bind(self, names):
        let t = {}
        for n in names:
            t[n] = self.fn(n)
        return t

# Open a shared library (nil = the running program, which already links libc and libm).
export func open(name = nil):
    return Lib(_ffi.open(name))
