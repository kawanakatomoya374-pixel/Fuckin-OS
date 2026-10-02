# Python Code to C Alpha0.6 — 客観監査・改善報告

> **結論:** Alpha0.6は、Python全仕様を装う処理系ではなく、明示されたPythonサブセットをCへ変換する実用基盤です。今回の監査では、従来「対応」とされていた構文の意味論差、GNU拡張の混入、GC/GUI境界、評価回数、コンパイラ側の所有権を検査し、危険な黙殺を診断へ置換しました。特に、短絡評価、loop-else、with例外経路、複合代入、TLS、GCルート容量、GUIコマンドバッファ、bit演算、AST・シンボル・先読みトークンの主要な解放漏れを修正しています。

## 監査方法

Hosted CLI、生成C、freestandingコア、GUIコマンドバッファ、GCを対象に、通常回帰、60ケース複雑コーパス、CPython出力差分、厳格ISO C11、GUI/GC C回帰、ASan/UBSan、低いGCルート容量でのビルドを実施しました。Python意味論の比較には言語リファレンスを、GNU拡張の識別にはGCCの一次資料を用いました。[1] [2] [3] [4]

| 評価領域 | 改修後の状態 | 判定 |
|---|---|---|
| 基本変換 | 既存スモーク、60ケース、starred unpack、メソッド既定引数、`del`属性/添字、bit演算を回帰化 | 継続可能 |
| Python意味論 | `and`/`or`の短絡とオペランド返却、for/while-else、with例外抑止、複合代入の評価回数を修正 | 改善済み |
| finally | finally内の`return`、`break`、`continue`を対応（finally本体を実行し、進行中の例外・`return`を上書きする）。クリーンアップフレームが「実行中のfinally」を認識し、再実行せず例外フレームだけ復元する | 実装済み（C500–C505） |
| ISO C11 | コンパイラ本体は厳格C11で構築可能。`--c11`はGNU拡張を要する生成構文を明示拒否 | サブセット保証 |
| GC/メモリ | ルート容量を`P2C_GC_ROOT_CAPACITY`で設定可能化し、超過を黙殺せず停止。主要なAST・トークン・シンボル・生成器マップの解放を追加 | 改善済み、残課題あり |
| 自作OS移植 | freestanding静的ライブラリとプラットフォーム抽象、GUIコマンドバッファを維持 | 継続可能 |
| GUI | テキスト領域をコマンド確保後に消費する順序へ変更し、満杯時の領域浪費を防止 | 改善済み |
| テスト | 通常回帰、60ケース、監査回帰、GC/GUI、C11受理・拒否、Sanitizerを追加・実行 | 強化済み |

## 解決済みまたは安全側へ変更した課題

| ID | 領域 | 監査で再現した問題 | 実施した対応 | 現在の保証 |
|---|---|---|---|---|
| S-001 | boolean | `and`/`or`が真偽値化し、右辺を常に評価した | GNUバックエンドで一時変数を使う短絡ブロックへ変換 | 評価順・返却オペランドを回帰で確認 |
| S-002 | loop | for/while-elseがbreak後にも実行され得た | ループ固有のbreakフラグを生成し、ネスト時に復元 | 正常終了時のみelseを実行 |
| S-003 | finally | finally内の制御移動を誤変換した | 当該パターンを意味解析段階で拒否 | 誤ったCを生成しない |
| S-004 | with | 例外時に`__exit__`を呼ばず、抑止も無視した | 単一itemのwithを例外フレームで生成し、正常/例外経路を分離 | `__exit__`の真値による抑止を確認 |
| C-002 | 評価回数 | 属性・添字への複合代入で対象式を複数評価した | 対象、キー、右辺を一時変数へ一度だけ評価 | 副作用付き回帰で確認 |
| P-002 | 移植性 | `__thread`とGNU形式可変長マクロがC11非適合だった | TLS抽象化と標準形式のログ呼出しへ置換 | コンパイラ本体は厳格C11で構築 |
| G-001 | GC | ルート上限を超えると登録が黙って失われた | 容量を設定可能化し、超過時はプラットフォーム停止 | サイレントな未回収を防止 |
| GUI-001 | GUI | コマンド満杯時にもテキストアリーナを消費した | コマンド確保を先行させる順序に変更 | 満杯時の不変条件をC回帰化 |
| M-001 | メモリ | 変換器がAST、内包表記、先読みトークン、シンボル、生成器名マップを解放しなかった | 所有権に沿った解放処理を追加 | 主要経路のリーク量を大幅に削減 |

