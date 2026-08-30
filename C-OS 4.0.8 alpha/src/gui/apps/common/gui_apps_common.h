/**
 * gui_apps_common.h - gui_apps.c 分割に伴う共通ユーティリティ宣言
 *
 * 旧 gui_apps.c (11,638行) に static として埋め込まれ、複数のアプリ実装
 * (ファイルマネージャ/電卓/ブラウザ/タスクマネージャ/MP3プレイヤー等) から
 * 横断的に呼ばれていた汎用ヘルパーをここに集約した。分割後の各アプリ実装
 * ファイルはすべてこのヘッダをincludeする。
 */
#ifndef COS_GUI_APPS_COMMON_H
#define COS_GUI_APPS_COMMON_H

#include "gui.h"
#include "rtc.h"
#include "mk_mp3.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* --- 文字列ユーティリティ --- */
int slen(const char* s);
void scopy(char* d, const char* s, int m);
void scat(char* d, const char* s, int m);
bool smatch(const char* s1, const char* s2);
bool sstartswith(const char* str, const char* prefix);
bool has_suffix(const char* str, const char* suffix);
void copy_cstr_local(char* dst, size_t dst_size, const char* src);
void uitostr(uint64_t num, char* buf);
void format_size_bytes(uint64_t bytes, char* out, int max);

/* --- ファイルマネージャ用パス/種別ヘルパー --- */
void fm_path_leaf(const char* path, char* out, size_t out_size);
bool fm_path_join(char* out, size_t out_size, const char* dir, const char* name);
void fm_join_path(char* out, size_t out_size, const char* dir, const char* name);
bool fm_is_music_file(const char* name);
bool fm_is_jpeg_file(const char* name);
bool fm_is_image_file(const char* name);
bool fm_is_html_file(const char* name);
void fm_open_image_viewer(const char* filename);
void fm_open_text_editor(const char* filename);
void make_copy_name(const char* leaf, int n, char* out, size_t out_size);

/* --- 共通描画ヘルパー --- */
void draw_menubar(int x, int y, int w);
void draw_tbtn(int x, int y, int w, int h, const char* label, uint64_t col, bool hov);
void draw_scrollbar_v(int x, int y, int h, int total, int visible, int offset);
void draw_statusbar(int x, int y, int w, const char* status, const char* path);

/* --- その他 --- */
const char* gui_get_prompt_hostname(void);
const char* window_kind_name(int kind);

/* gui_apps_core.c に実装があり、他ファイル(settings/taskmanager/clock/memory等)
 * からも呼ばれる非static関数。分割前は同一ファイル内だったため暗黙に見えていた。 */
uint64_t kmemory_used(void);
uint64_t kmemory_free(void);
int int_to_str(int num, char* buf, int buf_size);
uint64_t frame_counter_get(void);

/* 元gui_apps.c内で複数箇所からextern再宣言されていた関数
 * （時計アプリ等、vga.hの実体とは別に安全策としてローカルにexternされていた）。 */
extern int vga_isin(int angle_deg);
extern int vga_icos(int angle_deg);
extern void vga_draw_line_thick(int x0, int y0, int x1, int y1, int thickness, uint64_t color);
extern rtc_time_t rtc_get_datetime(void);

/* --- UI共通の色/レイアウト定数（元gui_apps.c内で1箇所定義され全アプリから
 * 参照されていたもの。分割後もヘッダ経由で全ファイルから見えるようにする） --- */
#ifndef C_ACCENT
#define C_ACCENT        rgb(0,   120, 212)
#endif
#ifndef C_WIN_BG
#define C_WIN_BG        rgb(243, 243, 243)
#endif
#ifndef C_WIN_BG2
#define C_WIN_BG2       rgb(249, 249, 249)
#endif
#ifndef C_TEXT
#define C_TEXT          rgb(0,   0,   0)
#endif
#ifndef C_TEXT_LIGHT
#define C_TEXT_LIGHT    rgb(255, 255, 255)
#endif
#ifndef C_TEXT_GRAY
#define C_TEXT_GRAY     rgb(100, 100, 100)
#endif
#ifndef C_TEXT_DIM
#define C_TEXT_DIM      rgb(150, 150, 150)
#endif
#ifndef C_BORDER
#define C_BORDER        rgb(200, 200, 200)
#endif
#ifndef C_SELECTED
#define C_SELECTED      rgb(0,   120, 212)
#endif
#ifndef C_SELECTED_BG
#define C_SELECTED_BG   rgb(204, 228, 247)
#endif
#ifndef C_HOVER_BG
#define C_HOVER_BG      rgb(230, 240, 250)
#endif
#ifndef C_INPUT_BG
#define C_INPUT_BG      rgb(255, 255, 255)
#endif
#ifndef C_INPUT_BORDER
#define C_INPUT_BORDER  rgb(180, 180, 180)
#endif
#ifndef C_INPUT_FOCUS
#define C_INPUT_FOCUS   rgb(0,   120, 212)
#endif
#ifndef C_TERM_BG
#define C_TERM_BG       rgb(12,  12,  12)
#endif
#ifndef C_TERM_TEXT
#define C_TERM_TEXT     rgb(204, 204, 204)
#endif
#ifndef C_TERM_PROMPT
#define C_TERM_PROMPT   rgb(0,   200, 0)
#endif
#ifndef C_TERM_CMD
#define C_TERM_CMD      rgb(255, 255, 255)
#endif
#ifndef C_TERM_ERR
#define C_TERM_ERR      rgb(255, 80,  80)
#endif
#ifndef C_TERM_INFO
#define C_TERM_INFO     rgb(80,  200, 255)
#endif
#ifndef C_SUCCESS
#define C_SUCCESS       rgb(16,  124, 16)
#endif
#ifndef C_WARNING
#define C_WARNING       rgb(255, 185, 0)
#endif
#ifndef C_ERROR_COL
#define C_ERROR_COL     rgb(196, 43,  28)
#endif
#ifndef C_TOOLBAR
#define C_TOOLBAR       rgb(230, 230, 230)
#endif
#ifndef C_TOOLBAR_BTN
#define C_TOOLBAR_BTN   rgb(215, 215, 215)
#endif
#ifndef C_TOOLBAR_HOV
#define C_TOOLBAR_HOV   rgb(190, 210, 240)
#endif
#ifndef C_SIDEBAR
#define C_SIDEBAR       rgb(240, 240, 240)
#endif
#ifndef C_SIDEBAR_SEL
#define C_SIDEBAR_SEL   rgb(204, 228, 247)
#endif
#ifndef C_TITLEBAR_H
#define C_TITLEBAR_H    32
#endif
#ifndef C_STATUSBAR_H
#define C_STATUSBAR_H   24
#endif
#ifndef C_SCROLLBAR_W
#define C_SCROLLBAR_W   14
#endif

