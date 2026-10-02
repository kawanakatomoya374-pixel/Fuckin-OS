# Python Code to C Alpha0.6 — リリースノート

## 概要

**Python Code to C Alpha0.6** は、Alpha0.3の厳格レビュー済み基盤を維持しながら、現実的なPython 3コードの変換範囲、例外意味論、コンテナ操作、文字列処理、および自己完結配布を大きく拡張したリリースです。製品名、文書名、テスト名、および配布物はすべてAlpha0.6へ統一されています。

> Alpha0.6の受入基準は、機能数ではなく、**CPythonとの差分出力、標準C11警告即エラー、freestanding構成、単一ヘッダー配布**を同時に満たすことです。

| 領域 | Alpha0.6で追加・強化した内容 | 検証契約 |
|---|---|---|
| 代入式 | `name := expression`を条件、while、内包表記、入れ子式で受理し、値を返しながら束縛する | C175–C178 |
| 構造的分岐 | `match/case`のliteral、`None`、capture、`_`、OR、sequence、mapping、star、as、bare/keyword属性class、`__match_args__` positional、dotted value/class、`if` guard、mapping `**rest`、subject単一評価 | C190–C194、C305–C334 |
| 例外 | `except`内のbare `raise`を同一例外の再送出として実装し、ネストした例外フレームのC変数名を一意化。`raise exc from cause`はGC追跡される明示cause参照として生成し、`__cause__`属性を参照可能にする | C198–C199、C304、C354 |
| 辞書 | `dict.copy()`、`dict.popitem()`、`dict | dict`、`dict |= dict`を挿入順・右優先更新で実装 | C195–C197 |
| 集合 | non-mutating `union`、`intersection`、`difference`、`symmetric_difference`、各update系、`isdisjoint`を追加 | C179–C189 |
| リスト | `list.copy()`を追加し、コピー独立性を確認 | C179–C189 |
| 文字列 | ASCII互換の`casefold`、`capitalize`、`swapcase`、`rfind`、`removeprefix`、`removesuffix`を追加 | C200–C203 |
| 文字列検索・置換 | 未検出時に`ValueError`を送出する`str.index`/`str.rindex`、回数制限・空文字列置換に対応する`str.replace(old, new, count)`を追加 | C212–C220 |
| list検索 | 正負の開始・終了境界を正規化する`list.index(value, start, stop)`を追加 | C221–C224 |
| 文字列範囲検索 | `find`/`index`/`rfind`/`rindex`/`count`のstart/stop、負境界、空部分文字列、`ValueError`契約を追加 | C225–C234 |
| 文字列分割 | 明示区切り・空白区切りの`str.split(sep, maxsplit)`で0・負・正の回数制限を実装 | C235–C240 |
| 文字列境界・整形 | `strip(chars)`、start/stopと文字列tupleを受けるprefix/suffix判定、`partition`/`rpartition`、`splitlines`、`expandtabs`を実装 | C241–C256、C269–C274 |
| 文字列分類・表記 | ASCII文字種判定とidentifier、`\r`/`\v`/`\f`を含むリテラルのCエスケープ、コンテナreprの制御文字エスケープを実装 | C257–C268 |
| 高次dict | `dict.fromkeys(iterable, value=None)`、pair iterableを受ける`dict.update`、重複キー後勝ち・共有value・pair arity例外を実装 | C275–C282 |
| 高次set | `issubset`、`issuperset`、空集合`KeyError`を含む`pop`を実装 | C275–C282 |
| 差分ファジング | 固定seedでdict/set状態遷移を生成しCPythonと比較。失敗時は最小prefixと再現コマンドを保存 | 既定144操作、拡張512操作 |
| for unpack | `for first, *middle, last in iterable`を受理し、残余list束縛、最小arity検査、入れ子forでの一意な生成C一時変数を実装 | C204–C207 |
| print | `print(sep=..., end=...)`、`None`の既定値復帰、非文字列キーワードに対する`TypeError`を実装 | C208–C211 |
| プラットフォーム | `p2c_platform_write()`を登録済み`P2C_Platform.write`へ接続し、OSアダプタへ`print`と例外診断を転送 | 直接Cプラットフォーム回帰 |
| ジェネレータ | `yield [value]`、`yield from iterable`、単一`for name in iterable`と任意filterを持つgenerator expressionを明示状態機械へ変換する。generator expressionは関数ローカル自由変数を生成時にGC管理localsへcaptureし、唯一の関数引数では外側の括弧を省略できる。generator式唯一引数のparser安全性も修正済み | C283–C284、C303、C335–C339、C348–C349、直接C回帰 |
| 協調async | `async def`、代入右辺の比較・短絡論理を含む単一await、`asyncio.run()`、`async for`の`__aiter__`/`__anext__`待機、`StopAsyncIteration`、body/else、`break`、`continue`を固定長FIFO状態機械へ変換 | C285–C287、C340–C342、直接C回帰 |
| async with | `AST_WITH.is_async`、複数context managerの左から右への`__aenter__`待機・右から左への`__aexit__`待機、通常exit、truthy exitによる抑止、falsy exit後の元例外再送出、class内protocol methodの内部awaitをstate machineへ変換 | C358–C362、C373–C383 |
| set comprehension | 同期`for`・filter・重複排除を伴う`{expression for ...}`を`p2c_set_new`/`p2c_set_add`へlowerし、strict ISO C11ではGNU拡張Cを出さず明示診断する | C363–C365、`test-set-comprehension-c11` |
| decorator | `@decorator`と`@factory(...)`をparserで受理し、module-level function/classについて上から下への式評価、下から上への適用、GC rootを持つcallable object再束縛を実装。bare-name decorator、default引数、async function、class objectをCPython差分で確認 | C366–C372、`test-decorator-diagnostics` |
| ネストしたクラス定義 | クラス本体の直下の`class`を、C名を外側クラス名で前置した`Outer__Inner`へlowerし、外側クラスの`__classobj()`が上から下への評価順で属性登録する。クラス本体の式からはその名前で参照でき、外部からは`Outer.Inner`で参照・構築できる。同名ネストクラスの衝突回避、二段ネスト、ネストクラス版`__str__`、`isinstance(x, Outer.Inner)`も対応。関数本体内の`class`定義とメソッド本体からのクラススコープ参照は明示診断 | C461–C479、`test-embed-generated`、`test-baremetal-generated`、`test-decorator-diagnostics` |
| C識別子の衝突回避 | `index`/`round`/`abs`/`pow`/`sqrt`/`log`/`main`等をパラメータ・ローカル・モジュール関数名に使っても、前方宣言・パラメータ宣言・`(void)`キャスト・本体参照がすべて同じ`p2c_user_<name>`へ揃うよう統一（クラスメソッドの`(void)`キャストだけ生名で、生成Cが`undeclared identifier`になっていた） | C480–C486、`test-baremetal-generated` |
| ベアメタル | 静的ヒープ・UART相当出力・tick・GC・起動入口・クロスC11テンプレート・実変換Python例を追加 | `test-baremetal-runtime`、`test-baremetal-build`、`test-baremetal-generated` |
| 文字コード・基数変換 | UTF-8単一Unicodeスカラー値の`ord`/`chr`、負値を含む`bin`/`oct`/`hex`、開始値付き`sum(iterable, start)`を追加し、`sum`の引数数を検証 | C288–C299、単一ヘッダー自己テスト |
| Python 3.13型構文 | soft keyword `type Alias[params] = expr`、既定型パラメータ、TypeVarTuple、ParamSpecをfreestanding互換の型消去で受理。位置専用引数`/`はキーワード誤用時に`TypeError` | C300–C302、`test-py313-syntax` |
| closure / lambda | `OBJ_CELL`、GC走査されるclosure環境、ネスト`def`の読取りcapture、direct child `nonlocal`の共有再束縛、関数ローカルcaptureを行うC11 lambdaを実装 | C343–C347、監査A01 |
| parser安全性 | starredアンパック代入で`NEXT()`後に解放済みtokenを読むuse-after-freeを修正し、token位置を消費前に値コピーする | `test-parser-sanitizers` |
| GC lifecycle hardening | shutdown/reinit後にmodule registry、class registry、async queue、pygame static class slot、root tableが解放済みobjectを保持しないようepoch resetを実装。constructorの後段allocation失敗はGC追跡リストからrollbackする | lifecycle/allocator-failure/LeakSanitizer直接C回帰 |
| Sanitizer / CI | `-fsanitize=address,undefined`によるcompiler・生成C・383件CPython差分、GC lifecycle、allocator rollbackを実行する`test-sanitizers`と、runtime shutdownを検査するLeakSanitizer gateをCIへ追加 | `make CC=clang test-sanitizers`、`make CC=clang test-gc-leaks` |
| 配布 | コンパイラ、ランタイム、GUI抽象化、platform中核、Hosted互換アダプタを含む`python_code_to_c_single.h`をAlpha0.6機能で再生成 | Hosted・strict C11・freestanding単一ヘッダー試験 |

