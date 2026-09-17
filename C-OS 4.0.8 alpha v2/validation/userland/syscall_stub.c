/* Host stubs for the syscalls the userland library makes.
 *
 * Only what the tested code actually reaches. Anything else is left
 * undefined on purpose: a test that accidentally calls into the kernel
 * should fail to link rather than silently exercise a fake. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long ssize_t_;

ssize_t_ cos_write(const char *buf, size_t len)
{
    fwrite(buf, 1, len, stdout);
    return (ssize_t_)len;
}
