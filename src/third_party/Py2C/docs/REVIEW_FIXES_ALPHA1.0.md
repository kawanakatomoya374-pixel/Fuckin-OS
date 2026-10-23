# Python Code to C Alpha1.0 — レビュー修正の記録

この文書は、コードレビューに基づく修正の内容と、その検証結果を記録します。
「直したこと」と「検証したこと」と「まだ残っていること」を分けて書いています。

## 1. 検証環境と結果

| 項目 | 内容 |
|---|---|
| コンパイラ | GCC 13.3.0（Ubuntu 24.04）、TinyCC 0.9.27 |
| Python（参照） | CPython 3.12.3 |
| 検証の手順 | クリーンなコピーでビルドし、`make test` を実行 |
| `make test` | 終了コード 0 |
| `make tcc-full` | 終了コード 0（TinyCC だけでビルド・C99 コーパス・freestanding・CLI ELF・HobbyOS ELF） |
| 差分コーパス | `conformance_regression_ok: 925 semantic assertions passed`（CPython との差分） |
| スレッド競合 | ThreadSanitizer で `test_threads` の競合 0 件 |
| スレッド安定性 | 最適化ビルドで 25 回連続成功（修正前は 25 回中 2 回失敗） |

検証の再現:

```sh
make -j8 all gui && make test        # GCC/Clang 側の全テスト
make tcc-full                        # TinyCC だけの全工程
make check-single-header check-unicode-tables
```

## 2. 修正の内容

### 2-1. ビルドシステム

- `make tcc-full` を追加（ヘルプには以前から記載があったが、ルールが存在しなかった）。
- `-c` に 2 つのソースと 1 つの `-o` を書いていたコンパイル行を、ソースごとに分割（埋め込み・スタック予算・TinyCC freestanding の 3 箇所）。
- 書式指定子の個数の誤り（`main.c`、`gui_main.c`）を修正。TinyCC は書式を検査しないため、GCC でしか見つからなかった。
- 新ターゲット: `unicode-tables`、`check-unicode-tables`、`test-gui-web-security`、`test-c2py`。`make test` の連鎖、ヘルプ、`.PHONY` に反映。
- テストスクリプトで、2 つのソースパスを 1 つの引用符に入れていた誤り（`container_fuzz_regression.sh`、`audit_regression.sh`）を修正。

### 2-2. 評価順（コード生成）

- 引数・辞書・リスト・タプルの要素は、Python の順（左から右）で 1 回ずつ評価する。
- 副作用のある部分式が 2 つ以上、または副作用のある部分式と可変変数の読み出しが同じ式にある場合だけ、GNU 文式で順序を固定する。変数の読み出しだけの引数列は、そのまま書く。
- 星引数の式は、展開のたびに再評価されていた不具合を直した。
- `--c11`（厳格 ISO C11）では、文式が使えないため、順序の固定が必要な式を診断して拒否する。

### 2-3. クロージャ（遅延束縛）

- 入れ子の関数・ラムダ・ジェネレータ式が参照する外側の局所は、共有セルにする。後からの再代入や、ループ変数の最終値が見える（CPython と同じ）。
- モジュール直下のラムダや入れ子の `def` は、モジュール変数を捕捉せず、生きた大域変数を読む。
- メソッドの本体でも、同じ規則を使う。
- 内包表記の対象変数が、要素の中のラムダから参照される場合は、1 つのセルを全反復で共有する。

### 2-4. 名前の衝突（`-Wshadow`）

- 関数の局所や引数が、モジュール変数と同名のとき、局所の C 名だけを `p2c_loc_` で変える。
- 引数は、宣言側と参照側で同じ規則を使う（宣言の場所に依存しない）。
- 関数の中の局所は、モジュール変数と同名でも局所として扱う（セル化の判定も同じ）。

### 2-5. frozenset

- `frozenset` を型として実装（型名、repr、`isinstance`、ハッシュ、変更系メソッドは `AttributeError`）。
- 集合の `in` 判定を、クラスの同一性ではなく型の種類で分岐するように直した。

### 2-6. Unicode

- 判定（`isalpha`、`isdecimal`、`isdigit`、`isnumeric`、`isspace`、`isprintable`、`isupper`、`islower`、`istitle`、`isidentifier`）と、大小変換（`lower`、`upper`、`casefold`、`capitalize`、`title`、`swapcase`）を、CPython の結果から生成した表で行う。
- 表は `tools/gen_unicode_tables.py` が生成する（`make unicode-tables`）。鮮度は `make check-unicode-tables` で検査する。
- 最終シグマの規則、タイトルケースの規則を CPython と同じにした。
- 生成元は CPython 3.12（Unicode 15.0）。3.13（Unicode 15.1）で生成し直すと、対象の版と完全に一致する。

### 2-7. スレッドとプロセス共有の状態

