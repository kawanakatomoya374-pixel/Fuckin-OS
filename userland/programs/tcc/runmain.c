/* runmain.c - the tiny entry stub `tcc -run` needs (upstream tccrun.c calls it
 * "runmain.o", looked up via -B<dir>/libtcc1.a's directory - see tcc_cos.c).
 *
 * Upstream's own lib/runmain.c (kept, unmodified, alongside this file for reference)
 * additionally defines exit()/atexit()/on_exit() and runs .init_array/.fini_array
 * constructors/destructors, unwinding back into the compiler via a longjmp-style
 * __rt_exit so a -run'd program's exit() doesn't kill the host tcc process.
 *
 * None of that is needed - or safe - to duplicate here: libcos.a (linked in for
 * -run same as for a normal build - see tcc_cos.c) already defines exit() and
 * atexit(), and linking both would be a duplicate-symbol error. The simplification
 * this environment allows: when the JIT'd program calls libcos's exit() (a raw
 * cos_exit() syscall) the ENTIRE tcc.c-os process ends immediately with that exact
 * code - which is already the exit code tcc.c-os itself should report, so nothing
 * is lost by not unwinding back into the compiler first. C++-style static
 * constructors are not a thing this environment's plain-C build model runs for a
 * normal program either (cos_crt0.o does not process .init_array), so skipping
 * them here keeps -run's behaviour consistent with a normal build's. */
/* main.c never sees libcos's own globals get set up the normal way - __cos_rt_start()
 * (cos_rtld.c) does that, and -run's JIT'd code skips it entirely (see the file comment
 * above): it is called straight from tcc_run() as a plain function, not launched as a
 * fresh process. Left unset, cos_environ stays NULL in the -run'd code's own copy of
 * libcos's globals (relocated into the JIT image right along with everything else it
 * linked against - not shared with tcc.c-os's own copy), and anything that reads it -
 * cos_getenv(), and through it cos_write()'s COS_STDOUT lookup - silently finds nothing:
 * output the person is actively looking at in Studio's terminal (which runs tcc.c-os with
 * COS_STDOUT set) would simply never arrive. cos_argc/cos_argv are set for the same
 * reason: some libcos code may reasonably expect them populated once a program is
 * running, "-run" included, rather than only via main()'s own parameters. */
extern int cos_argc;
extern char **cos_argv;
extern char **cos_environ;

int main(int argc, char **argv, char **envp);

int _runmain(int argc, char **argv, char **envp)
{
    cos_argc = argc;
    cos_argv = argv;
    cos_environ = envp;
    return main(argc, argv, envp);
}
