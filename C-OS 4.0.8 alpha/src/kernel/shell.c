/**
 * shell.c - Advanced Shell System for C-OS 4.0.8 alpha
 */
#include <stdbool.h>
#include <stdint.h>

#include <shell.h>
#include <serial.h>
#include <keyboard.h>
#include <vga.h>
#include <memory.h>
#include <timer.h>
#include <rtc.h>
#include <fs.h>
#include <gui.h>
#include <string.h>
#include <io.h>
#include <mk_mp3.h>
#include <ac97.h>

static char* shell_strtok_next = NULL;

static char* shell_strtok(char* str, const char* delim) {
    if (!str) { str = shell_strtok_next; }
    if (!str) { return NULL; }
    char* start = str;
    while (*str) {
        const char* d = delim;
        while (*d) {
            if (*str == *d) {
                *str = 0;
                shell_strtok_next = str + 1;
                return start;
            }
            d++;
        }
        str++;
    }
    shell_strtok_next = NULL;
    return start;
}

static int shell_strcmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) { return (int)*a - (int)*b; }
        a++; b++;
    }
    return (int)*a - (int)*b;
}

static void shell_trim_inplace(char* s) {
    if (!s) return;
    char* start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    char* end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }
    size_t len = (size_t)(end - start);
    if (start != s) {
        memmove(s, start, len);
    }
    s[len] = '\0';
}
extern uint64_t get_timer_ticks(void);
extern uint64_t memory_get_total(void);
extern uint64_t memory_get_free(void);
extern rtc_time_t rtc_get_datetime(void);
extern uint64_t storage_get_free_space(void);
extern uint64_t storage_get_used_space(void);
extern uint64_t storage_get_total_space(void);
extern int storage_sync(void);
extern bool storage_has_password(void);
extern bool storage_verify_password(const char* password);
extern bool storage_set_password(const char* password);
extern bool storage_clear_password(void);
extern const char* fs_read_file_at(const char* path, const char* name);
extern bool fs_write_file_at(const char* path, const char* name, const char* data, uint64_t size);
extern int gui_get_theme_idx(void);
extern void gui_set_theme_idx(int idx);
extern int gui_get_wallpaper_count(void);
extern const char* gui_get_wallpaper_name(int idx);
extern int gui_get_wallpaper_idx(void);
extern void gui_set_wallpaper(int idx);
extern bool gui_get_terminal_autoscroll(void);
extern void gui_set_terminal_autoscroll(bool enabled);
extern int config_manager_init(void);
extern int config_manager_cleanup(void);
extern const char* config_get_string(const char* key);
extern int config_set_string(const char* key, const char* value);
extern int config_save_all(void);
extern void vga_set_font_resolution(int res);
extern int vga_get_font_resolution(void);
extern int mk_mp3_set_volume(uint64_t volume) __attribute__((weak));
extern int mk_mp3_set_repeat(int mode) __attribute__((weak));
extern int mk_mp3_set_shuffle(bool enabled) __attribute__((weak));
extern int mk_mp3_load_directory(const char* dir_path) __attribute__((weak));
extern int mk_mp3_clear_playlist(void) __attribute__((weak));
extern int mk_mp3_load_file(const char* filename) __attribute__((weak));
extern int mk_mp3_play(void) __attribute__((weak));
extern int mk_mp3_pause(void) __attribute__((weak));
extern int mk_mp3_resume(void) __attribute__((weak));
extern int mk_mp3_stop(void) __attribute__((weak));
extern int mk_mp3_seek(uint64_t position_ms) __attribute__((weak));
extern mk_mp3_player_t* mk_mp3_get_player_state(void) __attribute__((weak));
extern int mk_mp3_playlist_play_next(void) __attribute__((weak));
extern int mk_mp3_playlist_play_previous(void) __attribute__((weak));
extern const char* mk_mp3_get_current_title(void) __attribute__((weak));
extern const char* mk_mp3_get_current_artist(void) __attribute__((weak));
extern const char* mk_mp3_get_current_album(void) __attribute__((weak));
extern uint64_t mk_mp3_get_current_year(void) __attribute__((weak));
extern mk_mp3_playlist_t* mk_mp3_get_playlist(void) __attribute__((weak));

#define MAX_CMD_LEN 128
#define MAX_ARGS 10
#define SHELL_HISTORY_MAX 32

static char shell_history[SHELL_HISTORY_MAX][MAX_CMD_LEN];
static int shell_history_count = 0;

static bool shell_running = true;
static char shell_cwd[FS_MAX_PATH] = "/";
static shell_output_callback_t shell_output_callback = NULL;
static shell_output_callback_t shell_capture_forward = NULL;
static const char* shell_pipe_input = NULL;
static char shell_capture_buffer[8192];
static size_t shell_capture_len = 0;

void shell_set_output_callback(shell_output_callback_t callback) {
    shell_output_callback = callback;
}

static const char* shell_translate_text(const char* s);
static void shell_emit(const char* s) {
    if (!s) return;
    const char* out = shell_translate_text(s);
    serial_puts(out);
    if (shell_output_callback) {
        shell_output_callback(out);
    }
}

