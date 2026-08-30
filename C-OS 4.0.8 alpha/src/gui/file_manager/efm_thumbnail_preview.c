/**
 * efm_thumbnail_preview.c - Enhanced File Manager (サムネイル・プレビュー読込・ステータスメッセージ)
 * enhanced_file_manager.c から分割生成。詳細は efm_internal.h を参照。
 */

#include "efm_internal.h"
#include "../include/serial.h"
#include "../include/memory.h"
#include "../include/types.h"
#include "vga.h"
#include "../fs/fs.h"
#include <string.h>

void efm_load_thumbnail(efm_state_t* state, int idx) {
    if (!state || idx < 0 || idx >= state->entry_count) return;
    efm_entry_t* e = &state->entries[idx];
    if (e->has_thumbnail || e->type != EFM_TYPE_IMAGE) return;
    
    uint32_t thumb_size = EFM_THUMBNAIL_W * EFM_THUMBNAIL_H * 4;
    e->thumbnail_data = (uint8_t*)kmalloc(thumb_size);
    if (!e->thumbnail_data) return;
    
    bool rendered = false;
    if (image_viewer_load_file && image_viewer_get_buffer &&
        image_viewer_get_buffer_width && image_viewer_get_buffer_height) {
        if (image_viewer_load_file(e->full_path) == 0 && image_viewer_is_loaded()) {
            const uint32_t* src = image_viewer_get_buffer();
            uint64_t sw = image_viewer_get_buffer_width();
            uint64_t sh = image_viewer_get_buffer_height();
            if (src && sw > 0 && sh > 0) {
                for (uint32_t ty = 0; ty < EFM_THUMBNAIL_H; ++ty) {
                    uint64_t sy = ((uint64_t)ty * sh) / EFM_THUMBNAIL_H;
                    if (sy >= sh) sy = sh - 1;
                    for (uint32_t tx = 0; tx < EFM_THUMBNAIL_W; ++tx) {
                        uint64_t sx = ((uint64_t)tx * sw) / EFM_THUMBNAIL_W;
                        if (sx >= sw) sx = sw - 1;
                        uint32_t c = src[sy * sw + sx];
                        uint32_t off = (ty * EFM_THUMBNAIL_W + tx) * 4;
                        e->thumbnail_data[off + 0] = (uint8_t)((c >> 16) & 0xFF);
                        e->thumbnail_data[off + 1] = (uint8_t)((c >> 8) & 0xFF);
                        e->thumbnail_data[off + 2] = (uint8_t)(c & 0xFF);
                        e->thumbnail_data[off + 3] = (uint8_t)((c >> 24) & 0xFF);
                    }
                }
                rendered = true;
            }
        }
    }

    if (!rendered) {
        /* fallback placeholder gradient */
        uint64_t type_color = efm_type_color(e->type);
        uint8_t r = (type_color >> 16) & 0xFF;
        uint8_t g = (type_color >> 8) & 0xFF;
        uint8_t b = type_color & 0xFF;
        for (uint32_t y = 0; y < EFM_THUMBNAIL_H; y++) {
            for (uint32_t x = 0; x < EFM_THUMBNAIL_W; x++) {
                uint32_t off = (y * EFM_THUMBNAIL_W + x) * 4;
                e->thumbnail_data[off+0] = (uint8_t)(r * (EFM_THUMBNAIL_W - x) / EFM_THUMBNAIL_W);
                e->thumbnail_data[off+1] = (uint8_t)(g * (EFM_THUMBNAIL_H - y) / EFM_THUMBNAIL_H);
                e->thumbnail_data[off+2] = (uint8_t)(b);
                e->thumbnail_data[off+3] = 255;
            }
        }
    }
    
    e->thumbnail_w = EFM_THUMBNAIL_W;
    e->thumbnail_h = EFM_THUMBNAIL_H;
    e->has_thumbnail = true;
}

void efm_load_all_thumbnails(efm_state_t* state) {
    if (!state) return;
    for (int i = 0; i < state->entry_count; i++) {
        if (state->entries[i].type == EFM_TYPE_IMAGE) {
            efm_load_thumbnail(state, i);
        }
    }
}

/* ============================================================
 * プレビュー
 * ============================================================ */

void efm_load_preview(efm_state_t* state, int idx) {
    if (!state || idx < 0 || idx >= state->entry_count) return;
    efm_entry_t* e = &state->entries[idx];
    state->preview_entry_idx = idx;
    state->preview_text[0] = '\0';
    state->preview_active = false;
    
    if (e->is_dir) {
        strncpy(state->preview_text, "Folder\n", 4095);
        strncat(state->preview_text, e->name, 4095-strlen(state->preview_text));
        return;
    }
    
    switch (e->type) {
        case EFM_TYPE_TEXT:
        case EFM_TYPE_CODE:
        case EFM_TYPE_LUA:
        case EFM_TYPE_CONFIG: {
            /* テキストファイルプレビュー */
            extern int cos_fs_read_file(const char* path, void* buffer, uint64_t size);
            int got = cos_fs_read_file(e->full_path, state->preview_text, 4095);
            if (got < 0) {
                strncpy(state->preview_text, "(preview unavailable)\n", 4095);
            } else {
                state->preview_text[got] = '\0';
            }
            break;
        }
        case EFM_TYPE_IMAGE: {
            char info[256];
            strncpy(info, "Image: ", 255);
            strncat(info, e->name, 255-strlen(info));
            strncpy(state->preview_text, info, 4095);
            state->preview_text[4095] = '\0';
            bool already_loaded = false;
            if (image_viewer_is_loaded && image_viewer_get_filename &&
                image_viewer_is_loaded()) {
                const char* loaded_name = image_viewer_get_filename();
                already_loaded = loaded_name != NULL &&
                    strcmp(loaded_name, e->full_path) == 0;
            }
            if (already_loaded ||
                (image_viewer_load_file && image_viewer_load_file(e->full_path) == 0 &&
                 image_viewer_is_loaded && image_viewer_is_loaded())) {
                state->preview_active = true;
            }
            break;
        }
        case EFM_TYPE_AUDIO: {
            char info[256];
            strncpy(info, "Audio: ", 255);
            strncat(info, e->name, 255-strlen(info));
            strncpy(state->preview_text, info, 4095);
            break;
        }
        default: {
            char info[256];
            strncpy(info, "File: ", 255);
            strncat(info, e->name, 255-strlen(info));
            strncpy(state->preview_text, info, 4095);
            break;
        }
    }
}

void efm_clear_preview(efm_state_t* state) {
    if (!state) return;
    state->preview_entry_idx = -1;
    state->preview_text[0] = '\0';
    if (state->preview_image) {
        kfree(state->preview_image);
        state->preview_image = NULL;
    }
}

/* ============================================================
 * ステータス
 * ============================================================ */

void efm_set_status(efm_state_t* state, const char* msg, int duration_ms) {
    if (!state || !msg) return;
    strncpy(state->status_msg, msg, 255);
    state->status_msg[255] = '\0';
    state->status_timer = duration_ms;
}

/* ============================================================
 * 描画
 * ============================================================ */

/* ファイルタイプアイコン描画 */
