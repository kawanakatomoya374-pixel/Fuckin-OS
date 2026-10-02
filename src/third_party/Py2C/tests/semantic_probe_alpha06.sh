#!/bin/sh
# 小さなスニペットを大量に CPython と比較し、残っている意味論の差分を洗い出す。
# 使い方: sh tests/semantic_probe_alpha06.sh
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-cc}
PY=${PYTHON:-python3}
# 使うコンパイラを差し替えられるようにしておく（ASan 版での探索用）。
#   P2C_COMPILER=./build/sanitize/python-code-to-c sh tests/semantic_probe_alpha06.sh
P2C_COMPILER=${P2C_COMPILER:-./build/python-code-to-c}
OUT=build/probe
SRCS="src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c"

rm -rf "$OUT"
mkdir -p "$OUT" || exit 1
[ -x "$P2C_COMPILER" ] || { echo "semantic_probe: compiler not built (make all)"; exit 1; }

fail=0
total=0

probe() {
    name=$1
    code=$2
    total=$((total + 1))
    printf '%s\n' "$code" > "$OUT/$name.py"
    if ! "$P2C_COMPILER" "$OUT/$name.py" -o "$OUT/$name.c" > "$OUT/$name.tperr" 2>&1; then
        printf 'PROBE %-24s TRANSPILE-FAIL  %s\n' "$name" "$(head -1 "$OUT/$name.tperr")"
        fail=$((fail + 1))
        return
    fi
    # shellcheck disable=SC2086
    if ! $CC -I./include -std=gnu11 "$OUT/$name.c" $SRCS -lm -o "$OUT/$name" 2> "$OUT/$name.ccerr"; then
        printf 'PROBE %-24s BUILD-FAIL\n' "$name"
        fail=$((fail + 1))
        return
    fi
    "$PY" "$OUT/$name.py" > "$OUT/$name.pyout" 2>&1
    "$OUT/$name" > "$OUT/$name.cout" 2>&1
    if diff -q "$OUT/$name.pyout" "$OUT/$name.cout" > /dev/null; then
        printf 'PROBE %-24s ok\n' "$name"
    else
        printf 'PROBE %-24s DIFF\n' "$name"
        diff "$OUT/$name.pyout" "$OUT/$name.cout" | head -4 | sed 's/^/    /'
        fail=$((fail + 1))
    fi
}

probe dict_views 'd = {"a": 1, "b": 2}; print(list(d.keys()), list(d.values()), list(d.items()))'
probe dict_methods 'd = {"a": 1}; print(d.setdefault("b", 2), d, d.get("z"), d.get("z", 9))'
probe dict_update 'd = {}; d.update([("a", 1)]); print(sorted(d.items())); d.update({"c": 3}); print(sorted(d))'
probe dict_popitem 'd = {"a": 1, "b": 2}; print(d.popitem(), d)'
probe dict_merge 'a = {"x": 1}; b = {"y": 2}; print(sorted((a | b).items()), sorted(a.items()))'
probe dict_fromkeys 'print(dict.fromkeys([1, 2, 3], 0))'
probe dict_nested_eq 'd = {"a": [1, {"b": 2}]}; print(d == {"a": [1, {"b": 2}]}, d == {"a": [1, {"b": 3}]})'
probe set_ops2 'a = {1, 2, 3}; b = {2, 3, 4}; print(sorted(a | b), sorted(a & b), sorted(a - b), sorted(a ^ b))'
probe set_methods 'a = {1, 2}; a.add(3); a.discard(1); print(sorted(a), a.issubset({2, 3}), a.isdisjoint({9}))'
probe set_update 'a = {1}; a.update([2, 3]); print(sorted(a), len(a))'
probe list_methods 'a = [3, 1, 2]; a.sort(); print(a); a.reverse(); print(a); a.insert(0, 9); print(a, a.index(1), a.count(1))'
probe list_extend 'a = [1]; a.extend([2, 3]); a += [4]; print(a, a * 2, [0] + a)'
probe list_sort_key 'a = ["bb", "a", "ccc"]; a.sort(key=len); print(a); print(sorted(a, reverse=True))'
probe list_remove 'a = [1, 2, 3, 2]; a.remove(2); print(a, a.pop(), a.pop(0), a)'
probe list_slice_assign 'a = [1, 2, 3, 4]; a[1:3] = [9]; print(a); a[0:0] = [0]; print(a); del a[1:2]; print(a)'
probe list_copy 'a = [1, 2]; b = a.copy(); b.append(3); print(a, b, a is b)'
probe str_methods 's = "a,b,,c"; print(s.split(","), s.split(",", 1), s.rsplit(",", 1), s.partition(","))'
probe str_split_ws 'print("  a  b  ".split(), " a b ".strip().split(), "a b c".split(" "))'
probe str_join 'print(",".join(["a", "b"]), "-".join(()), "".join(["x", "y"]))'
probe str_replace 'print("aaa".replace("a", "b", 2), "abc".startswith(("x", "a")), "abc".endswith(("z", "c")))'
probe str_bounds 's = "hello world"; print(s.find("o"), s.find("o", 5), s.rfind("o"), s.find("z"), s.count("l", 4))'
probe str_case 'print("Hello World".lower(), "hEllo".upper(), "hI tHere".title(), "aBC".capitalize(), "aB".swapcase())'
probe str_pad 'print("ab".ljust(5, "."), "ab".rjust(5, "."), "ab".center(6, "-"), "7".zfill(3), "-7".zfill(4))'
probe str_isdigit 'print("123".isdigit(), "12a".isdigit(), "abc".isalpha(), "a1".isalnum(), "".isdigit())'
probe str_index_slice 's = "abcdef"; print(s[1], s[-1], s[1:3], s[::-1], s[::2], s[10:20], len(s))'
probe str_mul 'print("ab" * 3, ("x" * 0) == "", "a" in "abc", "z" not in "abc")'

