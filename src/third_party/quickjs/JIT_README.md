# QuickJS Copy-and-Patch JIT Implementation for C-OS

## 概要

このディレクトリに配置された `quickjs_jit.c` と `quickjs_jit.h` は、C-OS のブラウザにおける JavaScript 実行速度を劇的に向上させるための Copy-and-Patch JIT コンパイラの実装です。

## 技術的特徴

### Copy-and-Patch JIT アプローチ

従来のトレースJITやメソッドJITとは異なり、Copy-and-Patch JITは以下の利点があります：

1. **軽量な実装**: 各オペコードに対応するネイティブコードの「パッチ」を事前に用意し、実行時にコピーしてリンクするだけ
2. **低いオーバーヘッド**: 複雑な最適化を行わないため、コンパイル時間が極めて短い
3. **予測可能な動作**: メモリ使用量とコンパイル時間が bytecode サイズに比例する

### 実装の詳細

#### コードキャッシュ
- 2MB の固定サイズコードキャッシュ
- 16 バイトアラインメントで x86_64 ネイティブコードを格納
- LRU フラッシュ対応（将来的に拡張可能）

#### ヒート検出
- 各関数の呼び出し回数をカウント
- 100 回（`JIT_HOT_THRESHOLD`）を超えると JIT コンパイル対象
- 再帰的コンパイル防止のためのガード

#### サポートされるオペコード
現在実装されている主要オペコード：
- `OP_nop`: No operation
- `OP_push_i32`: 32 ビット整数をスタックにプッシュ
- `OP_pop`: スタックからポップ
- `OP_add`: 加算
- `OP_sub`: 減算
- `OP_mul`: 乗算
- `OP_div`: 除算（ランタイムヘルパー呼び出し）
- `OP_ret`: リターン

### 統合方法

QuickJS の実行ループに統合するには、`quickjs.c` の関数実行部分を変更します：

```c
// quickjs.c 内の関数実行部分に追加
#include "quickjs_jit.h"

static JSValue js_call_bytecode_function(JSContext *ctx, JSFunctionBytecode *b, ...)
{
    // JIT を試行
    void *native_code = jit_try_execute(b);
    
    if (native_code) {
        // JIT コンパイル済みコードを実行
        typedef JSValue (*native_func_t)(void);
        native_func_t native_fn = (native_func_t)native_code;
        return native_fn();
    }
    
    // フォールバック：通常のインタープリタ実行
    // ... 既存のインタープリタコード ...
}
```

## パフォーマンス期待値

### ベンチマーク目標

| シナリオ | インタープリタ | JIT 有効時 | 改善率 |
|---------|--------------|-----------|--------|
| 単純な数値計算 | 100% | 300-500% | 3-5x |
| DOM 操作ループ | 100% | 200-300% | 2-3x |
| JSON パース | 100% | 150-200% | 1.5-2x |
| イベントハンドラ | 100% | 250-400% | 2.5-4x |

### メモリ使用量

- コードキャッシュ: 2MB（固定）
- 関数エントリ: 1024 エントリ × 48 バイト ≈ 48KB
- 総オーバーヘッド: 約 2.5MB

## 今後の拡張

### フェーズ 2: 高度な最適化

1. **インラインキャッシング**: プロパティアクセスの高速化
2. **型特殊化**: 数値演算の型チェック省略
3. **ループ最適化**: ホットループの検出と最適化

### フェーズ 3: 完全な Web ブラウザ統合

1. **DOM API の完全実装**: すべての標準 DOM メソッドをネイティブ化
2. **CSSOM の高速化**: スタイル計算のキャッシュと最適化
3. **レイアウトエンジンの改良**: NetSurf のレンダリングパイプライン最適化

## ネットワークスタックとの連携

HTTP 通信によるスクリプト取得後、JIT コンパイルが非同期で行われるため、ページ表示までの時間が大幅に短縮されます：

```
[HTTP 応答] → [HTML パース] → [JS ロード] → [JIT コンパイル] → [実行]
                                                    ↓
                                         （次回からはキャッシュ使用）
```

## デバッグ機能

シリアル出力による JIT 統計情報の取得：

```c
uint32_t compiled, jit_exec, interp_exec;
jit_get_stats(&compiled, &jit_exec, &interp_exec);
serial_puts("JIT Stats: compiled=");
serial_putdec(compiled);
serial_puts(", jit_exec=");
serial_putdec(jit_exec);
serial_puts(", interp_exec=");
serial_putdec(interp_exec);
serial_puts("\n");
```

## セキュリティ考慮事項

1. **W^X (Write XOR Execute)**: 本番環境では、コード生成後にメモリ領域を実行可能にする必要がある
2. **入力検証**: バイトコードの整合性チェックを強化
3. **サンドボックス**: JIT 生成コードも QuickJS のセキュリティ境界内で実行

## ライセンス

QuickJS のライセンス（MIT）に従います。
