/**
 * enhanced_context_menu.h - C-OS 5.0.0 Enhanced Context Menu System
 * 
 * 大幅改善された右クリックメニューシステム
 * 
 * 新機能:
 *   - アイコン付きメニュー項目
 *   - 階層型サブメニュー (無制限ネスト)
 *   - キーボードショートカット表示
 *   - アニメーション付き表示/非表示
 *   - テーマ対応 (ライト/ダーク)
 *   - セパレーター・ヘッダー・チェックボックス
 *   - ファイル操作専用メニュー
 *   - デスクトップ操作専用メニュー
 *   - ウィンドウ操作専用メニュー
 */
#ifndef ENHANCED_CONTEXT_MENU_H
#define ENHANCED_CONTEXT_MENU_H

#include "../include/types.h"
#include <stdbool.h>
#include <stdint.h>

/* メニュー項目の最大数 */
#define ECM_MAX_ITEMS       48
#define ECM_MAX_SUBMENUS    8
#define ECM_MAX_DEPTH       4
#define ECM_LABEL_LEN       64
#define ECM_SHORTCUT_LEN    16

/* メニュー項目タイプ */
typedef enum {
    ECM_ITEM_NORMAL     = 0,    /* 通常項目 */
    ECM_ITEM_SEPARATOR  = 1,    /* セパレーター */
    ECM_ITEM_HEADER     = 2,    /* ヘッダー (選択不可) */
    ECM_ITEM_CHECKBOX   = 3,    /* チェックボックス */
    ECM_ITEM_RADIO      = 4,    /* ラジオボタン */
    ECM_ITEM_SUBMENU    = 5,    /* サブメニュー付き */
    ECM_ITEM_DANGER     = 6,    /* 危険操作 (赤色) */
} ecm_item_type_t;

/* アイコン種別 */
typedef enum {
    ECM_ICON_NONE       = 0,
    ECM_ICON_OPEN       = 1,    /* フォルダ開く */
    ECM_ICON_EDIT       = 2,    /* 編集 */
    ECM_ICON_COPY       = 3,    /* コピー */
    ECM_ICON_CUT        = 4,    /* 切り取り */
    ECM_ICON_PASTE      = 5,    /* 貼り付け */
    ECM_ICON_DELETE     = 6,    /* 削除 */
    ECM_ICON_RENAME     = 7,    /* 名前変更 */
    ECM_ICON_NEW_FILE   = 8,    /* 新規ファイル */
    ECM_ICON_NEW_FOLDER = 9,    /* 新規フォルダ */
    ECM_ICON_PROPERTIES = 10,   /* プロパティ */
    ECM_ICON_SETTINGS   = 11,   /* 設定 */
    ECM_ICON_REFRESH    = 12,   /* 更新 */
    ECM_ICON_SORT       = 13,   /* 並べ替え */
    ECM_ICON_VIEW       = 14,   /* 表示 */
    ECM_ICON_SEARCH     = 15,   /* 検索 */
    ECM_ICON_SHARE      = 16,   /* 共有 */
    ECM_ICON_INFO       = 17,   /* 情報 */
    ECM_ICON_CLOSE      = 18,   /* 閉じる */
    ECM_ICON_MINIMIZE   = 19,   /* 最小化 */
    ECM_ICON_MAXIMIZE   = 20,   /* 最大化 */
    ECM_ICON_RESTORE    = 21,   /* 元に戻す */
    ECM_ICON_MOVE       = 22,   /* 移動 */
    ECM_ICON_RESIZE     = 23,   /* リサイズ */
    ECM_ICON_PLAY       = 24,   /* 再生 */
    ECM_ICON_TERMINAL   = 25,   /* ターミナル */
    ECM_ICON_WALLPAPER  = 26,   /* 壁紙 */
    ECM_ICON_POWER      = 27,   /* 電源 */
    ECM_ICON_LOCK       = 28,   /* ロック */
    ECM_ICON_STAR       = 29,   /* お気に入り */
    ECM_ICON_LUA        = 30,   /* Luaスクリプト */
    ECM_ICON_COMPRESS   = 31,   /* 圧縮 */
    ECM_ICON_EXTRACT    = 32,   /* 展開 */
    ECM_ICON_SEND_TO    = 33,   /* 送る */
    ECM_ICON_SHORTCUT   = 34,   /* ショートカット */
} ecm_icon_t;

