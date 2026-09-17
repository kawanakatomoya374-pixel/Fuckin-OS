#ifndef COS_JP_FONT16_H
#define COS_JP_FONT16_H

#include <stdint.h>

typedef struct {
    uint32_t codepoint;
    uint16_t rows[16];
} cos_jp_font16_glyph_t;

extern const cos_jp_font16_glyph_t cos_jp_font16[];
extern const uint32_t cos_jp_font16_count;

#endif
