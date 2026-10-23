#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
static int fails;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); fflush(stdout); if (!ok_) fails++; } while (0)
static volatile int got_usr1, got_alrm;
static void on_usr1(int s) { (void)s; got_usr1++; }
static void on_alrm(int s) { (void)s; got_alrm++; }
int main(int argc, char **argv) {
    int st;
    pid_t pid = fork();
    if (pid == 0) _exit(7);
    CHECK("fork + waitpid collects the exit status", waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7);

    int pf[2]; pipe(pf);
    char big[100]; memset(big, 'x', sizeof big);
    int shared = 5;
    pid = fork();
    if (pid == 0) { shared = 99; close(pf[0]); write(pf[1], "from child", 10); _exit(0); }
    close(pf[1]);
    char buf[64] = { 0 }; int n = read(pf[0], buf, sizeof buf);
    waitpid(pid, &st, 0);
    CHECK("a child writes into a pipe the parent reads", n == 10 && !strcmp(buf, "from child"));
    CHECK("the child has its own copy of memory", shared == 5);

    pid = fork();
    if (pid == 0) { char *a[] = { "hello_dyn", "execd", NULL }; execv("/linux-test/hello_dyn", a); _exit(127); }
    CHECK("fork + execve runs another program (exit status 3)", waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 3);

    pid_t sp; char *sa[] = { "hello_dyn", "spawned", NULL };
    int rc = posix_spawn(&sp, "/linux-test/hello_dyn", NULL, NULL, sa, environ);
    CHECK("posix_spawn", rc == 0 && waitpid(sp, &st, 0) == sp && WIFEXITED(st) && WEXITSTATUS(st) == 3);

    struct sigaction act; memset(&act, 0, sizeof act); act.sa_handler = on_usr1; sigaction(SIGUSR1, &act, NULL);
    raise(SIGUSR1);
    CHECK("raise runs the handler", got_usr1 == 1);
    kill(getpid(), SIGUSR1);
    CHECK("kill(getpid()) runs the handler", got_usr1 == 2);
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, NULL);
    raise(SIGUSR1); CHECK("a blocked signal waits", got_usr1 == 2);
    sigprocmask(SIG_UNBLOCK, &m, NULL); CHECK("... and arrives when unblocked", got_usr1 == 3);

    act.sa_handler = on_alrm; sigaction(SIGALRM, &act, NULL);
    alarm(1); pause();
    CHECK("alarm + pause", got_alrm == 1);

    pid = fork();
    if (pid == 0) { for (;;) sleep(1); }
    usleep(100000); kill(pid, SIGTERM);
    CHECK("SIGTERM ends a child", waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    CHECK("wait with no children gives ECHILD", waitpid(-1, &st, 0) == -1 && errno == ECHILD);
    printf("procs done, %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
