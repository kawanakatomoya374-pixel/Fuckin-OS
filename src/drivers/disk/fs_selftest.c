/**
 * fs_selftest.c - validation-only storage exerciser (COS_VALIDATION_FS=1). Never part of a normal image.
 *
 * Phase 1 (fresh disk): several threads rewrite a set of files forever, each version self-describing (header with
 * round + CRC) so any state of the disk can be judged without remembering anything. The host kills QEMU at a random
 * moment (tools/run_fs_crash_test.sh), i.e. a power cut in the middle of writes and catalog commits.
 * Phase 2 (next boot, same disk): every file must be a complete, intact version - the one before or the one after the
 * interrupted write, never a mix - and the catalog must pass storage_fsck(). Then the writers carry on from there.
 */
#include "storage.h"
#include "crc32_common.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "task.h"
#include "fat_fsck.h"
#include "ff.h"
#include "diskio.h"
#include "fs.h"

#ifndef COS_VALIDATION_FS
#define COS_VALIDATION_FS 0
#endif

#if COS_VALIDATION_FS
#define FST_FILES    8
#define FST_MAX_BODY (300u * 1024u)

typedef struct { uint32_t magic, file, round, size; uint64_t crc; } fst_hdr_t;
#define FST_MAGIC 0x31545346u   /* "FST1" */

static volatile uint32_t g_round[FST_FILES];
static volatile uint32_t g_live_fail;

static uint8_t body_byte(uint32_t k, uint32_t r, uint32_t i) { return (uint8_t)(i * 167u + k * 13u + r * 7u + (i >> 9)); }
static uint32_t size_for(uint32_t k, uint32_t r) { return 4096u + ((k * 37u + r * 101u) % 70u) * 4096u + (r % 5u) * 17u; }

static void make_name(char* out, uint32_t k) {
    const char* p = "/fstest/f"; int n = 0;
    while (*p) out[n++] = *p++;
    out[n++] = (char)('0' + k); out[n] = 0;
}

static int build(uint8_t* buf, uint32_t k, uint32_t r) {
    fst_hdr_t* h = (fst_hdr_t*)buf;
    uint32_t body = size_for(k, r);
    for (uint32_t i = 0; i < body; ++i) buf[sizeof(*h) + i] = body_byte(k, r, i);
    h->magic = FST_MAGIC; h->file = k; h->round = r; h->size = body;
    h->crc = cos_crc32(buf + sizeof(*h), body);
    return (int)(sizeof(*h) + body);
}

/* 0 = intact version, 1 = file missing, 2 = corrupt */
static int check(uint8_t* buf, uint32_t k, uint32_t* round_out) {
    char name[24]; make_name(name, k);
    uint64_t got = 0;
    if (!storage_read_file(name, buf, sizeof(fst_hdr_t) + FST_MAX_BODY, &got)) return 1;
    if (got < sizeof(fst_hdr_t)) return 2;
    fst_hdr_t* h = (fst_hdr_t*)buf;
    if (h->magic != FST_MAGIC || h->file != k || h->size > FST_MAX_BODY || got != sizeof(*h) + h->size) return 2;
    if (h->crc != cos_crc32(buf + sizeof(*h), h->size)) return 2;
    for (uint32_t i = 0; i < h->size; i += 509) if (buf[sizeof(*h) + i] != body_byte(k, h->round, i)) return 2;
    if (round_out) *round_out = h->round;
    return 0;
}

static uint8_t g_wbuf[2][sizeof(fst_hdr_t) + FST_MAX_BODY];
static uint8_t g_rbuf[sizeof(fst_hdr_t) + FST_MAX_BODY];

static void writer(void* arg) {
    uint32_t lane = (uint32_t)(uintptr_t)arg;
    uint8_t* buf = g_wbuf[lane];
    for (;;) {
        for (uint32_t k = lane; k < FST_FILES; k += 2) {
            uint32_t r = g_round[k] + 1u;
            char name[24]; make_name(name, k);
            int n = build(buf, k, r);
            if (storage_write_file(name, buf, (uint64_t)n)) g_round[k] = r;
            else serial_puts("[FSTEST] write failed\n");
        }
        uint32_t total = 0; for (int i = 0; i < FST_FILES; ++i) total += g_round[i];
        if (lane == 0 && total % 16u == 0) { serial_puts("[FSTEST] writes="); serial_putdec(total); serial_puts("\n"); }
        thread_yield();
    }
}

static void reader(void* arg) {
    (void)arg;
    for (;;) {                              /* concurrent readers must never see a half-written file */
        for (uint32_t k = 0; k < FST_FILES; ++k) {
            int rc = check(g_rbuf, k, NULL);
            if (rc == 2) { g_live_fail++; serial_puts("[FSTEST] FAIL live read saw a corrupt file "); serial_putdec(k); serial_puts("\n"); }
            thread_yield();
        }
    }
}

/* FAT32 checker: a healthy volume is clean; a cluster marked allocated in only one FAT copy is found (leak + FAT
 * mismatch) and repair fixes it. */