## 実装上の重要事項

代入式は専用トークン、ASTノード、事前宣言走査、C代入式生成を通じて実装されています。代入先はPythonの規則に合わせて名称だけに限定し、属性・添字などを代入式ターゲットとして受け入れて壊れたCを生成することはありません。事前宣言走査は、条件、ループ、呼出し、内包表記、with、return、assert、例外、match guardを再帰的に対象とするため、代入式由来のC未宣言変数を防止します。

`match/case`はsubjectを一度だけ評価し、先に現れた一致caseだけを実行します。captureとasはguardより前に束縛され、guardが偽の場合は次のcaseを評価します。Alpha0.6はリテラル、`None`、capture、wildcard、OR、list/tuple sequence、star capture、dict mapping、mapping `**rest`、as-pattern、bare class、keyword属性class pattern、`__match_args__`に基づくclass positional pattern、dotted value/class patternを保証します。`**rest`は指定キーを除く独立した新規dictを挿入順で返します。`__match_args__`を使わないclass positional推論、任意Mapping実装は明示診断または互換フォールバックの対象です。

bare `raise`は、ハンドラで処理中の例外をスレッドローカル状態に保持して再送出します。再送出の直前に状態をクリアするため、`longjmp`が通常のブロック終端を飛び越しても、後続のハンドラ外bare raiseが古い例外を再送出することはありません。

