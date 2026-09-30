#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char* s);
void serial_puthex(uint64_t val);
void serial_putdec(uint64_t val);
char serial_getc(void);

/* Rate-limited diagnostic channel - see the long note in serial.c.
 *
 * serial_putc() busy-waits on the UART, so a diagnostic line costs
 * milliseconds. Hot paths that log on a condition ("this allocation was
 * slow", "this frame was slow") can therefore become the reason the
 * condition keeps tripping. Gate any such line on serial_diag_allow(),
 * which caps output to a few lines per second and is OFF by default:
 *
 *     if (serial_diag_allow()) {
 *         serial_puts("[TAG] ...");  serial_putdec(v);  serial_puts("\n");
 *     }
 *
 * Gate the whole line on ONE call - checking per serial_puts() would let
 * a message be truncated halfway, which is worse than dropping it. */
void serial_diag_enable(int on);
int  serial_diag_enabled(void);
int  serial_diag_allow(void);

#endif
