# Python Code to C Alpha1.0 — `async with` と set comprehension 拡張仕様

## 目的

本書は、**Python Code to C Alpha1.0** に追加した`async with`状態機械と、既存のset comprehension生成経路を回帰スイートへ昇格した範囲を定義する。Python 3.13の複合文には`async with`が含まれ、非同期コンテキスト管理では`__aenter__()`と`__aexit__()`がawaitされる。[1] 集合内包表記は`{expression for ...}`の形式で、各要素をsetへ追加し重複を除く式である。[2]

> 本実装は未対応組合せを黙って同期化・無視・不正C化しない。ISO C11だけで表現できない内包表記の式位置loweringは、`--c11`で理由付き診断に切り替える。

| 機能 | Alpha1.0で保証する範囲 | 非保証または明示診断の範囲 |
|---|---|---|
| `async with` | 単一context manager、optional simple-name `as` target、`__aenter__`待機、`__aexit__`待機、通常終了、例外抑止、例外再送出 | 複数context manager、tuple/list/starred `as` target、body内の複数停止点、複雑な停止をまたぐ`try/finally` |
| class内async protocol method | protocol methodの本体に実際の`await`がある場合に、クラス名でmangleしたstate-machine entryへlowerする | 全てのclass async method構文が完全なCPython object modelを再現するものではない |
| set comprehension | 単純name target、任意数の同期`for`、任意数のfilter、重複排除 | async comprehension、`await`を含む内包表記、strict ISO C11での式位置container構築 |

## `async with`のlowering契約

各`async with`はcontext expressionを一度だけ評価してgenerator localsへ保存する。次に`p2c_async_call_attr(context, "__aenter__", ...)`を待機し、その戻り値を`as name`へ束縛する。bodyを例外フレームで実行し、正常時は`__aexit__(None, None, None)`、例外時は`__aexit__(type, exc, None)`を待機する。例外時の`__aexit__`結果がtruthyなら例外を抑止し、falsyなら**元の例外**を再送出する。

| state-machine上の値 | 保存先 | GC・再開の理由 |
|---|---|---|
| context manager | `__p2c_async_with_ctx_N` | `__aenter__`後も`__aexit__`を呼ぶため |
| `__aenter__`戻り値 | `as` targetまたは明示破棄 | bodyから参照し、`as`なしでも警告を出さないため |
| body例外 | `__p2c_async_with_exc_N` | `__aexit__`待機後の抑止判定または再送出のため |
| await結果・待機対象 | 既存generator locals / awaiting slot | mark-and-sweep GCのiterator traversalで到達可能にするため |

`p2c_async_call_attr()`は属性呼出しが既に値を返した場合にも完了済みcoroutineへ正規化し、同期例外は遅延exceptionを持つcoroutineへ変換する。このため、従来の協調scheduler・`p2c_generator_await()`・`p2c_generator_await_result()`と同じ経路でprotocol結果と例外を扱う。

class bodyの`async def`は従来、通常メソッド生成経路を通ると内部`await`を無視する危険があった。Alpha1.0ではmethod bodyを再帰走査し、実際のawait/async for/async withを含む場合だけ、通常のstate-machine emitterへ移す。awaitを持たないprotocol methodは、既存の同期戻り値正規化経路を維持する。これは性能と後方互換性を保ちつつ、停止点の黙殺を防ぐためである。

## set comprehensionとISO C11境界

set comprehensionは既存の`AST_COMPREHENSION`で`set_result=true`を使い、`p2c_set_new()`と`p2c_set_add()`へlowerする。通常のHosted生成では式位置にループと一時containerを置くためGNU statement expressionを利用する。したがって通常モードではClang/GCCの`-std=gnu11 -Werror`で差分検証し、`--c11`では生成を続けず、`comprehensions are unavailable in strict ISO C11 mode`を返す。

> これはC11対象へ不正なGNU拡張Cを出力するより安全な境界である。コンパイラ本体、runtime、single header、freestanding coreは引き続きISO C11で構築・検証する。

## 回帰受入基準

| ID | Python動作 | 検証する失敗モード |
|---|---|---|
| C358 | `__aenter__`、body、`__aexit__`の正常順序 | context・`as` valueのlocal喪失、状態遷移誤り |
| C359 | truthy `__aexit__`による例外抑止 | exception frame復帰漏れ、戻り値無視 |
| C360 | falsy `__aexit__`への元例外引渡し | exit未実行、exception喪失 |
| C361 | 外側`try/except`への元例外再送出 | type/message不一致、再送出漏れ |
| C362 | class内protocol methodからの内部await | class methodの同期誤lowering、await黙殺 |
| C363–C365 | set comprehensionの重複排除、filter、複数`for` | set構築漏れ、filter順序、nested loop誤り |
| `test-set-comprehension-c11` | `--c11`でset comprehensionを変換 | GNU拡張を含む不正なstrict C11生成 |

## 検証コマンド

```sh
make CC=clang full-build && make CC=clang test
make CC=gcc full-build && make CC=gcc test
make CC=clang test-sanitizers
make CC=clang single-header test-single-header test-single-header-c11 test-single-header-freestanding
```

ASan/UBSanの既定ゲートでは、短命CLIのarena/GC終了時保持をLeakSanitizerの停止条件から分離し、use-after-free、範囲外アクセス、未定義動作を停止条件とする。詳細は[Sanitizerと安全性](../testing/SANITIZER_AND_SAFETY_ALPHA1.0.md)を参照する。

## References

[1]: https://docs.python.org/3.13/reference/compound_stmts.html#the-async-with-statement "Python 3.13 Language Reference — The async with statement"
[2]: https://docs.python.org/3.13/reference/expressions.html#displays-for-lists-sets-and-dictionaries "Python 3.13 Language Reference — Displays for lists, sets and dictionaries"
