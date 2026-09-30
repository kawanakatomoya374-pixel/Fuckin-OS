/* cos_system_layout.c - the standard C-OS folder layout on the disk.
 *
 * Run after the FAT32 volume is mounted, on every boot, for new and
 * existing volumes alike. Missing folders are created; files the OS ships
 * (programs in /bin, samples in /apps/samples, /etc and /system text) are
 * written when absent or when the built-in copy differs in size (i.e. the
 * OS was updated). Nothing outside those OS-owned files is ever touched.
 *
 *   /bin            system programs (.c-os)
 *   /apps/samples   sample programs built from userland/programs/samples
 *   /etc            system configuration / identification
 *   /system         OS information
 *   /documents /pictures /music /downloads /desktop /tmp   user folders
 */
#include "types.h"
#include "string.h"
#include "serial.h"
#include "../fs/fs.h"
#include "memory.h"
#include "cos_sample_gallery_elf.h"
#include "cos_sample_tone_elf.h"
#include "cos_sdk_pack.h"
#include "cos_jpfont.h"
#include "cos_tcc_elf.h"

extern const unsigned char cos_asset_test_c_os[];
extern const unsigned int cos_asset_test_c_os_len;
extern const unsigned char *cos_music_image(unsigned int *len);
extern const unsigned char *cos_studio_image(unsigned int *len);

static void ensure_dir(const char *parent, const char *name) {
    char full[128];
    size_t n = 0;
    for (const char *p = parent; *p && n < sizeof(full) - 2; ++p) full[n++] = *p;
    if (n > 1) full[n++] = '/';
    for (const char *p = name; *p && n < sizeof(full) - 1; ++p) full[n++] = *p;
    full[n] = '\0';
    bool is_dir = false;
    if (fs_stat_path(full, NULL, &is_dir) && is_dir) return;
    if (fs_create_dir_at(parent, name)) {
        serial_puts("[LAYOUT] created "); serial_puts(full); serial_puts("\n");
    }
}

static void ensure_file(const char *dir, const char *name, const void *data, uint64_t size) {
    char full[160];
    size_t n = 0;
    for (const char *p = dir; *p && n < sizeof(full) - 2; ++p) full[n++] = *p;
    if (n > 1) full[n++] = '/';
    for (const char *p = name; *p && n < sizeof(full) - 1; ++p) full[n++] = *p;
    full[n] = '\0';
    uint64_t have = 0;
    bool is_dir = false;
    if (fs_stat_path(full, &have, &is_dir) && !is_dir && have == size) {
        /* Same size is not the same file: a rebuilt program (say, the same code with a fixed
         * allocator) is usually exactly as long as the one it replaces. Compare the bytes. */
        if (size == 0) return;
        void *cur = kmalloc((size_t)size);
        bool same = false;
        if (cur) {
            uint64_t got = 0;
            if (fs_read_path_into(full, cur, size, &got) && got == size) same = memcmp(cur, data, (size_t)size) == 0;
            kfree(cur);
        } else same = true;                 /* cannot compare: keep what is there rather than rewrite blindly */
        if (same) return;                   /* already current */
    }
    if (fs_write_file_at(dir, name, (const char *)data, size)) {
        serial_puts("[LAYOUT] installed "); serial_puts(full); serial_puts("\n");
    }
}

static const char k_readme[] =
    "C-OS folder layout\n"
    "==================\n\n"
    "/bin            System programs. Double-click to run.\n"
    "                  music.c-os  - Music player (MP3 / OGG / WAV)\n"
    "                  test.c-os   - Text to binary converter\n"
    "                  tcc.c-os    - TinyCC 0.9.28rc, the C compiler\n"
    "/apps/samples   Sample programs (source: userland/programs/samples)\n"
    "                  ui_gallery.c-os - cos_ui drawing demo\n"
    "                  tone.c-os       - audio API demo (plays a 660 Hz tone)\n"
    "/C-OS Studio    C-OS Studio.c-os - the C IDE (double-click a .c file to open it)\n"
    "                  deliverables/  - everything Studio builds or creates lands here\n"
    "/system/sdk     headers + libraries for compiling on C-OS (used by tcc.c-os)\n"
    "/etc            os-release, hostname\n"
    "/system         This file\n"
    "/documents /pictures /music /downloads /desktop /tmp  - your folders\n\n"
    "Programs are ordinary ELF files built with tools/cos-cc; files in /bin\n"
    "and /apps/samples are refreshed by the OS when it is updated.\n";
