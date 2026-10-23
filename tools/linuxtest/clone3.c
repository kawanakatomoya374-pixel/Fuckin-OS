#define _GNU_SOURCE
#include <linux/sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    struct clone_args ca; memset(&ca, 0, sizeof ca);
    ca.exit_signal = SIGCHLD;
    long pid = syscall(SYS_clone3, &ca, sizeof ca);
    if (pid == 0) { printf("clone3 child\n"); fflush(stdout); _exit(7); }
    int st = 0; long w = waitpid(pid, &st, 0);
    printf("%s clone3 fork-style: pid %ld, child status %d\n", w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7 ? "PASS" : "FAIL", pid, WEXITSTATUS(st));
    return 0;
}
