# C-OS ブラウザ強化 設計図 (Browser Enhancement Blueprint)

## 概要
C-OS 4.0.8 alpha のブラウザ機能（NetSurf + QuickJS）を劇的に強化するための段階的実装計画。
QuickJS Copy-and-Patch JIT、完全 DOM API、レンダリング最適化、画像デコーダ高速化、TinyCC 統合を含む。

---

## 現状分析 (As-Is Architecture)

### 1. ブラウザ構造
```
gui_apps_browser.c (3,016 行)
├── NetSurf エンジン連携
│   ├── cos_netsurf.c (22,641 行)
│   ├── cos_netsurf_browser.c (681 行)
│   ├── cos_netsurf_render.c (14,332 行) - DOM ツリー巡回・テキスト抽出
│   ├── cos_netsurf_adapter.c (33,052 行) - GUI アダプタ
│   └── cos_fetch_*.c - HTTP/File フェッチ
└── QuickJS 統合
    └── cos_js_web_api.c (1,029 行) - Web API サンドボックス
```

### 2. QuickJS 状況
```
third_party/quickjs/
├── quickjs.c (2,131,458 バイト) - メインエンジン
├── quickjs_jit.c (16,937 バイト) - ✅ Copy-and-Patch JIT 実装済み
├── quickjs_jit.h (876 バイト)
├── cos_js_web_api.c (40,758 バイト) - Web API
└── JIT_README.md - 技術ドキュメント

JIT 特徴:
- 2MB コードキャッシュ (x86_64 ネイティブ)
- ヒート検出 (100 回呼び出しで JIT コンパイル)
- サポート OP: nop, push_i32, pop, add, sub, mul, div, ret
- 期待性能: 単純計算 3-5 倍、DOM 操作 2-3 倍、JSON パース 1.5-2 倍
```

### 3. 画像デコーダ状況
```
アプリ層:
├── png_decoder.c - 軽量 PNG デコーダ (DEFLATE 実装済み)
├── jpeg_viewer.c - JPEG ビューワ
└── brotli_decoder.c - Brotli 解凍

NetSurf 統合:
├── cos_netsurf_png.c (6,843 行) - PNG ハンドラ
└── cos_netsurf_jpeg.c (7,151 行) - JPEG ハンドラ

課題:
- SIMD 最適化未実装
- マルチスレッド解凍未対応
- キャッシュ戦略が簡易的
```

### 4. TinyGL 状況
```
third_party/tinygl/src/
├── api.c, clip.c, light.c, matrix.c - OpenGL 互換 API
├── zbuffer.c, ztriangle.c - ソフトウェアラスタライザ
├── texture.c - テクスチャマッピング
└── tinygl_os.c - OS 抽象化層

現状: 簡易 3D ビューワレベル
課題: モデルローダー不足、シェーダ未対応、パフォーマンス低
```

### 5. TinyCC 状況
```
third_party/tinyc/
├── tcc.c (15,023 バイト) - メインエントリー
├── tccpp.c (111,976 バイト) - プリプロセッサ
├── tccgen.c (267,139 バイト) - コード生成
├── tccelf.c (138,801 バイト) - ELF 出力
├── libtcc.c (66,496 バイト) - ライブラリ API
└── x86_64-*.c - x86_64 バックエンド

✅ ソースファイルは配置済み
❌ C-OS 内からのコンパイル機能は未統合
```

---

## 目標アーキテクチャ (To-Be Architecture)