## 単一ヘッダー配布

`include/python_code_to_c_single.h` は生成物です。手編集せず、正本の`include/`と`src/`を変更した後に`make single-header`で再生成します。実装を出力する翻訳単位では`P2C_SINGLE_HEADER_IMPLEMENTATION`を定義します。自作OSでは`PYTHON_CODE_TO_C_NO_STDLIB`を定義して独自の`P2C_Platform`を登録します。`P2C_SINGLE_HEADER_NO_HOSTED`は、OS側が互換出力・入力・停止フックも提供する場合だけ定義します。

Alpha0.6の自己テストは、単一ヘッダーだけをincludeする翻訳単位でPython 3.13型パラメータと`type`文の型消去、位置専用引数、module-level decoratorのcallable adapter・GC root再束縛、`raise from`、`yield from`、async withを含む状態機械ジェネレータ、UTF-8 `ord`/`chr`、`bin`/`oct`/`hex`、開始値付き`sum`、協調スケジューラ、`:=`、sequence/mapping/star/as/OR/guard・bare/keyword属性class patternとmapping `**rest`を含む`match/case`、辞書マージ、for starred unpack、`print(sep=..., end=...)`、文字列探索範囲、`split(maxsplit)`、`str.index`/`rindex`、回数指定`replace`、範囲指定`list.index`、`strip(chars)`、prefix/suffix、partition、splitlines、expandtabs、文字種判定、`dict.fromkeys`、pair iterable `update`、setの包含関係・`pop`までを変換し、生成Cに必要な経路が含まれることを確認します。追加のPy2cソースはリンクしません。`make CC=clang test-single-header-c11`は、GCCを使わずにClangだけで`-std=c11 -pedantic-errors`の自己完結ビルドを実行します。`make CC=clang test-single-header-freestanding`は、Hosted allocator・出力・時刻参照のないfreestanding実装部を検証します。数値変換・書式化・数学補助はOS提供契約です。

## 品質結果

| 品質ゲート | 結果 |
|---|---|
| CPython差分コンフォーマンス | **C01–C749、749件一致** |
| GCC一括構築 | `make CC=gcc full-build` 合格 |
| GCC全回帰 | `make CC=gcc test` 合格 |
| Clang一括構築 | `make CC=clang full-build` 合格 |
| Clang全回帰 | `make CC=clang test` 合格 |
| 単一ヘッダー | GCC/Clangで`make test-single-header`合格。Clang単独の`test-single-header-c11`と`test-single-header-freestanding`も合格 |
| freestanding | GCC/Clang構成で汎用`freestanding-c11.mk`の静的ライブラリ生成を含む全回帰合格 |
| プラットフォーム出力 | `make test-platform-adapter`で登録済み`write`コールバックへの出力到達を確認 |
| ジェネレータ・協調async | 状態保存、停止・再開、`yield from`委譲、ネストawait、FIFO実行、GC到達性、async forの終了例外・body/else・break/continue、async withのenter/exit・抑止・再送出・class protocol await、C283–C287・C303・C340–C342・C358–C362が合格 |
| ベアメタル | 静的ヒープ・platform write・tick・GC・awaitの実行と、実変換Python例のfreestanding C11コンパイルが合格 |
| 決定的コンテナファジング | `make test-container-fuzz`で既定3 seed × 48操作が合格。追加8 seed × 64操作も合格 |
| 静的解析 | common、platform中核、Hostedアダプタ、runtime、parser、AST、AST dump、lexer、codegen、semanticの10翻訳単位でClang静的解析の診断0件 |
| ASan / UBSan | `make CC=clang test-sanitizers`でparser、GC lifecycle、allocator rollback、generator式関数引数、builtin key、exception cause、async with、set comprehension、decorator回帰、生成C runtime、C01–C383を診断なしで通過 |
| LeakSanitizer | `make CC=clang test-gc-leaks`で3 runtime epochのshutdown後にruntime所有heapが残らないことを確認 |
| 名称監査 | 文書・テスト・版番号・生成コード表記をPython Code to C Alpha0.6 / 0.6.0へ統一し、旧版参照は残存なし |

