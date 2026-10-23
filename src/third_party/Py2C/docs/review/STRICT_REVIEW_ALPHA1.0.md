# Python Code to C Alpha1.0 — 厳格レビュー報告書

## 結論

Alpha1.0では、Alpha0.3の厳格レビュー基準を引き継ぎ、機能拡張後に**314件のCPython差分アサーション**、sequence・mapping・as・star・OR・guard・mapping `**rest`、Python 3.13型パラメータ・soft keyword `type`文の型消去C11検証、位置専用引数、`yield from`、`raise from`の回帰、UTF-8単一Unicodeスカラー値の`ord`/`chr`、符号安全な`bin`/`oct`/`hex`、開始値付き`sum`の回帰、状態機械ジェネレータ・協調async直接C回帰、静的ヒープ・platform write・GC・awaitを通すベアメタル実行ハーネス、変換済みベアメタルPython例のfreestanding C11コンパイル、Hosted・厳格C11・freestanding単一ヘッダー試験、移植性診断、GUI/GCテスト、freestanding静的ライブラリ構築、make起動経路、および再現可能なdict/set差分ファジングを実行しました。GCCおよびClangの双方で、Hosted CLI、GUI、freestandingコア、単一ヘッダーを警告即エラーの条件で構築し、全回帰を完走しています。さらに、主要10モジュールに対するClang静的解析は診断0件でした。

| 監査軸 | 判定 | 証跡 |
|---|---|---|
| CPython差分 | 合格 | C01–C314の出力完全一致 |
| GCC品質ゲート | 合格 | `make CC=gcc full-build`、`make CC=gcc test` |
| Clang品質ゲート | 合格 | `make CC=clang full-build`、`make CC=clang test` |
| 単一ヘッダー | 合格 | GCC/Clangで`make test-single-header`、Clang単独の`test-single-header-c11`と`test-single-header-freestanding`を実行 |
| freestanding | 合格 | `freestanding-c11.mk`による静的ライブラリ生成、platform中核のHosted allocator・出力・時刻の未解決参照なし |
| 静的解析 | 合格 | common、platform中核、Hostedアダプタ、runtime、parser、AST、AST dump、lexer、codegen、semanticの10モジュールでClang診断0件 |
| 決定的ファジング | 合格 | 既定3 seed × 48操作、および追加8 seed × 64操作でdict/setのCPython差分一致。失敗時は最小prefixを保存 |
| 生成Cの移植性 | 合格 | `match/case`・bare raise・辞書マージ・高次dict/set・for starred unpack・ジェネレータ・協調async・`print`キーワード引数を含む経路を警告即エラーで検証 |
| ベアメタル | 合格 | 静的ヒープ・UART相当出力・tick・GC・協調awaitの実行ハーネス、および実際に変換したPython例のfreestanding C11オブジェクト化 |
| 文字コード・基数変換 | 合格 | UTF-8 `ord`/`chr`、負値を含む`bin`/`oct`/`hex`、開始値付き`sum`をC288–C299・単一ヘッダーで検証 |
| Python 3.13構文 | 合格 | 型パラメータ既定値、TypeVarTuple、ParamSpec、soft keyword `type`文を型消去して厳格C11へ変換。`/`、`yield from`、`raise from`はC300–C304でCPython差分確認 |

## Alpha1.0で発見・修正した事項

