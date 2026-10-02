# Python Code to C Alpha0.6 — Python 3.13 互換性契約

## 1. 目的

本書は、**Python Code to C Alpha0.6** がPython 3.13の公式言語仕様をどの範囲で受理・変換するかを定義する。対象はCPython自体の再実装ではなく、C11、freestanding、自作OS組込み、単一ヘッダー配布という制約下で、生成Cの意味を検証可能なPython言語サブセットである。

> **設計原則**: 構文を黙って受理して不正なCを出力するより、意味を保てる範囲で実装し、残る範囲は明示的な制限または安全な型消去として扱う。

Python 3.13の型パラメータ既定値はPEP 696で定義される言語機能である。[1] また、`type`文はsoft keywordであり、通常の識別子`type`の使用と字句的に区別しなければならない。[2]

## 2. 実装済み範囲

| 機能 | Alpha0.6の扱い | 実行意味論・移植性契約 | 検証 |
|---|---|---|---|
| 型パラメータ | `def f[T = int]`、`class C[T = int]`、`*Ts`、`**P`を受理 | 型専用情報として消去し、C11ランタイム型オブジェクトは作らない | `make test-py313-syntax` |
| `type`文 | `type Alias[params] = expression`をsoft keywordとして受理 | alias右辺を評価せず型専用宣言として消去する | `make test-py313-syntax` |
| 位置専用引数 | `def f(value, /)`を受理 | 該当名をキーワードで渡すと、生成Cが`TypeError`を送出する | C300–C302 |
| 明示exception cause | `raise exc from cause`を受理・生成 | exception objectがcause参照を保持し、GCトラバーサルが到達性を維持する | C304、単一ヘッダー |
| ジェネレータ委譲 | `yield from iterable`を受理・生成 | iterableの値を状態機械から順次委譲し、StopIterationで後続状態へ移る | C303 |
| 収集型組込みとの反復 | generatorを`list`、`tuple`、`set`、`sum`等に渡す | `OBJ_ITERATOR`をC11のiterator materializationで安全に収集する | C303、GCC/Clang全回帰 |
| 協調async | `async def`、制約付き`await`、`asyncio.run`、単純name targetの`async for` | 固定長FIFOの協調実行。スレッド、OS I/O、プリエンプションには依存しない | C285–C287、C340–C342 |
| `async with` | 単一context manager、optional simple-name `as` target、`__aenter__`/`__aexit__`待機 | normal exit、例外抑止、元例外再送出、class protocol method内awaitを状態機械とGC保持で処理 | C358–C362 |
| set comprehension | 同期`for`・filter・重複排除を伴う`{expression for ...}` | 通常生成ではGNU statement expressionを使い、`--c11`では明示診断 | C363–C365、`test-set-comprehension-c11` |
| decorator | module-level function、`async def`、classの`@decorator`と`@factory(...)` | 式を上から下へ評価し、function/class objectへ下から上へ適用してGC rootのcallable slotへ再束縛する | C366–C372、`test-decorator-diagnostics` |
| 構造的pattern | literal/capture/wildcard/OR、list/tuple sequence、star、dict mapping、as、guard、mapping `**rest`、bare/keyword属性class | subjectは一度だけ評価し、`**rest`は指定キーを除いた独立新規dictを返す。class属性不在はcase失敗として扱う | C305–C322、単一ヘッダー |

Python 3.13のfunction/class definitionは任意個のdecoratorを前置できる。[6] Alpha0.6はmodule-levelの安全な再束縛経路に限定してこの構文を実装する。

Python 3.13の`type`文は実行時に`typing.TypeAliasType`を作り、alias値の評価には遅延評価の規則がある。[2] Alpha0.6はfreestanding C11の小型ランタイムを優先するため、**この実行時型aliasモデルは採用せず型消去モード**として扱う。したがって、型aliasを値として参照するコードは対応対象ではない。

## 3. 意図的な制限

