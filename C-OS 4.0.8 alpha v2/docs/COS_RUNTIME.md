# The .c-os / .c-osll runtime

What a C-OS program can do, and what it could not do before.

Companion to `docs/ELF_LOADER.md`, which covers loading and linking.
This covers what the loaded program has available once it runs.

---

## 1. What was added

| Area | Before | Now |
|---|---|---|
| `printf` | `%s %c %d %i %u %x %X %p` and `l`. No flags, no width, no precision, no floats — `%-20s` came out as literal text | Complete C99: flags `-+ #0`, width and precision (incl. `*`), length `hh h l ll z t j L`, conversions `d i u o x X c s p f F e E g G` |
| Floating point | none anywhere | exact conversion, ties-to-even, verified against glibc over 40,000 random doubles |
| Math library | **none at all** | 30 functions, accuracy measured against the host libm |
| Thread sync | **nothing** — threads existed with no mutex, no condvar, not even a documented atomic | futex-backed mutex, condvar, semaphore, rwlock, `once` |
| Spawn arguments | impossible — `SYS_SPAWN` took only a path, so every child saw `argc == 1` | `cos_spawn_argv(path, argv, envp)` |
| Environment | none | `envp` on the initial stack, `cos_getenv()` |
| TLS | none | `__thread` works, per-thread blocks, `__tls_get_addr` |
| Constructors | none | `__attribute__((constructor))` / `(destructor)`, `cos_atexit` |
| auxv | none | full vector, `cos_getauxval()` |

---

## 2. Formatting

```c
cos_printf("%-12s %8.3f %+d %#x\n", name, value, delta, flags);
cos_snprintf(buf, sizeof buf, "%.*s", n, text);
```

Supported: `%d %i %u %o %x %X %c %s %p %f %F %e %E %g %G %%`, flags
`- + space # 0`, width and precision (literal or `*`), length modifiers
`hh h l ll z t j L`.

`%n` is **refused**, not ignored — it writes through a pointer from the
argument list, which is the classic format-string exploit primitive. A
program using it gets a visible `<%n refused>` marker rather than a
silent arbitrary write.

Float conversion is **exact**. A double is `m · 2^e` with `m` a 53-bit
integer, so its decimal expansion is finite and computable with integer
arithmetic alone. The obvious implementation — peel digits by repeatedly
multiplying by ten — accumulates error precisely where it matters, on
the tie test for the last kept digit. `%.3f` of `0.0005` is the case
that caught it: the true value is `5.0000000000000001e-04`, strictly
above the tie, so it must round to `0.001`; after four floating
multiplications the residue had drifted to exactly `5.0`, the code saw a
tie, rounded to even, and printed `0.000`.

Rounding is ties-to-**even** (the IEEE default, and what glibc does), so
`%.0f` of `2.5` is `"2"`. Note that `cos_round()` is ties-**away**, per
`round()`. The two genuinely differ; conflating them is a classic
off-by-one.

---

## 3. Math

```c
double d = cos_hypot(dx, dy);
double a = cos_atan2(dy, dx);
```

`sqrt cbrt hypot exp exp2 log log2 log10 pow sin cos tan asin acos atan
atan2 sinh cosh tanh floor ceil trunc round fmod modf frexp ldexp fabs
copysign fmin fmax isnan isinf isfinite signbit`

Accuracy is a few ULP across the normal range. That is **measured**, not
asserted: every function is fuzzed against the host libm over 20,000
random inputs per function and the build fails if the relative error
exceeds a stated bound. If you tighten a bound and it still passes, the
bound was loose.

No `errno`, no FP exception flags — this runtime has neither, and a
function that pretended to set `errno` would be lying. Domain errors
return NaN.

---

## 4. Threads and synchronization

```c
static cos_mutex_t lock = COS_MUTEX_INIT;
static cos_cond_t  ready = COS_COND_INIT;

cos_mutex_lock(&lock);
while (!condition) cos_cond_wait(&ready, &lock);   /* loop, always */
cos_mutex_unlock(&lock);
```

`cos_mutex_t`, `cos_cond_t`, `cos_sem_t`, `cos_rwlock_t`, `cos_once_t`,
plus the raw `cos_futex_wait` / `cos_futex_wake` for building anything
these do not cover.

**Every wait can return spuriously. Re-check your condition in a loop.**
That is not defensive padding: futex hash buckets are shared, so an
unrelated wake can reach your waiter, and condition variables are
specified to allow it regardless.

### Why a futex and not kernel mutex objects

Kernel objects would make every lock and unlock a ring transition even
when uncontended — the overwhelmingly common case — and would leave the
kernel owning a table whose lifetime it cannot see (a program that exits
holding a lock, or frees the memory a lock lives in).

