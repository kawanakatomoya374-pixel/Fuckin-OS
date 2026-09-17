/**
 * modern_browser.h - 高機能Webブラウザヘッダー
 */

#ifndef MODERN_BROWSER_H
#define MODERN_BROWSER_H

#include <stdint.h>
#include <stdbool.h>

// ブラウザ状態
typedef enum {
    BROWSER_STATE_IDLE = 0,
    BROWSER_STATE_LOADING = 1,
    BROWSER_STATE_READY = 2,
    BROWSER_STATE_ERROR = 3,
    BROWSER_STATE_SUBMITTED = 4
} browser_state_t;

// ブラウザ構造体前方宣言
typedef struct modern_browser_t modern_browser_t;

// ブラウザAPI
int modern_browser_init(void);
int modern_browser_open(const char* url);
void modern_browser_close(void);
void modern_browser_refresh(void);

modern_browser_t* modern_browser_get_instance(void);
const char* modern_browser_get_url(void);
const char* modern_browser_get_title(void);
browser_state_t modern_browser_get_state(void);

// ナビゲーション
int browser_navigate(const char* url);
int browser_back(void);
int browser_forward(void);
int browser_reload(void);
int browser_home(void);

// 入力処理
void browser_handle_keypress(char key, int scancode);
void browser_handle_click(int mx, int my, int button);
void browser_scroll(int delta);

// 描画
void browser_draw(int x, int y, int w, int h);

#endif // MODERN_BROWSER_H