## 追加構文と厳格化（2026-09-28の第2ラウンド）

第2ラウンドで、多重継承の**C3線形化MRO**、基底クラスのクラス属性の継承、**束縛メソッド**（`m = obj.method`）、`finally`内の`return`/`break`/`continue`、**複数for節・タプルターゲットを持つジェネレータ式**を追加し、`sorted()`/`min()`/`max()`のタプル・リスト比較と安定マージソート（O(n log n)）を実装しました。CPython差分は**549アサーション**（C487–C549を追加）へ増え、全一致です。

ビルドはさらに厳格化し、`-Wcast-align=strict`、`-Wlogical-op`、`-Wduplicated-cond/-branches`、`-Wstrict-overflow=2`、`-Wformat-overflow=2`/`-Wformat-truncation=2`/`-Wstringop-overflow=4`、`-Wuse-after-free=3`、`-Wjump-misses-init`、`-Wswitch-default`、`-Wunused-macros` などを追加（追加分の28警告はすべて修正）。ホスト向けに実行時ハードニング（`-fstack-protector-strong`、`-fstack-clash-protection`、`-D_FORTIFY_SOURCE=3`）を既定適用し、組込み向けに `make test-stack-usage`（フレーム上限4096バイト）と `make test-analyzer`（GCC `-fanalyzer`）を新設しました。`-fanalyzer` が検出した例外生成失敗時のNULL参照1件も修正しています。詳細は[厳格ビルドと静的検査](../testing/STRICT_BUILD_AND_ANALYSIS_ALPHA0.6.md)を参照してください。

## 既知の制限

Alpha0.6はPython実装全体ではありません。整数は任意精度ではなく安全な64ビット固定幅であり、範囲外は`OverflowError`として扱います。`yield from`へのsend/throw/close・委譲戻り値、star target・nested closure内の自由変数captureを持つgenerator expression、async generator、async withの複合`as` target・body内の複雑な停止点、Task、キャンセル、I/O待機、async for body/else内のawait、複数awaitを含む副作用順依存式、再帰closure、深い多段capture、**nested function/class method decorator、可変長引数decorator、decorated functionへのkeyword call**、`__match_args__`を使わないclass positional推論・任意Mapping実装などの未対応`match/case`パターン、実行時の`typing.TypeAliasType`、Unicode固有の文字列casefoldは未実装または明示診断です。`yield from iterable`の値委譲、`async def`、拡張await、`async for`、`asyncio.run`、ネスト`def`、direct child `nonlocal`、C11 lambda closure、型構文の型消去受理、**ネストしたクラス定義**（モジュール直下とクラス本体の直下のみ。関数本体内の`class`定義とメソッド本体からのクラススコープ参照は明示診断）は対応します。クラスオブジェクトの`__name__`が未実装であること、クラスオブジェクト経由のメソッド取り出し（`Class.method`）、クラスメソッドへのデコレータ（`@staticmethod`/`@classmethod`/`@property`）、関数本体内の`class`定義は、引き続き既知の制限です。これらの制限は、非標準C拡張や不正な生成Cへ暗黙にフォールバックしないための安全契約です。

詳細な構文範囲は[機能リファレンス](../spec/FEATURE_REFERENCE_ALPHA0.6.md)、[async with・set comprehension拡張仕様](../spec/ASYNC_WITH_AND_SET_COMPREHENSION_ALPHA0.6.md)、および[decorator拡張仕様](../spec/DECORATORS_ALPHA0.6.md)、起動・ビルド・クロス構築は[起動・ビルド手順](../build/BUILD_AND_LAUNCH_ALPHA0.6.md)、ファジング運用は[コンテナファジング手順](../testing/CONTAINER_FUZZING_ALPHA0.6.md)、移植条件は[構文・移植性ガイド](../spec/SYNTAX_AND_PORTABILITY.md)、テストIDは[コンフォーマンス台帳](../testing/CONFORMANCE_TEST_MATRIX_ALPHA0.6.md)を参照してください。
