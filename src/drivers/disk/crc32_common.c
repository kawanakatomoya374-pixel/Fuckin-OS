/**
 * crc32_common.c - C-OS 共通CRC32実装（テーブルはここに1箇所だけ存在）
 */
#include "crc32_common.h"

static uint64_t g_crc32_table[256];
static int g_crc32_ready = 0;

static void crc32_table_init(void) {
    if (g_crc32_ready) return;
    for (uint64_t i = 0; i < 256; ++i) {
        uint64_t crc = i;
        for (uint64_t j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320ULL & (uint64_t)(-(int64_t)(crc & 1ULL)));
        }
        g_crc32_table[i] = crc;
    }
    g_crc32_ready = 1;
}

uint64_t cos_crc32(const void* data, size_t length) {
    crc32_table_init();
    const uint8_t* p = (const uint8_t*)data;
    uint64_t crc = 0xFFFFFFFFULL;
    for (size_t i = 0; i < length; ++i) {
        crc = (crc >> 8) ^ g_crc32_table[(crc ^ p[i]) & 0xFFu];
    }
    return ~crc & 0xFFFFFFFFULL;
}

uint64_t cos_crc32_skip(const void* data, size_t length, size_t skip_off, size_t skip_len) {
    crc32_table_init();
    const uint8_t* p = (const uint8_t*)data;
    uint64_t crc = 0xFFFFFFFFULL;
    for (size_t i = 0; i < length; ++i) {
        if (i >= skip_off && i < (skip_off + skip_len)) continue;
        crc = (crc >> 8) ^ g_crc32_table[(crc ^ p[i]) & 0xFFu];
    }
    return ~crc & 0xFFFFFFFFULL;
}
