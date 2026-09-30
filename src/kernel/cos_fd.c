/**
 * cos_fd.c - per-process file descriptor table for ring3 programs.
 *
 * WHY THIS EXISTS ON TOP OF SYS_READ_FILE/SYS_WRITE_FILE
 * --------------------------------------------------------
 * Those two syscalls (implemented earlier) cover "load a whole file" and
 * "save a whole file", which is enough for a program that treats a file
 * as one blob. It is not enough for anything that streams: reading a
 * file larger than a program wants to buffer at once, appending to a
 * log, or writing incrementally. That needs a persistent handle with a
 * seek position, which is what this file adds.
 *
 * DESIGN
 * -------
 * Each process gets its own fixed-size table of slots (COS_FD_MAX_PER_PROC),
 * each holding either an open FatFs FIL or an open FatFs DIR - never both,
 * discriminated by `is_dir`. Handles are 1-based (matching the .c-osll
 * library handle convention already used elsewhere in this codebase), so
 * 0 is never valid and an uninitialised variable in a user program cannot
 * address a descriptor.
 *
 * Every fd use is checked against the CALLING process's pid before doing
 * anything with it - a table indexed only by a small integer would
 * otherwise let one process read or write through a descriptor that
 * happens to share a slot index with another process's open file. That
 * is exactly the class of bug the .c-osll library-handle code already
 * had to guard against (see cos_dlsym_handle's ownership check), so the
 * same discipline applies here.
 *
 * fs_lock()/fs_unlock() are held around every FatFs call, matching how
 * cos_fs_read_file()/cos_fs_write_file() already use them - FatFs is not
 * reentrant, and these syscalls can now run from ring3 threads that
 * genuinely execute on different cores under SMP.
 */
#include "cos_fd.h"
#include "serial.h"
#include "string.h"
#include "memory.h"
#include "task.h"
#include "sync.h"
#include "../third_party/fatfs/ff.h"

#define COS_FD_MAX_PER_PROC 16
/* Bounds how many bytes a single SYS_FD_READ/SYS_FD_WRITE call moves, so
 * one call cannot make the kernel hold an arbitrarily large bounce buffer
 * or block for an arbitrarily long single FatFs transfer. A program
 * wanting to move more just calls again - exactly like a real read()/
 * write() with a short-count contract. */
#define COS_FD_IO_CHUNK_MAX (64u * 1024u)

typedef struct {
    bool     used;
    bool     is_dir;
    bool     sync_each;      /* append-mode streams are logs: make every write visible at once */
    uint64_t owner_pid;
    union {
        FIL fil;
        DIR dir;
    } h;
} cos_fd_slot_t;

/* One table per process slot index, mirroring the process table rather
 * than being embedded in process_t: process_t is defined in a header
 * widely included across the kernel, and FIL/DIR (FatFs types) are not
 * lightweight enough to want pulled into every translation unit that
 * touches a process_t. Indexed by the process's OWN table slot would
 * require exposing that index; keyed by pid via linear scan instead,
 * bounded by MAX_TASKS which is already small. */
typedef struct {
    bool           used;
    uint64_t       owner_pid;
    cos_fd_slot_t  fds[COS_FD_MAX_PER_PROC];
} cos_fd_table_t;

#define COS_FD_MAX_PROCESSES 64
static cos_fd_table_t g_fd_tables[COS_FD_MAX_PROCESSES];

extern void fs_lock_for_external_use(void);
extern void fs_unlock_for_external_use(void);