- 統計フラグと統計カウンタを、ランタイムのインスタンス単位に移した。
- pygame のクラスと時計カウンタを、インスタンス単位の置き場（`p2c_runtime_module_slot` / `p2c_runtime_module_counter`）に移した。登録の前に空にするので、前のインスタンスの状態を引き継がない。
- 小整数（-5〜256）は、実行時に初期化せず、コンパイル時に初期化された不変オブジェクトにした。GC のマークと参照カウントの対象から外す。
- 上記の修正は、ThreadSanitizer が `test_threads` で検出した競合に対するものである。ランタイム全体の監査ではない（§4 を参照）。

### 2-8. c2py（C→Python 変換器）

- `typedef enum { ... } Name;` の宣言子名（`Name`）が、式文として出力されていた不具合を直した。
- 変換できなかった構文（struct、typedef、enum、union）を、末尾の `# --- c2py notes` 節に記録するようにした（以前は記録だけして捨てていた）。
- 構造検査 `make test-c2py` を追加（10 件）。

### 2-9. HobbyOS と組み込み

- 組込み ELF のロードアドレス（`HOBBYOS_LOAD_ADDR`）が `--defsym` で渡されること、TinyCC の制約を仕様書（`docs/spec/HOBBY_OS_EMBEDDING_ALPHA1.0.md` §9-1）に記載。
- 古い版表記（Alpha0.6）と、存在しないテストファイル名を、ビルド手順書で修正。
- バレットメタルの例（`examples/baremetal/baremetal_hello.py`）は、`--c11` で生成できる形にした。出力は変わらない。

### 2-10. Web GUI と HTTP サーバー（前の作業）

- Web GUI: 起動ごとのトークン、`Host` と `Origin` の検査、資源制限、`fork`/`execvp` による実行。検査は `tests/gui_web_security_alpha10.py`（21 項目）。
- HTTP サーバー: 接続ごとに `fork`、受信期限、`Content-Length` の検査、セキュリティ用の応答ヘッダ。

## 3. 参照の検証

上の修正のうち、次の振る舞いは CPython と突き合わせて一致を確認しました（回帰テストとして登録済み）。

- 評価順: `tests/eval_order_alpha10.py`（13 件）
- クロージャ: `tests/closure_capture_alpha10.py`（12 件）
- frozenset: `tests/frozenset_alpha10.py`（20 件）
- Unicode: `tests/unicode_semantics_alpha10.py`（12 件）
- set（既存）: `tests/set_alpha10.py` など

## 4. 残っていること（明示的な制限）

1. **`except ... as 名前` の削除**: CPython は節の終わりに名前を削除しますが、P2C では残ります。
2. **Unicode の版**: 生成元が Python 3.12（Unicode 15.0）。Python 3.13 の完全一致には、3.13 で `make unicode-tables` を実行する必要があります。
3. **最終シグマの `case_ignorable`**: CPython が公開していないため、一般カテゴリと既知の記号からの近似です。最終シグマの規則にだけ影響します。
4. **既定出力は GNU 文式を使う**: 評価順の固定に使います。ISO C11 だけを対象にする場合は `--c11` を使い、固定が必要な式は診断で拒否されます（書き直しが必要）。
5. **内包表記の対象変数と `-Wshadow`**: 内包表記の対象変数が、モジュール変数と同名の場合の `-Wshadow` 警告は、今回の検証の対象外です（局所の改名規則は、関数の局所と引数に適用されています）。
6. **スレッド対応の範囲**: `test_threads` が通ること、および ThreadSanitizer で競合 0 件であることは確認済みです。ランタイムの共有状態の全面的な監査は行っていません。インスタンスごとの分離は、既定インスタンスを 1 つのスレッドだけで使う前提の部分があります。
7. **c2py の範囲**: 構造体、typedef、列挙型は変換せず、末尾の notes に記録します。c2py は限定的な変換器です。
8. **`tcc-full` の TinyCC 制約**: スレッド別の回帰（`test-threads`）は、TinyCC が `P2C_THREAD_LOCAL` を持たないため対象外です。

## 5. 修正の場所（主なファイル）

| 領域 | ファイル |
|---|---|
| コード生成（評価順、クロージャ、名前） | `src/codegen/python_code_to_c_codegen.c` |
| ランタイム（Unicode、統計、小整数、モジュール置き場） | `src/runtime/python_code_to_c_runtime.c`、`include/runtime/python_code_to_c_runtime.h` |
| Unicode 表（生成物） | `tools/gen_unicode_tables.py`、`src/common/python_code_to_c_unicode_tables.c`、`include/common/python_code_to_c_unicode.h` |
| pygame | `src/modules/python_code_to_c_pygame.c` |
| c2py | `src/tools/c2py.c`、`tests/test_c2py_alpha10.c` |
| ビルド | `Makefile`、`tools/generate_single_header.sh` |
| 仕様・手順 | `docs/spec/HOBBY_OS_EMBEDDING_ALPHA1.0.md`、`docs/build/BUILD_AND_LAUNCH_ALPHA1.0.md`、`README.md` |
