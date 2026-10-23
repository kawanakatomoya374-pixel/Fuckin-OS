/* Public entry points for the FFmpeg-derived HEVC (H.265) decoder port (see
 * cos_hevc_entry.c). This is the only header media/main.c needs from this
 * directory. */
#ifndef COS_HEVC_H
#define COS_HEVC_H

#include <stdint.h>

typedef struct cos_hevc_frame {
    const uint8_t* y;
    const uint8_t* u;
    const uint8_t* v;
    int width, height;       /* visible size (conformance window already applied) */
    int stride_y, stride_c;  /* bytes */
    int bit_depth;           /* 8, 10 or 12; samples are 16-bit little-endian above 8 */
} cos_hevc_frame_t;

/* Open a decoder. `hvcc` is the payload of the MP4 'hvcC' box (the codec
 * configuration record: NAL length size plus VPS/SPS/PPS). NULL/0 is accepted
 * for streams whose parameter sets arrive in-band; samples are then expected
 * to be 4-byte length-prefixed. */
void* cos_hevc_open(const uint8_t* hvcc, uint32_t hvcc_len);
void  cos_hevc_close(void* h);

/* Coded picture size and bit depth, known once the decoder has processed a
 * first sample (it comes from the active SPS). Returns 0 and fills the
 * outputs, or -1 if no picture has been set up yet. */
int cos_hevc_info(void* h, int* width, int* height, int* bit_depth);

/* Feed one MP4 sample (length-prefixed NAL units). Return values:
 *   -1  decode error          0  no frame ready yet
 *    1  *out filled           2  a frame was decoded but its format is not 4:2:0
 * Output frames come out in display order, delayed as the stream requires.
 * The returned pointers stay valid until the next call on this handle. */
int cos_hevc_decode(void* h, const uint8_t* data, uint32_t size, cos_hevc_frame_t* out);

/* At end of stream: call repeatedly until it returns 0 to collect the frames the
 * decoder is still holding back for reordering. Same return values as above. */
int cos_hevc_drain(void* h, cos_hevc_frame_t* out);

/* ---- frame-threaded decoding across cores (see cos_hevc_entry.c) ---------------------
 * A group of N workers decodes N packets at once. Packet `seq` (0,1,2,... counting every call,
 * drain calls included) is handled by worker seq % N; the caller collects the results in
 * seq order, which reproduces the output of a single decoder exactly. */
typedef struct cos_hevc_group cos_hevc_group_t;
cos_hevc_group_t* cos_hevc_group_create(int nthreads);
void cos_hevc_group_destroy(cos_hevc_group_t* g);
int64_t cos_hevc_group_debug(cos_hevc_group_t* g, int idx);
void cos_hevc_group_abort(cos_hevc_group_t* g);        /* wake workers that are waiting on each other */
/* Worker side: each worker opens its own context (on its own stack/arena), then decodes its packets. */
void* cos_hevc_mt_open(cos_hevc_group_t* g, int idx, const uint8_t* hvcc, uint32_t hvcc_len);
/* size == 0 drains. Returns -1 error, 0 no frame, 1 frame (*out_frame is a handle), 2 unsupported format. */
int   cos_hevc_mt_decode(void* h, int64_t seq, const uint8_t* data, uint32_t size, void** out_frame);
/* Any thread: describe a frame handle (rc as cos_hevc_decode; pointers valid until released) and release it. */
int   cos_hevc_describe(const void* frame, cos_hevc_frame_t* out);
void  cos_hevc_release(void* frame);

#endif