static void fat_check_test(void) {
    fs_write_file_at("/", "fsck_probe.txt", "fsck probe data", 15);
    fat_fsck_t r;
    bool clean = fat_fsck(false, &r);
    serial_puts("[FSTEST] fat fsck healthy: "); serial_puts(clean ? "clean" : "DIRTY");
    serial_puts(" files="); serial_putdec(r.files); serial_puts(" dirs="); serial_putdec(r.dirs);
    serial_puts(" leaked="); serial_putdec(r.leaked_clusters); serial_puts("\n");
    serial_puts(clean && r.files > 0 ? "[FSTEST] FATCHECK PASS healthy volume is clean\n" : "[FSTEST] FATCHECK FAIL healthy volume\n");

    uint8_t sec[512];
    uint32_t base = fat_fsck_base();
    disk_read(0, sec, base, 1);
    uint32_t reserved = sec[14] | (sec[15] << 8), spc = sec[13], fatsz = sec[36] | (sec[37] << 8) | (sec[38] << 16) | ((uint32_t)sec[39] << 24);
    uint32_t total = sec[32] | (sec[33] << 8) | (sec[34] << 16) | ((uint32_t)sec[35] << 24);
    uint32_t ncl = (total - (reserved + 2 * fatsz)) / spc;
    uint32_t c = ncl + 1;                                   /* the last cluster: free on a young volume */
    uint32_t lba = base + reserved + (c * 4u) / 512u;
    disk_read(0, sec, lba, 1);
    *(uint32_t*)(sec + (c * 4u) % 512u) = 0x0FFFFFFFu;
    disk_write(0, sec, lba, 1);                             /* FAT1 only */
    bool dirty = !fat_fsck(false, &r);
    serial_puts("[FSTEST] fat fsck damaged: leaked="); serial_putdec(r.leaked_clusters);
    serial_puts(" fat_mismatch="); serial_putdec(r.fat_mismatch); serial_puts("\n");
    serial_puts(dirty && r.leaked_clusters == 1 ? "[FSTEST] FATCHECK PASS damage detected\n" : "[FSTEST] FATCHECK FAIL damage detection\n");
    fat_fsck(true, &r);
    clean = fat_fsck(false, &r);
    serial_puts(clean ? "[FSTEST] FATCHECK PASS repair restored a clean volume\n" : "[FSTEST] FATCHECK FAIL repair\n");
    FILINFO fi;
    serial_puts(f_stat("/fsck_probe.txt", &fi) == FR_OK ? "[FSTEST] FATCHECK PASS files still readable after repair\n" : "[FSTEST] FATCHECK FAIL file lost\n");
    serial_puts("[FSTEST] FATCHECK DONE\n");
}

static void fs_selftest_thread(void* arg) {
    (void)arg;
    uint64_t until = get_timer_ticks() + 1500; while (get_timer_ticks() < until) thread_yield();
    int bad = 0, present = 0; uint32_t maxr = 0;
    static uint8_t vbuf[sizeof(fst_hdr_t) + FST_MAX_BODY];
    for (uint32_t k = 0; k < FST_FILES; ++k) {
        uint32_t r = 0; int rc = check(vbuf, k, &r);
        if (rc == 0) { present++; g_round[k] = r; if (r > maxr) maxr = r; }
        else if (rc == 2) { bad++; serial_puts("[FSTEST] FAIL file "); serial_putdec(k); serial_puts(" is corrupt\n"); }
    }
    storage_fsck_t f; bool fsck_ok = storage_fsck(&f);
    serial_puts("[FSTEST] verify: intact="); serial_putdec((uint64_t)present);
    serial_puts(" corrupt="); serial_putdec((uint64_t)bad);
    serial_puts(" max_round="); serial_putdec(maxr);
    serial_puts(" fsck="); serial_puts(fsck_ok ? "clean" : "DIRTY");
    serial_puts(" files="); serial_putdec(f.files);
    serial_puts(" overlaps="); serial_putdec(f.overlaps);
    serial_puts(" leaked="); serial_putdec(f.leaked_sectors);
    serial_puts(" bad_bounds="); serial_putdec(f.bad_bounds); serial_puts("\n");
    serial_puts((bad == 0 && fsck_ok) ? "[FSTEST] VERIFY PASS\n" : "[FSTEST] VERIFY FAIL\n");

    fat_check_test();
    thread_create_kernel_stack_size("fst_w0", (void*)writer, (void*)0, 128 * 1024);
    thread_create_kernel_stack_size("fst_w1", (void*)writer, (void*)1, 128 * 1024);
    thread_create_kernel_stack_size("fst_rd", (void*)reader, NULL, 128 * 1024);
}

void fs_selftest_start(void) {
    thread_create_kernel_stack_size("fs_selftest", (void*)fs_selftest_thread, NULL, 128 * 1024);
}
#else
void fs_selftest_start(void) {}
#endif