static const char* shell_translate_text(const char* s) {
    if (!s) return s;
    if (gui_get_language_idx() != 1) return s;

    typedef struct { const char* en; const char* ja; } shell_txt_t;
    static const shell_txt_t map[] = {
        {"Show available commands", "利用可能なコマンドを表示"},
        {"Alias for help", "help の別名"},
        {"Clear the screen", "画面を消去"},
        {"Alias for clear", "clear の別名"},
        {"Print text to console", "文字をコンソールに表示"},
        {"Show command history", "コマンド履歴を表示"},
        {"Show current OS settings", "現在のOS設定を表示"},
        {"View or edit persistent configuration", "永続設定を表示または編集"},
        {"Save persistent settings and storage", "永続設定とストレージを保存"},
        {"Reload persistent settings", "永続設定を再読み込み"},
        {"Set or clear boot password", "起動パスワードを設定または解除"},
        {"List or change GUI theme", "GUIテーマを表示または変更"},
        {"List or change wallpaper", "壁紙を表示または変更"},
        {"Toggle terminal auto-scroll", "ターミナルの自動スクロールを切り替え"},
        {"View or change host name", "ホスト名を表示または変更"},
        {"View or change system volume", "システム音量を表示または変更"},
        {"View or change font resolution", "フォント解像度を表示または変更"},
        {"Control MP3 playback and playlists", "MP3再生とプレイリストを操作"},
        {"Alias for music", "music の別名"},
        {"View or change time zone", "タイムゾーンを表示または変更"},
        {"Show system information", "システム情報を表示"},
        {"Alias for ver", "ver の別名"},
        {"Alias for ls", "ls の別名"},
        {"Alias for exit", "exit の別名"},
        {"Show memory statistics", "メモリ統計を表示"},
        {"Show current time", "現在時刻を表示"},
        {"Show current date", "現在日付を表示"},
        {"Show system uptime", "システム稼働時間を表示"},
        {"Show OS version", "OSバージョンを表示"},
        {"List files in directory", "ディレクトリ内のファイル一覧を表示"},
        {"Display file content", "ファイル内容を表示"},
        {"Create an empty file", "空のファイルを作成"},
        {"Remove a file", "ファイルを削除"},
        {"Create a directory", "ディレクトリを作成"},
        {"Change directory", "ディレクトリを変更"},
        {"Print working directory", "現在の作業ディレクトリを表示"},
        {"Restart the system", "システムを再起動"},
        {"Power off the system", "システムを電源オフ"},
        {"List running processes", "実行中のプロセスを一覧表示"},
        {"Terminate a process", "プロセスを終了"},
        {"Display memory usage", "メモリ使用量を表示"},
        {"Display disk space usage", "ディスク使用量を表示"},
        {"Print system information", "システム情報を表示"},
        {"Print current user", "現在のユーザーを表示"},
        {"Filter lines from text or files", "テキストやファイルから行を抽出"},
        {"Search file names in directories", "ディレクトリ内のファイル名を検索"},
        {"Open a file in the text editor", "ファイルをテキストエディターで開く"},
        {"Launch an app by name", "名前でアプリを起動"},
        {"Package manager status", "パッケージマネージャー状態"},
        {"Module loader status", "モジュールローダー状態"},
        {"Widget API status", "ウィジェットAPI状態"},
        {"Service manager status", "サービスマネージャー状態"},
        {"Logging system status", "ログシステム状態"},
        {"Driver abstraction status", "ドライバー抽象化状態"},
        {"Virtual memory status", "仮想メモリ状態"},
        {"Filesystem API status", "ファイルシステムAPI状態"},
        {"Process API status", "プロセスAPI状態"},
        {"Task scheduler status", "タスクスケジューラー状態"},
        {"Plugin loader status", "プラグインローダー状態"},
        {"Network API status", "ネットワークAPI状態"},
        {"Power API status", "電源API状態"},
        {"Exit the shell", "シェルを終了"},
        {"Available commands (? also works as help):\n", "利用可能なコマンド（? も help の代わりです）:\n"},
        {"[SHELL] Screen cleared", "[SHELL] 画面を消去しました"},
        {"History cleared.", "履歴を消去しました"},
        {"Command History:", "コマンド履歴:"},
        {"Current settings:", "現在の設定:"},
        {"  Hostname: ", "  ホスト名: "},
        {"  Theme: ", "  テーマ: "},
        {"  Font resolution: ", "  フォント解像度: "},
        {"12x16 (forced)", "12x16（強制）"},
        {"  Wallpaper: ", "  壁紙: "},
        {"  Auto-scroll: ", "  自動スクロール: "},
        {"enabled", "有効"},
        {"disabled", "無効"},
        {"  Language: ", "  言語: "},
        {"Japanese", "日本語"},
        {"English", "英語"},
        {"  Boot password: ", "  起動パスワード: "},
        {"set", "設定済み"},
        {"not set", "未設定"},
        {"(unset)", "(未設定)"},
        {"UTC", "UTC"},
        {"12x16 (forced)", "12x16（強制）"},
        {"Persistent configuration:", "永続設定:"},
        {"Usage: config get <key>", "使い方: config get <key>"},
        {"Usage: config set <key> <value>", "使い方: config set <key> <value>"},
        {"config: failed to set value", "config: 値の設定に失敗しました"},
        {"config updated: ", "設定を更新しました: "},
        {" = ", " = "},
        {"Configuration saved.", "設定を保存しました"},
        {"config: save failed", "config: 保存に失敗しました"},
        {"config: reload failed", "config: 再読み込みに失敗しました"},
        {"Configuration reloaded.", "設定を再読み込みしました"},
        {"Exporting settings to serial log:", "設定をシリアルログへ出力します:"},
        {"  (Use save to persist changes)", "  （変更を保存するには save を使ってください）"},
        {"Usage: config [show|get|set|save|load|export|dump]", "使い方: config [show|get|set|save|load|export|dump]"},
        {"Saved persistent settings and storage state.", "永続設定とストレージ状態を保存しました"},
        {"Reloaded persistent settings.", "永続設定を再読み込みしました"},
        {"Password is set.", "パスワードは設定されています"},
        {"No boot password is set.", "起動パスワードは設定されていません"},
        {"Usage: passwd set <newpass> | passwd clear | passwd check <pass>", "使い方: passwd set <newpass> | passwd clear | passwd check <pass>"},
        {"Boot password cleared.", "起動パスワードを解除しました"},
        {"passwd: failed to clear password", "passwd: パスワード解除に失敗しました"},
        {"Usage: passwd set <newpass>", "使い方: passwd set <newpass>"},
        {"Boot password saved permanently.", "起動パスワードを永続保存しました"},
        {"passwd: failed to save password", "passwd: パスワード保存に失敗しました"},
        {"Usage: passwd check <pass>", "使い方: passwd check <pass>"},
        {"Password OK", "パスワード一致"},
        {"Password mismatch", "パスワード不一致"},
        {"Usage: passwd change <old> <new>", "使い方: passwd change <old> <new>"},
        {"No existing password. Use passwd set.", "既存のパスワードがありません。passwd set を使ってください"},
        {"Old password is wrong.", "古いパスワードが違います"},
        {"Password changed and saved.", "パスワードを変更して保存しました"},
        {"Usage: passwd [set|clear|check|change]", "使い方: passwd [set|clear|check|change]"},
        {"Available themes:", "利用可能なテーマ:"},
        {"Current theme: ", "現在のテーマ: "},
        {"Usage: theme [1-4|light|dark|blue|green]", "使い方: theme [1-4|light|dark|blue|green]"},
        {"Theme updated to ", "テーマを変更しました: "},
        {"Available wallpapers:", "利用可能な壁紙:"},
        {"Usage: wallpaper [1-n]", "使い方: wallpaper [1-n]"},
        {"Wallpaper updated to ", "壁紙を変更しました: "},
        {"Hostname: ", "ホスト名: "},
        {"Usage: hostname [new-name]", "使い方: hostname [new-name]"},
        {"hostname: failed to update", "hostname: 更新に失敗しました"},
        {"Hostname updated to ", "ホスト名を変更しました: "},
        {"Volume: ", "音量: "},
        {"Usage: volume [0-100]", "使い方: volume [0-100]"},
        {"volume: value must be 0-100", "volume: 値は0〜100で指定してください"},
        {"volume: failed to update", "volume: 更新に失敗しました"},
        {"Volume set to ", "音量を設定しました: "},
        {"MP3 backend is unavailable.", "MP3バックエンドが利用できません"},
        {"MP3 state is unavailable.", "MP3状態を取得できません"},
        {"State: ", "状態: "},
        {"playing", "再生中"},
        {"paused", "一時停止"},
        {"stopped", "停止中"},
        {"error", "エラー"},
        {"Unknown artist", "不明なアーティスト"},
        {"Unknown album", "不明なアルバム"},
        {"Sample rate: ", "サンプルレート: "},
        {" Hz", " Hz"},
        {"Channels: ", "チャンネル数: "},
        {"Bitrate: ", "ビットレート: "},
        {" kbps", " kbps"},
        {"Position: ", "位置: "},
        {"Duration: ", "再生時間: "},
        {" ms", " ms"},
        {"Repeat: ", "リピート: "},
        {"Shuffle: ", "シャッフル: "},
        {"on", "オン"},
        {"off", "オフ"},
        {"MP3 playlist API unavailable.", "MP3プレイリストAPIが利用できません"},
        {"No playlist state available.", "プレイリスト状態がありません"},
        {"Playlist entries: ", "プレイリスト項目数: "},
        {"Usage: music [status|load|play|pause|resume|stop|next|prev|seek|volume|repeat|shuffle|clear|list]", "使い方: music [status|load|play|pause|resume|stop|next|prev|seek|volume|repeat|shuffle|clear|list]"},
        {"Usage: music load <file-or-dir>", "使い方: music load <file-or-dir>"},
        {"MP3 loaded.", "MP3を読み込みました"},
        {"music: load failed", "music: 読み込みに失敗しました"},
        {"Playing.", "再生開始"},
        {"music: play failed", "music: 再生に失敗しました"},
        {"Paused.", "一時停止しました"},
        {"music: pause failed", "music: 一時停止に失敗しました"},
        {"Resumed.", "再開しました"},
        {"music: resume failed", "music: 再開に失敗しました"},
        {"Stopped.", "停止しました"},
        {"music: stop failed", "music: 停止に失敗しました"},
        {"Next track.", "次の曲へ"},
        {"music: next failed", "music: 次曲へ進めませんでした"},
        {"Previous track.", "前の曲へ"},
        {"music: prev failed", "music: 前曲へ戻れませんでした"},
        {"Usage: music seek <ms>", "使い方: music seek <ms>"},
        {"Seeked.", "シークしました"},
        {"music: seek failed", "music: シークに失敗しました"},
        {"Usage: music volume <0-100>", "使い方: music volume <0-100>"},
        {"Volume updated.", "音量を更新しました"},
        {"music: volume failed", "music: 音量更新に失敗しました"},
        {"Usage: music repeat [off|one|all]", "使い方: music repeat [off|one|all]"},
        {"Repeat updated.", "リピート設定を更新しました"},
        {"music: repeat failed", "music: リピート設定に失敗しました"},
        {"Shuffle updated.", "シャッフル設定を更新しました"},
        {"music: shuffle failed", "music: シャッフル設定に失敗しました"},
        {"Playlist cleared.", "プレイリストを消去しました"},
        {"music: clear failed", "music: 消去に失敗しました"},
        {"Font resolution is fixed at 12x16.", "フォント解像度は12x16に固定されています"},
        {"The system uses 12x16 ASCII everywhere.", "システム全体で12x16 ASCII表示を強制しています"},
        {"Framebuffer: ", "フレームバッファ: "},
        {"Timezone: ", "タイムゾーン: "},
        {"Usage: timezone [IANA/abbrev]", "使い方: timezone [IANA/略称]"},
        {"timezone: failed to update", "timezone: 更新に失敗しました"},
        {"Timezone updated to ", "タイムゾーンを変更しました: "},
        {"Terminal auto-scroll is ", "ターミナルの自動スクロールは "},
        {"Usage: autoscroll [on|off|toggle]", "使い方: autoscroll [on|off|toggle]"},
        {"Terminal auto-scroll is now ", "ターミナルの自動スクロールは現在 "},
        {"System Information:", "システム情報:"},
        {"  Version: ", "  バージョン: "},
        {"  Uptime: ", "  稼働時間: "},
        {" seconds", " 秒"},
        {"  Memory: ", "  メモリ: "},
        {" KB total, ", " KB 合計, "},
        {" KB used, ", " KB 使用済み, "},
        {" KB free", " KB 空き"},
        {"Memory Stats:\n", "メモリ統計:\n"},
        {"  Total: ", "  合計: "},
        {"  Free:  ", "  空き:  "},
        {"  Used:  ", "  使用済み: "},
        {"Current Time: ", "現在時刻: "},
        {"Current Date: 20", "現在日付: 20"},
        {"Uptime: ", "稼働時間: "},
        {"C-OS 4.0.8 alpha (64-bit Edition)\n", "C-OS 4.0.8 alpha（64ビット版）\n"},
        {"ls: cannot access '", "ls: アクセスできません '"},
        {"': No such directory", "': そのようなディレクトリはありません"},
        {"Directory listing: ", "ディレクトリ一覧: "},
        {" [FILE] ", " [FILE] "},
        {" bytes)", " bytes)"},
        {"Usage: cat filename", "使い方: cat filename"},
        {"cat: ", "cat: "},
        {"': No such file", "': そのようなファイルはありません"},
        {"Usage: grep pattern [file]", "使い方: grep pattern [file]"},
        {"grep: no input text", "grep: 入力テキストがありません"},
        {"Usage: find pattern [path]", "使い方: find pattern [path]"},
        {"find: no such directory: ", "find: そのようなディレクトリはありません: "},
        {"nano: opened a blank editor window", "nano: 空のエディターを開きました"},
        {"nano: opened ", "nano: 開きました "},
        {"Usage: open app-name", "使い方: open app-name"},
        {"open: unknown app: ", "open: 不明なアプリです: "},
        {"Launched app: ", "起動しました: "},
        {"Package manager API is available in the next integration step.", "パッケージマネージャーAPIは次の統合段階で利用可能になります"},
        {"Module loader API hook ready.", "モジュールローダーAPIフックの準備ができています"},
        {"Widget API hook ready.", "ウィジェットAPIフックの準備ができています"},
        {"Service manager hook ready.", "サービスマネージャーフックの準備ができています"},
        {"Logging API hook ready.", "ログAPIフックの準備ができています"},
        {"Driver abstraction hook ready.", "ドライバー抽象化フックの準備ができています"},
        {"Virtual memory manager hook ready.", "仮想メモリマネージャーフックの準備ができています"},
        {"Filesystem API hook ready.", "ファイルシステムAPIフックの準備ができています"},
        {"Process/task API hook ready.", "プロセス/タスクAPIフックの準備ができています"},
        {"Task scheduler API hook ready.", "タスクスケジューラーAPIフックの準備ができています"},
        {"Plugin/module API hook ready.", "プラグイン/モジュールAPIフックの準備ができています"},
        {"Network API hook ready.", "ネットワークAPIフックの準備ができています"},
        {"Power API hook ready.", "電源APIフックの準備ができています"},
        {"Usage: touch filename", "使い方: touch filename"},
        {"Created file: ", "ファイルを作成しました: "},
        {"touch: cannot create '", "touch: 作成できませんでした '"},
        {"rm is disabled in this safe build. Use the file manager to delete files.\n", "rm はこの安全版では無効です。ファイル削除はファイルマネージャーを使ってください。\n"},
        {"Usage: mkdir dirname", "使い方: mkdir dirname"},
        {"Created directory: ", "ディレクトリを作成しました: "},
        {"mkdir: cannot create directory '", "mkdir: ディレクトリを作成できませんでした '"},
        {"cd: no such directory: ", "cd: そのようなディレクトリはありません: "},
        {"Changed directory to: ", "移動しました: "},
        {"Reboot is disabled in this safe build. Use the GUI power controls.\n", "再起動はこの安全版では無効です。GUIの電源操作を使ってください。\n"},
        {"Shutdown is disabled in this safe build. Use the GUI power controls.\n", "電源オフはこの安全版では無効です。GUIの電源操作を使ってください。\n"},
        {"PID  NAME        STATE\n", "PID  名前        状態\n"},
        {"0    idle        RUNNING\n", "0    idle        実行中\n"},
        {"1    init        SLEEPING\n", "1    init        スリープ中\n"},
        {"2    shell       RUNNING\n", "2    shell       実行中\n"},
        {"kill is disabled in this safe build.\n", "kill はこの安全版では無効です。\n"},
        {"Filesystem     Size  Used  Avail  Use%", "ファイルシステム     サイズ  使用済み  空き  使用率%"},
        {"Exiting shell...\n", "シェルを終了しています...\n"},
        {"[SHELL] C-OS 4.0.8 alpha Shell Initialized\n", "[SHELL] C-OS 4.0.8 alpha シェルを初期化しました\n"},
        {"Type help for a list of commands.\n", "help でコマンド一覧を表示できます。\n"},
        {"Unknown command: ", "不明なコマンド: "},
        {"Redirected output to ", "出力をリダイレクトしました: "},
        {"redirect: failed to write ", "redirect: 書き込みに失敗しました: "},

        {"Light", "ライト"},
        {"Dark", "ダーク"},
        {"Blue", "ブルー"},
        {"Green", "グリーン"},
        {"Title: ", "タイトル: "},
        {"Artist: ", "アーティスト: "},
        {"Album: ", "アルバム: "},
        {"status", "状態"},
        {"play", "再生"},
        {"pause", "一時停止"},
        {"resume", "再開"},
        {"stop", "停止"},
        {"next", "次へ"},
        {"prev", "前へ"},
        {"seek", "シーク"},
        {"repeat", "リピート"},
        {"shuffle", "シャッフル"},
        {"yes", "はい"},
        {"no", "いいえ"},
        {"Terminal", "ターミナル"},
        {"Settings", "設定"},
        {"NetSurf", "NetSurf"},
        {"Calculator", "電卓"},
        {"File Manager", "ファイルマネージャー"},
        {"Text Editor", "テキストエディター"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (strcmp(s, map[i].en) == 0) return map[i].ja;
    }
    return s;
}

static void shell_print(const char* s) {
    shell_emit(s);
}

static void shell_print_line(const char* s) {
    shell_emit(s);
    shell_emit("\n");
}

static void shell_print_uint64(uint64_t value) {
    char buf[32];
    int i = 0;
    if (value == 0) {
        buf[i++] = '0';
    } else {
        char tmp[32];
        int j = 0;
        while (value > 0 && j < 31) {
            tmp[j++] = (char)('0' + (value % 10));
            value /= 10;
        }
        while (j-- > 0) {
            buf[i++] = tmp[j];
        }
    }
    buf[i] = '\0';
    shell_print(buf);
}

static void shell_history_add(const char* line) {
    if (!line || !line[0]) return;
    if (shell_history_count < SHELL_HISTORY_MAX) {
        strncpy(shell_history[shell_history_count], line, MAX_CMD_LEN - 1);
        shell_history[shell_history_count][MAX_CMD_LEN - 1] = '\0';
        shell_history_count++;
        return;
    }
    for (int i = 1; i < SHELL_HISTORY_MAX; ++i) {
        strncpy(shell_history[i - 1], shell_history[i], MAX_CMD_LEN - 1);
        shell_history[i - 1][MAX_CMD_LEN - 1] = '\0';
    }
    strncpy(shell_history[SHELL_HISTORY_MAX - 1], line, MAX_CMD_LEN - 1);
    shell_history[SHELL_HISTORY_MAX - 1][MAX_CMD_LEN - 1] = '\0';
}

static const char* shell_theme_name(int idx) {
    switch (idx) {
        case 1: return "Light";
        case 2: return "Dark";
        case 3: return "Blue";
        case 4: return "Green";
        default: return "Unknown";
    }
}


static char shell_hostname_cache[64] = "cos";

static const char* shell_get_hostname(void) {
    return shell_hostname_cache[0] ? shell_hostname_cache : "cos";
}

static void shell_set_hostname_cache(const char* hostname) {
    size_t i = 0;
    if (!hostname || !hostname[0]) {
        strncpy(shell_hostname_cache, "cos", sizeof(shell_hostname_cache) - 1);
        shell_hostname_cache[sizeof(shell_hostname_cache) - 1] = '\0';
        return;
    }
    while (i + 1 < sizeof(shell_hostname_cache) && hostname[i]) {
        shell_hostname_cache[i] = hostname[i];
        ++i;
    }
    shell_hostname_cache[i] = '\0';
}

static void shell_print_prompt(void) {
    shell_print("user@");
    shell_print(shell_get_hostname());
    shell_print(":");
    shell_print(shell_cwd);
    shell_print("# ");
}

static uint64_t shell_parse_u64(const char* s) {
    uint64_t v = 0;
    if (!s || !s[0]) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') break;
        v = v * 10ULL + (uint64_t)(*s - '0');
        ++s;
    }
    return v;
}

