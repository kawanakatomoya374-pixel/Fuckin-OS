/**
 * disk_bench.c - validation-only block device benchmark (COS_VALIDATION_DISKBENCH=1).
 *
 * Talks to the active block device directly (no cache) on a 128 MiB scratch extent at the end of the data partition and
 * prints [DISKBENCH] begin/end markers; tools/run_disk_bench.sh timestamps them on the host (guest time is unreliable
 * under TCG) and turns them into MB/s and IOPS. Sequential reads verify the pattern, so a driver that is fast but wrong
 * fails.
 */
#include "blk.h"
#include "serial.h"
#include "string.h"
#include "memory.h"
#include "task.h"
#include "timer.h"
#include "fs.h"

#ifndef COS_VALIDATION_DISKBENCH
#define COS_VALIDATION_DISKBENCH 0
#endif

#if COS_VALIDATION_DISKBENCH
extern blk_dev_t* storage_active_dev(uint64_t* scratch_lba, uint64_t* scratch_sectors);

static uint32_t pat32(uint64_t lba, uint32_t w) { return (uint32_t)(lba * 2654435761u) ^ (w * 40503u) ^ 0xA5A5A5A5u; }
static void fill(uint8_t* buf, uint64_t lba, uint32_t sectors) {
    for (uint32_t s = 0; s < sectors; ++s) { uint32_t* p = (uint32_t*)(buf + (size_t)s * 512); for (uint32_t w = 0; w < 128; ++w) p[w] = pat32(lba + s, w); }
}
static bool check(const uint8_t* buf, uint64_t lba, uint32_t sectors) {
    for (uint32_t s = 0; s < sectors; ++s) { const uint32_t* p = (const uint32_t*)(buf + (size_t)s * 512); for (uint32_t w = 0; w < 128; w += 17) if (p[w] != pat32(lba + s, w)) return false; }
    return true;
}

#define PASSES 4
static void seq(blk_dev_t* d, uint64_t base, uint64_t sectors, uint32_t chunk, bool write, const char* name, uint8_t* buf) {
    serial_puts("[DISKBENCH] begin "); serial_puts(name); serial_puts("\n");
    bool ok = true;
    for (uint64_t off = 0; off < sectors * PASSES && ok; off += chunk) {
        uint64_t o = off % sectors;
        if (o + chunk > sectors) continue;
        uint32_t n = chunk;
        if (write) { fill(buf, base + o, n); ok = d->write(d, base + o, n, buf); }
        else { ok = d->read(d, base + o, n, buf) && check(buf, base + o, n); }
    }
    serial_puts("[DISKBENCH] end "); serial_puts(name); serial_puts(" bytes="); serial_putdec(sectors * 512 * PASSES); serial_puts(ok ? " ok\n" : " FAILED\n");
}

static void rnd(blk_dev_t* d, uint64_t base, uint64_t sectors, uint32_t ops, bool write, const char* name, uint8_t* buf) {
    serial_puts("[DISKBENCH] begin "); serial_puts(name); serial_puts("\n");
    uint64_t x = 88172645463325252ull; bool ok = true;
    for (uint32_t i = 0; i < ops && ok; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint64_t lba = base + (x % (sectors / 8)) * 8;
        if (write) { fill(buf, lba, 8); ok = d->write(d, lba, 8, buf); }
        else { ok = d->read(d, lba, 8, buf); }
    }
    serial_puts("[DISKBENCH] end "); serial_puts(name); serial_puts(" ops="); serial_putdec(ops); serial_puts(ok ? " ok\n" : " FAILED\n");
}

/* N threads, each doing queue-depth-1 random reads: the aggregate shows whether the driver lets requests overlap. */
#define MT_THREADS 8
#define MT_OPS     1500
static volatile uint32_t g_mt_done, g_mt_bad;
static blk_dev_t* g_mt_dev; static uint64_t g_mt_base, g_mt_sectors;
static void mt_worker(void* arg) {
    uint64_t x = 0x9E3779B97F4A7C15ull * ((uint64_t)(uintptr_t)arg + 1);
    uint8_t* b = (uint8_t*)kmalloc(4096);
    for (uint32_t i = 0; b && i < MT_OPS; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint64_t lba = g_mt_base + (x % (g_mt_sectors / 8)) * 8;
        if (!g_mt_dev->read(g_mt_dev, lba, 8, b) || !check(b, lba, 8)) { __atomic_fetch_add(&g_mt_bad, 1u, __ATOMIC_RELAXED); break; }
    }
    if (b) kfree(b);
    __atomic_fetch_add(&g_mt_done, 1u, __ATOMIC_RELEASE);
}

