#ifndef FAT_FSCK_H
#define FAT_FSCK_H
#include "types.h"
#include <stdbool.h>

typedef struct {
    uint32_t total_clusters, used_clusters;
    uint32_t files, dirs;
    uint32_t bad_start;        /* directory entry whose first cluster is outside the volume */
    uint32_t truncated;        /* cluster chain shorter than the recorded size needs */
    uint32_t size_mismatch;    /* chain longer than the size needs (harmless, reported) */
    uint32_t cross_linked;     /* a cluster claimed by two chains */
    uint32_t chain_errors;     /* loops, free / bad clusters inside a chain */
    uint32_t leaked_clusters;  /* allocated in the FAT but referenced by nothing */
    uint32_t fat_mismatch;     /* entries where the two FAT copies differ */
    uint32_t repaired;         /* clusters freed / FAT entries re-synchronised when repair was requested */
} fat_fsck_t;

/* Checks the mounted FAT32 volume (read-only unless 'repair'). With repair, leaked clusters are freed and the second
 * FAT is rewritten from the first, then the volume is remounted. Returns true when the volume is consistent
 * (after repair, if requested). */
uint32_t fat_fsck_base(void);   /* first sector of the FAT32 volume (behind the MBR f_mkfs writes) */
bool fat_fsck(bool repair, fat_fsck_t* out);
#endif