static void shell_capture_output(const char* text) {
    if (!text) return;
    while (*text && shell_capture_len + 1 < sizeof(shell_capture_buffer)) {
        shell_capture_buffer[shell_capture_len++] = *text++;
    }
    shell_capture_buffer[shell_capture_len] = '\0';
}

const char* shell_get_pipe_input(void) {
    return shell_pipe_input;
}

const char* shell_get_cwd(void) {
    return shell_cwd;
}

static bool shell_split_path(const char* full, char* parent, size_t parent_size, char* leaf, size_t leaf_size);
static const char* shell_read_all_from_path(const char* path);
static void shell_find_in_text(const char* pattern, const char* text);
static void shell_open_text_editor(const char* fullpath);

static void shell_resolve_path(const char* name, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!name || !name[0]) return;
    if (name[0] == '/') {
        strncpy(out, name, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    if (shell_cwd[0] == '/' && shell_cwd[1] == '\0') {
        strncpy(out, "/", out_size - 1);
        out[out_size - 1] = '\0';
        strncat(out, name, out_size - strlen(out) - 1);
    } else {
        strncpy(out, shell_cwd, out_size - 1);
        out[out_size - 1] = '\0';
        strncat(out, "/", out_size - strlen(out) - 1);
        strncat(out, name, out_size - strlen(out) - 1);
    }
}


typedef struct {
    const char* name;
    const char* description;
    void (*handler)(int argc, char** argv);
} shell_command_t;

void cmd_help(int argc, char** argv);
void cmd_clear(int argc, char** argv);
void cmd_cls(int argc, char** argv);
void cmd_echo(int argc, char** argv);
void cmd_mem(int argc, char** argv);
void cmd_history(int argc, char** argv);
void cmd_settings(int argc, char** argv);
void cmd_config(int argc, char** argv);
void cmd_save(int argc, char** argv);
void cmd_load(int argc, char** argv);
void cmd_passwd(int argc, char** argv);
void cmd_theme(int argc, char** argv);
void cmd_wallpaper(int argc, char** argv);
void cmd_autoscroll(int argc, char** argv);
void cmd_hostname(int argc, char** argv);
void cmd_volume(int argc, char** argv);
void cmd_beep(int argc, char** argv);
void cmd_resolution(int argc, char** argv);
void cmd_music(int argc, char** argv);
void cmd_timezone(int argc, char** argv);
void cmd_sysinfo(int argc, char** argv);
void cmd_time(int argc, char** argv);
void cmd_date(int argc, char** argv);
void cmd_uptime(int argc, char** argv);
void cmd_ver(int argc, char** argv);
void cmd_ls(int argc, char** argv);
void cmd_cat(int argc, char** argv);
void cmd_grep(int argc, char** argv);
void cmd_find(int argc, char** argv);
void cmd_nano(int argc, char** argv);
void cmd_open(int argc, char** argv);
void cmd_touch(int argc, char** argv);
void cmd_rm(int argc, char** argv);
void cmd_mkdir(int argc, char** argv);
void cmd_cd(int argc, char** argv);
void cmd_pwd(int argc, char** argv);
void cmd_reboot(int argc, char** argv);
void cmd_shutdown(int argc, char** argv);
void cmd_ps(int argc, char** argv);
void cmd_kill(int argc, char** argv);
void cmd_free(int argc, char** argv);
void cmd_df(int argc, char** argv);
void cmd_uname(int argc, char** argv);
void cmd_whoami(int argc, char** argv);
void cmd_pkg(int argc, char** argv);
void cmd_module(int argc, char** argv);
void cmd_widget(int argc, char** argv);
void cmd_service(int argc, char** argv);
void cmd_log(int argc, char** argv);
void cmd_driver(int argc, char** argv);
void cmd_vmm(int argc, char** argv);
void cmd_fs(int argc, char** argv);
void cmd_proc(int argc, char** argv);
void cmd_task(int argc, char** argv);
void cmd_plugin(int argc, char** argv);
void cmd_net(int argc, char** argv);
void cmd_power(int argc, char** argv);
void cmd_exit(int argc, char** argv);

static shell_command_t commands[] = {
    {"help", "Show available commands", cmd_help},
    {"?", "Alias for help", cmd_help},
    {"clear", "Clear the screen", cmd_clear},
    {"cls", "Alias for clear", cmd_cls},
    {"echo", "Print text to console", cmd_echo},
    {"history", "Show command history", cmd_history},
    {"settings", "Show current OS settings", cmd_settings},
    {"config", "View or edit persistent configuration", cmd_config},
    {"save", "Save persistent settings and storage", cmd_save},
    {"load", "Reload persistent settings", cmd_load},
    {"passwd", "Set or clear boot password", cmd_passwd},
    {"theme", "List or change GUI theme", cmd_theme},
    {"wallpaper", "List or change wallpaper", cmd_wallpaper},
    {"autoscroll", "Toggle terminal auto-scroll", cmd_autoscroll},
    {"hostname", "View or change host name", cmd_hostname},
    {"volume", "View or change system volume", cmd_volume},
    {"beep", "Play a test tone through AC97 (beep [hz] [ms])", cmd_beep},
    {"resolution", "View or change font resolution", cmd_resolution},
    {"music", "Control MP3 playback and playlists", cmd_music},
    {"mp3", "Alias for music", cmd_music},
    {"timezone", "View or change time zone", cmd_timezone},
    {"sysinfo", "Show system information", cmd_sysinfo},
    {"version", "Alias for ver", cmd_ver},
    {"dir", "Alias for ls", cmd_ls},
    {"quit", "Alias for exit", cmd_exit},
    {"mem", "Show memory statistics", cmd_mem},
    {"time", "Show current time", cmd_time},
    {"date", "Show current date", cmd_date},
    {"uptime", "Show system uptime", cmd_uptime},
    {"ver", "Show OS version", cmd_ver},
    {"ls", "List files in directory", cmd_ls},
    {"cat", "Display file content", cmd_cat},
    {"touch", "Create an empty file", cmd_touch},
    {"rm", "Remove a file", cmd_rm},
    {"mkdir", "Create a directory", cmd_mkdir},
    {"cd", "Change directory", cmd_cd},
    {"pwd", "Print working directory", cmd_pwd},
    {"reboot", "Restart the system", cmd_reboot},
    {"shutdown", "Power off the system", cmd_shutdown},
    {"ps", "List running processes", cmd_ps},
    {"kill", "Terminate a process", cmd_kill},
    {"free", "Display memory usage", cmd_free},
    {"df", "Display disk space usage", cmd_df},
    {"uname", "Print system information", cmd_uname},
    {"whoami", "Print current user", cmd_whoami},
    {"grep", "Filter lines from text or files", cmd_grep},
    {"find", "Search file names in directories", cmd_find},
    {"nano", "Open a file in the text editor", cmd_nano},
    {"open", "Launch an app by name", cmd_open},
    {"pkg", "Package manager status", cmd_pkg},
    {"module", "Module loader status", cmd_module},
    {"widget", "Widget API status", cmd_widget},
    {"service", "Service manager status", cmd_service},
    {"log", "Logging system status", cmd_log},
    {"driver", "Driver abstraction status", cmd_driver},
    {"vmm", "Virtual memory status", cmd_vmm},
    {"fs", "Filesystem API status", cmd_fs},
    {"proc", "Process API status", cmd_proc},
    {"task", "Task scheduler status", cmd_task},
    {"plugin", "Plugin loader status", cmd_plugin},
    {"net", "Network API status", cmd_net},
    {"power", "Power API status", cmd_power},
    {"exit", "Exit the shell", cmd_exit},
};

#define NUM_COMMANDS (int)(sizeof(commands) / sizeof(shell_command_t))

int shell_complete_command(const char* prefix, char* out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!prefix) prefix = "";
    size_t prefix_len = strlen(prefix);
    int matches = 0;
    const char* candidate = NULL;
    for (int i = 0; i < NUM_COMMANDS; ++i) {
        const char* name = commands[i].name;
        if (!name) continue;
        if (strncmp(name, prefix, prefix_len) == 0) {
            if (!candidate) candidate = name;
            matches++;
        }
    }
    if (matches == 0) return 0;
    if (matches == 1) {
        strncpy(out, candidate, out_size - 1);
        out[out_size - 1] = '\0';
        return 1;
    }
    size_t lcp = prefix_len;
    bool grow = true;
    while (grow) {
        char ch = 0;
        for (int i = 0; i < NUM_COMMANDS; ++i) {
            const char* name = commands[i].name;
            if (!name || strncmp(name, prefix, prefix_len) != 0) continue;
            if (strlen(name) <= lcp) { grow = false; break; }
            if (ch == 0) ch = name[lcp];
            else if (name[lcp] != ch) { grow = false; break; }
        }
        if (grow) lcp++;
    }
    if (lcp > prefix_len) {
        size_t n = lcp < out_size - 1 ? lcp : out_size - 1;
        memcpy(out, prefix, n);
        out[n] = '\0';
    } else {
        strncpy(out, candidate, out_size - 1);
        out[out_size - 1] = '\0';
    }
    return matches;
}