static void bench_thread(void* arg) {
    (void)arg;
    uint64_t until = get_timer_ticks() + 3000; while (get_timer_ticks() < until) thread_yield();
    uint64_t base, n;
    blk_dev_t* d = storage_active_dev(&base, &n);
    if (!d) { serial_puts("[DISKBENCH] no device\n"); return; }
    serial_puts("[DISKBENCH] device "); serial_puts(d->name); serial_puts("\n");
    uint8_t* buf = (uint8_t*)kmalloc(2u << 20);
    if (!buf) return;
    const uint64_t MB64 = (64ull << 20) / 512;
    seq(d, base, MB64, 256, true,  "seqwrite_128k", buf);
    seq(d, base, MB64, 256, false, "seqread_128k", buf);
    seq(d, base, MB64, 4096, true,  "seqwrite_2m", buf);
    seq(d, base, MB64, 4096, false, "seqread_2m", buf);
    rnd(d, base, MB64, 16000, false, "randread_4k", buf);
    rnd(d, base, MB64, 8000, true, "randwrite_4k", buf);
    g_mt_dev = d; g_mt_base = base; g_mt_sectors = MB64; g_mt_done = g_mt_bad = 0;
    serial_puts("[DISKBENCH] begin mt_randread_8t\n");
    for (uintptr_t t = 0; t < MT_THREADS; ++t) thread_create_kernel_stack_size("bench_mt", (void*)mt_worker, (void*)t, 64 * 1024);
    while (__atomic_load_n(&g_mt_done, __ATOMIC_ACQUIRE) < MT_THREADS) thread_yield();
    serial_puts("[DISKBENCH] end mt_randread_8t ops="); serial_putdec((uint64_t)MT_THREADS * MT_OPS); serial_puts(g_mt_bad ? " FAILED\n" : " ok\n");
    if (d->trim) {   /* TRIM: discard the upper half, then the lower half must be intact and the upper half either old data or zeros */
        seq(d, base, MB64, 4096, true, "trim_prep", buf);
        serial_puts("[DISKBENCH] begin trim_32m\n");
        bool ok = d->trim(d, base + MB64 / 2, MB64 / 2);
        for (uint64_t off = 0; off < MB64 && ok; off += 4096) {
            ok = d->read(d, base + off, 4096, buf);
            if (!ok) break;
            if (off < MB64 / 2) ok = check(buf, base + off, 4096);
            else { bool zero = true; for (uint32_t w = 0; w < 4096 * 128 && zero; w += 97) if (((uint32_t*)buf)[w]) zero = false; ok = zero || check(buf, base + off, 4096); }
        }
        serial_puts("[DISKBENCH] end trim_32m bytes="); serial_putdec(MB64 * 512 / 2); serial_puts(ok ? " ok\n" : " FAILED\n");
    } else serial_puts("[DISKBENCH] note: device has no TRIM\n");
    {   /* what an application sees: whole files through FatFs on top of the cache + driver */
        const uint32_t FB = 16u << 20;
        uint8_t* fb = (uint8_t*)kmalloc(FB);
        if (fb) {
            fill(fb, 0, FB / 512);
            bool ok = true;
            serial_puts("[DISKBENCH] begin fs_write_16m\n");
            for (int i = 0; i < 2 && ok; ++i) ok = fs_write_file_at("/", "diskbench.bin", (const char*)fb, FB);
            serial_puts("[DISKBENCH] end fs_write_16m bytes="); serial_putdec((uint64_t)FB * 2); serial_puts(ok ? " ok\n" : " FAILED\n");
            memset(fb, 0, FB);
            uint64_t got = 0;
            serial_puts("[DISKBENCH] begin fs_read_16m\n");
            for (int i = 0; i < 2 && ok; ++i) { ok = fs_read_path_into("/diskbench.bin", fb, FB, &got) && got == FB && check(fb, 0, FB / 512); }
            serial_puts("[DISKBENCH] end fs_read_16m bytes="); serial_putdec((uint64_t)FB * 2); serial_puts(ok ? " ok\n" : " FAILED\n");
            kfree(fb);
        }
    }
    serial_puts("[DISKBENCH] begin flush\n"); d->flush(d); serial_puts("[DISKBENCH] end flush\n");
    serial_puts("[DISKBENCH] DONE\n");
    kfree(buf);
}
void disk_bench_start(void) { thread_create_kernel_stack_size("disk_bench", (void*)bench_thread, NULL, 128 * 1024); }
#else
void disk_bench_start(void) {}
#endif
