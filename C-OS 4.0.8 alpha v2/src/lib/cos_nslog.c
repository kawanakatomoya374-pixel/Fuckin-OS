/**
 * cos_nslog.c - real implementation of nslog_log(), the function
 * NetSurf's NSLOG(category, level, msg, ...) macro (utils/log.h)
 * calls when WITH_NSLOG isn't defined (which it isn't in this
 * build). Used throughout content/ and utils/ for diagnostic
 * logging - genuinely useful output, not just something to silence.
 *
 * Formats the same way real nslog_log() does conceptually (a
 * printf-style message, tagged with source location) but writes to
 * the serial console via vsnprintf() (lib/string.c) instead of real
 * hosted stdio, consistent with how the rest of this kernel logs.
 */
#include "serial.h"
#include "string.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

void nslog_log(const char *file, const char *func, int ln, const char *format, ...)
{
    char buf[512];

    serial_puts("[NSLOG] ");
    if (file != NULL) {
        serial_puts(file);
    }
    if (func != NULL && func[0] != '\0') {
        serial_puts(" (");
        serial_puts(func);
        serial_puts(")");
    }
    serial_puts(":");
    {
        char linebuf[16];
        utoa((uint64_t)(ln < 0 ? -ln : ln), linebuf, 10);
        if (ln < 0) serial_puts("-");
        serial_puts(linebuf);
    }
    serial_puts(": ");

    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    serial_puts(buf);
    serial_puts("\n");
}
