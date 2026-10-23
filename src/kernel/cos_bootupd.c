/* cos_bootupd.c - kernel updates (docs/app_update.md, stage 2).
 *
 * A C-OS disk (tools/make_hdd_image.py) boots through GRUB from its EFI System Partition (ESP): /boot/kernel.elf.gz ("slot a") and /boot/kernel.b.gz ("slot b"), and
 * /boot/grub/grubenv, GRUB's environment block, which says which slot to start. This file lets a running system put a NEW kernel into the slot that is not in use, and
 * make GRUB try it - with a way back, because a kernel that does not start must not leave the machine unbootable:
 *
 *   1. install(): the blob is verified HERE, in the kernel (SHA-256 + ECDSA P-256 with the kernel-update key compiled into this kernel, never by a program): its serial must be
 *      higher than the running kernel's, and than any update that already failed. It is written to the unused slot, read back and compared, and only then
 *      grubenv gets  trial=<slot>  (trial_serial=<serial>, trial_seen empty).
 *   2. grub.cfg: at the next start, `trial` set and trial_seen empty -> trial_seen=1 is saved and the trial slot is started. If that kernel never confirms
 *      itself, the start after that sees trial_seen=1: trial is cleared, failed=<slot> is saved and the active slot starts again. A kernel that GRUB cannot even load
 *      falls through to a menu entry that always starts the active slot.
 *   3. confirm: the trial kernel, once it has been running for CONFIRM_MS, sets  active=<its slot>  and clears the trial variables. The old slot stays as it was.
 *      The previous kernel therefore is always still there until the next update overwrites it.
 *   4. A start that finds failed=<slot> remembers the serial as bad: the same update is not installed again (cosupdate asks via status()).
 *
 * Only a disk whose ESP could be found next to the C-OS data partition is updated. A live CD, a raw data disk and the Secure Boot image (the kernel is inside the
 * signed loader) report "not supported" with the reason. Nothing outside the ESP can be reached through drive 1 (diskio.c). */
#include "types.h"
#include "string.h"
#include "serial.h"
#include "memory.h"
#include "sync.h"
#include "task.h"
#include "timer.h"
#include "ff.h"
#include "bearssl.h"
#include "kernel_pubkey.h"          /* cos_kupd_pubkey[65] */
#include "kernel_serial.h"          /* COS_KERNEL_SERIAL */
#include "cos_bootupd.h"

extern bool storage_find_esp(uint64_t *first, uint64_t *count);
extern void fatfs_diskio_set_esp(uint64_t first, uint64_t count);
extern char g_boot_cmdline[];                       /* kernel.c: the multiboot2 command line GRUB passed */

#define ENV_PATH   "1:/boot/grub/grubenv"
#define SLOT_A     "1:/boot/kernel.elf.gz"
#define SLOT_B     "1:/boot/kernel.b.gz"
#define MAX_BLOB   (64u << 20)
#define CONFIRM_MS 45000u
#define TRAILER    81u                               /* "COSKSIG1" + serial(8) + key id(1) + signature(64) */

static FATFS g_esp;
static bool g_esp_tried, g_esp_ok;
static char g_why[96] = "not looked at yet";
static mutex_t g_lock;
static bool g_lock_ready;
static char g_last_why[96];
static char g_running = 0;                            /* 'a' or 'b': the slot this kernel was started from (cos_slot= on GRUB's command line), 0 when unknown */

static void lock(void) { if (!g_lock_ready) { mutex_init(&g_lock); g_lock_ready = true; } mutex_lock(&g_lock); }
static void unlock(void) { mutex_unlock(&g_lock); }

/* ---------------------------------------------------------------- grubenv */
typedef struct { char name[24]; char val[64]; } kv_t;
typedef struct { kv_t v[24]; int n; } env_t;