| ID | 重大度 | 発見内容 | 修正 | 回帰確認 |
|---|---|---|---|---|
| A04-01 | 高 | 代入式で導入される名前が条件・内包表記などでC事前宣言されず、未宣言C変数となる可能性 | ASTを再帰走査する事前宣言経路を追加 | C175–C178 |
| A04-02 | 高 | `match/case`のsubjectを各caseで再評価すると副作用がCPythonと異なる | subjectを一時オブジェクトへ一度だけ評価し、順次case判定へ変換 | C190–C194 |
| A04-03 | 高 | nested tryが同名のC例外フレームを生成し、`-Wshadow -Werror`で失敗 | tryごとに例外フレーム、jump、処理済み、保存例外の識別子を一意化 | C198–C199、GCC/Clang全回帰 |
| A04-04 | 高 | bare raiseが`longjmp`後に古いアクティブ例外を残し、後続のハンドラ外raiseを誤再送出 | 再送出前にスレッドローカル例外状態を明示的にクリア | C198–C199 |
| A04-05 | 中 | 辞書マージが集合・整数の`|`分派と衝突する可能性 | dict同士のみを順序保持・右優先でマージする型分派を追加 | C195–C197 |
| A04-06 | 中 | 新規swapcaseがfreestanding構成に存在しない`islower`/`isupper`へ依存 | ASCII範囲比較へ置換し、標準ライブラリ未提供環境への依存を除去 | GCC freestanding一括構築 |
| A04-07 | 中 | ユーザー変数`index`がホストCの外部関数名と衝突 | 予約名マングリング対象を拡張 | 代入式回帰、GCC/Clang全回帰 |
| A04-08 | 低 | 単一ヘッダー自己テストが旧機能だけを確認していた | `:=`、match guard、dictマージを入力する統合テストへ拡張 | `make test-single-header` |
| A04-09 | 高 | forターゲットのstarred unpackが未実装で、通常の代入unpackとループ束縛の対応範囲が不一致 | パーサがforターゲットのstarred要素を受理し、コード生成が残余list、最小arity検査、ループごとに一意な一時変数を出力 | C204–C207、GCC/Clang全回帰 |
| A04-10 | 中 | `print`が位置引数だけで、`sep`/`end`、`None`既定化、型不正の例外契約を満たさなかった | freestanding互換の`p2c_print_multi_opts()`とキーワード分派を追加 | C208–C211、単一ヘッダー、GCC/Clang全回帰 |
| A04-11 | 中 | 新機能の単一ヘッダー収録が構文解析だけに留まると、配布物と通常ビルドの乖離を検出できない | for starred unpackと`print(sep=..., end=...)`を自己完結変換テストに追加 | `make test-single-header` |
| A04-12 | 中 | 文字列検索では`find`と異なり未検出を例外にするAPI、置換では回数制限と空文字列境界が未実装 | `str.index`/`str.rindex`の`ValueError`経路と、`str.replace(old, new, count)`の回数・空文字列処理を実装 | C212–C220、GCC/Clang全回帰 |
| A04-13 | 中 | `list.index`が第1引数だけで、開始・終了境界や負境界を持つPythonコードを正しく変換できなかった | `list.index(value, start, stop)`の範囲正規化を実装 | C221–C224、GCC/Clang全回帰 |
| A04-14 | 高 | 単一ヘッダーの自己完結性が通常の警告設定のみで、GCCなしの厳格ISO C11経路として明示されていなかった | `test-single-header-c11`を追加し、Clangだけで`-pedantic-errors`自己完結ビルドを実行 | Clang全回帰 |
| A04-15 | 中 | 文字列検索が全体文字列だけを対象とし、start/stopの負境界・空部分文字列・後方検索でCPythonとの差異を生んだ | 共通境界正規化と前後探索を導入し、`find`/`index`/`rfind`/`rindex`/`count`へ適用 | C225–C234、GCC/Clang全回帰 |
| A04-16 | 中 | `str.split`が回数制限を無視し、明示区切りと空白区切りの残余文字列規則を満たさなかった | `split(sep, maxsplit)`の0・負・正回数と空白分割の残余処理を実装 | C235–C240、GCC/Clang全回帰 |
| A04-17 | 高 | `print`等の互換出力が登録済み`P2C_Platform.write`ではなくHosted固定出力へ向かい、自作OSアダプタへ届かなかった | 互換出力APIを現在のプラットフォームの`write`コールバックへ一元接続し、直接C回帰を追加 | `make test-platform-adapter` |
| A04-18 | 高 | freestanding単一ヘッダーは通常ヘッダーのインクルード順でHosted標準ヘッダーと最小C関数が衝突し、実装部を翻訳できなかった | 標準ヘッダーをHosted条件化し、最小`isdigit`互換とfreestanding単一ヘッダー検査を追加 | GCC/Clang `test-single-header-freestanding` |
| A04-19 | 高 | `\r`、`\v`、`\f`を含むPython文字列リテラルが生成Cでは未エスケープとなり、C構文破壊または意味喪失を起こした | lexerとC文字列生成を拡張し、改行系・制御文字を正しいPython値とCエスケープへ変換 | C257–C268、GCC/Clang全回帰 |
| A04-20 | 中 | list/tuple内にある制御文字列の表示がPython reprでなく生の改行を出力し、CPython差分が崩れた | 直接出力・バッファ出力の双方で文字列reprをエスケープ | C257–C268 |
| A04-21 | 中 | ビルド・起動・クロスC11構築の操作がmakeターゲットとして一貫しておらず、OS統合で変数伝播を誤る余地があった | `check-tools`、`help`、`run`、`run-gui`、`print-config`、明示的なCC/AR/ABIフラグ伝播を追加 | GCC/Clang make起動検証 |
| A04-22 | 高 | `dict.fromkeys`、pair iterable `dict.update`、setの包含関係・`pop`が未対応で、複数操作が連続するコンテナ状態を固定回帰だけでは十分に探索できなかった | 高次メソッド分派、C275–C282、固定seedのCPython差分ファズ生成器、最小失敗prefix保存を追加 | GCC/Clang全回帰、既定144操作・追加512操作ファジング |
| A04-23 | 高 | generatorとasync/awaitが明示診断のみで、局所状態・例外・GC・freestandingの境界をまたぐ実行モデルが存在しなかった | `OBJ_ITERATOR`を状態機械へ拡張し、GC到達性、`next()`、汎用for、FIFO協調実行、`asyncio.run`変換、C283–C287を追加 | GCC/Clang全回帰、ジェネレータ・async直接C回帰 |
| A04-24 | 高 | 自作OS向け手順が静的アーカイブ構築までで、静的ヒープ・出力・GC・協調awaitを同一条件で実証していなかった | C11ベアメタルアダプタ、起動入口、実行ハーネス、クロスコンパイル用テンプレート、生成済みPython例のfreestanding検査を追加 | `test-baremetal-runtime`、`test-baremetal-build`、`test-baremetal-generated` |
| A05-01 | 中 | 文字コード変換と基数表記が未接続で、`sum()`は引数数不正時に安全な例外を保証していなかった | UTF-8単一スカラー値の`ord`/`chr`、符号安全な`bin`/`oct`/`hex`、`sum`の1–2引数検証をランタイム・コード生成・単一ヘッダーへ追加 | C288–C299、GCC/Clang全回帰、単一ヘッダー |
| A05-02 | 高 | Python 3.13型パラメータ、soft keyword `type`文、位置専用引数、委譲generator、明示exception causeが未対応または意味論未接続だった | 型消去構文受理、`/`のkeyword誤用TypeError、GC走査するcause、C11 caseブロックを伴う`yield from`状態機械、iterator materializationを追加 | C300–C304、`test-py313-syntax`、単一ヘッダー、GCC/Clang全回帰 |
| A05-03 | 高 | 構造的patternがliteral/capture/ORに限定され、sequence・mapping・star・as・残余mappingを安全に表現できなかった | 再帰pattern AST、単一評価のC11状態変数生成、非送出sequence/mapping lookup、`p2c_match_mapping_rest()`、単一ヘッダー自己検証を追加 | C305–C314、GCC/Clang全回帰、Clang静的解析 |

