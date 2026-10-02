# Python Code to C Alpha0.6 — 大規模構文拡張仕様

## 1. 目的

Alpha0.6は、Alpha0.3のC11・freestanding・警告ゼロの基準を維持したまま、日常的なPython 3コードをより広く変換できるようにする拡張版である。構文を増やすだけでなく、すべての追加機能をHosted CLI、GUI、freestandingコア、`python_code_to_c_single.h`に同時反映する。各機能はCPython差分テスト、直接Cランタイムテスト、または期待診断テストのいずれかを必須とする。

| 領域 | Alpha0.6で実装する契約 | 互換性・移植性方針 |
|---|---|---|
| 代入式 | `name := expression`を式として評価し、値を返しながら現在の有効スコープへ代入する | 標準C11のカンマ演算子を使い、式の評価順を固定する |
| アンパック | 代入starred targetとリテラルunpackに加え、forターゲットのstarred unpackを提供する | arity不足・過剰はPython互換例外とし、入れ子forでもCの一時変数衝突と未定義動作を避ける |
| 例外 | `try/except/else/finally`の完全な実行順、bare `raise`、`raise exc from cause`、例外型tupleを扱う | exception causeをGC走査し、既存例外フレームのスタック規約を保つ |
| 構造的条件分岐 | literal、`None`、capture、`_`、OR、sequence、mapping、star、as、bare/keyword属性class、`if` guard、mapping `**rest`による`match/case`を提供する | subjectは一度だけ評価し、`**rest`は指定済みキーを除く新規dictを返す。class positional・dotted class/value・任意Mappingは明示診断または互換フォールバック |
| コンテナAPI | list/dict/set/strの高頻度メソッド、辞書マージ・更新、集合のin-place更新、`dict.fromkeys`、pair iterable `dict.update`、setの包含関係・`pop`、範囲指定`find`/`index`/`rfind`/`rindex`/`count`、`split(sep, maxsplit)`、`strip(chars)`、prefix/suffix境界、partition、文字種判定、splitlines、expandtabs、回数指定`str.replace`、範囲指定`list.index`を追加する | 値等値・ハッシュ・挿入順・共有valueの既存契約を維持する |
| 呼出しと評価順 | `print`の位置引数と`sep`/`end`キーワードを含む対応済み組込み呼出しを正しい値・例外契約で処理する | 型不正なprintキーワードを`TypeError`とし、登録済み`P2C_Platform.write`へ出力を接続する |
| 文字コード・基数変換 | UTF-8単一Unicodeスカラー値の`ord`/`chr`、64ビット整数の`bin`/`oct`/`hex`、開始値付き`sum`を提供する | Unicode表やlocaleに依存せずC11・freestandingで動作し、符号・接頭辞・範囲例外を固定する |
| generator・async | `yield [value]`と`yield from iterable`をGC到達可能な状態機械へ、`async def`・単純await・`asyncio.run`をFIFO協調実行へ変換する | `yield from`へのsend/throw/close・委譲戻り値、async generator、I/O待機、キャンセル、任意のネスト停止点は誤生成せず互換フォールバックとする |
| ベアメタル | 静的ヒープ・UART相当出力・tick・起動入口・生成Python例を提供する | `-ffreestanding -fno-builtin -Werror`で直接実行ハーネスと生成Cオブジェクトを検証する |
| 表記 | raw文字列、隣接文字列リテラル、数値の基数プレフィックスとunderscoreを強化する | C11とfreestandingで必要な変換だけを内部実装する |
| 型構文 | 変数・引数・戻り値注釈、注釈のみの変数宣言、Python 3.13の型パラメータ既定値、`type`文、TypeVarTuple、ParamSpecを受理する | freestanding互換の型消去で実行時評価を行わず、`typing.TypeAliasType`は対象外 |

## 2. 実装順序

まず名称・ビルド・単一ヘッダー生成をAlpha0.6へ移し、その後に評価順基盤、代入式、アンパック、例外、パターン照合、コンテナAPIの順で実装する。評価順基盤を先行させる理由は、後続の新構文が副作用を持つ複数式を扱うためである。各段階でCPythonとの差分コーパスを増やし、GCCとClangのC11警告即エラー設定を通す。

> ネスト関数・`nonlocal`を「見かけ上動く」非標準Cとして復活させない。Alpha0.6では、実際のクロージャ環境とGC統合を実装できるまで、変換時診断を維持する。

## 3. 単一ヘッダーの配布契約

`include/python_code_to_c_single.h`は、公開ヘッダー、lexer、parser、semantic、codegen、runtime、GUI抽象化、platform中核、Hosted互換アダプタを一つに展開する。Alpha0.6で追加するAST、ランタイムAPI、構文変換、テスト用APIも、通常の複数ソース構成と単一ヘッダーで同一実装を使う。自己テストはPython 3.13型構文、位置専用引数、`raise from`、`yield from`を含む状態機械ジェネレータ、sequence/mapping/star/as/OR/guard・bare/keyword属性class patternとmapping `**rest`を含む`match/case`、UTF-8 `ord`/`chr`、`bin`/`oct`/`hex`、開始値付き`sum`、協調スケジューラ、for starred unpack、`print(sep=..., end=...)`、文字列探索範囲、`split(maxsplit)`、strip、prefix/suffix、partition、splitlines、expandtabs、文字種判定、`dict.fromkeys`、pair iterable `update`、setの包含関係・`pop`を含めて、通常構成との差異を検出する。`P2C_SINGLE_HEADER_IMPLEMENTATION`を定義する翻訳単位は一つだけとし、freestandingでは`PYTHON_CODE_TO_C_NO_STDLIB`を定義して`P2C_Platform`を登録する。`P2C_SINGLE_HEADER_NO_HOSTED`はOS側が互換フックを提供する場合だけ併用する。

## 4. 品質受入基準

| ゲート | 合格条件 |
|---|---|
| CPython差分 | C01–C322を対象に、Alpha0.6拡張ケースを含む全出力一致 |
| 直接Cテスト | 新ランタイムのexception cause、境界、評価順、登録済みプラットフォーム出力、状態機械generator、協調awaitを検証 |
| GCC | `make CC=gcc full-build`および`make CC=gcc test`が警告ゼロで成功 |
| Clang | `make CC=clang full-build`および`make CC=clang test`が警告ゼロで成功 |
| make起動 | `check-tools`、`help`、`run INPUT=...`、`print-config`がビルド・起動・クロス構成を再現可能にする |
| 決定的ファジング | 固定seedのdict/set操作列をCPythonと生成Cで比較し、失敗時は最小prefixと再現コマンドを保存する |
| 静的解析 | common、platform中核、Hostedアダプタ、runtime、parser、AST、AST dump、lexer、codegen、semanticの10モジュールでClang静的解析が警告0件 |
| 単一ヘッダー | GCC/Clangの双方で自己完結ビルド・実行が成功し、Clang単独では`-std=c11 -pedantic-errors`およびfreestanding実装部コンパイルが成功 |
| freestanding | 汎用`freestanding-c11.mk`で`CC`、`AR`、ABI・追加フラグを受けて静的ライブラリを生成し、単一ヘッダーにHosted allocator・出力・時刻参照を残さずコンパイル可能 |
| Python 3.13型構文 | `test-py313-syntax`が型パラメータ・`type`文を型消去して厳格C11生成Cで検証 |
| ベアメタル | `test-baremetal-runtime`、`test-baremetal-build`、`test-baremetal-generated`が静的ヒープ・出力・GC・協調await・実変換Cを検証 |