void cmd_help(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_print("Available commands (? also works as help):\n");
    for (int i = 0; i < NUM_COMMANDS; i++) {
        shell_print("  ");
        shell_print(commands[i].name);
        shell_print(" - ");
        shell_print(commands[i].description);
        shell_print("\n");
    }
}

void cmd_clear(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_clear(0);
    shell_print_line("[SHELL] Screen cleared");
}

void cmd_cls(int argc, char** argv) {
    cmd_clear(argc, argv);
}

void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        shell_print(argv[i]);
        if (i < argc - 1) { shell_print(" "); }
    }
    shell_print("\n");
}

void cmd_history(int argc, char** argv) {
    if (argc > 1 && (strcmp(argv[1], "-c") == 0 || strcmp(argv[1], "clear") == 0)) {
        shell_history_count = 0;
        shell_print_line("History cleared.");
        return;
    }
    shell_print_line("Command History:");
    for (int i = 0; i < shell_history_count; i++) {
        shell_print("  ");
        shell_print_uint64((uint64_t)(i + 1));
        shell_print(": ");
        shell_print_line(shell_history[i]);
    }
}

static int shell_parse_index(const char* s);

static bool shell_parse_bool(const char* s, bool* out) {
    if (!s || !out) return false;
    if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0 || strcmp(s, "on") == 0 || strcmp(s, "yes") == 0) { *out = true; return true; }
    if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0 || strcmp(s, "off") == 0 || strcmp(s, "no") == 0) { *out = false; return true; }
    return false;
}

