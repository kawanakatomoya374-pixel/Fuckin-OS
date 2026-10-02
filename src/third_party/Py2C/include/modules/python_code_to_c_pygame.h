#ifndef PYTHON_CODE_TO_C_PYGAME_H
#define PYTHON_CODE_TO_C_PYGAME_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * python_code_to_c_pygame: 「ヘッドレス」pygame互換モジュール。
 * ------------------------------------------------------------
 * 本物のSDL2による描画・音声・入力は行わない。目的は、
 * pygameスタイルで書かれたゲームのコード（ウィンドウ初期化、
 * イベントループ、Rectを使った当たり判定、Spriteのグループ管理など）が
 * "実際の画面表示なし"でも構文的・ロジック的に動く/検証できるようにすること。
 *
 * 対応する範囲:
 *   pygame.init()/quit()
 *   pygame.display.set_mode/set_caption/flip/update
 *   pygame.time.Clock（tick）、pygame.time.get_ticks
 *   pygame.event.get()（常に空リストを返す）/pump()
 *   pygame.draw.rect/circle/line（実際には何も描画しない）
 *   pygame.key.get_pressed()（すべて押されていない状態を返す）
 *   pygame.Surface（fill/blit/get_rect/get_width/get_height）
 *   pygame.Rect（move/colliderect/contains、x/y/width/height属性）
 *   pygame.sprite.Sprite/Group（基本的な追加・更新・列挙）
 *   QUIT, KEYDOWN, KEYUP, K_* などの主要な定数
 *
 * 対応しない範囲: 実際の描画・音声・画像読み込み・本物のイベント生成。
 */
void p2c_register_pygame_module(void);
/* runtime shutdown時にmodule内のGC object slotを無効化する。再初期化では新しいruntime epochのclass objectを生成する。 */
void p2c_pygame_runtime_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PYTHON_CODE_TO_C_PYGAME_H */
