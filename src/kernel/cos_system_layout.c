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
#include "task.h"
extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
#include "../fs/fs.h"
#include "memory.h"
#include "cos_sample_gallery_elf.h"
#include "cos_sample_tone_elf.h"
#include "cos_sample_widgets_elf.h"
#include "cos_sample_sandbox_elf.h"
#include "cos_sdk_pack.h"
#include "cos_jpfont.h"
#include "cos_langja_so.h"
#include "cos_kmod_hello.h"
#if COS_VALIDATION_TESTPROGS
#include "cos_ssptest.h"
#include "cos_posixtest.h"
#include "cos_dltest.h"
#include "cos_imgdev.h"
#include "cos_linuxtest.h"
#include "cos_linuxroot.h"
#endif
#include "cos_plugins.h"
#if COS_VALIDATION_KMOD
#include "cos_kmod_fixtures.h"
#endif
#include "cos_tcc_elf.h"
#include "cos_cosarc_elf.h"
#include "cos_craft_elf.h"
#include "cos_linuxgui_elf.h"
#include "cos_linuxpack_elf.h"
#include "cos_sprint_elf.h"
#include "cos_cosapps_sp.h"
#include "cos_cosapps_icon.h"
#include "cos_mc_games.h"
#include "cos_mc_audio.h"
#include "cos_py2c_elf.h"
#include "cos_p2ccli_elf.h"
#include "cos_p2csrc_pack.h"
#include "cos_terminal_elf.h"
#include "cos_cosmsgd_elf.h"
#include "cos_cosupdate_elf.h"
#include "update_url.h"          /* COS_UPDATE_URL: where a fresh system looks for updates */
#include "cos_media_elf.h"
#include "cos_photo_elf.h"

extern const unsigned char cos_asset_test_c_os[];
extern const unsigned int cos_asset_test_c_os_len;
extern const unsigned char *cos_music_image(unsigned int *len);
extern const unsigned char *cos_media_image(unsigned int *len);

/* Validation and GUI launch hook for the embedded ring-3 photo viewer.  The
 * generated Photo ELF header intentionally keeps its byte array translation
 * unit-local, so this wrapper is the single owner of that embedded image. */
void cos_photo_open(const char *path) {
    const char *argv[2] = { "Photo.c-os", path ? path : "/desktop/photo.jpg" };
    int64_t pid = cos_launch_elf_image_args("Photo.c-os", cos_photo_elf, cos_photo_elf_len,
                                            argv, 2, NULL, 0);
    serial_puts(pid > 0 ? "[PHOTO] launched\n" : "[PHOTO] launch FAILED\n");
}
extern const unsigned char cos_photo_elf[];
extern const unsigned int cos_photo_elf_len;
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

/* ---- app updates (docs/app_update.md) ----------------------------------------------------------------------------------------------------------------------
 * /system/update/state.txt (written by cosupdate) lists the files an update installed. Their copies inside this kernel image are older than what is on the disk,
 * so ensure_file() must leave them alone. The state belongs to one image: when this kernel is a different build (a new OS image was written) the list is dropped
 * and the image's own files win again. /system/update/image.txt always says which build this is. */
#define COS_IMAGE_STAMP __DATE__ " " __TIME__
#define UPD_MAX 192
static char upd_path[UPD_MAX][128];
static unsigned upd_n;
static bool update_pinned(const char *full) {
    for (unsigned i = 0; i < upd_n; ++i) if (strcmp(upd_path[i], full) == 0) return true;
    return false;
}
static void update_state_load(void) {
    upd_n = 0;
    uint64_t size = 0; bool is_dir = false;
    if (!fs_stat_path("/system/update/state.txt", &size, &is_dir) || is_dir || size == 0 || size > 65536) return;
    char *buf = kmalloc((size_t)size + 1);
    if (!buf) return;
    uint64_t got = 0;
    if (fs_read_path_into("/system/update/state.txt", buf, size, &got) && got == size) {
        buf[size] = 0;
        bool image_ok = false;
        for (char *line = buf; line && *line; ) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            if (strncmp(line, "image ", 6) == 0) image_ok = strcmp(line + 6, COS_IMAGE_STAMP) == 0;
            else if (strncmp(line, "file ", 5) == 0 && image_ok && upd_n < UPD_MAX) {
                char *path = line + 5, *sp = strchr(path, ' ');
                if (sp) *sp = 0;
                if (strlen(path) < sizeof upd_path[0]) { strcpy(upd_path[upd_n++], path); }
            }
            line = nl ? nl + 1 : NULL;
        }
        if (!image_ok) { serial_puts("[UPDATE] this is a different image: the updated files are replaced by the image's own\n"); fs_write_file_at("/system/update", "state.txt", "image " COS_IMAGE_STAMP "\n", sizeof("image " COS_IMAGE_STAMP "\n") - 1); }
        else { serial_puts("[UPDATE] files kept from updates: "); char b[12]; snprintf(b, sizeof b, "%u", upd_n); serial_puts(b); serial_puts("\n"); }
    }
    kfree(buf);
}