static bool env_load(env_t *e) {
    static char blk[1024];
    memset(e, 0, sizeof *e);
    FIL f; UINT got = 0;
    if (f_open(&f, ENV_PATH, FA_READ) != FR_OK) return false;
    FRESULT r = f_read(&f, blk, sizeof blk, &got);
    f_close(&f);
    if (r != FR_OK || got != sizeof blk || strncmp(blk, "# GRUB Environment Block\n", 25) != 0) return false;
    for (char *p = blk + 25; p < blk + sizeof blk && *p && *p != '#'; ) {
        char *nl = memchr(p, '\n', (size_t)(blk + sizeof blk - p)); if (!nl) break;
        *nl = 0; char *eq = strchr(p, '=');
        if (eq && e->n < 24) { *eq = 0; if (strlen(p) < sizeof e->v[0].name && strlen(eq + 1) < sizeof e->v[0].val) { strcpy(e->v[e->n].name, p); strcpy(e->v[e->n].val, eq + 1); e->n++; } }
        p = nl + 1;
    }
    return true;
}
static const char *env_get(const env_t *e, const char *name) { for (int i = 0; i < e->n; ++i) if (!strcmp(e->v[i].name, name)) return e->v[i].val; return ""; }
static void env_set(env_t *e, const char *name, const char *val) {
    for (int i = 0; i < e->n; ++i) if (!strcmp(e->v[i].name, name)) { snprintf(e->v[i].val, sizeof e->v[i].val, "%s", val); return; }
    if (e->n < 24) { snprintf(e->v[e->n].name, sizeof e->v[0].name, "%s", name); snprintf(e->v[e->n].val, sizeof e->v[0].val, "%s", val); e->n++; }
}
/* the block is rewritten in place (same file, same cluster, same length) so that GRUB's save_env, which writes by block list, keeps working */
static bool env_store(const env_t *e) {
    static char blk[1024];
    memset(blk, '#', sizeof blk);
    size_t o = 0; o += (size_t)snprintf(blk, sizeof blk, "# GRUB Environment Block\n");
    for (int i = 0; i < e->n; ++i) {
        size_t l = strlen(e->v[i].name) + 1 + strlen(e->v[i].val) + 1;
        if (o + l >= sizeof blk) return false;
        o += (size_t)snprintf(blk + o, sizeof blk - o, "%s=%s\n", e->v[i].name, e->v[i].val);
    }
    memset(blk + o, '#', sizeof blk - o);                 /* snprintf wrote a NUL: padding is '#' to exactly 1024 bytes */
    FIL f; UINT put = 0;
    if (f_open(&f, ENV_PATH, FA_WRITE | FA_OPEN_EXISTING) != FR_OK) return false;
    FRESULT r = f_write(&f, blk, sizeof blk, &put);
    if (r == FR_OK) r = f_sync(&f);
    f_close(&f);
    return r == FR_OK && put == sizeof blk;
}

/* ---------------------------------------------------------------- the boot partition */
static bool esp_ready(void) {
    if (g_esp_tried) return g_esp_ok;
    g_esp_tried = true;
    uint64_t first = 0, count = 0;
    if (!storage_find_esp(&first, &count)) { snprintf(g_why, sizeof g_why, "this system does not run from an installed C-OS disk (live CD, raw data disk or Secure Boot image)"); return false; }
    fatfs_diskio_set_esp(first, count);
    if (f_mount(&g_esp, "1:", 1) != FR_OK) { snprintf(g_why, sizeof g_why, "the boot partition cannot be read"); return false; }
    FILINFO fi;
    if (f_stat(SLOT_A, &fi) != FR_OK || f_stat(ENV_PATH, &fi) != FR_OK) { snprintf(g_why, sizeof g_why, "the boot partition has no /boot/kernel.elf.gz and /boot/grub/grubenv (an older disk image: write a new one)"); return false; }
    g_esp_ok = true; g_why[0] = 0;
    return true;
}

static uint64_t parse_u64(const char *s) { uint64_t v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (uint64_t)(*s++ - '0'); return v; }
static char slot_of(const char *s) { return (s[0] == 'a' || s[0] == 'b') && !s[1] ? s[0] : 0; }

/* ---------------------------------------------------------------- status */
int cos_bootupd_status(cos_kupd_status_t *o) {
    memset(o, 0, sizeof *o);
    o->running_serial = COS_KERNEL_SERIAL;
    lock();
    if (!esp_ready()) { snprintf(o->why, sizeof o->why, "%s", g_why); unlock(); return 0; }
    env_t e;
    if (!env_load(&e)) { snprintf(o->why, sizeof o->why, "grubenv cannot be read"); unlock(); return 0; }
    o->supported = 1;
    o->running_slot = g_running ? g_running : '?';
    o->active_slot = slot_of(env_get(&e, "active")) ? slot_of(env_get(&e, "active")) : 'a';
    o->trial_slot = slot_of(env_get(&e, "trial"));
    o->trial_serial = parse_u64(env_get(&e, "trial_serial"));
    o->bad_serial = parse_u64(env_get(&e, "bad_serial"));
    unlock();
    return 0;
}

