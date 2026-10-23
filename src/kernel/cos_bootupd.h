/* cos_bootupd.h - kernel updates (cos_bootupd.c). cos_kupd_status_t must match userland/include/cos.h. */
#ifndef COS_BOOTUPD_H
#define COS_BOOTUPD_H
#include <stdint.h>
#include <stddef.h>
typedef struct {
    int32_t  supported;          /* 1: this system can take a kernel update; else `why` says why not */
    char     running_slot;       /* 'a' / 'b' (the slot this kernel was started from), '?' unknown */
    char     active_slot;        /* the slot GRUB starts when nothing is on trial */
    char     trial_slot;         /* 0, or the slot that was installed and is waiting to be tried / confirmed */
    char     pad;
    uint64_t running_serial;     /* build number of the running kernel (make KERNEL_SERIAL=N) */
    uint64_t trial_serial;       /* serial of the kernel on trial, 0 if none */
    uint64_t bad_serial;         /* updates up to this serial failed to start here and are not installed again */
    char     why[96];
} cos_kupd_status_t;
int  cos_bootupd_status(cos_kupd_status_t *out);
/* blob = gzip kernel + 81-byte signed trailer (tools/sign_kernel.py). 0 ok, negative: refused (why says why) */
int  cos_bootupd_install(const uint8_t *blob, size_t len, char *why, size_t whycap);
void cos_bootupd_boot(void);     /* at every start, once the file systems are up */
#endif