static void shell_u64_to_text(uint64_t value, char* out, size_t out_size) {
    char tmp[32];
    size_t pos = 0;
    size_t i = 0;
    if (!out || out_size == 0) return;
    if (value == 0) {
        if (out_size > 1) { out[0] = '0'; out[1] = '\0'; }
        else out[0] = '\0';
        return;
    }
    while (value > 0 && pos < sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (pos > 0 && i + 1 < out_size) {
        out[i++] = tmp[--pos];
    }
    out[i] = '\0';
}

void shell_apply_config_snapshot(void) {
    const char* theme = config_get_string("gui.theme_idx");
    const char* wallpaper = config_get_string("gui.wallpaper_idx");
    const char* dark = config_get_string("gui.dark_mode");
    const char* auto_scroll = config_get_string("gui.terminal_autoscroll");
    const char* hostname = config_get_string("system.hostname");
    const char* volume = config_get_string("system.volume");
    const char* display_res = config_get_string("display.resolution");

    if (theme && theme[0]) {
        int idx = shell_parse_index(theme);
        if (idx >= 1 && idx <= 4) gui_set_theme_idx(idx);
    }
    if (wallpaper && wallpaper[0]) {
        int idx = shell_parse_index(wallpaper);
        if (idx >= 0 && idx < gui_get_wallpaper_count()) gui_set_wallpaper(idx);
    }
    if (dark && dark[0]) {
        bool enabled = false;
        if (shell_parse_bool(dark, &enabled)) {
            if (enabled && gui_get_theme_idx() != 2) gui_set_theme_idx(2);
            else if (!enabled && gui_get_theme_idx() == 2) gui_set_theme_idx(1);
        }
    }
    if (auto_scroll && auto_scroll[0]) {
        bool enabled = false;
        if (shell_parse_bool(auto_scroll, &enabled)) gui_set_terminal_autoscroll(enabled);
    }
    (void)display_res;
    vga_set_font_resolution(1);
    config_set_string("display.font_resolution", "12x16");
    config_set_string("display.resolution", "1024x768");
    if (volume && volume[0] && mk_mp3_set_volume) {
        uint64_t vol = shell_parse_u64(volume);
        if (vol > 100ULL) vol = 100ULL;
        mk_mp3_set_volume(vol);
    }
    if (hostname && hostname[0]) shell_set_hostname_cache(hostname);
    else shell_set_hostname_cache("cos");
}

void cmd_settings(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_print_line("Current settings:");
    shell_print("  Hostname: "); shell_print_line(shell_get_hostname());
    shell_print("  Theme: "); shell_print_line(shell_theme_name(gui_get_theme_idx()));
    shell_print("  Font resolution: "); shell_print_line("12x16 (forced)");
    shell_print("  Wallpaper: "); shell_print_line(gui_get_wallpaper_name(gui_get_wallpaper_idx()));
    shell_print("  Auto-scroll: "); shell_print_line(gui_get_terminal_autoscroll() ? "enabled" : "disabled");
    {
        const char* lang = config_get_string("gui.language");
        shell_print("  Language: "); shell_print_line((lang && strcmp(lang, "1") == 0) ? "Japanese" : "English");
    }
    shell_print("  Boot password: "); shell_print_line(storage_has_password() ? "set" : "not set");
    const char* hostname = config_get_string("system.hostname");
    const char* version = config_get_string("system.version");
    const char* timezone = config_get_string("system.timezone");
    const char* volume = config_get_string("system.volume");
    shell_print("  Hostname: "); shell_print_line(hostname ? hostname : "(unset)");
    shell_print("  Version: "); shell_print_line(version ? version : "(unset)");
    shell_print("  Timezone: "); shell_print_line(timezone ? timezone : "UTC");
    shell_print("  Volume: "); shell_print_line(volume ? volume : "75");
    shell_print("  Font resolution: "); shell_print_line("12x16 (forced)" );
}

void cmd_config(int argc, char** argv) {
    if (argc == 1 || strcmp(argv[1], "show") == 0 || strcmp(argv[1], "list") == 0) {
        shell_print_line("Persistent configuration:");
        const char* keys[] = {"system.name", "system.version", "system.architecture", "system.hostname", "system.timezone", "system.volume", "display.font_resolution", "display.resolution", "gui.theme_idx", "gui.wallpaper_idx", "gui.dark_mode", "gui.font_scale", "gui.window_animations", "gui.notifications_enabled", "gui.terminal_autoscroll", "gui.autostart_terminal", "gui.autostart_file_manager", "gui.autostart_browser", "gui.language", "keyboard.layout", "debug.enabled"};
        for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) {
            const char* v = config_get_string(keys[i]);
            shell_print("  "); shell_print(keys[i]); shell_print(": "); shell_print_line(v ? v : "(unset)");
        }
        return;
    }
    if (strcmp(argv[1], "get") == 0) {
        if (argc < 3) { shell_print_line("Usage: config get <key>"); return; }
        const char* v = config_get_string(argv[2]);
        shell_print_line(v ? v : "(unset)");
        return;
    }
    if (strcmp(argv[1], "set") == 0) {
        if (argc < 4) { shell_print_line("Usage: config set <key> <value>"); return; }
        char value[128];
        value[0] = '\0';
        for (int i = 3; i < argc; ++i) {
            if (i > 3) strncat(value, " ", sizeof(value) - strlen(value) - 1);
            strncat(value, argv[i], sizeof(value) - strlen(value) - 1);
        }
        if (config_set_string(argv[2], value) != 0) {
            shell_print_line("config: failed to set value");
            return;
        }
        shell_apply_config_snapshot();
        config_save_all();
        shell_print("config updated: "); shell_print(argv[2]); shell_print(" = "); shell_print_line(value);
        return;
    }
    if (strcmp(argv[1], "save") == 0) {
        shell_apply_config_snapshot();
        if (config_save_all() == 0) shell_print_line("Configuration saved.");
        else shell_print_line("config: save failed");
        return;
    }
    if (strcmp(argv[1], "load") == 0) {
        if (config_manager_init() != 0) {
            shell_print_line("config: reload failed");
            return;
        }
        shell_apply_config_snapshot();
        shell_print_line("Configuration reloaded.");
        return;
    }
    if (strcmp(argv[1], "export") == 0 || strcmp(argv[1], "dump") == 0) {
        shell_print_line("Exporting settings to serial log:");
        shell_print_line("  (Use save to persist changes)");
        return;
    }
    shell_print_line("Usage: config [show|get|set|save|load|export|dump]");
}

void cmd_save(int argc, char** argv) {
    (void)argc; (void)argv;
    config_save_all();
    storage_sync();
    shell_print_line("Saved persistent settings and storage state.");
}

void cmd_load(int argc, char** argv) {
    (void)argc; (void)argv;
    config_manager_init();
    shell_apply_config_snapshot();
    shell_print_line("Reloaded persistent settings.");
}

void cmd_passwd(int argc, char** argv) {
    if (argc == 1) {
        shell_print_line(storage_has_password() ? "Password is set." : "No boot password is set.");
        shell_print_line("Usage: passwd set <newpass> | passwd clear | passwd check <pass>");
        return;
    }
    if (strcmp(argv[1], "clear") == 0) {
        if (storage_clear_password()) shell_print_line("Boot password cleared.");
        else shell_print_line("passwd: failed to clear password");
        return;
    }
    if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) { shell_print_line("Usage: passwd set <newpass>"); return; }
        if (storage_set_password(argv[2])) shell_print_line("Boot password saved permanently.");
        else shell_print_line("passwd: failed to save password");
        return;
    }
    if (strcmp(argv[1], "check") == 0) {
        if (argc < 3) { shell_print_line("Usage: passwd check <pass>"); return; }
        shell_print_line(storage_verify_password(argv[2]) ? "Password OK" : "Password mismatch");
        return;
    }
    if (strcmp(argv[1], "change") == 0) {
        if (argc < 4) { shell_print_line("Usage: passwd change <old> <new>"); return; }
        if (!storage_has_password()) { shell_print_line("No existing password. Use passwd set."); return; }
        if (!storage_verify_password(argv[2])) { shell_print_line("Old password is wrong."); return; }
        if (storage_set_password(argv[3])) shell_print_line("Password changed and saved.");
        else shell_print_line("passwd: failed to change password");
        return;
    }
    shell_print_line("Usage: passwd [set|clear|check|change]");
}

static int shell_parse_index(const char* s) {
    if (!s || !s[0]) return -1;
    int idx = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        idx = idx * 10 + (*p - '0');
    }
    return idx;
}

void cmd_theme(int argc, char** argv) {
    if (argc == 1) {
        shell_print_line("Available themes:");
        for (int i = 1; i <= 4; i++) {
            shell_print("  ");
            shell_print_uint64((uint64_t)i);
            shell_print(": ");
            shell_print_line(shell_theme_name(i));
        }
        shell_print("Current theme: ");
        shell_print_line(shell_theme_name(gui_get_theme_idx()));
        return;
    }
    int idx = -1;
    if (strcmp(argv[1], "light") == 0) idx = 1;
    else if (strcmp(argv[1], "dark") == 0) idx = 2;
    else if (strcmp(argv[1], "blue") == 0) idx = 3;
    else if (strcmp(argv[1], "green") == 0) idx = 4;
    else idx = shell_parse_index(argv[1]);
    if (idx < 1 || idx > 4) {
        shell_print_line("Usage: theme [1-4|light|dark|blue|green]");
        return;
    }
    gui_set_theme_idx(idx);
    if (idx == 2) {
        config_set_string("gui.dark_mode", "1");
    } else {
        config_set_string("gui.dark_mode", "0");
    }
    {
        char buf[8];
        shell_u64_to_text((uint64_t)idx, buf, sizeof(buf));
        config_set_string("gui.theme_idx", buf);
    }
    config_save_all();
    shell_print("Theme updated to ");
    shell_print_line(shell_theme_name(idx));
}

void cmd_wallpaper(int argc, char** argv) {
    int count = gui_get_wallpaper_count();
    if (argc == 1) {
        shell_print_line("Available wallpapers:");
        for (int i = 0; i < count; i++) {
            shell_print("  ");
            shell_print_uint64((uint64_t)(i + 1));
            shell_print(": ");
            shell_print_line(gui_get_wallpaper_name(i));
        }
        return;
    }
    int idx = shell_parse_index(argv[1]);
    if (idx < 1 || idx > count) {
        shell_print_line("Usage: wallpaper [1-n]");
        return;
    }
    gui_set_wallpaper(idx - 1);
    {
        char buf[8];
        shell_u64_to_text((uint64_t)(idx - 1), buf, sizeof(buf));
        config_set_string("gui.wallpaper_idx", buf);
        config_save_all();
    }
    shell_print("Wallpaper updated to ");
    shell_print_line(gui_get_wallpaper_name(idx - 1));
}

void cmd_hostname(int argc, char** argv) {
    const char* current = config_get_string("system.hostname");
    if (argc == 1) {
        shell_print("Hostname: ");
        shell_print_line((current && current[0]) ? current : "cos");
        shell_print_line("Usage: hostname [new-name]");
        return;
    }
    if (config_set_string("system.hostname", argv[1]) != 0) {
        shell_print_line("hostname: failed to update");
        return;
    }
    shell_set_hostname_cache(argv[1]);
    config_save_all();
    shell_print("Hostname updated to ");
    shell_print_line(argv[1]);
}