/* メニュー項目 */
typedef struct {
    char            label[ECM_LABEL_LEN];
    char            label_ja[ECM_LABEL_LEN];
    char            shortcut[ECM_SHORTCUT_LEN];
    ecm_item_type_t type;
    ecm_icon_t      icon;
    bool            enabled;
    bool            checked;    /* チェックボックス/ラジオ用 */
    int             action_id;
    int             submenu_id; /* サブメニューID (-1=なし) */
    int             data;       /* 汎用データ */
} ecm_item_t;

/* サブメニュー */
typedef struct {
    int         id;
    char        title[ECM_LABEL_LEN];
    ecm_item_t  items[ECM_MAX_ITEMS];
    int         item_count;
    bool        visible;
    int         x, y;
    int         parent_item;    /* 親メニューの項目インデックス */
    int         hovered;
    int         anim_frame;     /* アニメーションフレーム */
} ecm_submenu_t;

/* メインコンテキストメニュー */
typedef struct {
    bool            visible;
    int             x, y;
    int             w, h;
    ecm_item_t      items[ECM_MAX_ITEMS];
    int             item_count;
    int             hovered;
    int             target_window;
    int             target_icon;
    int             context_type;   /* 0=desktop 1=window 2=file 3=taskbar */
    ecm_submenu_t   submenus[ECM_MAX_SUBMENUS];
    int             submenu_count;
    int             active_submenu; /* 現在表示中のサブメニューID */
    int             anim_frame;     /* 表示アニメーション */
    bool            dark_mode;
    
    /* クリップボード状態 */
    bool            clipboard_has_data;
    char            clipboard_path[256];
    bool            clipboard_is_cut;

    /* メニューを開いた対象のファイル/フォルダのフルパス
     * (ECM_CTX_FM_FILE / ECM_CTX_FM_FOLDER / ECM_CTX_FM_EMPTY 用。
     *  空き領域で開いた場合はカレントディレクトリのパスが入る) */
    char            target_path[512];
} ecm_context_menu_t;

/* コンテキストメニュー種別 */
typedef enum {
    ECM_CTX_DESKTOP_EMPTY   = 0,    /* デスクトップ空き領域 */
    ECM_CTX_DESKTOP_ICON    = 1,    /* デスクトップアイコン */
    ECM_CTX_WINDOW          = 2,    /* ウィンドウタイトルバー */
    ECM_CTX_TASKBAR         = 3,    /* タスクバー */
    ECM_CTX_FILE            = 4,    /* ファイル */
    ECM_CTX_FOLDER          = 5,    /* フォルダ */
    ECM_CTX_TEXT_EDITOR     = 6,    /* テキストエディター内 */
    ECM_CTX_BROWSER         = 7,    /* ブラウザ内 */
    ECM_CTX_FM_FILE         = 8,    /* ファイルマネージャー内ファイル */
    ECM_CTX_FM_FOLDER       = 9,    /* ファイルマネージャー内フォルダ */
    ECM_CTX_FM_EMPTY        = 10,   /* ファイルマネージャー空き領域 */
} ecm_context_type_t;

