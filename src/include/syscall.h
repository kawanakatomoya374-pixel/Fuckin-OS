#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

/*
 * Syscall numbers
 * Keep Linux-compatible historical values where possible, but avoid duplicate
 * macro names so the header stays warning-free in freestanding builds.
 */
#define SYS_EXIT          1
#define SYS_FORK          2
#define SYS_READ          3
#define SYS_WRITE         4
#define SYS_OPEN          5
#define SYS_CLOSE         6
#define SYS_WAIT          7
#define SYS_CREAT         8
#define SYS_LINK          9
#define SYS_UNLINK        10
#define SYS_EXEC          11
#define SYS_CHDIR         12
#define SYS_TIME          13
#define SYS_MKNOD         14
#define SYS_CHMOD         15
#define SYS_LCHOWN        16
#define SYS_BREAK         17
#define SYS_OLDSTAT       18
#define SYS_LSEEK         19
#define SYS_GETPID        20
#define SYS_MOUNT         21
#define SYS_UMOUNT        22
#define SYS_SETUID        23
#define SYS_GETUID        24
#define SYS_STIME         25
#define SYS_PTRACE        26
#define SYS_ALARM         27
#define SYS_FSTAT_OLD     28
#define SYS_PAUSE         29
#define SYS_UTIME         30
#define SYS_STTY          31
#define SYS_GTTY          32
#define SYS_ACCESS        33
#define SYS_NICE          34
#define SYS_FTIME         35
#define SYS_SYNC          36
#define SYS_KILL          37
#define SYS_RENAME        38
#define SYS_MKDIR         39
#define SYS_RMDIR         40
#define SYS_DUP           41
#define SYS_PIPE          42
#define SYS_TIMES         43
#define SYS_PROF          44
#define SYS_BRK           45
#define SYS_SETGID        46
#define SYS_GETGID        47
#define SYS_SIGNAL        48
#define SYS_GETEUID       49
#define SYS_GETEGID       50
#define SYS_ACCT          51
#define SYS_UMOUNT2       52
#define SYS_LOCK          53
#define SYS_IOCTL         54
#define SYS_FCNTL         55
#define SYS_MPX           56
#define SYS_SETPGID       57
#define SYS_ULIMIT        58
#define SYS_OLDOLDUNAME   59
#define SYS_UMASK         60
#define SYS_CHROOT        61
#define SYS_USTAT         62
#define SYS_DUP2          63
#define SYS_GETPPID       64
#define SYS_GETPGRP       65
#define SYS_SETSID        66
#define SYS_SIGACTION     67
#define SYS_SGETMASK      68
#define SYS_SSETMASK      69
#define SYS_SETREUID      70
#define SYS_SETREGID      71
#define SYS_SIGSUSPEND    72
#define SYS_SIGPENDING    73
#define SYS_SETHOSTNAME   74
#define SYS_SETRLIMIT     75
#define SYS_GETRLIMIT     76
#define SYS_GETRUSAGE     77
#define SYS_GETTIMEOFDAY  78
#define SYS_SETTIMEOFDAY  79
#define SYS_GETGROUPS     80
#define SYS_SETGROUPS     81
#define SYS_SELECT_OLD    82
#define SYS_SYMLINK       83
#define SYS_OLDLSTAT      84
#define SYS_READLINK      85
#define SYS_USELIB        86
#define SYS_SWAPON        87
#define SYS_REBOOT        88
#define SYS_READDIR       89
#define SYS_MMAP          90
#define SYS_MUNMAP        91
#define SYS_TRUNCATE      92
#define SYS_FTRUNCATE     93
#define SYS_FCHMOD        94
#define SYS_FCHOWN        95
#define SYS_GETPRIORITY   96
#define SYS_SETPRIORITY   97
#define SYS_PROFIL        98
#define SYS_STATFS        99
#define SYS_FSTATFS       100
#define SYS_IOPERM        101
#define SYS_SOCKETCALL    102
#define SYS_SYSLOG        103
#define SYS_SETITIMER     104
#define SYS_GETITIMER     105
#define SYS_STAT          106
#define SYS_LSTAT         107
#define SYS_FSTAT         108
#define SYS_OLDUNAME      109
#define SYS_IOPL          110
#define SYS_VHANGUP_OLD   111
#define SYS_IDLE          112
#define SYS_VM86OLD       113
#define SYS_WAIT4         114
#define SYS_SWAPOFF       115
#define SYS_SYSINFO       116
#define SYS_IPC           117
#define SYS_FSYNC         118
#define SYS_SIGRETURN     119
#define SYS_CLONE         120
#define SYS_SETDOMAINNAME 121
#define SYS_NEWUNAME      122
#define SYS_MODIFY_LDT_OLD 123
#define SYS_ADJTIMEX      124
#define SYS_MPROTECT      125
#define SYS_SIGPROCMASK   126
#define SYS_CREATE_MODULE 127
#define SYS_INIT_MODULE   128
#define SYS_DELETE_MODULE 129
#define SYS_GET_KERNEL_SYMS 130
#define SYS_QUOTACTL      131
#define SYS_GETPGID       132
#define SYS_FCHDIR        133
#define SYS_BDFLUSH       134
#define SYS_SYSFS         135
#define SYS_PERSONALITY   136
#define SYS_SETFSUID      137
#define SYS_SETFSGID      138
#define SYS_LLSEEK        139
#define SYS_GETDENTS      140
#define SYS_SELECT        141
#define SYS_FLOCK         142
#define SYS_MSYNC         143
#define SYS_READV         144
#define SYS_WRITEV        145
#define SYS_GETSID        146
#define SYS_FDATASYNC     147
#define SYS_SYSCTL        148
#define SYS_MLOCK         149
#define SYS_MUNLOCK       150
#define SYS_MLOCKALL      151
#define SYS_MUNLOCKALL    152
#define SYS_VHANGUP       153
#define SYS_MODIFY_LDT    154
#define SYS_PIVOT_ROOT    155
#define SYS_GETCWD        156
#define SYS_NANOSLEEP     157
#define SYS_MQ_TIMEDRECEIVE 158
#define SYS_GETTID        159
#define SYS_YIELD         160
#define SYS_THREAD_CREATE 161
#define SYS_THREAD_EXIT   162
#define SYS_THREAD_JOIN   163

