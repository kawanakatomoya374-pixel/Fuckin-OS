/* sandbox_test.c-os - regression target for SYS_SANDBOX (capabilities + file-system jail) and for ASLR.
 * Prints [SANDBOX] lines; tools/run_security_test.sh reads them. Launched by the kernel with VALIDATION_SANDBOX=1. */
#include <stdio.h>
#include <string.h>
#include "cos.h"

static int fails;
static void check(const char *name, int ok) {
    char b[160];
    snprintf(b, sizeof b, "[SANDBOX] %s %s\n", ok ? "PASS" : "FAIL", name);
    cos_puts(b);
    if (!ok) fails++;
}

int main(void) {
    int pid = cos_getpid();
    char b[160];
    int local;
    snprintf(b, sizeof b, "[SANDBOX] layout pid=%d stack=0x%lx heap=0x%lx code=0x%lx\n", pid, (unsigned long)(uintptr_t)&local,
             (unsigned long)(uintptr_t)cos_sbrk(0), (unsigned long)(uintptr_t)&main);
    cos_puts(b);

    /* Every instance runs the checks on its own files (named by pid), so concurrent runs do not interfere. */
    char jail[32], inside[48], outside[48], evil[48], dotdot[64], newf[48], sub[48];
    snprintf(jail, sizeof jail, "/sbj%d", pid);
    snprintf(inside, sizeof inside, "%s/inside.txt", jail);
    snprintf(outside, sizeof outside, "/sbo%d.txt", pid);
    snprintf(evil, sizeof evil, "%s_evil.txt", jail);
    snprintf(dotdot, sizeof dotdot, "%s/../sbo%d.txt", jail, pid);
    snprintf(newf, sizeof newf, "%s/new.txt", jail);
    snprintf(sub, sizeof sub, "%s/sub", jail);
    cos_mkdir(jail);
    cos_write_file(inside, "inside", 6);
    cos_write_file(outside, "outside", 7);

    check("starts with every capability", cos_sandbox(COS_CAP_ALL, NULL) == (int64_t)COS_CAP_ALL);

    int64_t left = cos_sandbox(COS_CAP_FS_READ | COS_CAP_FS_WRITE | COS_CAP_WINDOW, jail);
    check("sandbox call reports the remaining capabilities", left == (COS_CAP_FS_READ | COS_CAP_FS_WRITE | COS_CAP_WINDOW));

    char buf[16];
    check("read inside the jail works", cos_read_file(inside, buf, sizeof buf) == 6);
    check("write inside the jail works", cos_write_file(newf, "n", 1) == 1);
    check("read outside the jail is refused", cos_read_file(outside, buf, sizeof buf) < 0);
    check("write outside the jail is refused", cos_write_file(evil, "e", 1) < 0);
    check("'..' cannot climb out of the jail", cos_read_file(dotdot, buf, sizeof buf) < 0);
    check("relative paths are refused in a jail", cos_read_file("inside.txt", buf, sizeof buf) < 0);

    uint8_t ip[4];
    check("network is denied after dropping COS_CAP_NET", cos_net_resolve("example.com", ip) < 0);
    check("spawning is denied after dropping COS_CAP_SPAWN", cos_spawn(inside) < 0);
    check("device inventory is denied", cos_devices_list((cos_device_info_t *)buf, 1) < 0);

    check("capabilities cannot be regained", cos_sandbox(COS_CAP_ALL, NULL) == left);
    check("the jail cannot be widened", cos_sandbox(COS_CAP_ALL, "/") < 0 && cos_read_file(outside, buf, sizeof buf) < 0);
    check("the jail cannot be moved elsewhere", cos_sandbox(COS_CAP_ALL, "/sb_elsewhere") < 0);
    check("the jail can be narrowed", cos_sandbox(COS_CAP_ALL, sub) >= 0 && cos_read_file(inside, buf, sizeof buf) < 0);

    snprintf(b, sizeof b, "[SANDBOX] DONE failed=%d\n", fails);
    cos_puts(b);
    return fails;
}