void cmd_volume(int argc, char** argv) {
    const char* current = config_get_string("system.volume");
    if (argc == 1) {
        shell_print("Volume: ");
        shell_print_line(current ? current : "75");
        shell_print_line("Usage: volume [0-100]");
        return;
    }
    uint64_t vol = shell_parse_u64(argv[1]);
    if (vol > 100ULL) {
        shell_print_line("volume: value must be 0-100");
        return;
    }
    char buf[8];
    shell_u64_to_text(vol, buf, sizeof(buf));
    if (config_set_string("system.volume", buf) != 0) {
        shell_print_line("volume: failed to update");
        return;
    }
    if (mk_mp3_set_volume) mk_mp3_set_volume(vol);
    config_save_all();
    shell_print("Volume set to ");
    shell_print_line(buf);
}

void cmd_beep(int argc, char** argv) {
    if (!ac97_is_available()) {
        shell_print_line("beep: no AC97 audio device available");
        return;
    }
    uint64_t freq = (argc > 1) ? shell_parse_u64(argv[1]) : 880;
    uint64_t ms = (argc > 2) ? shell_parse_u64(argv[2]) : 150;
    if (freq == 0 || freq > 20000) {
        shell_print_line("beep: frequency must be 1-20000 Hz");
        return;
    }
    if (ms > 1000) ms = 1000; /* see ac97_beep()'s header comment */
    ac97_beep((uint32_t)freq, (uint32_t)ms);
    shell_print_line("Beeped.");
}

static bool shell_path_looks_like_dir(const char* path) {
    if (!path || !path[0]) return true;
    size_t len = strlen(path);
    if (len > 0 && path[len - 1] == '/') return true;
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return !(strchr(base, '.') && (strstr(base, ".mp3") || strstr(base, ".wav") || strstr(base, ".ogg") || strstr(base, ".flac")));
}

static void shell_mp3_print_track(void) {
    if (!mk_mp3_get_player_state) {
        shell_print_line("MP3 backend is unavailable.");
        return;
    }
    mk_mp3_player_t* st = mk_mp3_get_player_state();
    if (!st) {
        shell_print_line("MP3 state is unavailable.");
        return;
    }
    shell_print("State: ");
    switch (st->state) {
        case MK_MP3_STATE_PLAYING: shell_print_line("playing"); break;
        case MK_MP3_STATE_PAUSED: shell_print_line("paused"); break;
        case MK_MP3_STATE_STOPPED: shell_print_line("stopped"); break;
        default: shell_print_line("error"); break;
    }
    shell_print("Title: "); shell_print_line(mk_mp3_get_current_title ? mk_mp3_get_current_title() : "Unknown");
    shell_print("Artist: "); shell_print_line(mk_mp3_get_current_artist ? mk_mp3_get_current_artist() : "Unknown artist");
    shell_print("Album: "); shell_print_line(mk_mp3_get_current_album ? mk_mp3_get_current_album() : "Unknown album");
    shell_print("Sample rate: "); shell_print_uint64(st->sample_rate); shell_print_line(" Hz");
    shell_print("Channels: "); shell_print_uint64(st->channels); shell_print_line("");
    shell_print("Bitrate: "); shell_print_uint64(st->bitrate); shell_print_line(" kbps");
    shell_print("Position: "); shell_print_uint64(st->current_position); shell_print_line(" ms");
    shell_print("Duration: "); shell_print_uint64(st->total_duration); shell_print_line(" ms");
    shell_print("Volume: "); shell_print_uint64(st->volume); shell_print_line("%");
    shell_print("Repeat: "); shell_print_line(st->repeat ? "on" : "off");
    shell_print("Shuffle: "); shell_print_line(st->shuffle ? "on" : "off");
}

static void shell_mp3_print_playlist(void) {
    if (!mk_mp3_get_playlist) {
        shell_print_line("MP3 playlist API unavailable.");
        return;
    }
    mk_mp3_playlist_t* pl = mk_mp3_get_playlist();
    if (!pl) {
        shell_print_line("No playlist state available.");
        return;
    }
    shell_print("Playlist entries: "); shell_print_uint64(pl->count); shell_print_line("");
    for (uint64_t i = 0; i < pl->count; ++i) {
        shell_print("  ["); shell_print_uint64(i); shell_print("] ");
        shell_print(pl->current == i ? "* " : "  ");
        const char* title = pl->entries[i].title[0] ? pl->entries[i].title : pl->entries[i].filename;
        shell_print_line(title);
    }
}

void cmd_music(int argc, char** argv) {
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        shell_mp3_print_track();
        shell_mp3_print_playlist();
        shell_print_line("Usage: music [status|load|play|pause|resume|stop|next|prev|seek|volume|repeat|shuffle|clear|list]");
        return;
    }

    if (strcmp(argv[1], "load") == 0) {
        if (argc < 3) { shell_print_line("Usage: music load <file-or-dir>"); return; }
        const char* path = argv[2];
        int rc = -1;
        if (shell_path_looks_like_dir(path) && mk_mp3_load_directory) {
            rc = mk_mp3_load_directory(path);
        } else if (mk_mp3_load_file) {
            rc = mk_mp3_load_file(path);
        }
        if (rc == 0) shell_print_line("MP3 loaded.");
        else shell_print_line("music: load failed");
        return;
    }

    if (strcmp(argv[1], "play") == 0) {
        if (mk_mp3_play && mk_mp3_play() == 0) shell_print_line("Playing.");
        else shell_print_line("music: play failed");
        return;
    }
    if (strcmp(argv[1], "pause") == 0) {
        if (mk_mp3_pause && mk_mp3_pause() == 0) shell_print_line("Paused.");
        else shell_print_line("music: pause failed");
        return;
    }
    if (strcmp(argv[1], "resume") == 0) {
        if (mk_mp3_resume && mk_mp3_resume() == 0) shell_print_line("Resumed.");
        else shell_print_line("music: resume failed");
        return;
    }
    if (strcmp(argv[1], "stop") == 0) {
        if (mk_mp3_stop && mk_mp3_stop() == 0) shell_print_line("Stopped.");
        else shell_print_line("music: stop failed");
        return;
    }
    if (strcmp(argv[1], "next") == 0) {
        if (mk_mp3_playlist_play_next && mk_mp3_playlist_play_next() == 0) shell_print_line("Next track.");
        else shell_print_line("music: next failed");
        return;
    }
    if (strcmp(argv[1], "prev") == 0) {
        if (mk_mp3_playlist_play_previous && mk_mp3_playlist_play_previous() == 0) shell_print_line("Previous track.");
        else shell_print_line("music: prev failed");
        return;
    }
    if (strcmp(argv[1], "seek") == 0) {
        if (argc < 3) { shell_print_line("Usage: music seek <ms>"); return; }
        uint64_t pos = shell_parse_u64(argv[2]);
        if (mk_mp3_seek && mk_mp3_seek(pos) == 0) shell_print_line("Seeked.");
        else shell_print_line("music: seek failed");
        return;
    }
    if (strcmp(argv[1], "volume") == 0) {
        if (argc < 3) { shell_print_line("Usage: music volume <0-100>"); return; }
        uint64_t vol = shell_parse_u64(argv[2]);
        if (vol > 100ULL) vol = 100ULL;
        if (mk_mp3_set_volume && mk_mp3_set_volume(vol) == 0) shell_print_line("Volume updated.");
        else shell_print_line("music: volume failed");
        return;
    }
    if (strcmp(argv[1], "repeat") == 0) {
        if (argc < 3) { shell_print_line("Usage: music repeat [off|one|all]"); return; }
        int mode = 0;
        if (strcmp(argv[2], "off") == 0 || strcmp(argv[2], "0") == 0) mode = 0;
        else if (strcmp(argv[2], "one") == 0 || strcmp(argv[2], "1") == 0) mode = 1;
        else mode = 2;
        if (mk_mp3_set_repeat && mk_mp3_set_repeat(mode) == 0) shell_print_line("Repeat updated.");
        else shell_print_line("music: repeat failed");
        return;
    }
    if (strcmp(argv[1], "shuffle") == 0) {
        bool enabled = true;
        if (argc >= 3) {
            if (strcmp(argv[2], "off") == 0 || strcmp(argv[2], "0") == 0 || strcmp(argv[2], "false") == 0) enabled = false;
        }
        if (mk_mp3_set_shuffle && mk_mp3_set_shuffle(enabled) == 0) shell_print_line("Shuffle updated.");
        else shell_print_line("music: shuffle failed");
        return;
    }
    if (strcmp(argv[1], "clear") == 0) {
        if (mk_mp3_clear_playlist && mk_mp3_clear_playlist() == 0) shell_print_line("Playlist cleared.");
        else shell_print_line("music: clear failed");
        return;
    }
    if (strcmp(argv[1], "list") == 0) {
        shell_mp3_print_playlist();
        return;
    }

    shell_print_line("Usage: music [status|load|play|pause|resume|stop|next|prev|seek|volume|repeat|shuffle|clear|list]");
}

void cmd_resolution(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_print_line("Font resolution is fixed at 12x16.");
    shell_print_line("The system uses 12x16 ASCII everywhere.");
    shell_print("Framebuffer: ");
    shell_print_uint64((uint64_t)SCREEN_W);
    shell_print("x");
    shell_print_uint64((uint64_t)SCREEN_H);
    shell_print("  Font resolution: ");
    shell_print_line("12x16");
}

void cmd_timezone(int argc, char** argv) {
    const char* current = config_get_string("system.timezone");
    if (argc == 1) {
        shell_print("Timezone: ");
        shell_print_line((current && current[0]) ? current : "UTC");
        shell_print_line("Usage: timezone [IANA/abbrev]");
        return;
    }
    if (config_set_string("system.timezone", argv[1]) != 0) {
        shell_print_line("timezone: failed to update");
        return;
    }
    config_save_all();
    shell_print("Timezone updated to ");
    shell_print_line(argv[1]);
}

