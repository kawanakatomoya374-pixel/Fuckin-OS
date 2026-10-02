# Alpha0.6 監査に用いる一次資料

| 観点 | 要点 | 出典 |
|---|---|---|
| Pythonのコンテナ表示 | list、set、dictは明示列挙と内包表記を持つ。dict内包表記はkey/valueの順に評価され、同一keyは後の値が勝つ。内包表記は暗黙の独立スコープを持つ。 | [Python Expressions](https://docs.python.org/3/reference/expressions.html) |
| Pythonのfor | 反復子から取り出した値を通常の代入規則でターゲットへ割り当てる。 | [Python Compound Statements](https://docs.python.org/3/reference/compound_stmts.html) |
| Pythonのtry/with | finallyは制御フローをまたいで実行される。withは例外時の`__exit__`引数と真値による抑止を伴う。 | [Python Compound Statements](https://docs.python.org/3/reference/compound_stmts.html) |
| GCC statement expression | `({ ... })`はGNU C拡張であり、式中に文・局所変数を置ける。標準Cの構文ではない。 | [GCC Statement Expressions](https://gcc.gnu.org/onlinedocs/gcc/Statement-Exprs.html) |
| GCC nested function | 入れ子関数とクロージャ的な外側変数参照はGNU C拡張であり、関数ポインタ化にはトランポリンと寿命上のリスクを伴う。 | [GCC Nested Functions](https://gcc.gnu.org/onlinedocs/gcc/Nested-Functions.html) |
