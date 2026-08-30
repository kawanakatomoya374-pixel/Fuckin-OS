/**
 * efm_util.c - Enhanced File Manager (文字列/UTF-8ユーティリティ・ファイル種別判定・サイズ/時刻フォーマット)
 * enhanced_file_manager.c から分割生成。詳細は efm_internal.h を参照。
 */

#include "efm_internal.h"
#include "../include/serial.h"
#include "../include/memory.h"
#include "../include/types.h"
#include "vga.h"
#include "../fs/fs.h"
#include <string.h>
#include <stdio.h>

/* ============================================================
 * ユーティリティ
 * ============================================================ */

/* UTF-8 のマルチバイト文字を途中で切らないように、表示幅(半角換算)を
 * max_width 以内に収めて末尾に "..." を付与する。日本語などの全角文字を
 * 含むファイル名がアイコン/リスト表示で文字化けするのを防ぐ。 */
void efm_utf8_truncate(const char* src, char* out, size_t out_size, int max_width) {
    if (!src || !out || out_size == 0) return;
    if (max_width < 4) max_width = 4;
    size_t oi = 0;
    int width = 0;
    size_t i = 0;
    size_t src_len = strlen(src);
    size_t last_fit_i = 0;
    int last_fit_width = 0;
    bool truncated = false;
    while (i < src_len) {
        unsigned char c = (unsigned char)src[i];
        int clen = 1;
        int cw = 1; /* 半角幅換算 */
        if ((c & 0x80) == 0x00) { clen = 1; cw = 1; }
        else if ((c & 0xE0) == 0xC0) { clen = 2; cw = 1; }
        else if ((c & 0xF0) == 0xE0) { clen = 3; cw = 2; } /* 3バイト = 日本語等の全角想定 */
        else if ((c & 0xF8) == 0xF0) { clen = 4; cw = 2; }
        if (i + (size_t)clen > src_len) clen = 1;
        if (width + cw > max_width) { truncated = true; break; }
        if (oi + (size_t)clen >= out_size) { truncated = true; break; }
        for (int k = 0; k < clen; k++) out[oi++] = src[i + k];
        width += cw;
        i += (size_t)clen;
        last_fit_i = i;
        last_fit_width = width;
    }
    (void)last_fit_i; (void)last_fit_width;
    out[oi < out_size ? oi : out_size - 1] = '\0';
    if (truncated && i < src_len) {
        /* "..." を付けるための余白を確保して再構築 */
        size_t budget = out_size > 4 ? out_size - 4 : 0;
        if (oi > budget) oi = budget;
        out[oi] = '\0';
        strncat(out, "...", out_size - strlen(out) - 1);
    }
}

/* UTF-8 文字列の表示幅(半角換算)を計算する。中央揃え時のオフセット計算に使う。 */
int efm_utf8_display_width(const char* s) {
    if (!s) return 0;
    int width = 0;
    size_t i = 0, len = strlen(s);
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        int clen = 1, cw = 1;
        if ((c & 0x80) == 0x00) { clen = 1; cw = 1; }
        else if ((c & 0xE0) == 0xC0) { clen = 2; cw = 1; }
        else if ((c & 0xF0) == 0xE0) { clen = 3; cw = 2; }
        else if ((c & 0xF8) == 0xF0) { clen = 4; cw = 2; }
        if (i + (size_t)clen > len) clen = 1;
        width += cw;
        i += (size_t)clen;
    }
    return width;
}

