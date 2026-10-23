/* Public entry points for the FFmpeg-derived VP9 decoder port (see
 * cos_vp9_entry.c). This is the only header media/main.c needs to
 * include from this directory. */
#ifndef COS_VP9_H
#define COS_VP9_H

#include <stdint.h>

typedef struct cos_vp9_frame {
    const uint8_t* y;
    const uint8_t* u;
    const uint8_t* v;
    int width, height;
    int stride_y, stride_c;
    int bit_depth; /* 8, 10 or 12 - 12-bit is reported but media/main.c currently only accepts 8/10 */
} cos_vp9_frame_t;

void* cos_vp9_open(void);
void  cos_vp9_close(void* h);
/* -1 error, 0 no frame yet, 1 *out filled - see cos_vp9_entry.c's comment. */
int   cos_vp9_decode(void* h, const uint8_t* data, uint32_t size, cos_vp9_frame_t* out);

/* ---- frame-threaded decoding across cores (see cos_vp9_entry.c, and cos_hevc.h for the same scheme) ---- */
typedef struct cos_vp9_group cos_vp9_group_t;
cos_vp9_group_t* cos_vp9_group_create(int nthreads);
void cos_vp9_group_destroy(cos_vp9_group_t* g);
void cos_vp9_group_abort(cos_vp9_group_t* g);
void* cos_vp9_mt_open(cos_vp9_group_t* g, int idx);
/* Cuts an MP4 sample into its VP9 frames (a superframe holds several). Returns the count, 0 for an empty
 * sample, -1 if the index is damaged. */
int   cos_vp9_split(const uint8_t* data, uint32_t size, uint32_t offs[8], uint32_t lens[8]);
/* One VP9 frame as packet `seq`. Returns -1 error, 0 no frame, 1 frame (*out_frame is a handle). */
int   cos_vp9_mt_decode(void* h, int64_t seq, const uint8_t* data, uint32_t size, void** out_frame);
int   cos_vp9_describe(const void* frame, cos_vp9_frame_t* out);   /* 1 ok, 0 not 4:2:0 */
void  cos_vp9_release(void* frame);

#endif
