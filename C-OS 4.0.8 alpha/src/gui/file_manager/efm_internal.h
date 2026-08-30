/**
 * efm_internal.h - Enhanced File Manager: 内部共有ヘッダ
 *
 * enhanced_file_manager.c を機能単位で分割した際に、複数の .c ファイルから
 * 参照される外部依存・共通マクロ・内部専用関数の宣言をまとめたもの。
 * 公開 API (アプリ外部から呼ばれるもの) は enhanced_file_manager.h を見ること。
 *
 * ファイル分割マップ:
 *   efm_util.c              - UTF-8/文字列ユーティリティ、ファイル種別判定、サイズ/時刻フォーマット
 *   efm_core.c               - 初期化/後始末、既定値、ナビゲーション履歴、ソート、ディレクトリ再読込
 *   efm_fileops.c            - 作成/削除/リネーム/コピー/移動/貼り付け、選択、クリップボード
 *   efm_search_bookmark.c    - 検索、ソートAPI、ブックマーク
 *   efm_thumbnail_preview.c  - サムネイル読込、プレビュー読込、ステータスメッセージ
 *   efm_render.c             - 描画 (ツールバー/パンくず/一覧/プレビュー/ステータスバー/ダイアログ)
 *   efm_input.c              - マウス/キーボード入力処理、ファイルを開く
 */
#ifndef EFM_INTERNAL_H
#define EFM_INTERNAL_H

#include "enhanced_file_manager.h"

/* ============================================================
 * 外部依存 (VGA描画・GUIシェル・画像ビューア)
 * ============================================================ */
extern void vga_fill_rect(int x, int y, int w, int h, uint64_t color);
extern void vga_draw_rect(int x, int y, int w, int h, uint64_t color);
extern void vga_fill_rounded_rect(int x, int y, int w, int h, int r, uint64_t color);
extern void vga_draw_rounded_rect(int x, int y, int w, int h, int r, uint64_t color);
extern void vga_draw_string(int x, int y, const char* s, uint64_t fg, uint64_t bg);
extern void vga_set_pixel(int x, int y, uint64_t color);
extern bool gui_is_japanese(void);
extern void gui_open_file_in_app(const char* path, int file_type);
extern int image_viewer_load_file(const char* filename) __attribute__((weak));
extern int image_viewer_draw_scaled(uint64_t x, uint64_t y, uint64_t width, uint64_t height) __attribute__((weak));
extern bool image_viewer_is_loaded(void) __attribute__((weak));
extern const char* image_viewer_get_filename(void) __attribute__((weak));
extern const uint32_t* image_viewer_get_buffer(void) __attribute__((weak));
extern uint64_t image_viewer_get_buffer_width(void) __attribute__((weak));
extern uint64_t image_viewer_get_buffer_height(void) __attribute__((weak));
extern int  gui_dark_mode;

/* ============================================================
 * カラー定義 (全ファイル共通)
 * ============================================================ */
#define EFM_C_BG        (gui_dark_mode ? 0x001A1E28 : 0x00F4F6FA)
#define EFM_C_PANEL     (gui_dark_mode ? 0x00202430 : 0x00FFFFFF)
#define EFM_C_SIDEBAR   (gui_dark_mode ? 0x00181C26 : 0x00EEF2F8)
#define EFM_C_TOOLBAR   (gui_dark_mode ? 0x001E2230 : 0x00F0F4FA)
#define EFM_C_BORDER    (gui_dark_mode ? 0x00384050 : 0x00D0D8E8)
#define EFM_C_TEXT      (gui_dark_mode ? 0x00E8ECF4 : 0x00202428)
#define EFM_C_MUTED     (gui_dark_mode ? 0x00808898 : 0x00808898)
#define EFM_C_ACCENT    (gui_dark_mode ? 0x004D9FFF : 0x001A73E8)
#define EFM_C_HOVER     (gui_dark_mode ? 0x00303848 : 0x00E8F0FE)
#define EFM_C_SELECT    (gui_dark_mode ? 0x00284060 : 0x00C8DFFE)
#define EFM_C_FOLDER    0x00F5A623
#define EFM_C_IMAGE     0x0034A853
#define EFM_C_AUDIO     0x00EA4335
#define EFM_C_VIDEO     0x00FBBC04
#define EFM_C_CODE      0x004285F4
#define EFM_C_ARCHIVE   0x009E3AB5
#define EFM_C_LUA       0x000080FF
#define EFM_C_TEXT_FILE 0x00607080
#define EFM_C_UNKNOWN   0x00909090
#define EFM_C_DANGER    0x00D93025

/* ============================================================
 * ファイル間で共有される内部専用関数
 * (元々は static だったが、複数の .c にまたがって使われるため公開)
 * ============================================================ */

/* efm_util.c */
void efm_utf8_truncate(const char* src, char* out, size_t out_size, int max_width);
int  efm_utf8_display_width(const char* s);
uint64_t efm_type_color(efm_file_type_t type);

/* efm_core.c */
void efm_sort_entries(efm_state_t* state);

#endif /* EFM_INTERNAL_H */