static cos_fd_table_t *cos_fd_table_for(uint64_t pid, bool create)
{
    int free_slot = -1;
    for (int i = 0; i < COS_FD_MAX_PROCESSES; ++i) {
        if (g_fd_tables[i].used && g_fd_tables[i].owner_pid == pid) {
            return &g_fd_tables[i];
        }
        if (!g_fd_tables[i].used && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return NULL;
    memset(&g_fd_tables[free_slot], 0, sizeof(g_fd_tables[free_slot]));
    g_fd_tables[free_slot].used = true;
    g_fd_tables[free_slot].owner_pid = pid;
    return &g_fd_tables[free_slot];
}

static cos_fd_slot_t *cos_fd_lookup(uint64_t pid, int64_t fd)
{
    if (fd < 1 || fd > COS_FD_MAX_PER_PROC) return NULL;
    cos_fd_table_t *t = cos_fd_table_for(pid, false);
    if (!t) return NULL;
    cos_fd_slot_t *s = &t->fds[fd - 1];
    return s->used ? s : NULL;
}

int64_t cos_fd_open(uint64_t pid, const char *path, uint32_t cos_flags)
{
    if (!path || !path[0]) return -1;

    cos_fd_table_t *t = cos_fd_table_for(pid, true);
    if (!t) return -1;

    int slot = -1;
    for (int i = 0; i < COS_FD_MAX_PER_PROC; ++i) {
        if (!t->fds[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        serial_puts("[FD] open failed: process fd table full\n");
        return -1;
    }

    /* Translate the small, stable cos_flags ABI (see cos_fd.h) into
     * FatFs's own mode bits, rather than exposing FA_* directly to
     * ring3: those are this filesystem library's own constants and
     * could change independently of anything a .c-os program should
     * need to know about. */
    BYTE mode;
    switch (cos_flags) {
        case COS_O_RDONLY: mode = FA_READ; break;
        case COS_O_WRONLY_CREATE: mode = FA_WRITE | FA_CREATE_ALWAYS; break;
        case COS_O_WRONLY_APPEND: mode = FA_WRITE | FA_OPEN_APPEND; break;
        case COS_O_RDWR_CREATE: mode = FA_READ | FA_WRITE | FA_OPEN_ALWAYS; break;
        default:
            serial_puts("[FD] open rejected: unrecognised flags\n");
            return -1;
    }

    fs_lock_for_external_use();
    FRESULT fr = f_open(&t->fds[slot].h.fil, path, mode);
    fs_unlock_for_external_use();
    if (fr != FR_OK) return -1;

    t->fds[slot].used = true;
    t->fds[slot].is_dir = false;
    /* FatFs only updates the directory entry's size on f_sync/f_close, so without this a
     * reader (C-OS Studio tailing a build log through COS_STDOUT) sees a 0-byte file until
     * the writer exits. Only append-mode descriptors pay for it: large sequential writes
     * (a 10 MB .c-os from the compiler) go through the normal, unsynced path. */
    t->fds[slot].sync_each = (cos_flags == COS_O_WRONLY_APPEND);
    t->fds[slot].owner_pid = pid;
    return (int64_t)(slot + 1);
}

int64_t cos_fd_opendir(uint64_t pid, const char *path)
{
    if (!path || !path[0]) return -1;
    cos_fd_table_t *t = cos_fd_table_for(pid, true);
    if (!t) return -1;

    int slot = -1;
    for (int i = 0; i < COS_FD_MAX_PER_PROC; ++i) {
        if (!t->fds[i].used) { slot = i; break; }
    }
    if (slot < 0) return -1;

    fs_lock_for_external_use();
    FRESULT fr = f_opendir(&t->fds[slot].h.dir, path);
    fs_unlock_for_external_use();
    if (fr != FR_OK) return -1;

    t->fds[slot].used = true;
    t->fds[slot].is_dir = true;
    t->fds[slot].owner_pid = pid;
    return (int64_t)(slot + 1);
}

int64_t cos_fd_read(uint64_t pid, int64_t fd, void *kernel_dst, uint64_t len)
{
    cos_fd_slot_t *s = cos_fd_lookup(pid, fd);
    if (!s || s->is_dir) return -1;
    if (len > COS_FD_IO_CHUNK_MAX) len = COS_FD_IO_CHUNK_MAX;

    UINT got = 0;
    fs_lock_for_external_use();
    FRESULT fr = f_read(&s->h.fil, kernel_dst, (UINT)len, &got);
    fs_unlock_for_external_use();
    if (fr != FR_OK) return -1;
    return (int64_t)got;
}

int64_t cos_fd_write(uint64_t pid, int64_t fd, const void *kernel_src, uint64_t len)
{
    cos_fd_slot_t *s = cos_fd_lookup(pid, fd);
    if (!s || s->is_dir) return -1;
    if (len > COS_FD_IO_CHUNK_MAX) len = COS_FD_IO_CHUNK_MAX;

    UINT written = 0;
    fs_lock_for_external_use();
    FRESULT fr = f_write(&s->h.fil, kernel_src, (UINT)len, &written);
    if (fr == FR_OK && s->sync_each) f_sync(&s->h.fil);
    fs_unlock_for_external_use();
    if (fr != FR_OK) return -1;
    return (int64_t)written;
}

int64_t cos_fd_lseek(uint64_t pid, int64_t fd, int64_t offset)
{
    cos_fd_slot_t *s = cos_fd_lookup(pid, fd);
    if (!s || s->is_dir || offset < 0) return -1;

    fs_lock_for_external_use();
    FRESULT fr = f_lseek(&s->h.fil, (FSIZE_t)offset);
    fs_unlock_for_external_use();
    return (fr == FR_OK) ? offset : -1;
}

int64_t cos_fd_readdir(uint64_t pid, int64_t fd, cos_dirent_t *out)
{
    cos_fd_slot_t *s = cos_fd_lookup(pid, fd);
    if (!s || !s->is_dir || !out) return -1;

    FILINFO info;
    fs_lock_for_external_use();
    FRESULT fr = f_readdir(&s->h.dir, &info);
    fs_unlock_for_external_use();
    if (fr != FR_OK) return -1;

    /* FatFs signals "end of directory" by succeeding with an empty name,
     * not with an error code - a real distinct case a caller must be able
     * to tell apart from "read a real zero-length-named entry" (which
     * cannot happen) and from "an error occurred" (fr != FR_OK, above).
     * Returning 0 here for end-of-directory and 1 for a real entry, with
     * -1 reserved for genuine errors, is what SYS_FD_READDIR passes
     * straight through to ring3. */
    if (info.fname[0] == '\0') return 0;

    memset(out, 0, sizeof(*out));
    size_t i = 0;
    for (; info.fname[i] != '\0' && i < COS_DIRENT_NAME_MAX; ++i) {
        out->name[i] = info.fname[i];
    }
    out->name[i] = '\0';
    out->is_dir = (info.fattrib & AM_DIR) ? 1 : 0;
    out->size = (uint64_t)info.fsize;
    return 1;
}

bool cos_fd_close(uint64_t pid, int64_t fd)
{
    cos_fd_slot_t *s = cos_fd_lookup(pid, fd);
    if (!s) return false;

    fs_lock_for_external_use();
    if (s->is_dir) {
        (void)f_closedir(&s->h.dir);
    } else {
        (void)f_close(&s->h.fil);
    }
    fs_unlock_for_external_use();

    s->used = false;
    return true;
}

void cos_fd_close_all_for_pid(uint64_t pid)
{
    cos_fd_table_t *t = cos_fd_table_for(pid, false);
    if (!t) return;

    fs_lock_for_external_use();
    for (int i = 0; i < COS_FD_MAX_PER_PROC; ++i) {
        if (!t->fds[i].used) continue;
        if (t->fds[i].is_dir) (void)f_closedir(&t->fds[i].h.dir);
        else (void)f_close(&t->fds[i].h.fil);
        t->fds[i].used = false;
    }
    fs_unlock_for_external_use();

    t->used = false;
}

/* ---- filesystem mutation -------------------------------------------------
 *
 * Backing for SYS_MKDIR/UNLINK/RENAME/STAT. Placed here rather than in
 * fs.c because these go through the SAME fs_lock() the descriptor layer
 * above uses - FatFs is not reentrant, and these can now be called from
 * ring3 threads on different cores, exactly like the fd calls. */

bool cos_fs_mkdir(const char *path)
{
    if (!path || !path[0]) return false;
    fs_lock_for_external_use();
    FRESULT fr = f_mkdir(path);
    fs_unlock_for_external_use();
    /* FR_EXIST is reported as failure rather than silently treated as
     * success: "create this directory" and "a directory is now here"
     * are different questions, and a caller that wanted the second can
     * check with SYS_STAT. Conflating them would hide a real name
     * collision with an existing FILE of the same name. */
    return fr == FR_OK;
}

bool cos_fs_unlink(const char *path)
{
    if (!path || !path[0]) return false;
    /* Refuse to delete the root. FatFs would reject it anyway, but an
     * explicit guard means this cannot depend on that behaviour staying
     * the same. */
    if (path[0] == '/' && path[1] == '\0') return false;
    fs_lock_for_external_use();
    FRESULT fr = f_unlink(path);
    fs_unlock_for_external_use();
    return fr == FR_OK;
}

bool cos_fs_rename(const char *oldp, const char *newp)
{
    if (!oldp || !newp || !oldp[0] || !newp[0]) return false;
    fs_lock_for_external_use();
    FRESULT fr = f_rename(oldp, newp);
    fs_unlock_for_external_use();
    return fr == FR_OK;
}

bool cos_fs_stat(const char *path, uint64_t *out_size, uint8_t *out_is_dir)
{
    if (!path || !path[0]) return false;

    /* f_stat() cannot stat the root directory itself (it has no directory
     * entry to read), so that one case is answered directly rather than
     * reporting a spurious failure for a path that plainly exists. */
    if (path[0] == '/' && path[1] == '\0') {
        if (out_size) *out_size = 0;
        if (out_is_dir) *out_is_dir = 1;
        return true;
    }

    FILINFO info;
    fs_lock_for_external_use();
    FRESULT fr = f_stat(path, &info);
    fs_unlock_for_external_use();
    if (fr != FR_OK) return false;

    if (out_size) *out_size = (uint64_t)info.fsize;
    if (out_is_dir) *out_is_dir = (info.fattrib & AM_DIR) ? 1 : 0;
    return true;
}