/* Like ensure_file() but trusts the size: for the 15 MB IME dictionary, which is only ever replaced by a differently sized one. */
static void ensure_file_by_size(const char *dir, const char *name, const void *data, uint64_t size) {
    char full[160];
    size_t n = 0;
    for (const char *p = dir; *p && n < sizeof(full) - 2; ++p) full[n++] = *p;
    if (n > 1) full[n++] = '/';
    for (const char *p = name; *p && n < sizeof(full) - 1; ++p) full[n++] = *p;
    full[n] = '\0';
    uint64_t have = 0; bool is_dir = false;
    if (fs_stat_path(full, &have, &is_dir) && !is_dir && have > 0 && update_pinned(full)) return;      /* installed by an update */
    if (fs_stat_path(full, &have, &is_dir) && !is_dir && have == size) return;
    if (fs_write_file_at(dir, name, (const char *)data, size)) { serial_puts("[LAYOUT] installed "); serial_puts(full); serial_puts("\n"); }
    else { serial_puts("[LAYOUT] FAILED to install "); serial_puts(full); serial_puts("\n"); }
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
    if (fs_stat_path(full, &have, &is_dir) && !is_dir && have > 0 && update_pinned(full)) return;      /* installed by an update */
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
    "                  Photo.c-os  - PNG/JPEG photo viewer\n"
    "                  test.c-os   - Text to binary converter\n"
    "                  tcc.c-os    - TinyCC 0.9.28rc, the C compiler\n"
    "                  cosarc.c-os - tar, gzip, gunzip, zcat, zip, unzip (type the command in the Terminal)\n"
    "                  Py2C.c-os   - Python to C transpiler / Studio\n"
    "                  Terminal.c-os - modern cos_ui terminal\n"
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
    "NAME=\"C-OS\"\nVERSION=\"4.1.0\"\nID=cos\nPRETTY_NAME=\"C-OS 4.1.0\"\n"
    "HOME_URL=\"https://github.com/\"\n";
static const char k_hostname[] = "c-os\n";

/* Writes a table of {relative path, data} below `base`, making the directories on the way (existing files with the same bytes are left alone). */
static void install_pack(const char *base, const cos_sdk_file_t *files, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        /* "include/py2c/runtime/x.h" -> directories base/include, .../py2c, .../py2c/runtime (made as needed), then the file */
        char dir[160]; snprintf(dir, sizeof dir, "%s", base);
        const char *p = files[i].path;
        for (;;) {
            const char *slash = p;
            while (*slash && *slash != '/') ++slash;
            if (!*slash) break;
            char comp[48]; size_t cl = (size_t)(slash - p);
            if (cl == 0 || cl >= sizeof comp) break;
            memcpy(comp, p, cl); comp[cl] = 0;
            ensure_dir(dir, comp);
            size_t dl = strlen(dir);
            if (dl + 1 + cl >= sizeof dir) break;
            dir[dl] = '/'; memcpy(dir + dl + 1, comp, cl + 1);
            p = slash + 1;
        }
        ensure_file(dir, p, files[i].data, files[i].len);
    }
}