/* --- 元gui_apps.c内でファイル先頭付近にextern宣言されていたグローバル --- */
extern int active_window;
extern uint64_t gui_frame_counter;

/* --- 複数アプリ(電卓/設定/ブラウザ/ストレージ/MP3プレイヤー等)が横断的に
 * 必要とする追加ヘッダ。分割前は1箇所でincludeされ全アプリから見えていた。 --- */
#include "cosnet_state.h"
#include "../../kernel/drivers/http.h"
#include "calc_engine.h"
#include "../../kernel/drivers/dns.h"
#include "../../kernel/drivers/net.h"
#include "../../kernel/drivers/dhcp.h"
#include "../file_manager/enhanced_file_manager.h"
#include "../../cpp/browser_core.h"
#include "../../kernel/drivers/tls_backend.h"

/* Optional persistent storage hooks (weak-linked) */
extern bool storage_write_file(const char* filename, const void* data, uint64_t size) __attribute__((weak));
extern bool storage_read_file(const char* filename, void* buffer, uint64_t buffer_size, uint64_t* out_size) __attribute__((weak));
extern int unified_storage_get_device_count(void) __attribute__((weak));
extern uint64_t unified_storage_get_total_capacity(void) __attribute__((weak));
extern uint64_t unified_storage_get_free_capacity(void) __attribute__((weak));

/* Optional MP3 backend (weak-linked when available) */
extern int mk_mp3_load_file(const char* filename) __attribute__((weak));
extern int mk_mp3_load_directory(const char* dir_path) __attribute__((weak));
extern int mk_mp3_clear_playlist(void) __attribute__((weak));
extern int mk_mp3_play(void) __attribute__((weak));
extern int mk_mp3_pause(void) __attribute__((weak));
extern int mk_mp3_resume(void) __attribute__((weak));
extern int mk_mp3_stop(void) __attribute__((weak));
extern int mk_mp3_set_volume(uint64_t volume) __attribute__((weak));
extern int mk_mp3_seek(uint64_t position_ms) __attribute__((weak));
extern int mk_mp3_set_repeat(int mode) __attribute__((weak));
extern int mk_mp3_set_shuffle(bool enabled) __attribute__((weak));
extern int mk_mp3_playlist_play_next(void) __attribute__((weak));
extern int mk_mp3_playlist_play_previous(void) __attribute__((weak));
extern mk_mp3_player_t* mk_mp3_get_player_state(void) __attribute__((weak));
extern const char* mk_mp3_get_current_title(void) __attribute__((weak));
extern const char* mk_mp3_get_current_artist(void) __attribute__((weak));
extern const char* mk_mp3_get_current_album(void) __attribute__((weak));
extern uint64_t mk_mp3_get_current_year(void) __attribute__((weak));
extern mk_mp3_playlist_t* mk_mp3_get_playlist(void) __attribute__((weak));
extern uint64_t get_timer_ticks(void);

/* Universal image viewer (PNG/BMP/Image preview). */
extern int image_viewer_load_file(const char* file_path) __attribute__((weak));
extern int image_viewer_draw_scaled(uint64_t x, uint64_t y, uint64_t width, uint64_t height) __attribute__((weak));
extern bool image_viewer_is_loaded(void) __attribute__((weak));

/* Virtual memory stats used by the Settings screen. */
typedef struct {
    uint64_t total_pages;
    uint64_t kernel_pages;
    uint64_t user_pages;
    uint64_t mapped_pages;
    uint64_t page_faults;
    uint64_t tlb_flushes;
} virt_mem_stats_t;
extern virt_mem_stats_t* virt_memory_get_stats(void);

/* Define missing key-code constants (元gui_apps.c内で定義されていたもの) */
#ifndef KEY_ESC
#define KEY_ESC 0x01
#endif
#ifndef KEY_BACKSPACE
#define KEY_BACKSPACE 0x0E
#endif
#ifndef KEY_ENTER
#define KEY_ENTER 0x1C
#endif
#ifndef KEY_DELETE
#define KEY_DELETE 0x53
#endif
#ifndef KEY_HOME
#define KEY_HOME 0x47
#endif
#ifndef KEY_END
#define KEY_END 0x4F
#endif
#ifndef KEY_F2
#define KEY_F2 0x3C
#endif
#ifndef KEY_F5
#define KEY_F5 0x3F
#endif
#ifndef KEY_F6
#define KEY_F6 0x40
#endif
#ifndef KEY_F7
#define KEY_F7 0x41
#endif

#endif /* COS_GUI_APPS_COMMON_H */