static bool efm_has_suffix(const char* name, const char* suffix) {
    if (!name || !suffix) return false;
    size_t nl = strlen(name), sl = strlen(suffix);
    if (sl > nl) return false;
    const char* p = name + nl - sl;
    /* 大文字小文字無視 */
    for (size_t i = 0; i < sl; i++) {
        char a = p[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

efm_file_type_t efm_get_file_type(const char* name) {
    if (!name) return EFM_TYPE_UNKNOWN;
    if (efm_has_suffix(name, ".jpg") || efm_has_suffix(name, ".jpeg") ||
        efm_has_suffix(name, ".png") || efm_has_suffix(name, ".bmp") ||
        efm_has_suffix(name, ".gif")) return EFM_TYPE_IMAGE;
    if (efm_has_suffix(name, ".mp3") || efm_has_suffix(name, ".wav") ||
        efm_has_suffix(name, ".ogg") || efm_has_suffix(name, ".flac")) return EFM_TYPE_AUDIO;
    if (efm_has_suffix(name, ".mp4") || efm_has_suffix(name, ".avi") ||
        efm_has_suffix(name, ".mkv") || efm_has_suffix(name, ".mov")) return EFM_TYPE_VIDEO;
    if (efm_has_suffix(name, ".c") || efm_has_suffix(name, ".h") ||
        efm_has_suffix(name, ".cpp") || efm_has_suffix(name, ".py") ||
        efm_has_suffix(name, ".js") || efm_has_suffix(name, ".ts")) return EFM_TYPE_CODE;
    if (efm_has_suffix(name, ".lua")) return EFM_TYPE_LUA;
    if (efm_has_suffix(name, ".zip") || efm_has_suffix(name, ".tar") ||
        efm_has_suffix(name, ".gz") || efm_has_suffix(name, ".bz2") ||
        efm_has_suffix(name, ".7z") || efm_has_suffix(name, ".rar")) return EFM_TYPE_ARCHIVE;
    if (efm_has_suffix(name, ".txt") || efm_has_suffix(name, ".md") ||
        efm_has_suffix(name, ".log") || efm_has_suffix(name, ".csv")) return EFM_TYPE_TEXT;
    if (efm_has_suffix(name, ".cfg") || efm_has_suffix(name, ".ini") ||
        efm_has_suffix(name, ".json") || efm_has_suffix(name, ".xml") ||
        efm_has_suffix(name, ".yaml") || efm_has_suffix(name, ".toml")) return EFM_TYPE_CONFIG;
    if (efm_has_suffix(name, ".elf") || efm_has_suffix(name, ".exe") ||
        efm_has_suffix(name, ".bin") || efm_has_suffix(name, ".out")) return EFM_TYPE_EXECUTABLE;
    return EFM_TYPE_UNKNOWN;
}

const char* efm_get_type_label(efm_file_type_t type) {
    switch (type) {
        case EFM_TYPE_FOLDER:     return "Folder";
        case EFM_TYPE_TEXT:       return "Text";
        case EFM_TYPE_IMAGE:      return "Image";
        case EFM_TYPE_AUDIO:      return "Audio";
        case EFM_TYPE_VIDEO:      return "Video";
        case EFM_TYPE_CODE:       return "Source Code";
        case EFM_TYPE_ARCHIVE:    return "Archive";
        case EFM_TYPE_EXECUTABLE: return "Executable";
        case EFM_TYPE_CONFIG:     return "Config";
        case EFM_TYPE_LUA:        return "Lua Script";
        default:                  return "File";
    }
}

const char* efm_get_type_label_ja(efm_file_type_t type) {
    switch (type) {
        case EFM_TYPE_FOLDER:     return "フォルダ";
        case EFM_TYPE_TEXT:       return "テキスト";
        case EFM_TYPE_IMAGE:      return "画像";
        case EFM_TYPE_AUDIO:      return "音声";
        case EFM_TYPE_VIDEO:      return "動画";
        case EFM_TYPE_CODE:       return "ソースコード";
        case EFM_TYPE_ARCHIVE:    return "アーカイブ";
        case EFM_TYPE_EXECUTABLE: return "実行ファイル";
        case EFM_TYPE_CONFIG:     return "設定ファイル";
        case EFM_TYPE_LUA:        return "Luaスクリプト";
        default:                  return "ファイル";
    }
}

uint64_t efm_type_color(efm_file_type_t type) {
    switch (type) {
        case EFM_TYPE_FOLDER:     return EFM_C_FOLDER;
        case EFM_TYPE_IMAGE:      return EFM_C_IMAGE;
        case EFM_TYPE_AUDIO:      return EFM_C_AUDIO;
        case EFM_TYPE_VIDEO:      return EFM_C_VIDEO;
        case EFM_TYPE_CODE:       return EFM_C_CODE;
        case EFM_TYPE_ARCHIVE:    return EFM_C_ARCHIVE;
        case EFM_TYPE_LUA:        return EFM_C_LUA;
        case EFM_TYPE_TEXT:
        case EFM_TYPE_CONFIG:     return EFM_C_TEXT_FILE;
        default:                  return EFM_C_UNKNOWN;
    }
}

void efm_format_size(uint64_t size, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    if (size < 1024ULL) {
        /* Exact byte count - no rounding needed or possible. */
        snprintf(buf, buf_size, "%llu B", (unsigned long long)size);
    } else if (size < 1024ULL * 1024ULL) {
        snprintf(buf, buf_size, "%.1f KB", (double)size / 1024.0);
    } else if (size < 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buf, buf_size, "%.1f MB", (double)size / (1024.0 * 1024.0));
    } else {
        snprintf(buf, buf_size, "%.2f GB", (double)size / (1024.0 * 1024.0 * 1024.0));
    }
}

void efm_format_time(uint64_t time, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    if (time == 0) { strncpy(buf, "--", buf_size-1); return; }
    /* 簡易時刻フォーマット: Unix timestamp → YYYY-MM-DD HH:MM */
    uint64_t t = time;
    uint64_t secs = t % 60; t /= 60;
    uint64_t mins = t % 60; t /= 60;
    uint64_t hours = t % 24; t /= 24;
    /* 日付計算 (簡易) */
    uint64_t days = t;
    uint64_t year = 1970;
    while (true) {
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        uint64_t days_in_year = leap ? 366 : 365;
        if (days < days_in_year) break;
        days -= days_in_year;
        year++;
        if (year > 2099) break;
    }
    uint64_t month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    if (leap) month_days[1] = 29;
    uint64_t month = 0;
    while (month < 12 && days >= month_days[month]) {
        days -= month_days[month];
        month++;
    }
    uint64_t day = days + 1;
    
    /* フォーマット */
    char tmp[32];
    /* YYYY */
    tmp[0] = '0' + (year / 1000) % 10;
    tmp[1] = '0' + (year / 100) % 10;
    tmp[2] = '0' + (year / 10) % 10;
    tmp[3] = '0' + year % 10;
    tmp[4] = '-';
    tmp[5] = '0' + (month+1) / 10;
    tmp[6] = '0' + (month+1) % 10;
    tmp[7] = '-';
    tmp[8] = '0' + day / 10;
    tmp[9] = '0' + day % 10;
    tmp[10] = ' ';
    tmp[11] = '0' + hours / 10;
    tmp[12] = '0' + hours % 10;
    tmp[13] = ':';
    tmp[14] = '0' + mins / 10;
    tmp[15] = '0' + mins % 10;
    tmp[16] = '\0';
    (void)secs;
    strncpy(buf, tmp, buf_size-1);
    buf[buf_size-1] = '\0';
}

/* ============================================================
 * 初期化
 * ============================================================ */

/* ============================================================
 * ファイルマネージャーの既定動作 (設定画面 > Files タブから変更される)
 * ============================================================ */
