#ifndef COS_ERRNO_H
#define COS_ERRNO_H

/* C-OS has no per-thread/per-process errno today (no POSIX syscall
 * layer to set it from) - this is a single kernel-wide value, good
 * enough for QuickJS's handful of internal uses (it checks errno
 * after a couple of libc-shaped calls like strtod's overflow case,
 * not for anything requiring real thread-local semantics). */
extern int errno;

/* MicroPython's mperrno.h (third_party/micropython/py/mperrno.h)
 * expects the full standard POSIX errno set to already be defined
 * when it includes <errno.h> - not just the handful QuickJS itself
 * needs - so this covers everything it references, even though
 * C-OS's own kernel code doesn't otherwise have a use for most of
 * these (no real POSIX syscalls to fail with e.g. ECONNREFUSED). */
#define EPERM       1
#define ENOENT      2
#define ESRCH       3
#define EINTR       4
#define EIO         5
#define ENXIO       6
#define ENOEXEC     8
#define EBADF       9
#define ECHILD      10
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define ENOTBLK     15
#define EBUSY       16
#define EEXIST      17
#define EXDEV       18
#define ENODEV      19
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENFILE      23
#define EMFILE      24
#define ENOTTY      25
#define ETXTBSY     26
#define EFBIG       27
#define ENOSPC      28
#define ESPIPE      29
#define EROFS       30
#define EMLINK      31
#define EPIPE       32
#define EDOM        33
#define ERANGE      34
#define EDEADLK     35
#define ENAMETOOLONG 36
#define ENOLCK      37
#define ENOSYS      38
#define ENOTEMPTY   39
#define ELOOP       40
#define EWOULDBLOCK EAGAIN
#define ENOMSG      42
#define EIDRM       43
#define ENOSTR      60
#define ENODATA     61
#define ETIME       62
#define ENOSR       63
#define EPROTO      71
#define EBADMSG     74
#define EOVERFLOW   75
#define EILSEQ      84
#define ENOTSOCK    88
#define EDESTADDRREQ 89
#define EMSGSIZE    90
#define EPROTOTYPE  91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP  95
#define EAFNOSUPPORT 97
#define EADDRINUSE  98
#define EADDRNOTAVAIL 99
#define ENETDOWN    100
#define ENETUNREACH 101
#define ENETRESET   102
#define ECONNABORTED 103
#define ECONNRESET  104
#define ENOBUFS     105
#define EISCONN     106
#define ENOTCONN    107
#define ETIMEDOUT   110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EALREADY    114
#define EINPROGRESS 115
#define ECANCELED   125

#endif