## 構文対応の拡張

今回追加または回帰で明示化した構文は、starred unpack代入、属性・添字の`del`、`&`、`|`、`^`、`~`、`<<`、`>>`と、それらの複合代入です。シフトはAlpha0.6の64-bit整数表現を超える左シフトを`OverflowError`として扱い、Cの未定義動作を避けます。

| 構文 | 対応範囲 | 制約 |
|---|---|---|
| `a, *mid, z = seq` | 先頭・中間・末尾のstarredターゲット | 単一スターターゲット |
| `del obj.attr` | 属性マップから削除 | Pythonのdescriptor完全互換ではない |
| `del seq[index]` | list/dictの添字削除 | スライス削除・名前削除は未対応 |
| bit演算 | `&`、`|`、`^`、`~`、左右シフトと複合代入 | 整数は符号付き64-bit範囲 |
| `with item as name` | `__enter__`、`__exit__`、例外抑止 | itemは1個、束縛先は単純名、非局所制御移動は拒否 |

## ISO C11とGCC依存の整理

標準の変換モードは既存互換性のためGNU拡張を利用し得ます。一方、`--c11`は、list内包表記、lambda、`and`/`or`、`next(iterator, default)`、式としての`setattr`/`delattr`など、GNU statement expressionまたは入れ子関数を要する構文を**成功扱いにせず**変換エラーとして返します。対応サブセットは`-std=c11 -pedantic-errors -Wall -Wextra -Werror`で生成Cをコンパイルする回帰で確認しています。[3] [4]

> `--c11`は「すべてのPython構文をISO C11へ下ろす」機能ではありません。移植可能な対象だけを明示的に通す安全境界です。完全なISO C11バックエンドには、式の文レベルhoist、クロージャ環境構造体、内包表記の専用関数化が必要です。

## 残る危険箇所と次期優先順位

今回の監査で黙った誤変換は減らしましたが、完全なPython互換性を主張できる段階ではありません。Sanitizer検査では主要な所有権漏れを修正した後も、複雑コーパス上で構文解析のエラー経路・一部中間構造に残るリークが確認されています。そのため、長時間稼働するコンパイラサービスとして使う場合は、次期改修でarena allocatorまたは一元的なAST所有権コンテナを導入すべきです。

| 優先度 | 残課題 | 必要な改修 |
|---:|---|---|
| 1 | finally/withをまたぐreturn・break・continue | 制御フロー理由を持つアンワインドIR |
| 2 | 完全ISO C11バックエンド | 式hoist、lambda/内包表記のトップレベル関数化、クロージャ環境 |
| 3 | Sanitizer残リーク | parse失敗時の部分AST回収と中間ベクタ所有権の一元化 |
| 4 | set、dict/set内包表記、dict unpack | setオブジェクトと評価順を備えたランタイム |
| 5 | 例外型・traceback | 例外クラス階層、cause、context、OS向けpanic契約 |
| 6 | 完全なGC | 精密ルート、複数スレッド停止、可変根集合、増分/世代別収集 |

## 受入基準

出荷対象は、通常の`make test`、60ケース複雑コーパス、GC/GUI C回帰、freestanding静的ライブラリ、GUIビルド、`--c11`の受理・拒否回帰を通過することを必須とします。Sanitizerは未定義動作の検出に使用し、残るリークは本書に記録した既知制約として追跡します。自作OSでは、`P2C_GC_ROOT_CAPACITY`、プラットフォームフック、C11モードを設定した上で対象サブセットを固定してください。

## 参考文献

[1]: https://docs.python.org/3/reference/expressions.html "Python Language Reference — Expressions"
[2]: https://docs.python.org/3/reference/compound_stmts.html "Python Language Reference — Compound Statements"
[3]: https://gcc.gnu.org/onlinedocs/gcc/Statement-Exprs.html "GCC — Statements and Declarations in Expressions"
[4]: https://gcc.gnu.org/onlinedocs/gcc/Nested-Functions.html "GCC — Nested Functions"