```
┌─────────────────────────────────────────────────────────────┐
│                    C-OS Browser Stack                        │
├─────────────────────────────────────────────────────────────┤
│  Application Layer                                           │
│  ┌─────────────────┐  ┌─────────────────┐                   │
│  │ gui_apps_browser│  │ TinyGL Viewer   │                   │
│  └────────┬────────┘  └────────┬────────┘                   │
├─────────────────────────────────────────────────────────────┤
│  JavaScript Engine (QuickJS + JIT)                           │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  Copy-and-Patch JIT (2MB cache, x86_64 native)      │    │
│  │  - ヒート検出 (100 回) → JIT コンパイル               │    │
│  │  - インラインキャッシング (予定)                      │    │
│  │  - 型特殊化 (予定)                                  │    │
│  └─────────────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────────────┤
│  DOM/CSSOM Layer                                             │
│  ┌─────────────────┐  ┌─────────────────┐                   │
│  │ 完全 DOM API    │  │ CSSOM 高速化     │                   │
│  │ - Element       │  │ - スタイル計算   │                   │
│  │ - Document      │  │ - キャッシュ     │                   │
│  │ - EventTarget   │  │ - リフロー最適化 │                   │
│  │ - NodeList      │  │                  │                   │
│  └────────┬────────┘  └────────┬────────┘                   │
├─────────────────────────────────────────────────────────────┤
│  Rendering Engine (NetSurf)                                  │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  cos_netsurf_render.c (最適化済)                     │    │
│  │  - DOM ツリー巡回 (libdom)                           │    │
│  │  - テキスト抽出・レイアウト                          │    │
│  │  - シリアル出力デバッグ                              │    │
│  └─────────────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────────────┤
│  Image Decoders                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │ PNG (SIMD)   │  │ JPEG (SIMD)  │  │ Brotli (MT)  │      │
│  │ - SSE2/AVX2  │  │ - SIMD 展開   │  │ - マルチコア │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
├─────────────────────────────────────────────────────────────┤
│  Network Stack                                               │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  cos_fetch_http.c (47,326 行)                         │    │
│  │  - HTTP/1.1, HTTPS (BearSSL)                         │    │
│  │  - nghttp2 (HTTP/2 準備)                              │    │
│  └─────────────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────────────┤
│  Development Tools                                           │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  TinyCC (C-OS 内蔵コンパイラ)                        │    │
│  │  - C ソース → ELF バイナリ                            │    │
│  │  - 動的リンク対応                                    │    │
│  │  - REPL 実行環境                                     │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

---

## 実装フェーズ (Implementation Phases)

### フェーズ 0: 基盤整備 (現在地 - 完了済み)
- [x] QuickJS Copy-and-Patch JIT 実装
- [x] TinyCC ソースファイル配置
- [x] 既存コードの構造把握

### フェーズ 1: TinyCC 統合 (最優先)
**目的**: C-OS 内から C コードをコンパイルできる環境構築

#### 1.1 TinyCC カーネル統合
- **ファイル**: `src/kernel/tcc_compiler.c`, `src/kernel/tcc_compiler.h`
- **機能**:
  - TinyCC の初期化・終了処理
  - ファイルシステム連携（ソース読み込み、バイナリ書き出し）
  - エラーハンドリング（シリアル出力）
  - メモリ管理（カーネルヒープ使用）

```c
// tcc_compiler.h 想定インターフェース
typedef struct {
    bool initialized;
    void* tcc_state;
    char output_path[256];
} tcc_context_t;

int tcc_init(tcc_context_t* ctx);
int tcc_compile_file(tcc_context_t* ctx, const char* src_path, const char* out_path);
int tcc_compile_memory(tcc_context_t* ctx, const char* src_code, size_t len, void** out_bin, size_t* out_size);
void tcc_cleanup(tcc_context_t* ctx);
```

#### 1.2 シェルコマンド追加
- **ファイル**: `src/shell/cmd_tcc.c`
- **コマンド**: `tcc <source.c> [output]`
- **機能**:
  - ユーザーが C ソースを指定してコンパイル
  - エラーメッセージ表示
  - 成功時、出力バイナリパスを表示

#### 1.3 サンプルプログラム
- **ファイル**: `src/assets/sample_hello_c.c`
- **内容**: Hello World プログラム
- **用途**: TinyCC 動作確認テスト

#### 1.4 検証手順
1. C-OS シェルで `tcc /sample_hello_c.c /hello` を実行
2. `/hello` バイナリが生成されることを確認
3. 生成バイナリを実行し "Hello World" 出力を確認

**工数**: 2-3 日
**リスク**: 
- メモリ制約（TinyCC は比較的小さめだが、カーネル空間での動作確認必要）
- ファイルシステム権限管理

---

### フェーズ 2: 完全 DOM API 実装
**目的**: Web サイトの JavaScript が標準 DOM API を利用可能に

#### 2.1 DOM API 階層設計
```
cos_js_dom_api.c (新規)
├── Document オブジェクト
│   ├── createElement()
│   ├── createTextNode()
│   ├── getElementById()
│   ├── getElementsByClassName()
│   ├── querySelector()
│   └── querySelectorAll()
├── Element オブジェクト
│   ├── innerHTML
│   ├── textContent
│   ├── appendChild()
│   ├── removeChild()
│   ├── setAttribute()
│   └── getAttribute()
├── Node オブジェクト
│   ├── nodeType
│   ├── nodeName
│   ├── nodeValue
│   └── parentNode
└── EventTarget オブジェクト
    ├── addEventListener()
    ├── removeEventListener()
    └── dispatchEvent()