void cmd_autoscroll(int argc, char** argv) {
    if (argc == 1) {
        shell_print("Terminal auto-scroll is ");
        shell_print_line(gui_get_terminal_autoscroll() ? "enabled" : "disabled");
        return;
    }
    if (strcmp(argv[1], "toggle") == 0) {
        gui_set_terminal_autoscroll(!gui_get_terminal_autoscroll());
    } else if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0 || strcmp(argv[1], "true") == 0) {
        gui_set_terminal_autoscroll(true);
    } else if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0 || strcmp(argv[1], "false") == 0) {
        gui_set_terminal_autoscroll(false);
    } else {
        shell_print_line("Usage: autoscroll [on|off|toggle]");
        return;
    }
    config_set_string("gui.terminal_autoscroll", gui_get_terminal_autoscroll() ? "1" : "0");
    config_save_all();
    shell_print("Terminal auto-scroll is now ");
    shell_print_line(gui_get_terminal_autoscroll() ? "enabled" : "disabled");
}

void cmd_sysinfo(int argc, char** argv) {
    (void)argc; (void)argv;
    uint64_t total = memory_get_total();
    uint64_t free_mem = memory_get_free();
    shell_print_line("System Information:");
    shell_print("  Version: "); shell_print_line("C-OS 4.0.8 alpha");
    shell_print("  Uptime: "); shell_print_uint64(get_timer_ticks() / 1000); shell_print_line(" seconds");
    shell_print("  Memory: "); shell_print_uint64(total / 1024); shell_print(" KB total, ");
    shell_print_uint64((total - free_mem) / 1024); shell_print(" KB used, ");
    shell_print_uint64(free_mem / 1024); shell_print_line(" KB free");
    shell_print("  Hostname: "); shell_print_line(shell_get_hostname());
    shell_print("  Theme: "); shell_print_line(shell_theme_name(gui_get_theme_idx()));
    shell_print("  Font resolution: "); shell_print_line("12x16 (forced)");
}

void cmd_mem(int argc, char** argv) {
    (void)argc; (void)argv;
    uint64_t total = memory_get_total();
    uint64_t free_mem = memory_get_free();
    shell_print("Memory Stats:\n");
    shell_print("  Total: ");
    shell_print_uint64(total / 1024);
    shell_print(" KB\n");
    shell_print("  Free:  ");
    shell_print_uint64(free_mem / 1024);
    shell_print(" KB\n");
    shell_print("  Used:  ");
    shell_print_uint64((total - free_mem) / 1024);
    shell_print(" KB\n");
}

void cmd_time(int argc, char** argv) {
    (void)argc; (void)argv;
    rtc_time_t t = rtc_get_datetime();
    shell_print("Current Time: ");
    if (t.hour < 10) { shell_print("0"); }
    shell_print_uint64(t.hour);
    shell_print(":");
    if (t.minute < 10) { shell_print("0"); }
    shell_print_uint64(t.minute);
    shell_print(":");
    if (t.second < 10) { shell_print("0"); }
    shell_print_uint64(t.second);
    shell_print("\n");
}

void cmd_date(int argc, char** argv) {
    (void)argc; (void)argv;
    rtc_time_t t = rtc_get_datetime();
    shell_print("Current Date: 20");
    shell_print_uint64(t.year);
    shell_print("-");
    if (t.month < 10) { shell_print("0"); }
    shell_print_uint64(t.month);
    shell_print("-");
    if (t.day < 10) { shell_print("0"); }
    shell_print_uint64(t.day);
    shell_print("\n");
}

void cmd_uptime(int argc, char** argv) {
    (void)argc; (void)argv;
    uint64_t ticks = get_timer_ticks();
    shell_print("Uptime: ");
    shell_print_uint64(ticks / 1000);
    shell_print(" seconds\n");
}

void cmd_ver(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_print("C-OS 4.0.8 alpha (64-bit Edition)\n");
}

void cmd_ls(int argc, char** argv) {
    const char* path = shell_cwd;
    static char resolved[FS_MAX_PATH];
    if (argc >= 2 && argv[1] && argv[1][0]) {
        shell_resolve_path(argv[1], resolved, sizeof(resolved));
        path = resolved;
    }
    int count = fs_entry_count_for_path(path);
    fs_entry_t* entries = fs_list_dir(path);
    if (!entries || count <= 0) {
        shell_print("ls: cannot access '"); shell_print(path); shell_print_line("': No such directory");
        return;
    }
    shell_print("Directory listing: "); shell_print_line(path);
    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] == '\0') continue;
        if (entries[i].is_dir) {
            shell_print("  [DIR] "); shell_print(entries[i].name); shell_print_line("/");
        } else {
            shell_print("  [FILE] "); shell_print(entries[i].name); shell_print(" (");
            shell_print_uint64((uint64_t)entries[i].size);
            shell_print_line(" bytes)");
        }
    }
}

void cmd_cat(int argc, char** argv) {
    if (argc < 2) {
        if (shell_get_pipe_input()) {
            shell_print_line(shell_get_pipe_input());
        } else {
            shell_print_line("Usage: cat filename");
        }
        return;
    }
    char resolved[FS_MAX_PATH];
    shell_resolve_path(argv[1], resolved, sizeof(resolved));
    const char* data = fs_read_file(resolved);
    if (!data) {
        shell_print("cat: "); shell_print(resolved); shell_print_line(": No such file");
        return;
    }
    shell_print_line(data);
}


static bool shell_split_path(const char* full, char* parent, size_t parent_size, char* leaf, size_t leaf_size) {
    if (!parent || !leaf || parent_size == 0 || leaf_size == 0) return false;
    parent[0] = '\0';
    leaf[0] = '\0';
    if (!full || !full[0]) return false;
    const char* last = NULL;
    for (const char* p = full; *p; ++p) if (*p == '/') last = p;
    if (!last) {
        strncpy(parent, "/", parent_size - 1);
        parent[parent_size - 1] = '\0';
        strncpy(leaf, full, leaf_size - 1);
        leaf[leaf_size - 1] = '\0';
        return true;
    }
    size_t plen = (size_t)(last - full);
    if (plen == 0) {
        strncpy(parent, "/", parent_size - 1);
        parent[parent_size - 1] = '\0';
    } else {
        if (plen >= parent_size) plen = parent_size - 1;
        memcpy(parent, full, plen);
        parent[plen] = '\0';
    }
    strncpy(leaf, last + 1, leaf_size - 1);
    leaf[leaf_size - 1] = '\0';
    return true;
}

static const char* shell_read_all_from_path(const char* path) {
    if (!path || !path[0]) return NULL;
    char parent[FS_MAX_PATH];
    char leaf[FS_MAX_NAME];
    if (!shell_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf))) return NULL;
    return fs_read_file_at(parent, leaf);
}

static void shell_find_in_text(const char* pattern, const char* text) {
    if (!pattern || !pattern[0] || !text) return;
    const char* p = text;
    char line[256];
    while (*p) {
        size_t li = 0;
        while (p[li] && p[li] != '\n' && li < sizeof(line) - 1) li++;
        memcpy(line, p, li);
        line[li] = '\0';
        if (strstr(line, pattern)) shell_print_line(line);
        p += li;
        if (*p == '\n') p++;
    }
}

void cmd_grep(int argc, char** argv) {
    if (argc < 2) { shell_print_line("Usage: grep pattern [file]"); return; }
    const char* pattern = argv[1];
    const char* text = NULL;
    if (argc >= 3) text = shell_read_all_from_path(argv[2]);
    else text = shell_get_pipe_input();
    if (!text) { shell_print_line("grep: no input text"); return; }
    shell_find_in_text(pattern, text);
}

void cmd_find(int argc, char** argv) {
    if (argc < 2) { shell_print_line("Usage: find pattern [path]"); return; }
    const char* pattern = argv[1];
    const char* path = shell_cwd;
    static char resolved[FS_MAX_PATH];
    if (argc >= 3) { shell_resolve_path(argv[2], resolved, sizeof(resolved)); path = resolved; }
    int count = fs_entry_count_for_path(path);
    fs_entry_t* entries = fs_list_dir(path);
    if (!entries || count <= 0) { shell_print("find: no such directory: "); shell_print_line(path); return; }
    for (int i = 0; i < count; ++i) {
        if (entries[i].name[0] && strstr(entries[i].name, pattern)) {
            shell_print(path); shell_print("/"); shell_print_line(entries[i].name);
        }
    }
}

static void shell_open_text_editor(const char* fullpath) {
    window_t* w = gui_open_window(WIN_TEXT_EDITOR, "Text Editor", 140, 90, 900, 650);
    if (!w) return;
    memset(w->text_buf, 0, sizeof(w->text_buf));
    w->text_cursor = 0;
    w->text_sel_start = 0;
    w->text_sel_end = 0;
    w->text_modified = false;
    w->scroll_x = 0;
    w->scroll_y = 0;
    w->filename[0] = '\0';
    if (fullpath && fullpath[0]) {
        strncpy(w->filename, fullpath, sizeof(w->filename) - 1);
        w->filename[sizeof(w->filename) - 1] = '\0';
        const char* text = shell_read_all_from_path(fullpath);
        if (text) {
            strncpy(w->text_buf, text, sizeof(w->text_buf) - 1);
            w->text_buf[sizeof(w->text_buf) - 1] = '\0';
            w->text_cursor = (int)strlen(w->text_buf);
        }
    }
}

void cmd_nano(int argc, char** argv) {
    if (argc < 2) {
        shell_open_text_editor(NULL);
        shell_print_line("nano: opened a blank editor window");
        return;
    }
    char resolved[FS_MAX_PATH];
    shell_resolve_path(argv[1], resolved, sizeof(resolved));
    shell_open_text_editor(resolved);
    shell_print("nano: opened "); shell_print_line(resolved);
}