/* アクションID定義 */
#define ECM_ACT_OPEN            1
#define ECM_ACT_OPEN_WITH       2
#define ECM_ACT_EDIT            3
#define ECM_ACT_COPY            4
#define ECM_ACT_CUT             5
#define ECM_ACT_PASTE           6
#define ECM_ACT_DELETE          7
#define ECM_ACT_RENAME          8
#define ECM_ACT_NEW_FILE        9
#define ECM_ACT_NEW_FOLDER      10
#define ECM_ACT_PROPERTIES      11
#define ECM_ACT_REFRESH         12
#define ECM_ACT_SELECT_ALL      13
#define ECM_ACT_SORT_NAME       14
#define ECM_ACT_SORT_SIZE       15
#define ECM_ACT_SORT_DATE       16
#define ECM_ACT_SORT_TYPE       17
#define ECM_ACT_VIEW_LIST       18
#define ECM_ACT_VIEW_ICONS      19
#define ECM_ACT_VIEW_DETAILS    20
#define ECM_ACT_VIEW_THUMBNAILS 21
#define ECM_ACT_SEARCH          22
#define ECM_ACT_SHARE           23
#define ECM_ACT_COMPRESS        24
#define ECM_ACT_EXTRACT         25
#define ECM_ACT_SEND_TO_DESKTOP 26
#define ECM_ACT_CREATE_SHORTCUT 27
#define ECM_ACT_ADD_FAVORITE    28
#define ECM_ACT_RUN_LUA         29
#define ECM_ACT_OPEN_TERMINAL   30
#define ECM_ACT_WIN_RESTORE     31
#define ECM_ACT_WIN_MINIMIZE    32
#define ECM_ACT_WIN_MAXIMIZE    33
#define ECM_ACT_WIN_CLOSE       34
#define ECM_ACT_WIN_CENTER      35
#define ECM_ACT_WIN_LEFT_HALF   36
#define ECM_ACT_WIN_RIGHT_HALF  37
#define ECM_ACT_WIN_FULLSCREEN  38
#define ECM_ACT_DESKTOP_WALLPAPER 39
#define ECM_ACT_DESKTOP_SETTINGS 40
#define ECM_ACT_DESKTOP_REFRESH 41
#define ECM_ACT_DESKTOP_RESET   42
#define ECM_ACT_ICON_SIZE_SMALL 43
#define ECM_ACT_ICON_SIZE_MEDIUM 44
#define ECM_ACT_ICON_SIZE_LARGE 45
#define ECM_ACT_POWER_SLEEP     46
#define ECM_ACT_POWER_RESTART   47
#define ECM_ACT_POWER_SHUTDOWN  48
#define ECM_ACT_ABOUT           49
#define ECM_ACT_SHOW_DESKTOP    50
#define ECM_ACT_SAVE_LAYOUT     51
#define ECM_ACT_OPEN_SETTINGS   52
#define ECM_ACT_OPEN_FILE_MGR   53
#define ECM_ACT_OPEN_BROWSER    54
#define ECM_ACT_OPEN_TERMINAL2  55
#define ECM_ACT_OPEN_MUSIC      56
#define ECM_ACT_OPEN_JPEG       57
#define ECM_ACT_OPEN_CALC       58
#define ECM_ACT_OPEN_TEXT_EDITOR 59
#define ECM_ACT_LOCK_SCREEN     60
#define ECM_ACT_COPY_PATH       61
#define ECM_ACT_MOVE_TO         62
#define ECM_ACT_COPY_TO         63

/* 初期化・終了 */
void ecm_init(void);
void ecm_cleanup(void);

/* メニュー表示 */
void ecm_show(ecm_context_type_t ctx_type, int x, int y, int target_window, int target_icon, const char* file_path);
void ecm_close(void);
bool ecm_is_visible(void);

/* 描画 */
void ecm_draw(void);

/* 入力処理 */
bool ecm_handle_click(int mx, int my);
void ecm_handle_mouse_move(int mx, int my);
bool ecm_handle_key(int key);

/* アクション処理 */
int  ecm_get_last_action(void);
void ecm_clear_action(void);

/* クリップボード */
void ecm_clipboard_copy(const char* path, bool is_cut);
bool ecm_clipboard_has_data(void);
const char* ecm_clipboard_get_path(void);
bool ecm_clipboard_is_cut(void);
void ecm_clipboard_clear(void);

/* メニューを開いた対象のパス (呼び出し元がアクション実行時にどのファイルに
 * 対する操作かを知るために使う) */
const char* ecm_get_target_path(void);

/* アイコン描画ヘルパー */
void ecm_draw_icon(int x, int y, ecm_icon_t icon, uint64_t color);

/* ダークモード */
void ecm_set_dark_mode(bool dark);

/* 外部参照 */
extern ecm_context_menu_t g_ecm;

#endif /* ENHANCED_CONTEXT_MENU_H */
