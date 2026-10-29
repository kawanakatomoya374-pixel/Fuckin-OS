/* cosarc.h - gzip, zip and tar for C-OS programs (and for the host: the same source is tested against gzip, tar and unzip).
 *
 * Streams, not buffers: every function reads through a callback and writes through a callback, with 32 KiB of window and a few 32-64 KiB buffers, so a
 * file of any size (an exFAT file of many GiB) is handled in constant memory. Sizes and offsets are 64 bit.
 *
 *   arc_inflate   raw deflate stream (RFC 1951) -> bytes
 *   arc_deflate   bytes -> raw deflate stream (LZ77 + the fixed Huffman code: a little larger than zlib's output, but fast and small)
 *   arc_crc32     CRC-32 (the gzip/zip one)
 *   cosarc_main   the commands: tar, gzip, gunzip, zcat, zip, unzip (see cosarc_cli.c)
 */
#ifndef COSARC_H
#define COSARC_H
#include <stdint.h>
#include <stddef.h>

typedef size_t (*arc_read_fn)(void *ctx, void *buf, size_t n);          /* returns the bytes read, 0 at the end (or on an error) */
typedef int    (*arc_write_fn)(void *ctx, const void *buf, size_t n);   /* returns 0 on success */

uint32_t arc_crc32(uint32_t crc, const void *buf, size_t n);            /* start with crc = 0 */

/* 0 = ok, otherwise: 1 truncated input, 2 bad data, 3 write failed, 4 out of memory. *crc / *size (optional) get the CRC-32 and the length of what was produced. */
int arc_inflate(arc_read_fn rd, void *rctx, arc_write_fn wr, void *wctx, uint32_t *crc, uint64_t *size);
int arc_deflate(arc_read_fn rd, void *rctx, arc_write_fn wr, void *wctx, uint32_t *crc, uint64_t *size, uint64_t *csize, int level);   /* level 1 (fast) .. 9; csize = bytes written */

int cosarc_main(int argc, char **argv, const char *cwd, void (*print)(const char *line));   /* argv[0] is the command name: tar, gzip, gunzip, zcat, zip, unzip; relative paths are taken from cwd; messages go to print, one line each */
int cosarc_is_command(const char *name);
void cosarc_set_progress(const char *path);   /* the commands then write "DONE TOTAL\nNAME\n" to this file as they work (bytes); "" turns it off */
#endif
