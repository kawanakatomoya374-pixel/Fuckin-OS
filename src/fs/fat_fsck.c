/**
 * fat_fsck.c - FAT32 consistency checker for the user filesystem (the FatFs volume).
 *
 * Works on the raw sectors through the FatFs disk layer, independent of FatFs's own state: reads the BPB and the FAT,
 * walks every directory from the root and verifies each entry's cluster chain (range, loops, cross-links, length against
 * the recorded size), then compares the FAT's allocation with what the tree actually owns (leaked clusters) and the two
 * FAT copies with each other.
 */
#include "fat_fsck.h"
#include "ff.h"
#include "diskio.h"
#include "memory.h"
#include "string.h"
#include "serial.h"

extern void fs_lock_for_external_use(void);
extern void fs_unlock_for_external_use(void);
extern void fs_remount(void);

#define EOC(v)  ((v) >= 0x0FFFFFF8u)
#define BADC    0x0FFFFFF7u

typedef struct {
    uint32_t spc, reserved, nfats, fatsz, root, total, data_start, nclusters, cbytes;
} bpb_t;

static uint8_t* g_owned;

static inline uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline bool owned_get(uint32_t c) { return g_owned[c >> 3] & (1u << (c & 7)); }
static inline void owned_set(uint32_t c) { g_owned[c >> 3] |= (uint8_t)(1u << (c & 7)); }

/* Follows a chain from 'start'. Returns its length in clusters; marks them owned. */
static uint32_t walk_chain(const uint32_t* fat, const bpb_t* b, uint32_t start, fat_fsck_t* r) {
    uint32_t n = 0, c = start;
    while (c >= 2 && c < b->nclusters + 2) {
        if (owned_get(c)) { r->cross_linked++; return n; }
        owned_set(c);
        ++n;
        uint32_t next = fat[c] & 0x0FFFFFFFu;
        if (EOC(next)) return n;
        if (next == 0 || next == BADC) { r->chain_errors++; return n; }
        if (n > b->nclusters) { r->chain_errors++; return n; }       /* loop */
        c = next;
    }
    r->chain_errors++;                                                 /* left the volume */
    return n;
}

/* f_mkfs normally writes a partition table (MBR) first and puts the FAT32 volume behind it; this returns the LBA of the
 * volume's boot sector (0 when the disk has no partition table). */
uint32_t fat_fsck_base(void) {
    uint8_t sec[512];
    if (disk_read(0, sec, 0, 1) != RES_OK || le16(sec + 510) != 0xAA55) return 0;
    if (le16(sec + 11) == 512 && sec[16] >= 1) return 0;                    /* sector 0 is itself a FAT boot sector */
    uint8_t type = sec[446 + 4];
    return (type == 0x0B || type == 0x0C) ? le32(sec + 446 + 8) : 0;
}

