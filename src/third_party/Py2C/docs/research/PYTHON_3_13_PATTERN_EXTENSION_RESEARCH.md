# Python 3.13 構造的pattern拡張調査

## 公式根拠

Python 3.13の複合文仕様は、`match`のclosed patternとしてliteral、capture、wildcard、value、group、sequence、mapping、class patternを定義する。また`try`では`except`とは混在できない`except*`とExceptionGroup分割・再結合の意味論を定義する。[1]

PEP 634では、class patternはclass名に対する`isinstance()`、属性keyword pattern、`__match_args__`を使う位置pattern変換を含むこと、mapping patternは`**name`に残余の新しいdictを束縛することを規定する。OR patternの各枝は同一のcapture集合を束縛しなければならない。[2]

## 優先順位

| 候補 | 優先度 | 判断 |
|---|---:|---|
| class patternのbare型・keyword属性pattern | 高 | 既存のclass/instance/attributeランタイムを活用でき、C11・freestandingで完全に局所実装できる |
| dotted value pattern | 中 | 名前・属性解決の副作用と同一case中のキャッシュ契約を設計する必要がある |
| class positional patternと`__match_args__` | 中 | keyword class patternの後に設計する。`__match_args__`を追加するためランタイム表面積が増える |
| `except*`とExceptionGroup | 低 | subgroup split/merge、禁止制御移譲、例外階層を新設する必要があり、現行freestanding例外ランタイムへの影響が大きい |

次の実装は、`case TypeName()`および`case TypeName(attribute=pattern, ...)`を対象とするclass patternにする。未対応の位置pattern、dotted class名、`__match_args__`、例外グループは明示診断または既存互換フォールバックとして維持する。

## 参照

[1]: https://docs.python.org/3.13/reference/compound_stmts.html "Python 3.13 Language Reference — Compound statements"
[2]: https://peps.python.org/pep-0634/ "PEP 634 — Structural Pattern Matching: Specification"
