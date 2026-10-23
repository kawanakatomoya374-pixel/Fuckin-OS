# Python Code to C Alpha1.0 — 次期Python 3.13構文監査

## 公式仕様から確認した事項

Python 3.13の完全文法は、function/class definitionに任意個のdecoratorを許可し、decoratorの構文を`'@' named_expression NEWLINE`として定義する。[1] 同じ文法は`except*`、`async for`を含むcomprehension、`async with`の複数item・star targetを定義している。[1]

公式の式仕様では、list/set/dict displayは明示要素列またはcomprehensionで構築される。comprehensionは少なくとも一つのfor節を持ち、left-to-rightの入れ子として評価される。[2] async def内のcomprehensionは`async for`と`await`を含められるが、停止可能なimplicit scopeになる。[2]

公式の複合文仕様では、`except*`はExceptionGroupをmatching/non-matching subgroupに分割し、各handler実行後に未処理部分と新規例外をmergeする。[3] これは専用の例外群object、split/merge、複数handler制御を必要とするため、既存の単一例外frameだけで安全に部分実装してはならない。[3]

## 優先候補

| 優先度 | 構文 | 判断 | 理由 |
|---|---|---|---|
| P0 | decorator | **module-level function/classで実装済み** | parser、AST、semantic、codegen、GC root、single-headerへ接続し、上から下への評価・下から上への適用をC366–C372で検証。nested/method/vararg/keyword-callは明示診断 |
| P0 | nested class definition | **実装済み（C461–C479）** | クラス本体の直下の`class`をC名`Outer__Inner`へ解決し、外側クラスの`__classobj()`が評価順に属性登録する。parser変更なし、新しいランタイムAPI・GCルートなしで組込み構成でも動く。メソッド本体からのクラススコープ参照と関数本体内の`class`定義は明示診断 |
| P1 | multi-context `async with` | **実装済み（C373–C383）** | itemごとのenter/exit stateを生成し、enter途中・body例外・exit例外の逆順cleanupを検証済み |
| P2 | async comprehension | 後続対象 | implicit scope、async iterator、await、複数停止点を同時に実装する必要がある |
| P3 | `except*` / ExceptionGroup | 設計先行 | subgroup split/mergeとtraceback意味論を持つruntimeが必要 |
| P4 | bytes / complex / ellipsis / `@` | 値モデル先行 | lexer/parser受理だけでは不十分で、object typeと演算意味論が必要 |
| P5 | 複数for節のgenerator expression / メソッドデコレータ | 次の候補 | 前者は既存のstep関数state machineの入れ子化、後者はdescriptor相当のメソッドフラグとgetattr経路が必要。受理だけでは完了としない |
> decoratorはmodule-level function/classの安全な範囲で実装し、Clang/GCC full-build、ASan/UBSan、strict C11、freestanding、single-header、C01–C372のCPython差分を通過した。複数contextを持つ`async with`もC373–C383で実装済みである。ネストしたクラス定義はC461–C479に加え、`make test-embed-generated`（`examples/embed/embed_boot.py`）と`make test-baremetal-generated`（`examples/baremetal/baremetal_hello.py`）で、カーネル側ヒープだけを与えた組込み構成でも検証している。次の候補は、メソッドデコレータ（`@property`/`@staticmethod`/`@classmethod`）、関数本体内の`class`定義、デコレータ関数側の`*args`/`**kwargs`、複素数型、`bytes`/`bytearray`である（複数for節のgenerator expressionと多重継承のC3線形化はC506–C525で実装済み）。いずれの候補もparser受理だけでは完了とせず、AST、semantic、codegen、runtime/GC、strict diagnostics、CPython差分を一貫して実装できない場合は理由付きdiagnosticを維持する。

## 2026-09-28 追加監査: ネストしたクラス定義

Pythonのgrammarは`classdef`をsuiteの一行として許可するため、クラス本体の直下にも`class`を書ける。本実装はこれを次の規則でlowerする。