probe str_format 'print("{} {} {}".format(1, "a", 2.5), "{1}{0}".format("a", "b"), "{x}".format(x=7))'
probe str_format_spec 'print("{:>5}|{:<5}|{:^5}|{:05d}|{:.2f}|{:+.1f}".format("a", "b", "c", 7, 3.14159, 2.0))'
probe str_percent 'print("%s %d %5.2f %x %o %e" % ("a", 3, 3.14159, 255, 8, 12345.678))'
probe percent_star 'print("%*d|%-*d|%.*f" % (5, 42, 5, 42, 2, 3.14159))'
probe int_ops 'print(7 // 2, -7 // 2, 7 % 3, -7 % 3, 7 % -3, divmod(7, 3), divmod(-7, 3))'
probe int_bases 'print(int("ff", 16), int("0xff", 16), int("777", 8), int("101", 2), int("-0x10", 16))'
probe int_builtin 'print(int("42"), int("-42"), int(3.9), int(True), abs(-5), abs(-2.5))'
probe float_ops 'print(round(2.5), round(3.5), round(2.345, 2), 1.5 + 2, 7 / 2, 2 ** 10, 2.0 ** 0.5)'
probe round_neg 'print(round(-2.5), round(-0.5), round(1.5), round(2.675, 2))'
probe bool_ops 'print(1 and 2, 0 or 3, not [], bool(""), bool("a"), bool(0.0), bool(None))'
probe compare_chain 'a = 2; print(1 < a < 3, 3 > a > 1, a == 2 != 3)'
probe compare_mixed 'print(1 == 1.0, 1 < 1.5, "a" < "b", [1, 2] < [1, 3], (1, 2) == (1, 2), [1] == (1,))'
probe is_none 'x = None; print(x is None, x is not None, None == None)'
probe func_defaults 'def f(a, b=2, *args, **kw): return (a, b, args, sorted(kw.items()))'
probe func_call 'def f(a, b=2, *args, **kw): return (a, b, args, sorted(kw.items()))
print(f(1), f(1, 3), f(1, 3, 4, 5), f(1, k=9), f(b=8, a=7))'
probe closures 'def outer(x):
    def inner(y):
        return x + y
    return inner

f = outer(10)
print(f(5), outer(1)(2))'
probe recursion 'def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

print([fib(i) for i in range(10)])'
probe lambda_sort 'pairs = [(1, "b"), (2, "a")]; print(sorted(pairs, key=lambda p: p[1]))'
probe enumerate_zip 'print(list(enumerate(["a", "b"], 1)), list(zip([1, 2], "ab")), list(zip([1], [2], [3])))'
probe map_filter 'print(list(map(lambda x: x * 2, [1, 2, 3])), list(filter(lambda x: x > 1, [1, 2, 3])))'
probe any_all 'print(any([0, 1]), all([1, 2]), any([]), all([]), any(x > 2 for x in [1, 3]))'
probe minmax_key 'print(min([(1, "b"), (2, "a")], key=lambda p: p[1]), max("bca"))'
probe sum_start 'print(sum([1, 2, 3]), sum([1.5, 2], 0.5), sum([], 10))'
probe unpack_assign 'a, b = 1, 2; a, b = b, a; print(a, b); x, rest = [1, 2]; print(x, rest)'
probe unpack_star_call 'def f(a, b, c): return a + b + c

print(f(*[1, 2, 3]), f(**{"a": 1, "b": 2, "c": 3}))'
probe comprehension 'print([x * x for x in range(5)], [x for x in range(10) if x % 3 == 0])'
probe comp_dict_set 'print({x: x * 2 for x in range(3)}, sorted({x % 3 for x in range(7)}))'
probe comp_nested 'print([(x, y) for x in range(2) for y in range(2)])'
probe fstring_fmt 'x = 3.14159; print(f"{x:.2f} {x:>8.3f} {42:05d}")'
probe ternary 'x = 5; print("big" if x > 3 else "small", (1 if x else 2))'
probe while_break 'i = 0
while True:
    i += 1
    if i > 3:
        break
print(i)'
probe for_else 'for i in range(3):
    pass
else:
    print("done")'
probe del_stmt 'a = [1, 2, 3]; del a[0]; print(a); d = {"a": 1}; del d["a"]; print(d)'
probe global_stmt 'g = 1

def bump():
    global g
    g += 1

bump()
print(g)'
probe nonlocal_stmt 'def outer():
    n = 0
    def inner():
        nonlocal n
        n += 1
        return n
    return inner()

print(outer())'

printf 'semantic_probe: %d probes, %d mismatches\n' "$total" "$fail"
[ "$fail" -eq 0 ]