bool fat_fsck(bool repair, fat_fsck_t* out) {
    fat_fsck_t r;
    memset(&r, 0, sizeof r);
    fs_lock_for_external_use();

    uint8_t sec[512];
    bool ok = false;
    uint32_t base = fat_fsck_base();
    DRESULT dr = disk_read(0, sec, base, 1);
    if (dr != RES_OK || le16(sec + 510) != 0xAA55 || le16(sec + 11) != 512) {
        serial_puts("[FATFSCK] cannot read a FAT32 boot sector: disk_read="); serial_putdec((uint64_t)dr);
        serial_puts(" sig=0x"); serial_puthex(le16(sec + 510)); serial_puts(" bps="); serial_putdec(le16(sec + 11)); serial_puts("\n");
        goto done;
    }
    bpb_t b;
    b.spc = sec[13]; b.reserved = le16(sec + 14); b.nfats = sec[16];
    b.fatsz = le32(sec + 36); b.root = le32(sec + 44); b.total = le32(sec + 32);
    if (!b.spc || !b.fatsz || b.nfats < 1 || b.nfats > 2 || b.root < 2) goto done;
    b.data_start = b.reserved + b.nfats * b.fatsz;
    if (b.total <= b.data_start) goto done;
    b.nclusters = (b.total - b.data_start) / b.spc;
    b.cbytes = b.spc * 512u;
    r.total_clusters = b.nclusters;
    uint64_t fat_bytes = (uint64_t)b.fatsz * 512u;
    if (fat_bytes > (64u << 20) || (uint64_t)(b.nclusters + 2) * 4u > fat_bytes) goto done;

    uint32_t* fat = (uint32_t*)kmalloc((size_t)fat_bytes);
    uint32_t* fat2 = b.nfats > 1 ? (uint32_t*)kmalloc((size_t)fat_bytes) : NULL;
    g_owned = (uint8_t*)kmalloc((b.nclusters + 2 + 7) / 8);
    uint8_t* cbuf = (uint8_t*)kmalloc(b.cbytes);
    if (!fat || (b.nfats > 1 && !fat2) || !g_owned || !cbuf) goto cleanup;
    memset(g_owned, 0, (b.nclusters + 2 + 7) / 8);
    for (uint32_t s = 0; s < b.fatsz; s += 64) {
        uint32_t n = b.fatsz - s < 64 ? b.fatsz - s : 64;
        if (disk_read(0, (BYTE*)fat + (size_t)s * 512, base + b.reserved + s, n) != RES_OK) goto cleanup;
        if (fat2 && disk_read(0, (BYTE*)fat2 + (size_t)s * 512, base + b.reserved + b.fatsz + s, n) != RES_OK) goto cleanup;
    }
    if (fat2) for (uint32_t c = 0; c < b.nclusters + 2; ++c) if ((fat[c] & 0x0FFFFFFFu) != (fat2[c] & 0x0FFFFFFFu)) r.fat_mismatch++;

    /* Directory walk: a work list of directory start clusters */
    uint32_t cap = 256, nq = 0, qi = 0;
    uint32_t* queue = (uint32_t*)kmalloc(cap * sizeof(uint32_t));
    if (!queue) goto cleanup;
    walk_chain(fat, &b, b.root, &r);
    queue[nq++] = b.root;
    while (qi < nq) {
        uint32_t c = queue[qi++];
        for (uint32_t guard = 0; c >= 2 && c < b.nclusters + 2 && guard <= b.nclusters; ++guard) {
            uint32_t lba = base + b.data_start + (c - 2) * b.spc;
            if (disk_read(0, cbuf, lba, b.spc) != RES_OK) { r.chain_errors++; break; }
            bool end = false;
            for (uint32_t off = 0; off < b.cbytes; off += 32) {
                const uint8_t* e = cbuf + off;
                if (e[0] == 0x00) { end = true; break; }
                if (e[0] == 0xE5 || (e[11] & 0x3F) == 0x0F || (e[11] & 0x08)) continue;   /* deleted, LFN part, volume label */
                if (e[0] == '.' ) continue;                                                  /* "." and ".." */
                uint32_t start = ((uint32_t)le16(e + 20) << 16) | le16(e + 26), size = le32(e + 28);
                bool is_dir = (e[11] & 0x10) != 0;
                if (is_dir) r.dirs++; else r.files++;
                if (start == 0) { if (!is_dir && size) r.truncated++; continue; }
                if (start < 2 || start >= b.nclusters + 2) { r.bad_start++; continue; }
                uint32_t len = walk_chain(fat, &b, start, &r);
                if (!is_dir) {
                    uint32_t need = (uint32_t)(((uint64_t)size + b.cbytes - 1) / b.cbytes);
                    if (len < need) r.truncated++; else if (len > need) r.size_mismatch++;
                } else if (nq < cap) queue[nq++] = start;
                else {
                    uint32_t* nqp = (uint32_t*)kmalloc(cap * 2 * sizeof(uint32_t));
                    if (nqp) { memcpy(nqp, queue, cap * sizeof(uint32_t)); kfree(queue); queue = nqp; cap *= 2; queue[nq++] = start; }
                }
            }
            if (end) break;
            uint32_t next = fat[c] & 0x0FFFFFFFu;
            if (EOC(next)) break;
            c = next;
        }
    }
    kfree(queue);

    for (uint32_t c = 2; c < b.nclusters + 2; ++c) {
        uint32_t v = fat[c] & 0x0FFFFFFFu;
        if (v != 0) r.used_clusters++;
        if (v != 0 && v != BADC && !owned_get(c)) r.leaked_clusters++;
    }

    if (repair && (r.leaked_clusters || r.fat_mismatch)) {
        for (uint32_t c = 2; c < b.nclusters + 2; ++c) {
            uint32_t v = fat[c] & 0x0FFFFFFFu;
            if (v != 0 && v != BADC && !owned_get(c)) { fat[c] = 0; r.repaired++; }
        }
        for (uint32_t s = 0; s < b.fatsz; s += 64) {                /* FAT1 is authoritative: rewrite every copy from it */
            uint32_t n = b.fatsz - s < 64 ? b.fatsz - s : 64;
            for (uint32_t k = 0; k < b.nfats; ++k)
                (void)disk_write(0, (const BYTE*)fat + (size_t)s * 512, base + b.reserved + k * b.fatsz + s, n);
        }
        (void)disk_ioctl(0, CTRL_SYNC, NULL);
        r.repaired += r.fat_mismatch;
    }
    ok = true;
cleanup:
    if (fat) kfree(fat);
    if (fat2) kfree(fat2);
    if (g_owned) kfree(g_owned);
    if (cbuf) kfree(cbuf);
    g_owned = NULL;
done:
    fs_unlock_for_external_use();
    if (repair && ok && r.repaired) fs_remount();
    if (out) *out = r;
    if (!ok) return false;
    if (repair && r.repaired) return true;
    return r.bad_start == 0 && r.truncated == 0 && r.cross_linked == 0 && r.chain_errors == 0 && r.leaked_clusters == 0 &&
           r.fat_mismatch == 0;
}