```

#### 2.2 NetSurf libdom 連携
- **既存資産**: `cos_netsurf_render.c` の DOM 巡回コードを流用
- **バインディング**: QuickJS JSValue ↔ libdom dom_node
- **ライフサイクル**: GC 連動によるノード解放

#### 2.3 実装ファイル
- `src/third_party/quickjs/cos_js_dom_api.c` (新規，予想 2,000-3,000 行)
- `src/third_party/quickjs/cos_js_dom_api.h` (新規)
- `src/third_party/quickjs/cos_js_events.c` (新規，イベントシステム)

#### 2.4 検証ページ
- `iso/browser/dom_test.html` - DOM 操作テストページ
- `iso/browser/events_test.html` - イベントハンドラテスト

**工数**: 5-7 日
**リスク**:
- libdom と QuickJS のメモリ管理モデルの違い
- ガベージコレクションとの整合性

---

### フェーズ 3: レンダリング最適化
**目的**: NetSurf ページ表示速度の極限向上

#### 3.1 DOM 巡回キャッシュ
- **問題**: ページスクロール毎に DOM 全巡回
- **解決**: 差分更新＋結果キャッシュ
- **実装**: `cos_netsurf_render.c` 改造

```c
// 疑似コード
typedef struct {
    dom_node* node;
    uint32_t hash;
    browser_line_t lines[MAX_LINES];
    uint32_t line_count;
    uint64_t last_modified;
} render_cache_entry_t;

render_cache_entry_t* render_cache_lookup(dom_node* root);
void render_cache_update(render_cache_entry_t* entry, dom_node* root);
```

#### 3.2 並列パース
- **問題**: HTML パース→DOM 構築→レンダリングが直列
- **解決**: パイプライン並列化
- **実装**:
  - スレッド 1: HTTP 受信
  - スレッド 2: HTML パース（libhubbub）
  - スレッド 3: DOM 構築（libdom）
  - スレッド 4: レンダリング（cos_netsurf_render）

#### 3.3 CSS 選択子最適化
- **問題**: 各要素に対して全 CSS ルールを評価
- **解決**: 選択子インデックス＋ブルームフィルタ
- **実装**: `src/netsurf/cos_css_selector_cache.c` (新規)

#### 3.4 预期効果
- ページ表示時間：現行比 50-70% 短縮
- スクロール応答性：2-3 倍向上
- メモリ使用量：+10-15%（キャッシュ分）

**工数**: 4-6 日
**リスク**:
- マルチスレッド同期オーバーヘッド
- キャッシュ一貫性維持

---

### フェーズ 4: 画像デコーダ高速化
**目的**: 画像表示速度の劇的改善

#### 4.1 PNG SIMD 最適化
- **対象**: `src/apps/png_decoder.c`
- **最適化箇所**:
  - DEFLATE Huffman デコード（SSE2 命令）
  - フィルタ処理（AVX2 並列化）
  - αブレンディング（SIMD 展開）

```c
// 例：フィルタ処理の SIMD 化
#include <immintrin.h>