With a futex the lock word lives in the program's own memory and is
manipulated with atomic instructions; the kernel is entered only when a
thread must actually sleep. An uncontended lock is one `lock cmpxchg`
and no syscall. The kernel holds no per-lock state, so there is nothing
to leak when a program dies.

The mutex uses three states (`0` free, `1` held, `2` held-with-waiters)
rather than two. With two, unlock cannot tell whether anyone is waiting
and must issue a wake syscall every time. With three, it only enters the
kernel when the word says someone is parked.

The rwlock has a writer-waiting bit. Without it a steady stream of
readers holds the lock indefinitely and a writer never acquires it —
which looks like a hang and is very hard to diagnose, because every
individual operation is behaving correctly.

`cos_once` has a distinct in-progress state. A thread arriving mid-init
**waits** rather than proceeding with half-built state; the naive
`if (!done) { init(); done = 1; }` gets that wrong in a way that only
appears under load.

---

## 5. Process startup

The initial stack now carries `argv`, `envp` **and** an auxiliary vector:

```c
int main(int argc, char **argv, char **envp);   /* two-arg main still fine */

char *home = cos_getenv("HOME");
uint64_t pagesz = cos_getauxval(6 /* AT_PAGESZ */);
```

Spawning with arguments:

```c
const char *argv[] = { "child.c-os", "--verbose", NULL };
const char *envp[] = { "LANG=ja_JP.UTF-8", NULL };
int64_t pid = cos_spawn_argv("/bin/child.c-os", argv, envp);
```

`cos_spawn()` remains available and still passes nothing — the syscall
behind it takes only a path.

Constructors and destructors run via the ring3 loader half
(`cos_rtld.c`), which reads the kernel's link map, applies ifunc
relocations, then runs each object's init arrays with dependencies
first. See `docs/ELF_LOADER.md` §4 for why the kernel does not do this.

---

## 6. Testing

```sh
make check-host          # both suites
make check-userland      # just this runtime
```

**279 checks across the two suites**, all green.

The formatter and math tests are **differential**: each case runs
through both the C-OS implementation and the host's, and the outputs are
compared — plus 80,000 randomised inputs. A golden-string test only
proves the formatter agrees with whatever its author believed C99 says,
and the corners (`%.0f` of `2.5`, `%#o` of zero, a negative `*` width)
are exactly where the author is wrong.

The lock tests run under **real concurrent host threads** forwarding to
the **real Linux futex**, because a lock that is subtly wrong passes
every single-threaded assertion. What finds a missing barrier or a lost
wakeup is eight threads hammering one word 20,000 times.

### Bugs this found

1. **`ldexp` subtracted the wrong amount** in its range-reduction loop —
   multiplied by `2^1023` while subtracting only `1000`. `ldexp(1.0,
   1023)` returned infinity, and `exp()` inherited it for every argument
   above ~693. Found by the math fuzz, not by any hand-written case.
2. **`%f` rounded ties away from zero** where every other C library
   rounds to even.
3. **`%g` never stripped trailing zeros from its scientific form**, so
   `1000000.0` printed as `1.00000e+06`.
4. **`%e` of `0.0` omitted the exponent**, producing something that
   looks like a valid `%f`.
5. **Exact conversion needed `__udivti3`** from libgcc, which
   `-nostdlib` excludes — it compiled fine and failed to *link* in the
   environment it ships to. Rewritten in 32-bit halves; `cos-cc` now
   also passes `-lgcc` so user code hitting the same wall gets a link
   rather than an error naming a symbol nobody recognises.

---

## 7. Known gaps

- **No `errno`.** Functions return `-1` or NaN. Adding it is easy now
  that TLS works; it is not there yet.
- **No `strtod`/`atof`** — parsing floats is not implemented, only
  formatting them.
- **No `%ls`/`%lc`** — there is no wide-character support to convert to.
- **`long double` is formatted as `double`.** `%Lf` is accepted and the
  value is read as a double, which is what x86-64 code overwhelmingly
  means, but it is not the 80-bit format.
- **No barriers or thread-local destructors.** The primitives to build a
  barrier are exported (`cos_futex_*`); the barrier itself is not.
- **`dlopen` of a library with its own `PT_TLS` is refused** after
  startup — the static TLS block is fixed once the process starts.
- **Math range reduction is not multi-precision.** `sin`/`cos` are
  accurate to the stated bound for `|x|` up to a few million; beyond
  that the reduction dominates the error. A serious libm uses a
  1000-bit π for that case. This one does not, and the test bounds cover
  the range where the claim holds.
