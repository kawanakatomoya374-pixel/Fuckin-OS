/* zlib.h - the little of zlib that mc-classic-c uses, for C-OS: gzFile (files it writes itself are stored uncompressed; gzip files are
 * inflated when read) and the one-shot inflate that its multiplayer code uses. Implemented in mcrt.c on top of stb_image's inflate. */
#ifndef COS_MC_ZLIB_H
#define COS_MC_ZLIB_H
typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;
typedef void *gzFile;
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_DATA_ERROR (-3)
#define Z_BUF_ERROR (-5)
#define Z_NO_FLUSH 0
#define Z_SYNC_FLUSH 2
#define Z_FINISH 4
#define Z_DEFLATED 8
#define Z_DEFAULT_STRATEGY 0
typedef struct z_stream_s {
    const Bytef *next_in; uInt avail_in; uLong total_in;
    Bytef *next_out; uInt avail_out; uLong total_out;
    int window_bits; void *state;
} z_stream;
gzFile gzopen(const char *path, const char *mode);
int gzread(gzFile f, void *buf, unsigned len);
int gzwrite(gzFile f, const void *buf, unsigned len);
int gzclose(gzFile f);
int gzdirect(gzFile f);
int inflateInit2(z_stream *s, int window_bits);
int inflate(z_stream *s, int flush);
int inflateEnd(z_stream *s);
int deflateInit2(z_stream *s, int level, int method, int window_bits, int mem_level, int strategy);
int deflate(z_stream *s, int flush);
int deflateEnd(z_stream *s);
uLong deflateBound(z_stream *s, uLong len);
uLong crc32(uLong crc, const Bytef *buf, uInt len);
#endif