void png_filter_optimized_sse2(uint8_t* row, const uint8_t* prev_row, size_t width) {
    __m128i zero = _mm_setzero_si128();
    for (size_t i = 0; i < width; i += 16) {
        __m128i cur = _mm_loadu_si128((__m128i*)&row[i]);
        __m128i pred = _mm_loadu_si128((__m128i*)&prev_row[i]);
        __m128i result = _mm_add_epi8(cur, pred);
        _mm_storeu_si128((__m128i*)&row[i], result);
    }
}
```

#### 4.2 JPEG SIMD 最適化
- **対象**: `src/apps/jpeg_viewer.c`
- **最適化箇所**:
  - IDCT（整数 DCT の SIMD 化）
  - YCbCr→RGB 変換
  - サンプリング補間

#### 4.3 マルチスレッド解凍
- **対象**: `src/kernel/drivers/brotli_decoder.c`
- **方式**: メタブロック単位での並列解凍
- **実装**: 作業スレッドプール（4-8 スレッド）

#### 4.4 画像キャッシュ
- **新設**: `src/cache/image_cache.c`
- **機能**:
  - LRU キャッシュ（最大 16MB）
  - 事前フェッチ（リンク先画像をバックグラウンド取得）
  - 圧縮保持（メモリ節約のため Brotli 圧縮状態でキャッシュ）

#### 4.5 预期効果
- PNG 表示：3-5 倍高速
- JPEG 表示：4-6 倍高速
- 画像豊富ページ：全体表示時間 40-60% 短縮

**工数**: 5-7 日
**リスク**:
- CPU 機能フラグ検出（SSE2/AVX2 非対応 CPU へのフォールバック）
- アラインメント制約

---

### フェーズ 5: TinyGL 本格 3D ビューワ化
**目的**: 簡易 3D ビューワから実用的 3D エンジンへ

#### 5.1 モデルローダー追加
- **形式**: OBJ, glTF 2.0
- **実装**: `src/third_party/tinygl/src/loader_obj.c`, `loader_gltf.c`
- **機能**:
  - メッシュ読み込み
  - マテリアル解析
  - テクスチャ座標展開

#### 5.2 シェーダ風エミュレーション
- **制限**: TinyGL は固定機能パイプライン
- **工夫**:
  - 頂点変形テーブル（疑似頂点シェーダ）
  - フラグメント処理テーブル（疑似ピクセルシェーダ）
  - 法線マップ対応

#### 5.3 パフォーマンス最適化
- **Z バッファ**: 16bit→32bit フロート精度向上
- **三角形セットアップ**: SIMD 化
- **テクスチャフィルタ**: バイリニア/トライリニア補間

#### 5.4 ブラウザ統合
- **WebGL 風 API**: `cos_js_webgl_api.c` (新規)
- **使い方**:
```javascript
const gl = canvas.getContext("webgl");
const program = gl.createProgram();
// ... 3D 描画コード
```

#### 5.5 デモアプリケーション
- `iso/browser/demo_cube.html` - 回転キューブ
- `iso/browser/demo_terrain.html` - 地形表示
- `iso/browser/demo_model.html` - OBJ モデル表示

**工数**: 6-8 日
**リスク**:
- ソフトウェアラスタライザの限界（複雑なシーンは遅い）
- メモリ容量制約

---

### フェーズ 6: 統合最適化・ドキュメント
**目的**: 全体調整とユーザードキュメント整備

#### 6.1 パフォーマンス計測
- **ベンチマークスイート**: `tools/browser_benchmark.js`
- **指標**:
  - SunSpider 相当スコア
  - DOM 操作速度（要素追加/削除/変更）
  - 画像表示時間（PNG/JPEG 各サイズ）
  - ページ表示時間（サンプルサイト）

#### 6.2 ドキュメント
- `docs/BROWSER_ENHANCEMENT.md` - 本設計図の実装版
- `docs/QUICKJS_JIT_GUIDE.md` - JIT 開発者向けガイド
- `docs/DOM_API_REFERENCE.md` - 対応 DOM API 一覧

#### 6.3 チュートリアル
- `iso/browser/tutorial/index.html` - ブラウザ機能紹介
- `iso/browser/tutorial/js_intro.html` - JavaScript 入門
- `iso/browser/tutorial/3d_demo.html` - 3D 機能デモ

**工数**: 2-3 日

---

## 全体スケジュール

| フェーズ | 内容 | 工数 | 累積 |
|---------|------|------|------|
| 0 | 基盤整備（完了） | - | 0 日 |
| 1 | TinyCC 統合 | 2-3 日 | 3 日 |
| 2 | 完全 DOM API | 5-7 日 | 10 日 |
| 3 | レンダリング最適化 | 4-6 日 | 16 日 |
| 4 | 画像デコーダ高速化 | 5-7 日 | 23 日 |
| 5 | TinyGL 本格化 | 6-8 日 | 31 日 |
| 6 | 統合・ドキュメント | 2-3 日 | 34 日 |

**総工数**: 約 34 営業日（1.5 ヶ月）

---

## リスク管理

### 技術的リスク
1. **メモリ制約**: カーネル空間での大規模ライブラリ動作
   - 対策: メモリプロファイリング、段階的ロード

2. **マルチスレッド同期**: デッドロック・競合状態
   - 対策: ロック順序厳守、デッドロック検出ログ

3. **CPU 互換性**: SIMD 命令非対応環境
   - 対策: CPUID 検出、フォールバック実装

### スケジュールリスク
1. **依存関係**: NetSurf/libdom の予期せぬ挙動
   - 対策: 早期プロトタイピング、代替案準備

2. **テスト工数**: ブラウザテストの網羅性
   - 対策: 自動化テストスクリプト整備

---

## 成功基準 (Success Criteria)

### 定量的指標
- JavaScript 実行速度：SunSpider 相当で 3 倍以上
- ページ表示時間：主要サイトで 50% 短縮
- DOM 操作速度：10,000 要素追加を 100ms 以内
- 画像表示：1MB PNG を 100ms 以内に表示
- C コンパイル：1,000 行 C コードを 5 秒以内

### 定性的指標
- 現代 Web サイト（React/Vue 製）が実用レベルで動作
- C-OS 上で C コードの開発・コンパイル・実行が可能
- 3D コンテンツが滑らかに表示・操作可能
- 開発者ドキュメントが整備され、サードパーティ開発が可能

---

## 付録 A: 主要ファイル一覧（新規・改修）

### 新規ファイル
```
src/kernel/tcc_compiler.c/h
src/shell/cmd_tcc.c
src/third_party/quickjs/cos_js_dom_api.c/h
src/third_party/quickjs/cos_js_events.c/h
src/netsurf/cos_css_selector_cache.c/h
src/cache/image_cache.c/h
src/third_party/tinygl/src/loader_obj.c
src/third_party/tinygl/src/loader_gltf.c
src/third_party/quickjs/cos_js_webgl_api.c/h
```

### 改修ファイル
```
src/apps/png_decoder.c (SIMD 最適化)
src/apps/jpeg_viewer.c (SIMD 最適化)
src/kernel/drivers/brotli_decoder.c (マルチスレッド)
src/netsurf/cos_netsurf_render.c (キャッシュ追加)
src/gui/apps/browser/gui_apps_browser.c (DOM API 連携)
```

---

## 付録 B: テスト計画

### 単体テスト
- TinyCC: 各種 C 構文コンパイルテスト
- DOM API: 各メソッドの戻り値・副作用検証
- 画像デコーダ: テストパターン比較

### 結合テスト
- ブラウザ全体：サンプルページ表示
- JavaScript+DOM：インタラクティブ操作
- 3D 描画：フレームレート測定

### パフォーマンステスト
- ベンチマークスイート実行
- メモリプロファイリング
- 長時間動作テスト（メモリリーク検出）

---

## 付録 C: 参照ドキュメント

- QuickJS 公式: https://bellard.org/quickjs/
- NetSurf libdom: https://www.netsurf-browser.org/
- TinyCC: https://bellard.org/tcc/
- TinyGL: https://github.com/fabricebellard/tinygl
- MDN DOM API: https://developer.mozilla.org/ja/docs/Web/API/Document_Object_Model

---

**文書バージョン**: 1.0  
**作成日**: 2024  
**最終更新**: 2024  
**著者**: C-OS 開発チーム
