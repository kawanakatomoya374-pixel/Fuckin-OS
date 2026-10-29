/* cosarc_in.h - the buffered input of cosarc, shared with the archive formats (gzip, zip, tar) */
#ifndef COSARC_IN_H
#define COSARC_IN_H
#include "cosarc.h"
#define ARC_IN_BUF 32768
typedef struct { arc_read_fn rd; void *ctx; uint8_t buf[ARC_IN_BUF]; size_t len, pos; int eof; } arc_in;
void   arc_in_init(arc_in *in, arc_read_fn rd, void *ctx);
int    arc_in_getc(arc_in *in);                       /* -1 at the end */
size_t arc_in_read(arc_in *in, void *dst, size_t n);
/* like arc_inflate / arc_deflate, but on a buffered input that stays usable afterwards: the bytes behind the deflate stream (the gzip trailer, the next member) are still there */
int arc_inflate_in(arc_in *in, arc_write_fn wr, void *wctx, uint32_t *crc, uint64_t *size);
int arc_deflate_in(arc_in *in, arc_write_fn wr, void *wctx, uint32_t *crc, uint64_t *size, uint64_t *csize, int level);
#endif
