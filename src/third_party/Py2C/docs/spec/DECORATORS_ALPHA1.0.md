# Python Code to C Alpha1.0 — Decorator 拡張仕様

## 目的

Alpha1.0は、Python 3.13のfunction definitionおよびclass definitionに前置できるdecorator構文を受理する。[1] 実装の目的は`@decorator`と`@factory(...)`を安全なC11生成へlowerし、**評価順、適用順、再束縛、GC到達性**をCPython差分で確認可能にすることである。

> Pythonのdecorator構文はdefinitionの直前に複数行置ける。Alpha1.0は各decorator式を**上から下へ一度ずつ評価**し、生成されたdefinition objectに対して**下から上へ**callableを適用する。[1]

## 対応範囲

| 対象 | 対応 | 実装契約 |
|---|---|---|
| module-level function | 対応 | decoratorの戻り値をグローバルcallable slotへ再束縛する |
| module-level `async def` | 対応 | state-machine entryをcallable adapterで包み、decorator後も`asyncio.run()`へcoroutineを渡せる |
| module-level class | 対応 | class objectをdecoratorへ渡し、返り値をclass名のcallable slotへ再束縛する |
| bare-name decorator | 対応 | 通常C関数を`P2C_CallableFn` adapterでfunction objectへ変換して渡す |
| factory decorator | 対応 | 例: `@factory("label")`。factory expressionはdefinition objectより先に評価する |
| 複数decorator | 対応 | temporary slotに式の評価結果を保存して評価順と適用順を分離する |
| default引数を持つdecorated function | 位置呼出しで対応 | adapterが位置引数個数を検査し、欠落分を既存default値生成経路へ渡す |
| nested function decorator | 明示診断 | closureおよびlocal名再束縛の完全な評価規約を実装するまで拒否する |
| class method decorator | 明示診断 | descriptor、`staticmethod`、`classmethod`、property再束縛の意味論を誤実装しないため拒否する |
| `*args` / `**kwargs`を使うdecorator | 明示診断 | generic callable adapterに可変長引数ABIを追加するまで拒否する |
| decorated functionへのkeyword call | 明示診断 | keyword名解決をgeneric callable adapterへ安全に追加するまで拒否する |

## Loweringと所有権

module-level function `f`では、通常のC entryと`f__decorator_adapter(P2C_Object **args, size_t nargs)`を生成する。adapterは固定引数のarityを検査し、元のentryへ渡す。その後、definition位置に次の概念的手順を出力する。

```text
D0 = evaluate(topmost_decorator)
D1 = evaluate(next_decorator)
value = function_object(f__decorator_adapter)
value = D1(value)
value = D0(value)
_p2c_decorated_f = value
```

`_p2c_decorated_f`はfile-scopeの`P2C_Object*`であり、起動時にGC rootとして登録する。以降の`f(...)`と式位置の`f`参照はこのslotを使う。この設計により、decoratorがclosureまたはclass objectを返しても、保守的スタックスキャンに依存せず到達可能性を維持する。

class decoratorも同じ順序を使うが、初期valueは`Class__classobj()`が構築したclass objectである。decorator適用後の`Class(...)`は再束縛済みslotをgeneric callableとして呼び出す。

## C11とfreestanding

decorator loweringはC11の関数、固定長配列初期化子、runtimeの`p2c_function_new()`と`p2c_call()`だけを用いる。GNU nested function、statement expression、compiler固有attributeは使用しない。そのため、decoratorを含む生成Cはstrict ISO C11の警告即エラー条件で検証対象にできる。コンパイラcoreと単一ヘッダーには同じcodegenを含むため、`PYTHON_CODE_TO_C_NO_STDLIB`のfreestanding構成にも新しい構文受理・生成経路が含まれる。

## 回帰と期待診断

| 契約 | テスト |
|---|---|
| 複数decoratorの評価順と適用順 | C366–C369, `tests/decorator_alpha10.py` |
| bare-name decoratorのfunction object adapter | C370 |
| decorated async function | C371 |
| decorated class | C372 |
| keyword call、vararg decorator、nested function、method decoratorの拒否 | `tests/decorator_diagnostics_alpha10.sh` |
| AST dumpでのdecorator可観測性 | `tests/decorator_alpha10.py`に対する`--dump-ast` |
| memory safety | `make CC=clang test-sanitizers`のC01–C372差分実行 |

## References

[1]: https://docs.python.org/3.13/reference/compound_stmts.html#function-definitions "Python 3.13 Language Reference — Function definitions and decorators"
