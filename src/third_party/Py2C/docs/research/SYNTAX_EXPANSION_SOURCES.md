# 構文拡張の比較基準

対応構文の追加では、構文を受理することよりも、評価順・例外・束縛・フォールバックの意味論を守ることを優先する。

| 対象 | 実装判断に使う規則 | 一次資料 |
|---|---|---|
| 代入・starred target・複合代入・`del` | 通常代入の左から右への束縛、starred targetの残余リスト、複合代入の左辺一度だけの評価、`del`の左から右への処理 | [Python simple statements][1] |
| for/while・try・with・match | loop-else、finallyの制御フロー、withの`__enter__`/`__exit__`、pattern matchingの評価順 | [Python compound statements][2] |
| import | importの検索・ロード・名前束縛を完全再実装せず、組込み/静的レジストリ/明示フォールバックの境界を定める | [Python import system][3] |

[1]: https://docs.python.org/3/reference/simple_stmts.html "Python Language Reference — Simple statements"
[2]: https://docs.python.org/3/reference/compound_stmts.html "Python Language Reference — Compound statements"
[3]: https://docs.python.org/3/reference/import.html "Python Language Reference — The import system"