static const char k_os_release[] =
    "NAME=\"C-OS\"\nVERSION=\"4.0.8 alpha\"\nID=cos\nPRETTY_NAME=\"C-OS 4.0.8 alpha\"\n"
    "HOME_URL=\"https://github.com/\"\n";
static const char k_hostname[] = "c-os\n";

void cos_system_layout_ensure(void) {
    ensure_dir("/", "bin");
    ensure_dir("/", "apps");
    ensure_dir("/apps", "samples");
    ensure_dir("/", "etc");
    ensure_dir("/", "system");
    ensure_dir("/", "documents");
    ensure_dir("/", "pictures");
    ensure_dir("/", "music");
    ensure_dir("/", "downloads");
    ensure_dir("/", "desktop");
    ensure_dir("/", "tmp");
    ensure_dir("/", "C-OS Studio");
    ensure_dir("/C-OS Studio", "deliverables");
    ensure_dir("/system", "fonts");
    ensure_dir("/system", "sdk");
    ensure_dir("/system/sdk", "include");
    ensure_dir("/system/sdk/include", "sys");
    ensure_dir("/system/sdk", "lib");

    unsigned int mlen = 0;
    const unsigned char *music = cos_music_image(&mlen);
    if (music && mlen) ensure_file("/bin", "music.c-os", music, mlen);
    ensure_file("/bin", "test.c-os", cos_asset_test_c_os, cos_asset_test_c_os_len);
    ensure_file("/bin", "tcc.c-os", cos_tcc_elf, cos_tcc_elf_len);
    {   /* the File Manager (also embedded in the kernel: the desktop starts it from memory) and the CJK font it draws with */
        extern const unsigned char *cos_files_image(unsigned int *len);
        unsigned int flen = 0; const unsigned char *files = cos_files_image(&flen);
        if (files && flen) ensure_file("/bin", "files.c-os", files, flen);
        ensure_file("/system/fonts", "NotoSansJP-Regular.ttf", cos_jpfont, cos_jpfont_len);
    }
    unsigned int slen = 0;
    const unsigned char *studio = cos_studio_image(&slen);
    if (studio && slen) ensure_file("/C-OS Studio", "C-OS Studio.c-os", studio, slen);
    for (unsigned i = 0; i < COS_SDK_FILE_COUNT; ++i) {
        /* "include/sys/stat.h" -> dir "/system/sdk/include/sys", name "stat.h" */
        char dir[96] = "/system/sdk/";
        const char *p = cos_sdk_files[i].path, *slash = p;
        for (const char *q = p; *q; ++q) if (*q == '/') slash = q;
        size_t dl = strlen(dir), pl = (size_t)(slash - p);
        if (slash != p && dl + pl < sizeof(dir) - 1) { memcpy(dir + dl, p, pl); dir[dl + pl] = 0; }
        else dir[dl - 1] = 0;
        ensure_file(dir, slash != p ? slash + 1 : p, cos_sdk_files[i].data, cos_sdk_files[i].len);
    }
    ensure_file("/apps/samples", "ui_gallery.c-os", cos_sample_gallery_elf, cos_sample_gallery_elf_len);
    ensure_file("/apps/samples", "tone.c-os", cos_sample_tone_elf, cos_sample_tone_elf_len);
    ensure_file("/system", "README.txt", k_readme, sizeof(k_readme) - 1);
    ensure_file("/etc", "os-release", k_os_release, sizeof(k_os_release) - 1);
    ensure_file("/etc", "hostname", k_hostname, sizeof(k_hostname) - 1);
}

/* Validation only (COS_VALIDATION_MULTI_LAUNCH): launch the shipped ring-3
 * programs repeatedly, back to back, the way a real session does. */
void cos_validation_multi_launch(int rounds) {
    extern int64_t cos_launch_elf_image(const char *name, const unsigned char *image, unsigned int len);
    extern void thread_sleep(uint64_t ms);
    extern void cos_music_open(const char *path);
    cos_music_open(NULL);                                   /* the music player */
    for (int r = 0; r < rounds; ++r) {
        thread_sleep(400);
        (void)cos_launch_elf_image("ui_gallery.c-os", cos_sample_gallery_elf, cos_sample_gallery_elf_len);
        thread_sleep(400);
        (void)cos_launch_elf_image("tone.c-os", cos_sample_tone_elf, cos_sample_tone_elf_len);
    }
    serial_puts("[VALIDATION] multi-launch finished\n");
}
