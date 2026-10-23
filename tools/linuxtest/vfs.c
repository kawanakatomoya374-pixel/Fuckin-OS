#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int fails;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); if (!ok_) fails++; } while (0)
static int slurp(const char *path, char *buf, int cap) {
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    int n = 0, r; while (n < cap - 1 && (r = read(fd, buf + n, cap - 1 - n)) > 0) n += r;
    buf[n] = 0; close(fd); return n;
}
int main(void) {
    char buf[4096];
    int fd = open("/dev/null", O_RDWR);
    CHECK("/dev/null write", fd >= 0 && write(fd, "abc", 3) == 3);
    CHECK("/dev/null read is EOF", read(fd, buf, 8) == 0); close(fd);
    fd = open("/dev/zero", O_RDONLY); memset(buf, 1, 16);
    CHECK("/dev/zero", fd >= 0 && read(fd, buf, 16) == 16 && buf[0] == 0 && buf[15] == 0); close(fd);
    fd = open("/dev/urandom", O_RDONLY); char a[16], b[16];
    CHECK("/dev/urandom differs", fd >= 0 && read(fd, a, 16) == 16 && read(fd, b, 16) == 16 && memcmp(a, b, 16) != 0); close(fd);
    struct stat st;
    CHECK("stat /dev/null is a character device", stat("/dev/null", &st) == 0 && S_ISCHR(st.st_mode));
    CHECK("access /proc/self/status", access("/proc/self/status", R_OK) == 0);
    CHECK("/proc/self/status has Name", slurp("/proc/self/status", buf, sizeof buf) > 0 && strstr(buf, "Name:"));
    CHECK("/proc/self/maps lists mappings", slurp("/proc/self/maps", buf, sizeof buf) > 0 && strstr(buf, "r-xp") != NULL);
    CHECK("/proc/cpuinfo", slurp("/proc/cpuinfo", buf, sizeof buf) > 0 && strstr(buf, "processor"));
    CHECK("/proc/meminfo", slurp("/proc/meminfo", buf, sizeof buf) > 0 && strstr(buf, "MemTotal:"));
    CHECK("/proc/uptime", slurp("/proc/uptime", buf, sizeof buf) > 0);
    fd = open("/proc/self/status", O_RDONLY); read(fd, buf, 4);
    CHECK("lseek on a /proc file", lseek(fd, 0, SEEK_SET) == 0 && read(fd, buf, 4) == 4 && !memcmp(buf, "Name", 4)); close(fd);
    CHECK("missing /proc entry", open("/proc/nonexistent", O_RDONLY) < 0);
    CHECK("sysconf cpus", sysconf(_SC_NPROCESSORS_ONLN) >= 1);
    {
        int hit_self = 0, hit_cpu = 0, hit_pid = 0; char pidname[16]; snprintf(pidname, sizeof pidname, "%d", getpid());
        DIR *d = opendir("/proc"); struct dirent *de;
        while (d && (de = readdir(d))) { if (!strcmp(de->d_name, "self")) hit_self = 1; if (!strcmp(de->d_name, "cpuinfo")) hit_cpu = 1; if (!strcmp(de->d_name, pidname)) hit_pid = 1; }
        CHECK("listing /proc", d && hit_self && hit_cpu && hit_pid); if (d) closedir(d);
        int null_seen = 0; d = opendir("/dev");
        while (d && (de = readdir(d))) if (!strcmp(de->d_name, "null")) null_seen = 1;
        CHECK("listing /dev", d && null_seen); if (d) closedir(d);
        int fd2 = open("/dev/null", O_RDONLY), seen = 0; char want[16]; snprintf(want, sizeof want, "%d", fd2);
        d = opendir("/proc/self/fd");
        while (d && (de = readdir(d))) if (!strcmp(de->d_name, want)) seen = 1;
        CHECK("listing /proc/self/fd", d && seen); if (d) closedir(d);
        char lnk[200], pth[64]; snprintf(pth, sizeof pth, "/proc/self/fd/%d", fd2);
        ssize_t ln = readlink(pth, lnk, sizeof lnk - 1); if (ln > 0) lnk[ln] = 0;
        CHECK("readlink /proc/self/fd/N", ln > 0 && !strcmp(lnk, "/dev/null"));
        snprintf(pth, sizeof pth, "/proc/%d/status", getpid());
        CHECK("/proc/<pid>/status", slurp(pth, buf, sizeof buf) > 0 && strstr(buf, "Name:"));
    }
    printf("%s\n", fails ? "VFS FAILED" : "VFS OK");
    return fails != 0;
}