| 項目 | 非対応または限定内容 | 理由 |
|---|---|---|
| 完全な`yield from`プロトコル | `send`、`throw`、`close`、StopIteration戻り値の受取り、式としての完全意味論は非対応 | 委譲先への双方向制御と例外注入を、現行のC11状態機械・GC契約を壊さず追加する必要がある |
| async拡張 | async generator、async comprehension、async withの複数context/複合`as` target/複雑な停止点、Task、キャンセル、I/O待機、独自awaitableは非対応 | async forと単一contextのasync withは状態機械で対応済みだが、残る構文は停止・cleanup・例外の横断状態をさらに拡張する必要がある。[3] |
| ExceptionGroup | `except*`およびExceptionGroupの分割規則は非対応 | 例外群の分割・再結合・traceback意味論を持つ専用ランタイムが必要である。[3] |
| `locals()`・frame API | PEP 667のwrite-through frame proxyを実装しない | Alpha0.6はCPython frame、`exec`、`eval`を持たないfreestandingコンパイラである。PEP 667はoptimized scopeの`locals()`と`frame.f_locals`の意味を区別する。[4] |
| decorator拡張 | nested function/class method、`*args`/`**kwargs`を持つdecorator、decorated functionへのkeyword callは非対応 | closure・descriptor・generic keyword callable ABIを安全に拡張するまで明示診断する |
| CPython実装機能 | free-threaded build、JIT、REPL固有挙動は対象外 | これらは言語構文ではなくCPython実装または対話環境の機能である。[5] |

## 4. 品質ゲート

Python実行環境の版によって型構文の実行可否が変わるため、型パラメータ・`type`文はCPython実行差分ではなく、**Alpha0.6自身の変換と厳格C11生成Cの実行**で検証する。位置専用引数、`yield from`、`raise from`は、CPython出力との差分比較にも登録する。

| 品質層 | 実行コマンド | 合格条件 |
|---|---|---|
| Python 3.13型構文 | `make test-py313-syntax` | 型消去変換した生成Cが警告即エラーのC11で実行可能 |
| CPython差分 | `make test-conformance` | C01–C372の出力完全一致 |
| 単一ヘッダー | `make test-single-header` | 構造的pattern、`**rest`、bare/keyword属性class、module-level decoratorのadapter・GC rootを含む変換経路が自己完結ヘッダーへ収録 |
| 厳格C11 | `make CC=clang test-single-header-c11` | Clang単独、`-pedantic-errors`、警告即エラーで合格 |
| freestanding | `make test-single-header-freestanding` | Hosted allocator、出力、時刻シンボルへの依存なし |
| ベアメタル | `make test-baremetal-runtime test-baremetal-build test-baremetal-generated` | 静的ヒープ、platform出力、GC、協調await、生成CのC11オブジェクト化が合格 |
| クロスコンパイラ | `make CC=gcc full-build && make CC=gcc test` およびClang版 | GCCとClangの両方で警告即エラー全回帰が合格 |

## 5. 自作OSへの適用

型パラメータと`type`文はコンパイル時に消去されるため、自作OS側は`typing`やCPython frame APIを提供する必要がない。一方、実行する生成Cでは、通常どおり`P2C_Platform`へアロケータ、再確保、解放、出力、必要な時刻フックを接続する。mapping `**rest`も通常のdict確保・GC経路だけを用い、bare/keyword属性class patternも既存instance照合・属性取得経路だけを使うため、新しいOSコールバックを要求しない。module-level decoratorの再束縛slotも通常のGC root登録だけを使う。exception cause、状態機械generator、協調コルーチンはいずれもGC到達性を前提にするため、OS固有キューに`P2C_Object*`を保持する場合は`p2c_gc_register_root()`と`p2c_gc_unregister_root()`で明示管理する。

詳細な初期化順序、静的ヒープ、UART相当出力、クロスC11テンプレートは[`../build/BUILD_AND_LAUNCH_ALPHA0.6.md`](../../build/BUILD_AND_LAUNCH_ALPHA0.6.md)を参照する。async withとset comprehensionの具体的な状態保存・C11境界は[`ASYNC_WITH_AND_SET_COMPREHENSION_ALPHA0.6.md`](ASYNC_WITH_AND_SET_COMPREHENSION_ALPHA0.6.md)に記録する。

## 参考文献

[1]: https://peps.python.org/pep-0696/ "PEP 696 — Type Defaults for Type Parameters"
[2]: https://docs.python.org/3.13/reference/simple_stmts.html#the-type-statement "Python 3.13 Language Reference — The type statement"
[3]: https://docs.python.org/3/reference/compound_stmts.html "Python Language Reference — Compound statements"
[4]: https://peps.python.org/pep-0667/ "PEP 667 — Consistent views of namespaces"
[5]: https://docs.python.org/3/whatsnew/3.13.html "What’s New In Python 3.13"
[6]: https://docs.python.org/3.13/reference/compound_stmts.html#function-definitions "Python 3.13 Language Reference — Function definitions and decorators"