/* ---------------------------------------------------------------- install */
static bool signature_ok(const uint8_t *p, size_t n, uint64_t *serial_out) {
    if (n < TRAILER + 1024) return false;
    const uint8_t *t = p + n - TRAILER;
    if (memcmp(t, "COSKSIG1", 8) != 0 || t[16] != 1) return false;
    uint64_t serial = 0; for (int i = 7; i >= 0; --i) serial = serial << 8 | t[8 + i];
    br_sha256_context c; uint8_t h[32];
    br_sha256_init(&c); br_sha256_update(&c, p, n - TRAILER); br_sha256_update(&c, "COSKSER1", 8); br_sha256_update(&c, t + 8, 8); br_sha256_out(&c, h);
    br_ec_public_key pk = { BR_EC_secp256r1, (void *)cos_kupd_pubkey, sizeof cos_kupd_pubkey };
    if (br_ecdsa_i31_vrfy_raw(&br_ec_prime_i31, h, sizeof h, &pk, t + 17, 64) != 1) return false;
    *serial_out = serial;
    return true;
}
static void u64s(uint64_t v, char *o, size_t cap) { snprintf(o, cap, "%llu", (unsigned long long)v); }

int cos_bootupd_install(const uint8_t *blob, size_t len, char *why, size_t whycap) {
#define REFUSE(code, ...) do { snprintf(why, whycap, __VA_ARGS__); snprintf(g_last_why, sizeof g_last_why, "%s", why); serial_puts("[BOOTUPD] refused: "); serial_puts(why); serial_puts("\n"); unlock(); return (code); } while (0)
    lock();
    if (!esp_ready()) REFUSE(-2, "%s", g_why);
    if (len < TRAILER + 1024 || len > MAX_BLOB) REFUSE(-3, "the kernel image has an impossible size (%u bytes)", (unsigned)len);
    uint64_t serial = 0;
    if (!signature_ok(blob, len, &serial)) REFUSE(-4, "the kernel image is not signed with this system's kernel-update key");
    size_t plen = len - TRAILER;
    if (blob[0] != 0x1f || blob[1] != 0x8b) REFUSE(-5, "the kernel image is not a gzip file");
    env_t e;
    if (!env_load(&e)) REFUSE(-6, "grubenv cannot be read");
    if (serial <= COS_KERNEL_SERIAL) REFUSE(-7, "this kernel (serial %llu) is not newer than the running one (%u)", (unsigned long long)serial, (unsigned)COS_KERNEL_SERIAL);
    if (serial <= parse_u64(env_get(&e, "bad_serial"))) REFUSE(-8, "an update with serial %llu or older failed to start on this machine", (unsigned long long)parse_u64(env_get(&e, "bad_serial")));
    char active = slot_of(env_get(&e, "active")) ? slot_of(env_get(&e, "active")) : 'a';
    char slot = active == 'a' ? 'b' : 'a';
    const char *path = slot == 'a' ? SLOT_A : SLOT_B;
    /* 1: nothing may try the slot while it is being written */
    env_set(&e, "trial", ""); env_set(&e, "trial_seen", ""); env_set(&e, "trial_serial", "");
    if (!env_store(&e)) REFUSE(-9, "grubenv cannot be written");
    /* 2: write, 3: read back */
    FIL f; UINT w;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) REFUSE(-10, "cannot create %s on the boot partition", path + 2);
    FRESULT r = FR_OK;
    for (size_t off = 0; off < plen && r == FR_OK; ) {
        UINT n = plen - off > 65536 ? 65536 : (UINT)(plen - off);
        r = f_write(&f, blob + off, n, &w);
        if (r == FR_OK && w != n) r = FR_DISK_ERR;
        off += n;
    }
    if (r == FR_OK) r = f_sync(&f);
    f_close(&f);
    if (r != FR_OK) REFUSE(-11, "writing %s failed (FatFs %d); the active kernel is untouched", path + 2, (int)r);
    static uint8_t chunk[65536];
    br_sha256_context c1, c2; uint8_t h1[32], h2[32];
    br_sha256_init(&c1); br_sha256_update(&c1, blob, plen); br_sha256_out(&c1, h1);
    br_sha256_init(&c2);
    if (f_open(&f, path, FA_READ) != FR_OK) REFUSE(-12, "cannot read %s back", path + 2);
    size_t total = 0;
    for (;;) { UINT n = 0; if (f_read(&f, chunk, sizeof chunk, &n) != FR_OK) { total = (size_t)-1; break; } if (!n) break; br_sha256_update(&c2, chunk, n); total += n; }
    f_close(&f);
    br_sha256_out(&c2, h2);
    if (total != plen || memcmp(h1, h2, 32) != 0) REFUSE(-13, "%s differs from what was written; the active kernel is untouched", path + 2);
    /* 4: only now does GRUB get to see it */
    char sb[24]; u64s(serial, sb, sizeof sb);
    char sl[2] = { slot, 0 };
    env_set(&e, "trial", sl); env_set(&e, "trial_serial", sb); env_set(&e, "trial_seen", "");
    if (!env_store(&e)) REFUSE(-14, "grubenv cannot be written");
    serial_puts("[BOOTUPD] kernel serial "); serial_puts(sb); serial_puts(" staged in slot "); serial_puts(sl); serial_puts(": GRUB starts it at the next start\n");
    snprintf(why, whycap, "staged in slot %c", slot);
    snprintf(g_last_why, sizeof g_last_why, "%s", why);
    unlock();
    return 0;
