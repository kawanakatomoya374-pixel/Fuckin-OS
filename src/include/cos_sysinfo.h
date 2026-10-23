/* cos_sysinfo.h - snapshot structs for SYS_SYSTEM_INFO / SYS_PROCLIST. Mirrored in userland/include/cos.h. */
#ifndef COS_SYSINFO_H
#define COS_SYSINFO_H

#include <stdint.h>

typedef struct {
    uint64_t uptime_ticks, ctx_switches, load_now, load_1m, load_5m, load_15m;
    uint64_t ram_total, kheap_used, kheap_free, pages_total, pages_free;
    uint64_t stor_total, stor_used, stor_free;
    uint64_t ustor_total, ustor_free;
    uint32_t ustor_devices, has_password, cpus_online, cpus_possible, cpus_bios, has_apic, has_acpi, reserved;
    char cpu_vendor[16], bios_vendor[16], bios_version[16], bios_date[16];
    char os_name[48], os_version[32], boot_msg[96];
    uint32_t pcache_frames, pcache_maps;      /* shared read-only pages (cos_pcache.c): frames held, page-table entries that map them */
} cos_sysinfo_t;

enum { COS_PROC_STATE_CREATED = 1, COS_PROC_STATE_READY, COS_PROC_STATE_RUNNING, COS_PROC_STATE_BLOCKED,
       COS_PROC_STATE_SLEEPING, COS_PROC_STATE_ZOMBIE };

typedef struct {
    uint64_t pid, parent_pid, cpu_time, ctx_switches, page_faults, heap_bytes;
    uint32_t state, is_user, threads, caps;
    char name[32];
} cos_procinfo_t;

/* Result block of SYS_NET_HTTP_FETCH. Mirrored in userland/include/cos.h. */
typedef struct {
    int32_t  status;          /* HTTP status code, 0 when no response arrived */
    int32_t  error;           /* non-zero when the transfer itself failed */
    uint64_t body_len;        /* full body length, even when it did not fit the caller's buffer */
    uint32_t truncated, pad;
    char     content_type[64];
} cos_http_result_t;

/* Fills up to max entries from the live process table; returns the count. */
int process_snapshot(cos_procinfo_t *out, int max);

#endif
