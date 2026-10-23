/* smp_user.h - run ring3 compute jobs on application processors.
 *
 * The general scheduler is BSP-only, so ordinary user threads cannot run in
 * parallel. This gives a process a narrow way to use the other cores: a job
 * is one user-mode function that runs to completion on an AP under the
 * process's own page tables. It must be pure computation: no system calls,
 * no blocking, no touching pages that are not already present (the AP cannot
 * service page faults). Anything else stops the job and reports a fault.
 * Typical use is a persistent worker loop that polls a queue in shared user
 * memory. */
#ifndef SMP_USER_H
#define SMP_USER_H

#include "types.h"

enum {
    SMP_USER_JOB_FREE = 0,
    SMP_USER_JOB_QUEUED = 1,
    SMP_USER_JOB_RUNNING = 2,
    SMP_USER_JOB_DONE = 3,     /* the function returned */
    SMP_USER_JOB_FAULTED = 4,  /* CPU exception or forbidden system call */
    SMP_USER_JOB_KILLED = 5    /* stopped on request or at process exit */
};

typedef struct smp_user_job {
    /* inputs (offsets are used by the assembly entry path) */
    uint64_t cr3;              /*  0 */
    uint64_t entry;            /*  8 */
    uint64_t arg;              /* 16 */
    uint64_t user_rsp;         /* 24: 16-byte aligned stack top */
    uint64_t exit_stub;        /* 32: user address of `int $0x82` */
    uint64_t fs_base;          /* 40 */
    volatile uint32_t kill_req;/* 48 */
    uint32_t claim;            /* CPU slot reserved for this job (a thief may still run it) */
    void *owner;               /* process that owns the job */
    /* results */
    volatile uint32_t state;
    volatile uint32_t cpu;
    volatile uint64_t fault_vec, fault_err, fault_rip, fault_cr2;
} smp_user_job_t;

#define SMP_USER_MAX_JOBS 16

/* Number of APs that can run user jobs. */
uint32_t smp_user_cpu_count(void);
/* Starts a job. Returns a job id (>= 0) or a negative errno. */
int smp_user_start(void *owner, uint64_t cr3, uint64_t entry, uint64_t arg,
                   uint64_t user_rsp, uint64_t exit_stub, uint64_t fs_base);
/* Returns the job state; fills `out` (may be NULL) with the result fields. */
int smp_user_poll(void *owner, int id, smp_user_job_t *out);
int smp_user_stop(void *owner, int id);
int smp_user_free(void *owner, int id);
/* Stops every job owned by `owner` and waits for them; called before the
 * process's page tables go away. */
void smp_user_kill_owner(void *owner);

#endif