## 検証方法

全ビルドは`-std=c11 -O2 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes`を含む設定で実施しました。通常の差分ケースは生成CをGNU C11でコンパイルします。これは内包表記とlambdaの既知のGNU文式経路を既存設計として維持するためです。一方、Alpha1.0で追加した代入式、match/case、bare raise、`raise from`、dictマージ、高次dict/setメソッド、文字列メソッド、for starred unpack、`print`キーワード引数、Python 3.13型構文、位置専用引数、`yield from`、UTF-8 `ord`/`chr`、`bin`/`oct`/`hex`、開始値付き`sum`のテストは警告即エラーのビルドおよびCPython出力差分で確認しています。common、platform中核、Hostedアダプタ、runtime、parser、AST、AST dump、lexer、codegen、semanticの10モジュールには`clang --analyze`を実行し、診断は0件でした。

> Pythonの意味論を完全に再実装できない領域では、機能を黙って受理して不正なCを出力するより、明確な診断または既知の安全なフォールバックを優先します。

## 残存制限

| 領域 | 現在の扱い | 安全上の理由 |
|---|---|---|
| 任意精度整数 | 未実装。64ビット境界を超える演算は`OverflowError` | C符号付き整数の未定義動作を防止 |
| ネスト関数・完全なnonlocal | 明示診断 | 非標準Cネスト関数と未完成クロージャGCを回避 |
| `match/case`未対応パターン | class、属性value、任意Mapping、複数star、OR枝で異なるcapture集合は未実装診断または互換フォールバック | 不完全な束縛・backtrackingを生成しない |
| `yield from`の完全プロトコル、send/throw/close、async generator、async for/with、Task、I/O待機 | `yield from iterable`の値委譲・StopIteration終了のみ対応。send/throw/close・委譲戻り値・async系拡張は非対応 | 完全な停止点分割、キャンセル・I/O・並行安全性を誤実装しないため |
| Unicode固有文字列処理 | ASCII互換のcasefold/case変換のみ | freestandingでのUnicode表の依存を避ける |
| list内包表記・lambda | 通常バックエンドではGNU拡張経路、strict C11では診断 | ISO C11移植性を明示するため |

## 再現コマンド

```sh
make CC=gcc full-build
make CC=gcc test
make CC=clang full-build
make CC=clang test
make test-single-header
make CC=clang test-single-header-c11
make CC=clang test-single-header-freestanding
make test-platform-adapter
make test-conformance
mkdir -p build/audit
for source in src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/runtime/python_code_to_c_runtime.c src/parser/python_code_to_c_parser.c src/parser/python_code_to_c_ast.c src/parser/python_code_to_c_astdump.c src/lexer/python_code_to_c_lexer.c src/codegen/python_code_to_c_codegen.c src/semantic/python_code_to_c_semantic.c; do
  clang --analyze -I./include -std=c11 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2 -Wno-format-nonliteral -Wstrict-prototypes -Wmissing-prototypes "$source"
done
```

この報告書はAlpha1.0の配布時点における厳格レビューの記録です。機能一覧と単一ヘッダー利用法は[Alpha1.0リリースノート](../release/ALPHA1.0_RELEASE_NOTES.md)を参照してください。
