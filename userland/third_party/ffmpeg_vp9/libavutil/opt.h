/* Minimal stand-in for libavutil/opt.h: just the AVClass/AVOption shapes the
 * ported HEVC decoder uses for its (never queried) private-options table. */
#ifndef COS_FFMPEG_LIBAVUTIL_OPT_H
#define COS_FFMPEG_LIBAVUTIL_OPT_H
#include <stddef.h>
#include <stdint.h>
#include "log.h"

#define AV_OPT_FLAG_DECODING_PARAM 2
#define AV_OPT_FLAG_VIDEO_PARAM    16
#define AV_OPT_FLAG_AUDIO_PARAM    8
#define AV_OPT_FLAG_ENCODING_PARAM 1

enum AVOptionType { AV_OPT_TYPE_FLAGS, AV_OPT_TYPE_INT, AV_OPT_TYPE_INT64, AV_OPT_TYPE_DOUBLE,
                    AV_OPT_TYPE_FLOAT, AV_OPT_TYPE_STRING, AV_OPT_TYPE_RATIONAL, AV_OPT_TYPE_BINARY,
                    AV_OPT_TYPE_CONST, AV_OPT_TYPE_BOOL };

typedef struct AVOption {
    const char* name;
    const char* help;
    int offset;
    enum AVOptionType type;
    union { int64_t i64; double dbl; const char* str; } default_val;
    double min, max;
    int flags;
    const char* unit;
} AVOption;

struct AVClass {
    const char* class_name;
    const char* (*item_name)(void* ctx);
    const AVOption* option;
    int version;
};

const char* av_default_item_name(void* ctx);
#endif
