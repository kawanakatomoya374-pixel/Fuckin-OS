# Python Code to C Alpha0.6 — 集合拡張の設計・検証記録

## 目的

本更新では、`Python Code to C Alpha0.6` に **setリテラル・`set()`・set内包表記**を追加した。実装は既存の辞書ハッシュ表を再利用しつつ、集合の意味に必要な「キーだけを保持する」「重複を除外する」「イテラブルとして要素を返す」という契約を明確にした。freestandingコアへホスト依存の機構は追加していない。

| 領域 | 実装内容 | 生成コードで使うAPI |
|---|---|---|
| リテラル | `{1, 2, 3}`を`AST_SET`として解析し、重複を構築時に排除 | `p2c_set_from_array()` |
| 組込み関数 | `set()`と`set(iterable)`を変換 | `p2c_set_new()`、`p2c_builtin_set()` |
| 内包表記 | `{expr for ... if ...}`。複数`for`と複数`if`を既存の生成器再帰へ統合 | `p2c_set_new()`、`p2c_set_add()` |
| 基本操作 | `len`、`in`/`not in`、真偽値、`for`、`iter`/`next`、`type`、`isinstance` | `p2c_len()`、`p2c_iter_at()`、`p2c_obj_is_set()` |
| 文字列表現 | 非空集合は`{...}`、空集合はPythonと同じ`set()` | `p2c_print_raw_ex()`、`p2c_obj_to_buf_ex()` |

## 順序と表示

Pythonのsetは順序を仕様として保証しない。一方、Alpha0.6の内部実装はテスト再現性と小規模OS上での観測しやすさを優先し、辞書と共有する挿入順連鎖を使用する。このため集合の走査と表示は**挿入順で決定的**であるが、アプリケーションはその順序に依存してはならない。

> CPythonとの差分回帰では、順序が意味を持たない集合値を `sorted()` で正規化して比較する。これにより、集合の意味論を検証しながら処理系固有のハッシュ走査順に依存しない。

## frozenset の互換フォールバック

`frozenset()`および`frozenset(iterable)`は、変換を停止させず実行可能な出力を得るため、**可変setへフォールバック**する。構築・重複排除・反復・長さ・包含判定は利用できる。ただし不変性、`frozenset`固有の型名、ハッシュ可能性、辞書キー利用の契約は実装しない。

| 使用例 | Alpha0.6の挙動 | 利用可否 |
|---|---|---|
| `sorted(frozenset(xs))` | setとして要素を構築・反復 | 利用可能 |
| `x in frozenset(xs)` | setの包含判定 | 利用可能 |
| `len(frozenset(xs))` | setの要素数 | 利用可能 |
| `type(frozenset(xs))` | `<class 'set'>` | 互換差分あり |
| 辞書キーまたは`hash(frozenset(xs))` | 不変・ハッシュ可能性を要求 | 非対応 |

## 移植性

コンパイラ本体とランタイムはC11でビルドでき、`make freestanding`は標準Cライブラリに依存しない静的コアを生成する。setの通常リテラル、`set()`、反復、長さ、表示はいずれもこのコアの範囲に留まる。

set内包表記は既存のlist内包表記と同様、生成Cの式位置で一時コンテナを構築するためGNU statement expressionを使用する。そのため`--c11`の厳格生成モードでは内包表記を明示的に拒否する。これはホスト・freestandingコンパイラ本体のC11適合性とは別の、生成Cバックエンドの制約である。

## 回帰と品質検証

`tests/set_regression.sh`は以下をCPythonと差分比較する。スクリプトは`make test`から実行される。

| テスト | 確認内容 |
|---|---|
| `tests/set_alpha06.py` | リテラル、重複排除、空集合、`len`、包含、真偽値、型、`isinstance`、`set(iterable)`、反復 |
| `tests/set_comprehension_alpha06.py` | 条件付きset内包表記、複数生成器、重複排除 |
| `tests/frozenset_fallback_alpha06.py` | 構築・反復・包含・長さのフォールバック経路 |

今回の品質確認では、Hosted CLI、ホストGUI、freestanding静的ライブラリのビルド、`make test`、およびset内包表記生成Cに対するAddressSanitizerとUndefinedBehaviorSanitizerの実行を完了した。Sanitizer実行はリーク検出を無効化し、追加した集合実行経路の不正メモリアクセスおよび未定義動作を対象にした。