#define SYS_MAX 256

/* Syscall error codes */
#define SYSCALL_EPERM   1
#define SYSCALL_ENOENT  2
#define SYSCALL_ESRCH   3
#define SYSCALL_EINTR   4
#define SYSCALL_EIO     5
#define SYSCALL_ENXIO   6
#define SYSCALL_E2BIG   7
#define SYSCALL_ENOEXEC 8
#define SYSCALL_EBADF   9
#define SYSCALL_ECHILD  10
#define SYSCALL_EAGAIN  11
#define SYSCALL_ENOMEM  12
#define SYSCALL_EACCES  13
#define SYSCALL_EFAULT  14
#define SYSCALL_ENOTBLK 15
#define SYSCALL_EBUSY   16
#define SYSCALL_EEXIST  17
#define SYSCALL_ENODEV  18
#define SYSCALL_ENOTDIR 19
#define SYSCALL_EISDIR  20
#define SYSCALL_EINVAL  22
#define SYSCALL_ENFILE  23
#define SYSCALL_EMFILE  24
#define SYSCALL_ENOTTY  25
#define SYSCALL_ETXTBSY 26
#define SYSCALL_EFBIG   27
#define SYSCALL_ENOSPC  28
#define SYSCALL_ESPIPE  29
#define SYSCALL_EROFS   30
#define SYSCALL_EMLINK  31
#define SYSCALL_EPIPE   32
#define SYSCALL_EDOM    33
#define SYSCALL_ERANGE  34
#define SYSCALL_ENOSYS  38

/* Register structure for interrupts */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef int64_t (*syscall_handler_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

typedef struct {
    syscall_handler_t handler;
    int num_args;
    const char* name;
} syscall_entry_t;

void syscall_init(void);
void syscall_install_handler(int num, syscall_handler_t handler, int num_args, const char* name);
void syscall_handler(struct regs *r);
int64_t syscall_dispatch(int num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);

void syscall_enable_tracing(void);
void syscall_disable_tracing(void);
void syscall_set_tracing(int num, int enable);
void syscall_trace(int num, uint64_t args[5], int result);

int sys_exit(int code);
int sys_fork(void);
int sys_exec(const char* path, char* const argv[], char* const envp[]);
int sys_wait(int* status);
int sys_kill(int pid, int sig);
int sys_getpid(void);
int sys_getppid(void);
int sys_gettid(void);
int sys_yield(void);
int sys_sleep(uint64_t ms);

int sys_brk(void* addr);

int sys_open(const char* pathname, int flags, int mode);
int sys_close(int fd);
ssize_t sys_read(int fd, void* buf, size_t count);
ssize_t sys_write(int fd, const void* buf, size_t count);
int64_t sys_seek(int fd, int64_t offset, int whence);

int sys_chdir(const char* path);
int sys_getcwd(char* buf, size_t size);
int sys_mkdir(const char* path, int mode);
int sys_rmdir(const char* path);

int sys_signal(int sig, void (*handler)(int));

uint64_t sys_time(void);
int sys_gettimeofday(void* tv, void* tz);
int sys_nanosleep(const void* req, void* rem);

int sys_thread_create(void* entry, void* arg);
void sys_thread_exit(int code);
int sys_thread_join(int tid, int* status);

#endif // SYSCALL_H