| 対象 | 今回の判断 | 安全境界 |
|---|---|---|
| `class Outer: class Inner:` | 実装済み | C名を`Outer__Inner`に前置してシンボルを分離する。Python名は`p2c_class_new`の表示名として維持 |
| `Outer.Inner`による外部参照 | 実装済み | 外側クラスの`__classobj()`が`p2c_setattr`で属性登録するため、既存の属性解決経路だけで動く |
| クラス本体での`alias = Inner` | 実装済み | クラス本体の文を上から下へ評価し、その文脈でだけ見える「Python名→C名」マップで解決する |
| メソッド本体からのクラススコープ参照 | 明示診断 | Pythonでは`NameError`。黙って未定義C識別子を出さない |
| 関数本体の中の`class`定義 | 明示診断 | Cの入れ子関数定義になり得ないため、意味解析で拒否する |
| 同じ単純名の別の外側クラス | 実装済み | C名が前置されるため衝突しない（`Left__Node`と`Right__Node`） |
| 基底クラスのクラス属性の継承 | 対象外（既知の制限） | 継承で引き継がれるのはメソッドと`__init__`のみ（本変更前からの制限） |

## References

[1]: https://docs.python.org/3.13/reference/grammar.html "Python 3.13 Full Grammar specification"
[2]: https://docs.python.org/3.13/reference/expressions.html#displays-for-lists-sets-and-dictionaries "Python 3.13 Language Reference — Displays for lists, sets and dictionaries"
[3]: https://docs.python.org/3.13/reference/compound_stmts.html#except-star "Python 3.13 Language Reference — The except* clause"

## 2026-08-22 追加監査: 複数context manager async with

Python 3.13のcompound statement grammarは`async_with_stmt`を独立したcompound statementとして定義し、`async with`のsuiteを持つ。[2] 非同期context managerは`__aenter__()`と`__aexit__()`をawaitするプロトコルであり、PEP 492はenter・exitの非同期呼出しをasync withの目的として定義している。[3] 複数itemは単一の文として受理できるが、実装上は外側から内側へenterし、body終了時は内側から外側へexitする入れ子相当のcleanup順を維持しなければならない。[2]

| 対象 | 今回の判断 | 安全境界 |
|---|---|---|
| `async with A() as a, B() as b:` | 次の実装対象 | Aのenter完了後にBをenterし、Bのexit完了後にAをexitする状態列へlowerする |
| `async with A(), B():` | 同時実装対象 | enter結果は破棄するが、各protocol awaitと例外cleanupは保持する |
| 複合`as` target | 複数itemの後段 | tuple/list/star targetは通常withのtarget binderを再利用し、未対応形は明示診断する |
| enter途中の例外 | 必須回帰 | 既に成功した内側/外側contextだけを正しい逆順でexitする必要がある |
| body例外 | 必須回帰 | 全てのexitへ例外情報を渡し、各exitのtruthy結果に応じて抑止または再送出する |
| exit途中の例外 | 必須回帰 | 新しいexit例外が元例外を置換するPython規則を状態機械で保持する |
| `@asynccontextmanager` | 後続対象 | async generator、yield境界、再利用可能なdecoratorを同時に要するため別設計とする。[4] |

現行Alpha1.0は単一context managerのasync withを実装済みであり、今回の拡張ではASTのitem vectorと既存`P2C_AstWith.is_async`を保持したまま、context object、enter result、pending exception、exit resultをitemごとのgenerator localへ分離する。本文に複雑なawaitやreturnを混在させる既存制約は維持し、状態機械で安全に表現できない入力を暗黙変換しない。

## References

[2]: https://docs.python.org/3.13/reference/compound_stmts.html "Python 3.13 Language Reference — Compound statements"
[3]: https://peps.python.org/pep-0492/ "PEP 492 — Coroutines with async and await syntax"
[4]: https://docs.python.org/3.13/library/contextlib.html "Python 3.13 Library Reference — contextlib"
