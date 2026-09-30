/**
 * crc32_common.h - C-OS 共通CRC32ユーティリティ
 *
 * これまで storage.c / ide.c / (フォーマット違いで) storage_image.c に
 * それぞれ独自実装されていたCRC32テーブル・計算ロジックを一本化したもの。
 * 新規にCRC32が必要な箇所は、ここを include して cos_crc32() /
 * cos_crc32_skip() を使うこと。テーブル自体は本体側に一つだけ存在する
 * (crc32_common.c)。
 */
#ifndef COS_CRC32_COMMON_H
#define COS_CRC32_COMMON_H

#include <stdint.h>
#include <stddef.h>

/* 標準CRC-32 (poly 0xEDB88320, IEEE 802.3)。data[0..length) に対する値。 */
uint64_t cos_crc32(const void* data, size_t length);

/* data[0..length) の CRC32 を計算するが、[skip_off, skip_off+skip_len) の
 * 範囲だけは計算対象から除外する。
 * 用途: 構造体自身にchecksumフィールドを埋め込んでいる場合、そのフィールド
 * 自身を計算対象から除外して自己参照を避けるため。
 */
uint64_t cos_crc32_skip(const void* data, size_t length, size_t skip_off, size_t skip_len);

#endif /* COS_CRC32_COMMON_H */
