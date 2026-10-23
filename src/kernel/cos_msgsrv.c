/* cos_msgsrv.c - validation only (COS_VALIDATION_MSGSRV=1): start the ring-3 Message relay server (userland/programs/cosmsgd).
 * =2: first write /cosmsgd/google.conf and /cosmsgd/ca.der from build/cosmsgd_test_conf.h (tools/run_cosmsgd_google_qemu.sh makes it),
 * so the Google sign-in can be tested against a fake Google. */
#include <stdint.h>
#include <stdbool.h>
#include "cos_cosmsgd_elf.h"
#if COS_VALIDATION_MSGSRV >= 2
#include "cosmsgd_test_conf.h"
extern bool cos_fs_mkdir(const char *path);
extern bool fs_write_file_at(const char *dir, const char *name, const char *data, uint64_t size);
#endif
extern int64_t cos_launch_elf_image(const char *name, const unsigned char *image, unsigned int len);
void cos_validation_launch_msgsrv(void) {
#if COS_VALIDATION_MSGSRV >= 2
    cos_fs_mkdir("/cosmsgd");
    fs_write_file_at("/cosmsgd", "google.conf", (const char *)TEST_CONF, sizeof TEST_CONF);
    fs_write_file_at("/cosmsgd", "ca.der", (const char *)TEST_CA, sizeof TEST_CA);
#endif
    (void)cos_launch_elf_image("cosmsgd.c-os", cos_cosmsgd_elf, cos_cosmsgd_elf_len);
}
