/* 自動生成ファイル（tools/gen_unicode_tables.py）。手で編集しないこと。
 * Unicode の文字判定と大小変換。判定は CPython の str.isalpha() 等と同じ結果を返す。 */
#ifndef PYTHON_CODE_TO_C_UNICODE_H
#define PYTHON_CODE_TO_C_UNICODE_H

#include "common/python_code_to_c_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 判定（CPython の str.is* と同じ定義を1文字に対して適用した結果） */
bool p2c_uni_is_alpha(uint32_t cp);
bool p2c_uni_is_decimal(uint32_t cp);
bool p2c_uni_is_digit(uint32_t cp);
bool p2c_uni_is_numeric(uint32_t cp);
bool p2c_uni_is_space(uint32_t cp);
bool p2c_uni_is_printable(uint32_t cp);
bool p2c_uni_is_upper(uint32_t cp);
bool p2c_uni_is_lower(uint32_t cp);
bool p2c_uni_is_title(uint32_t cp);
bool p2c_uni_is_cased(uint32_t cp);
bool p2c_uni_is_xid_start(uint32_t cp);
bool p2c_uni_is_xid_continue(uint32_t cp);
bool p2c_uni_is_case_ignorable(uint32_t cp);

/* 大小変換の種類 */
enum {
    P2C_UNI_CASE_LOWER = 0,
    P2C_UNI_CASE_UPPER = 1,
    P2C_UNI_CASE_TITLE = 2,
    P2C_UNI_CASE_FOLD = 3,
};

/* cp の変換結果（完全な写像）を out へ書き、コードポイント数（1〜3）を返す。
 * 変換しない文字は out[0] = cp と 1 を返す。 */
int p2c_uni_case_map(uint32_t cp, int mode, uint32_t out[3]);

#ifdef __cplusplus
}
#endif

#endif
