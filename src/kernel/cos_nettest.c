/* cos_nettest.c - validation only (COS_VALIDATION_NETSOCK=1): start the ring-3 socket test (userland/programs/netsock_test). */
#include <stdint.h>
#include "cos_netsock_test_elf.h"
extern int64_t cos_launch_elf_image(const char *name, const unsigned char *image, unsigned int len);
void cos_validation_launch_netsock_test(void) {
    (void)cos_launch_elf_image("netsock_test.c-os", cos_netsock_test_elf, cos_netsock_test_elf_len);
}