#undef REFUSE
}

/* ---------------------------------------------------------------- at every start */
static void confirm_thread(void *arg) {
    (void)arg;
    thread_sleep(CONFIRM_MS);                                   /* still running after this long: the new kernel is good */
    lock();
    env_t e;
    if (esp_ready() && env_load(&e) && g_running && slot_of(env_get(&e, "trial")) == g_running) {
        char sl[2] = { g_running, 0 };
        env_set(&e, "active", sl); env_set(&e, "trial", ""); env_set(&e, "trial_seen", ""); env_set(&e, "trial_serial", ""); env_set(&e, "failed", "");
        if (env_store(&e)) { serial_puts("[BOOTUPD] confirmed: slot "); serial_puts(sl); serial_puts(" is the active kernel now\n"); }
        else serial_puts("[BOOTUPD] cannot write grubenv to confirm the new kernel\n");
    }
    unlock();
}

void cos_bootupd_boot(void) {
    const char *p = strstr(g_boot_cmdline, "cos_slot=");
    g_running = p && (p[9] == 'a' || p[9] == 'b') ? p[9] : 0;
    lock();
    char num[24]; u64s(COS_KERNEL_SERIAL, num, sizeof num);
    serial_puts("[BOOTUPD] kernel serial "); serial_puts(num); serial_puts(", started from slot "); { char s[2] = { g_running ? g_running : '?', 0 }; serial_puts(s); } serial_puts("\n");
    if (!esp_ready()) { serial_puts("[BOOTUPD] kernel updates are not available here: "); serial_puts(g_why); serial_puts("\n"); unlock(); return; }
    env_t e;
    if (!env_load(&e)) { serial_puts("[BOOTUPD] grubenv cannot be read\n"); unlock(); return; }
    if (e.n == 0 || !*env_get(&e, "active")) { env_set(&e, "active", "a"); }
    const char *failed = env_get(&e, "failed");
    if (slot_of(failed)) {                                       /* GRUB gave up on the trial slot and started the active one */
        uint64_t ts = parse_u64(env_get(&e, "trial_serial")), bs = parse_u64(env_get(&e, "bad_serial"));
        char sb[24]; u64s(ts > bs ? ts : bs, sb, sizeof sb);
        env_set(&e, "bad_serial", sb); env_set(&e, "failed", ""); env_set(&e, "trial_serial", "");
        env_store(&e);
        serial_puts("[BOOTUPD] the last kernel update (slot "); serial_puts(failed); serial_puts(") did not start: back on the previous kernel; serials up to "); serial_puts(sb); serial_puts(" are not tried again\n");
    }
    char tr = slot_of(env_get(&e, "trial"));
    bool confirm = tr && g_running == tr;
    unlock();
#ifdef COS_KUPD_NOCONFIRM          /* tests only (make KUPD_NOCONFIRM=1): a kernel that starts but is never confirmed, to see the fallback */
    if (confirm) { serial_puts("[BOOTUPD] trial kernel (test build: never confirms)\n"); confirm = false; }
#endif
    if (confirm) {
        serial_puts("[BOOTUPD] this is a trial kernel: it confirms itself after it has run for a while\n");
        thread_create_kernel("bootupd_confirm", (void *)confirm_thread, NULL);
    }
}

const char *cos_bootupd_last_why(void) { return g_last_why; }          /* the reason the last install was refused / how it went, for the program that asked */
