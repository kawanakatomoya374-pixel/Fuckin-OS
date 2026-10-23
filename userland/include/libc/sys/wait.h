#ifndef _COS_SYS_WAIT_H
#define _COS_SYS_WAIT_H
/* wait()-style status values as returned by system() and pclose() on C-OS: the exit code is in bits 8..15 */
#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0)
#define WTERMSIG(s)    ((s) & 0x7f)
#endif