void cos_system_layout_ensure(void) {
    { uint64_t sz = 0; bool d = false; if (fs_stat_path("/system/serial_verbose", &sz, &d)) serial_set_verbose(1); }       /* a file with this name turns the full log on (docs: LINUX_COMPAT.md) */
    ensure_dir("/", "bin");
    ensure_dir("/", "lib");                                              /* the default library search path: /lib:/usr/lib:/ */
    ensure_dir("/", "apps");
    ensure_dir("/apps", "samples");
    ensure_dir("/apps", "linux");                                                                             /* the viewer for Linux GUI programs (userland/programs/linuxgui), started by the Launcher's Linux tab */
    ensure_dir("/apps", "cosapps");
    ensure_dir("/apps", "minecraft");                                                                         /* the games the Launcher starts: Craft and the versions of mc-classic-c (docs/MC_CLASSIC.md) */
    ensure_dir("/", "etc");
    ensure_dir("/", "system");
    ensure_dir("/", "documents");
    ensure_dir("/", "pictures");
    ensure_dir("/", "music");
    ensure_dir("/", "downloads");
    ensure_dir("/music", "downloads");
    ensure_dir("/", "screenshots");
    ensure_dir("/", "desktop");
    ensure_dir("/", "tmp");
    ensure_dir("/", "C-OS Studio");
    ensure_dir("/C-OS Studio", "deliverables");
    ensure_dir("/system", "fonts");
    ensure_dir("/system", "ime");
    ensure_dir("/system", "lang");
    ensure_dir("/system", "drivers");
    ensure_dir("/system", "modtest");
    ensure_dir("/system", "plugins");                                      /* what the running programs scan (empty until a plugin is switched on) */
    ensure_dir("/system/plugins", "available");                            /* the plugins that ship with the system */
    ensure_dir("/system/plugins", "config");                               /* the plugins' settings (key=value files) */
    ensure_dir("/system", "update");
    ensure_dir("/system/update", "backup");
    fs_write_file_at("/system/update", "image.txt", "image " COS_IMAGE_STAMP "\n", sizeof("image " COS_IMAGE_STAMP "\n") - 1);
    update_state_load();
    {   /* a fresh system knows where its updates come from; the user can change it (Settings > Update) and an empty choice stays */
        uint64_t usz = 0; bool udir = false;
        if (!fs_stat_path("/system/update/url.txt", &usz, &udir) && sizeof COS_UPDATE_URL > 1) fs_write_file_at("/system/update", "url.txt", COS_UPDATE_URL, sizeof COS_UPDATE_URL - 1);
    }
    ensure_dir("/system", "sdk");
    ensure_dir("/system/sdk", "include");
    ensure_dir("/system/sdk/include", "sys");
    ensure_dir("/system/sdk", "lib");

    unsigned int mlen = 0;
    const unsigned char *music = cos_music_image(&mlen);
    if (music && mlen) ensure_file("/bin", "music.c-os", music, mlen);
    unsigned int vlen = 0;
    const unsigned char *media = cos_media_image(&vlen);
    if (media && vlen) ensure_file("/bin", "Media.c-os", media, vlen);
    ensure_file("/bin", "Photo.c-os", cos_photo_elf, cos_photo_elf_len);
    ensure_file("/bin", "test.c-os", cos_asset_test_c_os, cos_asset_test_c_os_len);
    ensure_file("/bin", "tcc.c-os", cos_tcc_elf, cos_tcc_elf_len);
    ensure_file("/bin", "cosarc.c-os", cos_cosarc_elf, cos_cosarc_elf_len);                               /* tar, gzip, gunzip, zcat, zip, unzip (the Terminal runs it) */
    ensure_file("/bin", "Py2C.c-os", cos_py2c_elf, cos_py2c_elf_len);
    ensure_file("/bin", "Terminal.c-os", cos_terminal_elf, cos_terminal_elf_len);
    ensure_file("/bin", "cosupdate.c-os", cos_cosupdate_elf, cos_cosupdate_elf_len);               /* app updates: cosupdate check | install | rollback (docs/app_update.md) */
    ensure_file("/bin", "CosMsgServer.c-os", cos_cosmsgd_elf, cos_cosmsgd_elf_len);       /* the Message relay server: C-OS Message /host on starts it */
    {   /* the File Manager (also embedded in the kernel: the desktop starts it from memory) and the CJK font it draws with */
        extern const unsigned char *cos_files_image(unsigned int *len);
        unsigned int flen = 0; const unsigned char *files = cos_files_image(&flen);
        if (files && flen) ensure_file("/bin", "files.c-os", files, flen);
        ensure_file("/system/fonts", "NotoSansJP-Regular.ttf", cos_jpfont, cos_jpfont_len);
        ensure_file("/system/lang", "ja.c-osll", cos_langja_so, cos_langja_so_len);          /* the Japanese pack of C-OS Message (a shared library of data) */
#include "cos_plugins.inc"
    }
    {   extern const unsigned char cos_ime_dic[], cos_ime_dic_end[];
        ensure_file_by_size("/system/ime", "cosime.dic", cos_ime_dic, (uint64_t)(cos_ime_dic_end - cos_ime_dic));
    }
    unsigned int slen = 0;
    const unsigned char *studio = cos_studio_image(&slen);
    if (studio && slen) ensure_file("/C-OS Studio", "C-OS Studio.c-os", studio, slen);
    if (studio && slen) ensure_file("/bin", "studio.c-os", studio, slen);                                        /* studio PATH [--build | --make [TARGET]] from the terminal */
    install_pack("/system/sdk", cos_sdk_files, COS_SDK_FILE_COUNT);
    ensure_dir("/apps", "Py2C");                                                                              /* the Py2C source tree: visible in the File Manager, buildable with tcc (docs/py2c_cos_port.md) */
    install_pack("/apps/Py2C", cos_p2csrc_files, COS_P2CSRC_FILE_COUNT);
    ensure_file("/bin", "python-code-to-c.c-os", cos_p2ccli_elf, cos_p2ccli_elf_len);                         /* the Py2C command line: cd /bin ; ./python-code-to-c run hello.py */
    ensure_file("/apps/minecraft", "Craft.c-os", cos_craft_elf, cos_craft_elf_len);
    ensure_file("/apps/linux", "Linux.c-os", cos_linuxgui_elf, cos_linuxgui_elf_len);
    ensure_file("/apps/linux", "LinuxInstall.c-os", cos_linuxpack_elf, cos_linuxpack_elf_len);                  /* downloads and installs a Linux app pack (userland/programs/linuxpack) */
    ensure_file("/bin", "sprint.c-os", cos_sprint_elf, cos_sprint_elf_len);                                     /* the Sprint interpreter (ports/sprint) */
    ensure_file("/apps/cosapps", "CosApps.sp", cos_cosapps_sp, cos_cosapps_sp_len);                              /* the store: C-OS Apps, a Sprint program (ports/sprint/apps/cosapps.sp) */
    ensure_file("/apps/cosapps", "icon.png", cos_cosapps_icon, cos_cosapps_icon_len);
    {   /* its desktop shortcut: put there once (the user may delete it); the first version of it started a C program, which is replaced by the Sprint one */
        static const char sc[] = "# C-OS app shortcut\nname=C-OS Apps\nexec=/apps/cosapps/CosApps.sp\nicon=/apps/cosapps/icon.png\n"
            "args=--allow-ffi --allow-read=/apps --allow-read=/system --allow-read=/desktop --allow-write=/apps --allow-write=/desktop --allow-write=/system\n";
        uint64_t sz = 0; bool dir = false;
        if (fs_stat_path("/apps/cosapps/CosApps.c-os", &sz, &dir)) fs_delete_file("/apps/cosapps/CosApps.c-os");
        if (!fs_stat_path("/system/apps/seeded2", &sz, &dir)) {
            ensure_dir("/", "system"); ensure_dir("/system", "apps");
            if (fs_write_file_at("/desktop", "C-OS Apps.app", sc, sizeof sc - 1)) fs_write_file_at("/system/apps", "seeded2", "1", 1);
        }
    }
#include "cos_mc_games.inc"
#include "cos_mc_audio.inc"
    ensure_file("/apps/samples", "ui_gallery.c-os", cos_sample_gallery_elf, cos_sample_gallery_elf_len);
    ensure_file("/apps/samples", "tone.c-os", cos_sample_tone_elf, cos_sample_tone_elf_len);
    ensure_file("/apps/samples", "widgets.c-os", cos_sample_widgets_elf, cos_sample_widgets_elf_len);
    ensure_file("/system/drivers", "hello.c-osys", cos_kmod_hello, cos_kmod_hello_len);                 /* kernel modules: signed objects loaded by cos_kmod_init() */
    ensure_file("/system/drivers", "kbase.c-osys", cos_kmod_kbase, cos_kmod_kbase_len);                 /* a library module ... */
    ensure_file("/system/drivers", "kuser.c-osys", cos_kmod_kuser, cos_kmod_kuser_len);                 /* ... and a module that needs it */
    ensure_file("/system/drivers", "audit.c-osys", cos_kmod_audit, cos_kmod_audit_len);                 /* the audit log of sandbox refusals and process events */
    ensure_file("/system/drivers", "pci.c-osys", cos_kmod_pci, cos_kmod_pci_len);                       /* the PCI inventory (a DEVICE-level module) */
    ensure_file("/system/drivers", "virtiorng.c-osys", cos_kmod_virtiorng, cos_kmod_virtiorng_len);    /* a device driver: the virtio random-number device */
    ensure_file("/system/drivers", "wprotect.c-osys", cos_kmod_wprotect, cos_kmod_wprotect_len);        /* SYSTEM level: write protection of /protected/ */
    ensure_file("/system/drivers", "netfw.c-osys", cos_kmod_netfw, cos_kmod_netfw_len);                 /* SYSTEM level: frame counters and a small firewall */
    ensure_file("/system/drivers", "edu.c-osys", cos_kmod_edu, cos_kmod_edu_len);                       /* test device: MSI and I/O APIC interrupts */
#if COS_VALIDATION_KMOD
    ensure_file("/system/drivers", "needs_kernel.c-osys", cos_kmod_fx_needs_kernel, cos_kmod_fx_needs_kernel_len);      /* the loader must refuse these six */
    ensure_file("/system/drivers", "tampered.c-osys", cos_kmod_fx_tampered, cos_kmod_fx_tampered_len);
    ensure_file("/system/drivers", "unsigned.c-osys", cos_kmod_fx_unsigned, cos_kmod_fx_unsigned_len);
    ensure_file("/system/drivers", "wrongkey.c-osys", cos_kmod_fx_wrongkey, cos_kmod_fx_wrongkey_len);
    ensure_file("/system/drivers", "too_privileged.c-osys", cos_kmod_fx_too_privileged, cos_kmod_fx_too_privileged_len);
    ensure_file("/system/drivers", "lowkey_system.c-osys", cos_kmod_fx_lowkey_system, cos_kmod_fx_lowkey_system_len);    /* a SYSTEM-level module signed with the level-1 key */
    ensure_file("/system/modtest", "rt.c-osys", cos_kmod_fx_rt, cos_kmod_fx_rt_len);                 /* not loaded at boot: the tests load it at run time */
#endif
#if COS_VALIDATION_TESTPROGS
    ensure_dir("/", "linux-test");                                                                        /* Linux programs for tools/run_linux_test.sh */
    ensure_file("/linux-test", "hello", cos_linuxtest_hello, cos_linuxtest_hello_len);
    ensure_file("/linux-test", "hello_dyn", cos_linuxtest_hello_dyn, cos_linuxtest_hello_dyn_len);
    ensure_file("/linux-test", "threads", cos_linuxtest_threads, cos_linuxtest_threads_len);
    ensure_file("/linux-test", "fds", cos_linuxtest_fds, cos_linuxtest_fds_len);
    ensure_file("/linux-test", "procs", cos_linuxtest_procs, cos_linuxtest_procs_len);
    ensure_file("/linux-test", "avx", cos_linuxtest_avx, cos_linuxtest_avx_len);
    ensure_dir("/", "linux"); ensure_dir("/linux", "lib64"); ensure_dir("/linux", "lib"); ensure_dir("/linux/lib", "x86_64-linux-gnu"); ensure_dir("/linux", "etc");   /* the root file system of Linux programs */
    ensure_file("/linux/lib64", "ld-linux-x86-64.so.2", cos_linuxtest_ld, cos_linuxtest_ld_len);
    ensure_file("/linux/lib/x86_64-linux-gnu", "libc.so.6", cos_linuxtest_libc, cos_linuxtest_libc_len);
#include "cos_linuxroot.inc"                                                                          /* LINUX_ROOTFS: more files (tools/linux_rootfs_pack.py) */
    ensure_file("/bin", "dltest.c-os", cos_dltest, cos_dltest_len);                                      /* loader checks: tools/dltest, run by tools/run_dl_test.sh */
    ensure_file("/bin", "dlchild.c-os", cos_dlchild, cos_dlchild_len);
    ensure_file("/bin", "dluse.c-os", cos_dluse, cos_dluse_len);
    ensure_file("/lib", "dla.c-osll", cos_dla_so, cos_dla_so_len);                                       /* test libraries: constructors, a dependency, a failing import, a preload */
    ensure_file("/lib", "dlb.c-osll", cos_dlb_so, cos_dlb_so_len);
    ensure_file("/lib", "dlbad.c-osll", cos_dlbad_so, cos_dlbad_so_len);
    ensure_file("/lib", "dlp.c-osll", cos_dlp_so, cos_dlp_so_len);
    ensure_file("/bin", "imgdev.c-os", cos_imgdev, cos_imgdev_len);                                     /* image decoder checks: tools/imgdev, run by tools/run_imgdev_test.sh */
    ensure_dir("/system", "imgdev");
    for (int i = 0; cos_imgdev_files[i].name; ++i)
        if (cos_imgdev_files[i].len) ensure_file("/system/imgdev", cos_imgdev_files[i].name, cos_imgdev_files[i].data, cos_imgdev_files[i].len);
    ensure_file("/bin", "ssptest.c-os", cos_ssptest, cos_ssptest_len);                                  /* overflows a buffer: the stack protector must stop it */
    ensure_file("/bin", "posixtest.c-os", cos_posixtest, cos_posixtest_len);                           /* POSIX layer checks: tools/posixtest, run by tools/run_posix_test.sh */
#endif
    { extern void cos_kmod_init(void); cos_kmod_init(); }                                              /* loads /system/drivers/*.c-osys, before any program runs */
    ensure_file("/system", "README.txt", k_readme, sizeof(k_readme) - 1);
    ensure_file("/etc", "os-release", k_os_release, sizeof(k_os_release) - 1);
    ensure_file("/etc", "hostname", k_hostname, sizeof(k_hostname) - 1);
    {   extern void cos_bootupd_boot(void); cos_bootupd_boot(); }                                   /* kernel updates: which slot this kernel came from, confirm a trial kernel (docs/app_update.md) */
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

/* Validation only (COS_VALIDATION_LAUNCH_WIDGETS): start the cos_widget demo. */
void cos_validation_launch_widgets(void) {
    extern int64_t cos_launch_elf_image(const char *name, const unsigned char *image, unsigned int len);
    (void)cos_launch_elf_image("widgets.c-os", cos_sample_widgets_elf, cos_sample_widgets_elf_len);
}

/* Validation only (COS_VALIDATION_SANDBOX): three instances of the sandbox/ASLR test. The first does the capability
 * checks; all three print their address-space layout so the run can verify it differs per process. */
void cos_validation_launch_sandbox_test(void) {
    extern int64_t cos_launch_elf_image(const char *name, const unsigned char *image, unsigned int len);
    for (int i = 0; i < 3; ++i) (void)cos_launch_elf_image("sandbox.c-os", cos_sample_sandbox_elf, cos_sample_sandbox_elf_len);
}

/* Desktop and shell Py2C entry point: use the same ring-3 binary installed as
 * /bin/Py2C.c-os, so there is no second incomplete kernel UI. */
void cos_py2c_open(void) {
    static uint32_t pid;
    if (pid) {
        process_t *p = process_get_by_pid(pid);
        if (p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED) return;
    }
    const char *argv[1] = { "Py2C.c-os" };
    int64_t launched = cos_launch_elf_image_args("Py2C.c-os", cos_py2c_elf, cos_py2c_elf_len, argv, 1, NULL, 0);
    pid = launched > 0 ? (uint32_t)launched : 0;
    serial_puts(launched > 0 ? "[PY2C] launched\n" : "[PY2C] launch FAILED\n");
}