void cmd_open(int argc, char** argv) {
    if (argc < 2) { shell_print_line("Usage: open app-name"); return; }
    if (strcmp(argv[1], "terminal") == 0) { gui_open_window(WIN_TERMINAL, "Terminal", 120, 80, 1024, 680); }
    else if (strcmp(argv[1], "settings") == 0) { gui_open_window(WIN_SETTINGS, "Settings", 160, 90, 980, 700); }
    else if (strcmp(argv[1], "files") == 0 || strcmp(argv[1], "file-manager") == 0) { gui_open_window(WIN_FILE_MGR, "File Manager", 100, 70, 1080, 720); }
    else if (strcmp(argv[1], "editor") == 0 || strcmp(argv[1], "text") == 0) { gui_open_window(WIN_TEXT_EDITOR, "Text Editor", 140, 90, 900, 650); }
    else if (strcmp(argv[1], "browser") == 0 || strcmp(argv[1], "netsurf") == 0) { gui_open_window(WIN_BROWSER, "NetSurf", 120, 80, 1100, 760); }
    else if (strcmp(argv[1], "calc") == 0 || strcmp(argv[1], "calculator") == 0) { gui_open_window(WIN_CALC, "Calculator", 160, 100, 720, 520); }
    else { shell_print("open: unknown app: "); shell_print_line(argv[1]); return; }
    shell_print("Launched app: "); shell_print_line(argv[1]);
}

void cmd_pkg(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Package manager API is available in the next integration step."); }
void cmd_module(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Module loader API hook ready."); }
void cmd_widget(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Widget API hook ready."); }
void cmd_service(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Service manager hook ready."); }
void cmd_log(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Logging API hook ready."); }
void cmd_driver(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Driver abstraction hook ready."); }
void cmd_vmm(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Virtual memory manager hook ready."); }
void cmd_fs(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Filesystem API hook ready."); }
void cmd_proc(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Process/task API hook ready."); }
void cmd_task(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Task scheduler API hook ready."); }
void cmd_plugin(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Plugin/module API hook ready."); }
void cmd_net(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Network API hook ready."); }
void cmd_power(int argc, char** argv) { (void)argc; (void)argv; shell_print_line("Power API hook ready."); }

void cmd_touch(int argc, char** argv) {
    if (argc < 2) { shell_print_line("Usage: touch filename"); return; }
    char resolved[FS_MAX_PATH];
    shell_resolve_path(argv[1], resolved, sizeof(resolved));
    if (fs_create_file(resolved)) {
        shell_print("Created file: "); shell_print_line(resolved);
    } else {
        shell_print("touch: cannot create '"); shell_print(resolved); shell_print_line("'");
    }
}

void cmd_rm(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_print("rm is disabled in this safe build. Use the file manager to delete files.\n");
}

void cmd_mkdir(int argc, char** argv) {
    if (argc < 2) { shell_print_line("Usage: mkdir dirname"); return; }
    char resolved[FS_MAX_PATH];
    shell_resolve_path(argv[1], resolved, sizeof(resolved));
    if (fs_create_dir(resolved)) {
        shell_print("Created directory: "); shell_print_line(resolved);
    } else {
        shell_print("mkdir: cannot create directory '"); shell_print(resolved); shell_print_line("'");
    }
}

void cmd_cd(int argc, char** argv) {
    if (argc < 2) { shell_print_line(shell_cwd); return; }
    char resolved[FS_MAX_PATH];
    shell_resolve_path(argv[1], resolved, sizeof(resolved));
    int count = fs_entry_count_for_path(resolved);
    fs_entry_t* entries = fs_list_dir(resolved);
    if (!entries || count <= 0) {
        shell_print("cd: no such directory: "); shell_print_line(resolved);
        return;
    }
    strncpy(shell_cwd, resolved, sizeof(shell_cwd) - 1);
    shell_cwd[sizeof(shell_cwd)-1] = '\0';
    shell_print("Changed directory to: "); shell_print_line(shell_cwd);
}

void cmd_pwd(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_print_line(shell_cwd);
}

void cmd_reboot(int argc, char** argv) {
    (void)argc; (void)argv;
    storage_sync();
    shell_print("Reboot is disabled in this safe build. Use the GUI power controls.\n");
}

void cmd_shutdown(int argc, char** argv) {
    (void)argc; (void)argv;
    storage_sync();
    shell_print("Shutdown is disabled in this safe build. Use the GUI power controls.\n");
}

void cmd_ps(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_print("PID  NAME        STATE\n");
    shell_print("0    idle        RUNNING\n");
    shell_print("1    init        SLEEPING\n");
    shell_print("2    shell       RUNNING\n");
}

void cmd_kill(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_print("kill is disabled in this safe build.\n");
}

void cmd_free(int argc, char** argv) {
    cmd_mem(argc, argv);
}

void cmd_df(int argc, char** argv) {
    (void)argc; (void)argv;
    uint64_t total = storage_get_total_space();
    uint64_t used = storage_get_used_space();
    uint64_t free_space = storage_get_free_space();
    shell_print_line("Filesystem     Size  Used  Avail  Use%");
    shell_print("/dev/vfs      "); shell_print_uint64(total / (1024 * 1024)); shell_print("M   ");
    shell_print_uint64(used / (1024 * 1024)); shell_print("M    ");
    shell_print_uint64(free_space / (1024 * 1024)); shell_print_line("M");
}

void cmd_uname(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_print("C-OS 4.0.8 alpha x86_64\n");
}

void cmd_whoami(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_print("root\n");
}

void cmd_exit(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_running = false;
    shell_print("Exiting shell...\n");
}

void shell_init(void) {
    shell_cwd[0] = '/';
    shell_cwd[1] = '\0';
    shell_print("[SHELL] C-OS 4.0.8 alpha Shell Initialized\n");
    shell_print("Type help for a list of commands.\n");
    shell_print_prompt();
}

static void shell_run_command_stage(char* stage, const char* pipe_input, bool capture_output) {
    if (!stage || !stage[0]) return;

    char* argv[MAX_ARGS];
    int argc = 0;

    shell_pipe_input = pipe_input;
    shell_capture_len = 0;
    shell_capture_buffer[0] = '\0';

    shell_output_callback_t prev_callback = shell_output_callback;
    if (capture_output) {
        shell_capture_forward = prev_callback;
        shell_set_output_callback(shell_capture_output);
    }

    shell_strtok_next = NULL;
    char* token = shell_strtok(stage, " ");
    while (token != NULL && argc < MAX_ARGS) {
        argv[argc++] = token;
        token = shell_strtok(NULL, " ");
    }

    if (argc > 0) {
        bool found = false;
        for (int i = 0; i < NUM_COMMANDS; i++) {
            if (shell_strcmp(argv[0], commands[i].name) == 0) {
                commands[i].handler(argc, argv);
                found = true;
                break;
            }
        }
        if (!found) {
            shell_print("Unknown command: ");
            shell_print(argv[0]);
            shell_print("\n");
        }
    }

    if (capture_output) {
        shell_set_output_callback(prev_callback);
        shell_capture_forward = NULL;
    }
    shell_pipe_input = NULL;
}

void shell_execute(char* line, bool print_prompt) {
    if (!line || line[0] == 0) { return; }

    char original[MAX_CMD_LEN];
    strncpy(original, line, sizeof(original) - 1);
    original[sizeof(original) - 1] = '\0';

    char work[MAX_CMD_LEN];
    strncpy(work, line, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char redirect_target[FS_MAX_PATH];
    redirect_target[0] = '\0';

    char* redir = strchr(work, '>');
    if (redir) {
        *redir = '\0';
        redir++;
        shell_trim_inplace(redir);
        if (redir[0]) {
            strncpy(redirect_target, redir, sizeof(redirect_target) - 1);
            redirect_target[sizeof(redirect_target) - 1] = '\0';
        }
    }

    shell_trim_inplace(work);

    char* stages[8];
    int stage_count = 0;
    shell_strtok_next = NULL;
    char* stage = shell_strtok(work, "|");
    while (stage != NULL && stage_count < 8) {
        shell_trim_inplace(stage);
        if (stage[0]) {
            stages[stage_count++] = stage;
        }
        stage = shell_strtok(NULL, "|");
    }

    if (stage_count == 0) {
        shell_history_add(original);
        if (print_prompt) {
            shell_print_prompt();
        }
        return;
    }

    char pipe_buffer[8192];
    const char* pipe_input = NULL;

    for (int i = 0; i < stage_count; i++) {
        bool capture = (i < stage_count - 1) || (redirect_target[0] != '\0');
        shell_run_command_stage(stages[i], pipe_input, capture);
        if (i < stage_count - 1) {
            strncpy(pipe_buffer, shell_capture_buffer, sizeof(pipe_buffer) - 1);
            pipe_buffer[sizeof(pipe_buffer) - 1] = '\0';
            pipe_input = pipe_buffer;
        }
    }

    if (redirect_target[0]) {
        char resolved[FS_MAX_PATH];
        shell_resolve_path(redirect_target, resolved, sizeof(resolved));
        char parent[FS_MAX_PATH];
        char leaf[FS_MAX_NAME];
        if (shell_split_path(resolved, parent, sizeof(parent), leaf, sizeof(leaf)) &&
            fs_write_file_at(parent, leaf, shell_capture_buffer, (uint64_t)strlen(shell_capture_buffer))) {
            shell_print("Redirected output to ");
            shell_print_line(resolved);
        } else {
            shell_print("redirect: failed to write ");
            shell_print_line(resolved);
        }
    }

    shell_history_add(original);
    if (print_prompt) {
        shell_print_prompt();
    }
}

void shell_process(void) {

    shell_print_prompt();
}

bool shell_is_running(void) {
    return shell_running;
}

void shell_set_running(bool running) {
    shell_running = running;
}
